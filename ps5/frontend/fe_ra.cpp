// Snes9x PS5 (2.11): RetroAchievements (fe_ra.h).
// SPDX-License-Identifier: MIT

#include "fe_ra.h"

#include "fe_borderimg.h"
#include "fe_emu.h"
#include "fe_i18n.h"
#include "fe_rahash.h"
#include "fe_ranet.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_version.h"

#include "snes9x.h"
#include "memmap.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>

#ifndef SNES9X_PS5_VERSION
#define SNES9X_PS5_VERSION "dev"
#endif

namespace ra
{
namespace
{
// What went wrong with the requests, as far as the player can do something about it
enum class Issue
{
	None,
	NoHelper,
	OldHelper,
	Offline,
	Server, // the server answered 5xx
};

// What the game is to RetroAchievements
enum class GameRa
{
	Off, // no game, or no account
	Loading, // hashed, its set asked for
	Active, // achievements or leaderboards loaded
	NoSet, // known to RetroAchievements, but nothing to unlock (maybe rich presence)
	Unknown, // the server doesn't know the hash
	Failed, // the set could not be had (no network...)
};

struct Toast
{
	std::string title, text, right, badge;
	fe::border::Image picture; // the badge, once it is here
	double shown_at = -1; // the first frame that drew it
	double seconds = 5;
};

struct State
{
	rc_client_t* client = nullptr;
	Login login = Login::None;
	int login_result = RC_OK; // why the last login failed
	bool login_with_token = false;
	std::string login_message; // the server's own words, for the log and the rare error with no category
	std::string password_sent; // redacted from the logs while its login runs
	double login_tried_at = -1000;
	Issue issue = Issue::None;
	bool game_open = false;
	GameRa game = GameRa::Off;
	std::string rom;
	double last_frame = -1000, last_idle = -1000;
	bool reset_seen = false;
	std::deque<Toast> toasts;
	std::map<uint32_t, std::string> trackers; // leaderboard trackers shown
	std::map<uint32_t, std::string> challenges; // achievements primed (challenge indicators)
	std::string progress; // the progress indicator's text
	double progress_until = 0;
	std::map<std::string, fe::border::Image> badges; // decoded badges, by name
};
State g;

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

// A form value as rcheevos's URL builder writes it (letters, digits and -_.~ as they are, the rest as %XX): the password
// also appears this way in the request's body, so both forms are kept out of the logs
std::string FormEncoded(const std::string& v)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : v)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
			out += char(c);
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

void Wipe(std::string& s)
{
	for (char& c : s)
		*(volatile char*)&c = 0;
	s.clear();
}

// ---- files: /data/snes9x/retroachievements/ -------------------------------------------------------------------------
std::string Dir()
{
	return OrbisRoot() + "/retroachievements";
}

std::string LoginFile()
{
	return Dir() + "/login.txt";
}

std::string BadgeFile(const std::string& name)
{
	return Dir() + "/badges/" + name + ".png";
}

// a badge's name, as a file name: letters, digits, '_' and '-' only ("" when it has anything else)
std::string SafeName(const char* name)
{
	std::string s = name ? name : "";
	for (char c : s)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return "";
	return s.size() <= 32 ? s : "";
}

bool ReadLogin(std::string* user, std::string* token)
{
	FILE* f = fopen(LoginFile().c_str(), "r");
	if (!f)
		return false;
	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "user=", 5))
			*user = line + 5;
		else if (!strncmp(line, "token=", 6))
			*token = line + 6;
	}
	memset(line, 0, sizeof(line));
	fclose(f);
	// owner only, whatever put the file there
	struct stat st = {};
	if (stat(LoginFile().c_str(), &st) == 0 && (st.st_mode & 077))
		chmod(LoginFile().c_str(), 0600);
	return !user->empty() && !token->empty();
}

// The token (never the password), readable by its owner only: written to a .part file opened 0600, put on the disk, then
// renamed over the old one.
bool WriteLogin(const std::string& user, const std::string& token)
{
	OrbisMkdirs(Dir());
	chmod(Dir().c_str(), 0700);
	const std::string part = LoginFile() + ".part";
	unlink(part.c_str());
	const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return false;
	fchmod(fd, 0600);
	const std::string text = "# Snes9x PS5: your RetroAchievements login. The token the server gave (not your password).\n"
							 "# Settings, RetroAchievements, Log out deletes this file.\nuser=" +
		user + "\ntoken=" + token + "\n";
	bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
	ok = fsync(fd) == 0 && ok;
	ok = close(fd) == 0 && ok;
	std::string wipe = text;
	Wipe(wipe);
	if (!ok || rename(part.c_str(), LoginFile().c_str()) != 0)
	{
		unlink(part.c_str());
		return false;
	}
	return true;
}

void ForgetLogin()
{
	unlink(LoginFile().c_str());
}

// ---- the notifications ----------------------------------------------------------------------------------------------
void Notify(const std::string& title, const std::string& text, const std::string& right = std::string(),
	const std::string& badge = std::string(), double seconds = 5)
{
	OrbisLog("[ra] notification: %s | %s%s%s", title.c_str(), text.c_str(), right.empty() ? "" : " | ", right.c_str());
	Toast t;
	t.title = title;
	t.text = text;
	t.right = right;
	t.badge = badge;
	t.seconds = seconds;
	auto it = g.badges.find(badge);
	if (!badge.empty() && it != g.badges.end())
		t.picture = it->second;
	g.toasts.push_back(std::move(t));
	while (g.toasts.size() > 6)
		g.toasts.pop_front();
}

void AttachBadge(const std::string& name, const fe::border::Image& img)
{
	g.badges[name] = img;
	for (Toast& t : g.toasts)
		if (t.badge == name)
			t.picture = img;
}

// The badge of an achievement just unlocked: from the cache (retroachievements/badges/), else fetched through the helper
// and kept there, as the covers are. The notification shows the text at once and the picture when it comes.
void FetchBadge(const rc_client_achievement_t* ach)
{
	const std::string name = SafeName(ach->badge_name);
	if (name.empty() || g.badges.count(name))
		return;
	const std::string file = BadgeFile(name);
	if (FILE* f = fopen(file.c_str(), "rb"))
	{
		std::vector<uint8_t> data(64 * 1024);
		data.resize(fread(data.data(), 1, data.size(), f));
		fclose(f);
		fe::border::Image img;
		if (fe::border::DecodeSmallPng(data.data(), data.size(), 256, &img))
		{
			AttachBadge(name, img);
			return;
		}
	}
	char buf[512];
	if (rc_client_achievement_get_image_url(ach, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, buf, sizeof(buf)) != RC_OK)
		return;
	std::string url = buf;
#ifndef __PROSPERO__
	// the host tests: rc_client names the pictures' host itself; they come from the test server instead
	const char* media = "https://media.retroachievements.org";
	if (const char* host = getenv("SNES9X_RA_HOST"); host && url.compare(0, strlen(media), media) == 0)
		url = host + url.substr(strlen(media));
#endif
	net::Send(url, std::string(), UserAgent(), "badge " + name, [name, file](const net::Answer& a) {
		fe::border::Image img;
		if (a.outcome != net::Outcome::Http || a.http != 200 ||
			!fe::border::DecodeSmallPng(reinterpret_cast<const uint8_t*>(a.body.data()), a.body.size(), 256, &img))
			return;
		AttachBadge(name, img);
		OrbisMkdirs(Dir() + "/badges");
		const std::string part = file + ".part";
		if (FILE* f = fopen(part.c_str(), "wb"))
		{
			const bool ok = fwrite(a.body.data(), 1, a.body.size(), f) == a.body.size();
			if (fclose(f) == 0 && ok)
				rename(part.c_str(), file.c_str());
			else
				unlink(part.c_str());
		}
	});
}

std::string IssueText(Issue issue)
{
	switch (issue)
	{
		case Issue::OldHelper:
			return Fmt(fe::T("Update the helper: send the Snes9xPS5-v%s.elf payload."), SNES9X_PS5_VERSION);
		case Issue::NoHelper:
			return Fmt(fe::T("No helper running: send the Snes9xPS5-v%s.elf payload."), SNES9X_PS5_VERSION);
		case Issue::Offline: return fe::T("No connection to RetroAchievements (no network?).");
		case Issue::Server: return fe::T("The RetroAchievements server is not answering: try later.");
		default: return "";
	}
}

const char* ModeText()
{
	return fe::T(rc_client_get_hardcore_enabled(g.client) ? "Hardcore mode" : "Softcore mode");
}

// ---- rc_client's callbacks ------------------------------------------------------------------------------------------
// The game's memory as RetroAchievements numbers it for the SNES (rcheevos's consoleinfo.c): $000000-$01FFFF the
// 128 KiB of work RAM ($7E0000-$7FFFFF), $020000-$09FFFF the cartridge's RAM (battery RAM, the SA-1's BW-RAM: Snes9x keeps
// both in Memory.SRAM), $0A0000-$0A07FF the SA-1's 2 KiB I-RAM (Snes9x: Memory.FillRAM + $3000). A read past what the
// game has stops there (rc_client then turns off the achievements that need it).
uint32_t ReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t*)
{
	if (!emu::GameLoaded())
		return 0;
	size_t sram = Memory.SRAMSize ? size_t(1024) << Memory.SRAMSize : 0;
	sram = std::min(sram, size_t(0x80000));
	uint32_t done = 0;
	while (done < num_bytes)
	{
		const uint32_t a = address + done;
		const uint8_t* base;
		uint32_t left; // bytes readable from a on
		if (a < 0x20000)
		{
			base = Memory.RAM + a;
			left = 0x20000 - a;
		}
		else if (a < 0xA0000)
		{
			if (a - 0x20000 >= sram)
				break;
			base = Memory.SRAM + (a - 0x20000);
			left = uint32_t(sram - (a - 0x20000));
		}
		else if (a < 0xA0800 && Settings.SA1)
		{
			base = Memory.FillRAM + 0x3000 + (a - 0xA0000);
			left = 0xA0800 - a;
		}
		else
			break;
		const uint32_t n = std::min(left, num_bytes - done);
		memcpy(buffer + done, base, n);
		done += n;
	}
	return done;
}

// "r=login2" from a request's body (the API's name only), for the log
std::string WhatOf(const char* post)
{
	if (!post || strncmp(post, "r=", 2) != 0)
		return "request";
	const char* end = strchr(post, '&');
	return std::string(post, end ? size_t(end - post) : strlen(post)).substr(0, 34);
}

void ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t*)
{
	const std::string post = request->post_data ? request->post_data : "";
#ifndef __PROSPERO__
	// the host tests: the body logged on purpose, to show the password and the token never reach a log line
	if (getenv("SNES9X_HOST_LOG_SECRETS"))
		OrbisLog("[ra] (test) body: %s", post.c_str());
#endif
	net::Send(request->url, post, UserAgent(), WhatOf(request->post_data), [callback, callback_data](const net::Answer& a) {
		rc_api_server_response_t r;
		memset(&r, 0, sizeof(r));
		switch (a.outcome)
		{
			case net::Outcome::Http:
				g.issue = a.http >= 500 ? Issue::Server : Issue::None;
				r.http_status_code = a.http;
				r.body = a.body.data();
				r.body_length = a.body.size();
				break;
			case net::Outcome::OldHelper:
				g.issue = Issue::OldHelper;
				r.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
				break;
			case net::Outcome::Refused:
				r.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
				break;
			case net::Outcome::NoHelper:
				g.issue = Issue::NoHelper;
				r.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
				break;
			case net::Outcome::Offline:
				g.issue = Issue::Offline;
				r.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
				break;
		}
		if (!r.body)
		{
			r.body = "";
			r.body_length = 0;
		}
		callback(&r, callback_data);
	});
}

void LogMessage(const char* message, const rc_client_t*)
{
	OrbisLog("[rcheevos] %s", message);
}

#ifndef __PROSPERO__
// the host tests: SNES9X_HOST_RA_CLOCK=<n> runs rc_client's clock n times faster (pings, retries)
rc_clock_t ScaledClock(const rc_client_t*)
{
	static const double scale = std::max(1.0, atof(getenv("SNES9X_HOST_RA_CLOCK")));
	return rc_clock_t(Now() * 1000.0 * scale);
}
#endif

void OnEvent(const rc_client_event_t* e, rc_client_t*)
{
	switch (e->type)
	{
		case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		{
			const rc_client_achievement_t* a = e->achievement;
			// the server's own notices ("Warning: Unsupported Emulator") come as achievements of no points
			if (a->id >= 101000000u)
			{
				Notify(a->title ? a->title : "", a->description ? a->description : "", "", "", 8);
				break;
			}
			Notify(a->title ? a->title : "", a->description ? a->description : "", Fmt(fe::T("%u points"), a->points),
				SafeName(a->badge_name), 6);
			FetchBadge(a);
			break;
		}
		case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
			Notify(fe::T("Leaderboard attempt started"), e->leaderboard->title ? e->leaderboard->title : "");
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
			Notify(fe::T("Leaderboard attempt failed"), e->leaderboard->title ? e->leaderboard->title : "");
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
			Notify(fe::T("Leaderboard score submitted"), e->leaderboard->title ? e->leaderboard->title : "",
				e->leaderboard->tracker_value ? e->leaderboard->tracker_value : "");
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
			if (e->leaderboard_scoreboard && e->leaderboard)
				Notify(e->leaderboard->title ? e->leaderboard->title : "",
					Fmt(fe::T("Your rank: %u of %u"), e->leaderboard_scoreboard->new_rank, e->leaderboard_scoreboard->num_entries));
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
		case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
			g.trackers[e->leaderboard_tracker->id] = e->leaderboard_tracker->display;
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE: g.trackers.erase(e->leaderboard_tracker->id); break;
		case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
			g.challenges[e->achievement->id] = e->achievement->title ? e->achievement->title : "";
			break;
		case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE: g.challenges.erase(e->achievement->id); break;
		case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
		case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
			g.progress = std::string(e->achievement->title ? e->achievement->title : "") + ": " + e->achievement->measured_progress;
			g.progress_until = Now() + 3.0;
			break;
		case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE: g.progress_until = 0; break;
		case RC_CLIENT_EVENT_GAME_COMPLETED:
		{
			const rc_client_game_t* game = rc_client_get_game_info(g.client);
			Notify(fe::T(rc_client_get_hardcore_enabled(g.client) ? "Game mastered!" : "Game completed!"),
				game && game->title ? game->title : "", "", "", 8);
			break;
		}
		case RC_CLIENT_EVENT_SUBSET_COMPLETED:
			Notify(fe::T("Set completed!"), e->subset && e->subset->title ? e->subset->title : "", "", "", 8);
			break;
		case RC_CLIENT_EVENT_RESET:
			// hardcore was turned on: the game starts again, then rc_client is told (emu::Reset -> OnReset)
			g.reset_seen = true;
			OrbisLog("[ra] hardcore on: the game is reset");
			emu::Reset();
			break;
		case RC_CLIENT_EVENT_SERVER_ERROR:
			if (e->server_error)
			{
				OrbisLog("[ra] server error (%s): %s", e->server_error->api ? e->server_error->api : "?",
					e->server_error->error_message ? e->server_error->error_message : "");
				Notify(fe::T("RetroAchievements"), e->server_error->error_message ? e->server_error->error_message : "");
			}
			break;
		case RC_CLIENT_EVENT_DISCONNECTED:
			Notify(fe::T("RetroAchievements"), fe::T("Offline: unlocks will be sent when the connection is back."));
			break;
		case RC_CLIENT_EVENT_RECONNECTED:
			Notify(fe::T("RetroAchievements"), fe::T("Back online: the unlocks were sent."));
			break;
		default: break;
	}
}

void OnLogin(int result, const char* error_message, rc_client_t* client, void*)
{
	if (!g.password_sent.empty())
	{
		// the password's request is answered: it is no longer anywhere
		OrbisLogUnredact(g.password_sent);
		OrbisLogUnredact(FormEncoded(g.password_sent));
		Wipe(g.password_sent);
	}
	g.login_result = result;
	g.login_message = error_message ? error_message : "";
	const rc_client_user_t* user = rc_client_get_user_info(client);
	if (result == RC_OK && user)
	{
		g.login = Login::LoggedIn;
		g.issue = Issue::None;
		if (user->token)
		{
			OrbisLogRedact(user->token);
			OrbisLogRedact(FormEncoded(user->token));
		}
		// the token, kept for the next start (written again only when it changed: a token login gets the same one)
		std::string saved_user, saved_token;
		ReadLogin(&saved_user, &saved_token);
		if (user->token && (saved_user != user->username || saved_token != user->token) && !WriteLogin(user->username, user->token))
			OrbisLog("[ra] could not save the login in %s", LoginFile().c_str());
		Wipe(saved_token);
		OrbisLog("[ra] logged in as %s (%u points, softcore %u)", user->display_name, user->score, user->score_softcore);
		return;
	}
	g.login = Login::Failed;
	OrbisLog("[ra] login failed (%d): %s", result, g.login_message.c_str());
	// a token the server refuses is no use: forgotten (the player logs in again from Settings). A network problem keeps it.
	if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED)
	{
		if (g.login_with_token)
			ForgetLogin();
	}
}

void BeginTokenLogin(const std::string& user, std::string& token)
{
	g.login = Login::Pending;
	g.login_with_token = true;
	g.login_tried_at = Now();
	OrbisLogRedact(token);
	OrbisLogRedact(FormEncoded(token));
	OrbisLog("[ra] logging in as %s with the saved token", user.c_str());
	rc_client_begin_login_with_token(g.client, user.c_str(), token.c_str(), OnLogin, nullptr);
	Wipe(token);
}

void OnGameLoaded(int result, const char* error_message, rc_client_t* client, void*)
{
	if (!g.game_open || result == RC_ABORTED)
		return;
	if (result == RC_OK)
	{
		const bool things = rc_client_has_achievements(client) || rc_client_has_leaderboards(client);
		g.game = things ? GameRa::Active : GameRa::NoSet;
		const rc_client_game_t* game = rc_client_get_game_info(client);
		const std::string title = game && game->title ? game->title : emu::GameName();
		if (rc_client_has_achievements(client))
		{
			rc_client_user_game_summary_t s;
			rc_client_get_user_game_summary(client, &s);
			Notify(title, Fmt(fe::T("%u of %u achievements unlocked"), s.num_unlocked_achievements, s.num_core_achievements),
				ModeText(), "", 6);
		}
		else
			Notify(title, fe::T("No achievements for this game"), "", "", 5);
		OrbisLog("[ra] game %u \"%s\": %s", game ? game->id : 0, title.c_str(), things ? "achievements loaded" : "nothing to unlock");
	}
	else if (result == RC_NO_GAME_LOADED)
	{
		g.game = GameRa::Unknown;
		Notify(fe::T("RetroAchievements"), fe::T("Unknown game: no achievements"));
	}
	else
	{
		g.game = GameRa::Failed;
		const std::string why = IssueText(g.issue);
		OrbisLog("[ra] the game's achievements could not be loaded (%d): %s", result, error_message ? error_message : "");
		Notify(fe::T("Achievements unavailable"), why.empty() ? std::string(error_message ? error_message : "") : why);
	}
	emu::HardcoreChanged(); // the restrictions may be lifted (an unknown game, nothing to unlock, no network)
}
// rc_client, made when the first login starts (none without an account)
bool EnsureClient()
{
	if (g.client)
		return true;
	g.client = rc_client_create(ReadMemory, ServerCall);
	if (!g.client)
		return false;
	rc_client_enable_logging(g.client, RC_CLIENT_LOG_LEVEL_INFO, LogMessage);
	rc_client_set_event_handler(g.client, OnEvent);
	rc_client_set_hardcore_enabled(g.client, fe::Config().ra_hardcore ? 1 : 0);
#ifndef __PROSPERO__
	if (const char* host = getenv("SNES9X_RA_HOST"))
		rc_client_set_host(g.client, host);
	if (getenv("SNES9X_HOST_RA_CLOCK"))
		rc_client_set_get_time_millisecs_function(g.client, ScaledClock);
#endif
	OrbisLog("[ra] rcheevos %s, User-Agent: %s", RCHEEVOS_VERSION_STRING, UserAgent().c_str());
	return true;
}
} // namespace

// =====================================================================================================================
std::string UserAgent()
{
	char clause[64] = "";
	rc_client_get_user_agent_clause(g.client, clause, sizeof(clause));
	return std::string("Snes9xPS5/" SNES9X_PS5_VERSION " (PlayStation 5) Snes9x/" VERSION " ") + clause;
}

void Init()
{
	std::string user, token;
	if (!ReadLogin(&user, &token))
	{
		OrbisLog("[ra] no RetroAchievements login: achievements off");
		return;
	}
	if (!EnsureClient())
	{
		Wipe(token);
		return;
	}
	BeginTokenLogin(user, token);
}

void Shutdown()
{
	net::Stop();
}

void Pump()
{
	if (!g.client)
		return;
	net::Pump();
	const double now = Now();
	if (g.login == Login::LoggedIn && now - g.last_frame > 1.0 && now - g.last_idle >= 1.0)
	{
		g.last_idle = now;
		rc_client_idle(g.client);
	}
}

Login LoginState()
{
	return g.login;
}

std::string UserName()
{
	const rc_client_user_t* u = g.client ? rc_client_get_user_info(g.client) : nullptr;
	return u && u->display_name ? u->display_name : "";
}

uint32_t Points()
{
	const rc_client_user_t* u = g.client ? rc_client_get_user_info(g.client) : nullptr;
	if (!u)
		return 0;
	return fe::Config().ra_hardcore ? u->score : u->score_softcore;
}

std::string Problem()
{
	if (g.login == Login::Failed)
	{
		if (g.login_result == RC_INVALID_CREDENTIALS)
			return fe::T(g.login_with_token ? "Your saved login is no longer valid: log in again." : "Wrong user name or password.");
		if (g.login_result == RC_EXPIRED_TOKEN)
			return fe::T("Your login has expired: log in again.");
		if (g.login_result == RC_ACCESS_DENIED)
			return fe::T("RetroAchievements refused this account.");
		const std::string why = IssueText(g.issue);
		if (!why.empty())
			return why;
		return Fmt(fe::T("Login failed: %s"), g.login_message.c_str());
	}
	if (g.login == Login::LoggedIn)
		return IssueText(g.issue);
	return "";
}

void BeginLogin(const std::string& user, std::string& password)
{
	if (!EnsureClient())
	{
		Wipe(password);
		return;
	}
	if (g.login == Login::LoggedIn)
		rc_client_logout(g.client);
	// redacted from every log line until the server has answered (rc_client keeps it only to build the request)
	g.password_sent = password;
	OrbisLogRedact(password);
	OrbisLogRedact(FormEncoded(password));
	g.login = Login::Pending;
	g.login_with_token = false;
	g.login_tried_at = Now();
	g.issue = Issue::None;
	OrbisLog("[ra] logging in as %s with a password", user.c_str());
	rc_client_begin_login_with_password(g.client, user.c_str(), password.c_str(), OnLogin, nullptr);
	Wipe(password);
}

void Logout()
{
	if (g.client)
	{
		const rc_client_user_t* u = rc_client_get_user_info(g.client);
		if (u && u->token)
			OrbisLogRedact(u->token);
		rc_client_logout(g.client);
	}
	ForgetLogin();
	g.login = Login::None;
	g.issue = Issue::None;
	if (g.game != GameRa::Off)
		g.game = GameRa::Off;
	g.trackers.clear();
	g.challenges.clear();
	OrbisLog("[ra] logged out: the saved login deleted");
	emu::HardcoreChanged();
}

bool HardcoreSetting()
{
	return fe::Config().ra_hardcore;
}

void SetHardcore(bool on)
{
	fe::Config().ra_hardcore = on;
	fe::Config().Save();
	if (!g.client)
		return;
	g.reset_seen = false;
	rc_client_set_hardcore_enabled(g.client, on ? 1 : 0);
	OrbisLog("[ra] hardcore %s", on ? "on" : "off");
	// RetroAchievements: hardcore can't start in the middle of a session. rc_client asks for the reset itself once the set
	// is loaded (RC_CLIENT_EVENT_RESET); while it is still loading the game is reset here.
	if (on && g.game_open && g.game == GameRa::Loading && !g.reset_seen && g.login != Login::None)
	{
		OrbisLog("[ra] hardcore on while the set loads: the game is reset");
		emu::Reset();
	}
	emu::HardcoreChanged();
}

bool HardcoreActive()
{
	return g.client && fe::Config().ra_hardcore && (g.login == Login::LoggedIn || g.login == Login::Pending) && g.game_open &&
		(g.game == GameRa::Loading || g.game == GameRa::Active);
}

bool CanPause(int* seconds_left)
{
	if (seconds_left)
		*seconds_left = 0;
	if (!HardcoreActive() || !rc_client_is_game_loaded(g.client))
		return true;
	uint32_t frames = 0;
	if (rc_client_can_pause(g.client, &frames))
		return true;
	if (seconds_left)
		*seconds_left = int((frames + 59) / 60);
	return false;
}

void GameStarted(const std::string& rom_path)
{
	g.game_open = true;
	g.game = GameRa::Off;
	g.rom = rom_path;
	g.toasts.clear();
	g.trackers.clear();
	g.challenges.clear();
	g.progress_until = 0;
	if (!g.client || g.login == Login::None)
		return;
	// a saved token whose login failed for want of a network: tried again (at most every 20 s)
	if (g.login == Login::Failed && g.issue != Issue::None && Now() - g.login_tried_at > 20)
	{
		std::string user, token;
		if (ReadLogin(&user, &token))
			BeginTokenLogin(user, token);
	}
	if (g.login == Login::Failed)
	{
		const std::string why = Problem();
		Notify(fe::T("Achievements unavailable"), why);
		return;
	}
	std::string hash, why;
	const double t0 = Now();
	if (!fe::rahash::HashFile(rom_path, &hash, &why))
	{
		OrbisLog("[ra] %s: no hash (%s)", rom_path.c_str(), why.c_str());
		g.game = GameRa::Unknown;
		Notify(fe::T("RetroAchievements"), fe::T("Unknown game: no achievements"));
		return;
	}
	OrbisLog("[ra] %s: hash %s (%.0f ms)", rom_path.c_str(), hash.c_str(), (Now() - t0) * 1000.0);
	g.game = GameRa::Loading;
	rc_client_begin_load_game(g.client, hash.c_str(), OnGameLoaded, nullptr);
}

void GameClosed()
{
	if (g.client && g.game_open && g.login != Login::None)
		rc_client_unload_game(g.client);
	g.game_open = false;
	g.game = GameRa::Off;
	g.toasts.clear();
	g.trackers.clear();
	g.challenges.clear();
	g.progress_until = 0;
}

void Frame()
{
	if (!g.client || !g.game_open || g.login == Login::None)
		return;
	rc_client_do_frame(g.client);
	g.last_frame = Now();
}

void OnReset()
{
	if (g.client && g.game_open && g.login != Login::None)
		rc_client_reset(g.client);
}

bool SaveProgress(std::vector<uint8_t>* out)
{
	if (!g.client || !rc_client_is_game_loaded(g.client))
		return false;
	out->resize(rc_client_progress_size(g.client));
	return !out->empty() && rc_client_serialize_progress_sized(g.client, out->data(), out->size()) == RC_OK;
}

void LoadProgress(const std::vector<uint8_t>* data)
{
	if (!g.client || !rc_client_is_game_loaded(g.client))
		return;
	const int r = data ? rc_client_deserialize_progress_sized(g.client, data->data(), data->size())
					   : rc_client_deserialize_progress_sized(g.client, nullptr, 0);
	OrbisLog("[ra] progress %s (%d)", data ? "restored from the state" : "reset (the state has none)", r);
}

bool GameHasSet()
{
	return g.client && g.game == GameRa::Active && rc_client_has_achievements(g.client);
}

std::string GameTitle()
{
	const rc_client_game_t* game = g.client && g.game == GameRa::Active ? rc_client_get_game_info(g.client) : nullptr;
	return game && game->title ? game->title : "";
}

std::string GameSummary()
{
	switch (g.game)
	{
		case GameRa::Loading: return fe::T("Loading...");
		case GameRa::Unknown: return fe::T("Unknown game");
		case GameRa::NoSet: return fe::T("None for this game");
		case GameRa::Failed: return fe::T("Unavailable");
		case GameRa::Active:
		{
			if (!rc_client_has_achievements(g.client))
				return fe::T("None for this game");
			rc_client_user_game_summary_t s;
			rc_client_get_user_game_summary(g.client, &s);
			return Fmt(fe::T("%u of %u"), s.num_unlocked_achievements, s.num_core_achievements);
		}
		default: return "";
	}
}

std::vector<Item> AchievementItems()
{
	std::vector<Item> items;
	if (!GameHasSet())
		return items;
	rc_client_achievement_list_t* list = rc_client_create_achievement_list(g.client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
	if (!list)
		return items;
	for (uint32_t b = 0; b < list->num_buckets; b++)
	{
		const rc_client_achievement_bucket_t& bucket = list->buckets[b];
		const char* label = "Locked";
		switch (bucket.bucket_type)
		{
			case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED: label = "Unlocked"; break;
			case RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED: label = "Recently unlocked"; break;
			case RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE: label = "Active challenges"; break;
			case RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE: label = "Almost there"; break;
			case RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL: label = "Unofficial"; break;
			case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED: label = "Not supported"; break;
			case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSYNCED: label = "Not sent yet"; break;
			default: break;
		}
		Item head;
		head.section = true;
		head.title = Fmt("%s (%u)", fe::T(label), bucket.num_achievements);
		items.push_back(head);
		for (uint32_t i = 0; i < bucket.num_achievements; i++)
		{
			const rc_client_achievement_t* a = bucket.achievements[i];
			Item it;
			it.title = a->title ? a->title : "";
			it.description = a->description ? a->description : "";
			it.points = a->points;
			it.unlocked = a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
			it.progress = a->measured_progress;
			it.percent = a->measured_percent;
			items.push_back(it);
		}
	}
	rc_client_destroy_achievement_list(list);
	return items;
}

// ---- the overlay ----------------------------------------------------------------------------------------------------
namespace
{
uint32_t Blend(uint32_t dst, uint32_t src)
{
	const uint32_t a = src >> 24;
	if (a == 0)
		return dst;
	if (a == 255)
		return src | 0xff000000u;
	uint32_t out = 0xff000000u;
	for (int s = 0; s < 24; s += 8)
	{
		const uint32_t d = (dst >> s) & 0xff, c = (src >> s) & 0xff;
		out |= ((c * a + d * (255 - a)) / 255) << s;
	}
	return out;
}

void DrawPicture(const fe::border::Image& img, int x, int y, int size, const ps5video::Rect& clip)
{
	uint32_t* px = ps5video::Surface();
	for (int j = 0; j < size; j++)
	{
		const int sy = y + j;
		if (sy < clip.y || sy >= clip.y + clip.h)
			continue;
		const int iy = j * img.h / size;
		for (int i = 0; i < size; i++)
		{
			const int sx = x + i;
			if (sx < clip.x || sx >= clip.x + clip.w)
				continue;
			uint32_t& d = px[size_t(sy) * ps5video::kWidth + sx];
			d = Blend(d, img.px[size_t(iy) * img.w + i * img.w / size]);
		}
	}
}

// A dark box with an accent line on its left, clipped to the picture
void Box(int x, int y, int w, int h, const ps5video::Rect& r)
{
	const int x0 = std::max(x, r.x), y0 = std::max(y, r.y);
	const int x1 = std::min(x + w, r.x + r.w), y1 = std::min(y + h, r.y + r.h);
	if (x1 <= x0 || y1 <= y0)
		return;
	ps5video::DarkenRect(x0, y0, x1 - x0, y1 - y0);
	ps5video::DarkenRect(x0, y0, x1 - x0, y1 - y0);
	ps5video::DarkenRect(x0, y0, x1 - x0, y1 - y0);
	ps5video::FillRect(x0, y0, std::min(5, x1 - x0), y1 - y0, ps5video::Rgb(150, 125, 255));
}
} // namespace

void DrawOverlay(const ps5video::Rect& r)
{
	if (g.toasts.empty() && g.trackers.empty() && g.challenges.empty() && g.progress_until <= 0)
		return;
	const double now = Now();
	const int margin = 16;
	const int width = std::min(760, r.w - 2 * margin);
	if (width < 200)
		return;
	// the notifications, newest at the bottom, at most 3 at a time; each stays its seconds from the first frame that shows it
	while (!g.toasts.empty() && g.toasts.front().shown_at >= 0 && now - g.toasts.front().shown_at > g.toasts.front().seconds)
		g.toasts.pop_front();
	int y = r.y + r.h - margin;
	const int shown = std::min<int>(3, int(g.toasts.size()));
	for (int k = shown - 1; k >= 0; k--)
	{
		Toast& t = g.toasts[size_t(k)];
		if (t.shown_at < 0)
			t.shown_at = now;
		const int h = 96;
		y -= h;
		const int x = r.x + margin;
		Box(x, y, width, h, r);
		int tx = x + 22;
		if (!t.picture.px.empty())
		{
			DrawPicture(t.picture, x + 18, y + 16, 64, r);
			tx = x + 98;
		}
		const int right_w = t.right.empty() ? 0 : fe::TextWidth(t.right.c_str(), 2) + 16;
		const std::string title = fe::FitText(t.title, 3, x + width - 16 - right_w - tx);
		fe::DrawText(tx, y + 14, title.c_str(), 3, ps5video::Rgb(240, 240, 250));
		if (!t.right.empty())
			fe::DrawText(x + width - 16 - fe::TextWidth(t.right.c_str(), 2), y + 20, t.right.c_str(), 2, ps5video::Rgb(255, 210, 110));
		const std::string text = fe::FitText(t.text, 2, x + width - 16 - tx);
		fe::DrawText(tx, y + 56, text.c_str(), 2, ps5video::Rgb(200, 200, 220));
		y -= 10;
	}
	// top right: leaderboard trackers, challenge indicators, the progress indicator
	std::vector<std::string> corner;
	for (const auto& c : g.challenges)
		corner.push_back(Fmt(fe::T("Challenge: %s"), c.second.c_str()));
	if (g.progress_until > now)
		corner.push_back(g.progress);
	for (const auto& t : g.trackers)
		corner.push_back(t.second);
	int cy = r.y + margin;
	for (const std::string& line : corner)
	{
		const std::string fit = fe::FitText(line, 2, width - 40);
		const int w = fe::TextWidth(fit.c_str(), 2) + 36;
		const int x = r.x + r.w - margin - w;
		Box(x, cy, w, 44, r);
		fe::DrawText(x + 20, cy + 10, fit.c_str(), 2, ps5video::Rgb(240, 240, 250));
		cy += 52;
		if (cy > r.y + r.h / 2)
			break;
	}
}
} // namespace ra

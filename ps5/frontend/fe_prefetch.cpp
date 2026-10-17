// Snes9x PS5: the cover prefetch (fe_prefetch.h).
//
// SPDX-License-Identifier: MIT

#include "fe_prefetch.h"

#include "fe_coverfetch.h"
#include "fe_covers.h"
#include "fe_http.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"
#include "ProsperoJailbreak.h"
#include "ProsperoNotify.h"
#include "fe_i18n.h"
#include "ProsperoSce.h"

#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace fe
{
namespace
{
double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

PrefetchResult g_last;
bool g_have_last = false;
} // namespace

void RememberPrefetch(const PrefetchResult& result)
{
	g_last.helper_ok = result.helper_ok;
	g_last.background = result.background;
	g_last.attempted = result.attempted;
	g_have_last = true;
}

bool BackgroundCovers()
{
	return g_have_last && g_last.background;
}

void CoversRestartIfNeeded(const std::vector<GameInfo>& games, bool downloads_on, bool force, void (*show)(const char*))
{
	if (!downloads_on)
	{
		WriteWantedList({});
		return;
	}
	const std::vector<WantedCover> wanted = MissingCovers(games);
	WriteWantedList(wanted);
	if (BackgroundCovers())
	{
		if (!wanted.empty())
			OrbisLog("[covers] %zu cover(s) wanted: the helper downloads them in the background", wanted.size());
		return;
	}
	int fresh = 0;
	for (const WantedCover& w : wanted)
		fresh += g_last.attempted.count(w.file) ? 0 : 1;
	if (wanted.empty() || (!force && fresh == 0))
		return;
	if (!g_have_last || !g_last.helper_ok)
	{
		OrbisLog("[covers] %zu cover(s) wanted, but no helper answered at start: not restarting", wanted.size());
		return;
	}
	if (!NetConnected())
	{
		OrbisLog("[covers] %zu cover(s) wanted; the console is offline: next start", wanted.size());
		return;
	}
	// A guard against a loop: no second restart within two minutes (one for Square: ten seconds).
	const std::string stamp = OrbisDir("covers") + "/restart.stamp";
	const time_t now = time(nullptr);
	if (FILE* f = fopen(stamp.c_str(), "r"))
	{
		long long t = 0;
		const int n = fscanf(f, "%lld", &t);
		fclose(f);
		if (n == 1 && now - time_t(t) >= 0 && now - time_t(t) < (force ? 10 : 120))
		{
			OrbisLog("[covers] restarted %lld s ago: not again", (long long)(now - time_t(t)));
			return;
		}
	}
	{
		// the stamp is the loop guard: when it can't be written, don't restart (it could not stop the next one)
		FILE* f = fopen(stamp.c_str(), "w");
		bool ok = f && fprintf(f, "%lld\n", (long long)now) > 0;
		ok = f && fclose(f) == 0 && ok;
		if (!ok)
		{
			OrbisLog("[covers] can't write %s: not restarting", stamp.c_str());
			return;
		}
	}
	const char* path = "/data/homebrew/" SNES9X_TITLE_ID "/eboot.bin";
	if (access(path, F_OK) != 0)
		path = "/app0/eboot.bin";
	OrbisLog("[covers] %d new cover(s) to fetch (%zu wanted): restarting %s so the prefetch gets them", fresh,
		wanted.size(), path);
	if (show)
		show(T("Downloading covers..."));
	ProsperoNotifyFlush();
	OrbisLogClose();
	const int rc = sceSystemServiceLoadExec(path, nullptr);
	if (rc == 0)
		for (int i = 0; i < 100; i++)
			usleep(100 * 1000);
	OrbisLogOpen("boot-after-restart");
	OrbisLog("[covers] sceSystemServiceLoadExec(%s) -> %x: carrying on without the new covers", path, unsigned(rc));
}

bool NetConnected()
{
	const int nc = sceNetCtlInit();
	int state[4] = {-1, 0, 0, 0};
	const int gs = sceNetCtlGetState(state);
	if (nc == 0)
		sceNetCtlTerm();
	return gs == 0 && state[0] == 3;
}

PrefetchResult PrefetchCovers(double budget_s)
{
	S9X_STAGE(Boot, "cover prefetch");
	PrefetchResult r;
	const double t0 = Now();
	std::string text;
	r.helper_ok = jailbreak::FetchWantedCovers(text, &r.background);
	if (r.background)
	{
		OrbisLog("[prefetch] the helper downloads the covers while the app runs: nothing to wait for");
		r.seconds = Now() - t0;
		return r;
	}
	const std::vector<WantedCover> wanted = ParseWantedList(text);
	OrbisLog("[prefetch] %zu cover(s) to fetch before asking for /data (budget %.0f s)", wanted.size(), budget_s);
	if (wanted.empty())
	{
		r.seconds = Now() - t0;
		return r;
	}
	if (wanted.size() == 1)
		ProsperoNotify("%s", T("Snes9x PS5: downloading 1 cover..."));
	else
		ProsperoNotify(T("Snes9x PS5: downloading %d covers..."), int(wanted.size()));

	Http http;
	http.SetDeadline(t0 + budget_s); // a slow server can't hold the start past the budget: one GET retries for up to a minute
	int saved = 0;
	for (const WantedCover& w : wanted)
	{
		if (Now() - t0 > budget_s)
		{
			OrbisLog("[prefetch] out of time: the rest next start");
			break;
		}
		PrefetchItem item;
		item.file = w.file;
		item.url = w.url;
		item.status = FetchCoverUrl(http, w.url, w.file.substr(0, w.file.size() - 4), item.data);
		r.attempted.insert(w.file);
		if (item.status != 200)
			item.data.clear();
		else
			saved++;
		OrbisLog("[prefetch] %s -> %d (%zu bytes)", w.file.c_str(), item.status, item.data.size());
		r.items.push_back(std::move(item));
		if (http.Offline())
		{
			OrbisLog("[prefetch] the console is offline: stopping");
			break;
		}
	}
	http.Term();
	r.seconds = Now() - t0;
	OrbisLog("[prefetch] %d of %zu fetched in %.1f s", saved, wanted.size(), r.seconds);
	return r;
}

void SavePrefetched(const PrefetchResult& result)
{
	if (result.items.empty())
		return;
	const std::string dir = OrbisDir("covers");
	OrbisMkdirs(dir);
	int saved = 0, missing = 0;
	for (const PrefetchItem& it : result.items)
	{
		const int r = SaveCoverResult(dir, WantedCover{it.file, it.url}, it.status, it.data);
		saved += r == 1 ? 1 : 0;
		missing += r == 0 ? 1 : 0;
	}
	OrbisLog("[prefetch] saved %d cover(s) in %s, %d not on the server", saved, dir.c_str(), missing);
}
} // namespace fe

// Snes9x PS5 frontend: the border around the picture (fe_border.h).
// SPDX-License-Identifier: MIT

#include "fe_border.h"

#include "fe_borderfetch.h"
#include "fe_borderimg.h"
#include "fe_covers.h"
#include "fe_games.h"
#include "fe_settings.h"

#include "OrbisPaths.h"
#include "ProsperoThread.h"
#include "ProsperoVideo.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#ifndef BORDER_DEFAULT_PNG
#error BORDER_DEFAULT_PNG must name the built-in border picture
#endif
#define S9X_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const unsigned char sym##_begin[];                                                              \
	extern "C" const unsigned char sym##_end[];

S9X_INCBIN(s9x_border_default, BORDER_DEFAULT_PNG)

namespace fe::border
{
namespace
{
// ---- the .ini beside a picture ----
struct Ini
{
	bool off = false;
	bool has_window = false;
	ps5video::Rect window = {};
};

std::string Trim(const std::string& s)
{
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return std::string();
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// A missing file is the defaults; a value that makes no sense is ignored with one log line.
Ini ReadIni(const std::string& path)
{
	Ini ini;
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return ini;
	char raw[512];
	while (fgets(raw, sizeof(raw), f))
	{
		const std::string line = Trim(raw);
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == std::string::npos)
			continue;
		std::string key = Trim(line.substr(0, eq));
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(tolower(c)); });
		const std::string val = Trim(line.substr(eq + 1));
		if (key == "off")
		{
			if (val == "1" || val == "0")
				ini.off = val == "1";
			else
				OrbisLog("[border] %s: off=%s ignored (use 0 or 1)", path.c_str(), val.c_str());
		}
		else if (key == "window")
		{
			ps5video::Rect w = {};
			int used = 0;
			if (sscanf(val.c_str(), " %d , %d , %d , %d %n", &w.x, &w.y, &w.w, &w.h, &used) == 4 && val[size_t(used)] == 0 &&
				WindowFits(w))
			{
				ini.has_window = true;
				ini.window = w;
			}
			else
				OrbisLog("[border] %s: window=%s ignored (x,y,w,h inside 1920x1080, at least %dx%d)", path.c_str(), val.c_str(),
					kMinWindowW, kMinWindowH);
		}
	}
	fclose(f);
	return ini;
}

// ---- files ----
bool IsFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out, std::string* why)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
	{
		*why = "cannot be read";
		return false;
	}
	if (st.st_size > off_t(kMaxFileBytes))
	{
		*why = "the file is over 8 MiB";
		return false;
	}
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
	{
		*why = "cannot be opened";
		return false;
	}
	out->resize(size_t(st.st_size));
	const bool ok = fread(out->data(), 1, out->size(), f) == out->size();
	fclose(f);
	if (!ok)
		*why = "cannot be read";
	return ok;
}

struct PackFile
{
	std::string stem; // "Super Mario World (USA)"
	std::string file; // "Super Mario World (USA).png"
};

// The .png files in games/, by stem. A pack of 1500 is a few milliseconds.
std::vector<PackFile> ListPack(const std::string& games_dir)
{
	std::vector<PackFile> pack;
	DIR* d = opendir(games_dir.c_str());
	if (!d)
		return pack;
	while (dirent* e = readdir(d))
	{
		const std::string name = e->d_name;
		if (name.size() <= 4 || name[0] == '.' || strcasecmp(name.c_str() + name.size() - 4, ".png") != 0)
			continue;
		if (!IsFile(games_dir + "/" + name))
			continue;
		pack.push_back({name.substr(0, name.size() - 4), name});
	}
	closedir(d);
	std::sort(pack.begin(), pack.end(), [](const PackFile& a, const PackFile& b) { return a.stem != b.stem ? a.stem < b.stem : a.file < b.file; });
	return pack;
}

// ---- one picture ----
// From the bytes of a PNG to a border; null (one log line) when it cannot be used. label: what the log calls the file. The art
// is refitted so its window has the shape of `aspect`, or the size of its whole-multiple picture for Integer scale (FitToAspect).
std::shared_ptr<const ps5video::Border> Build(const std::string& label, const uint8_t* data, size_t size, const Ini& ini,
	ps5video::Aspect aspect)
{
	Image img;
	std::string why;
	if (!DecodePng(data, size, &img, &why))
	{
		OrbisLog("[border] %s: %s", label.c_str(), why.c_str());
		return nullptr;
	}
	ps5video::Rect window = ini.window;
	if (ini.has_window)
		PunchWindow(&img, window);
	else if (!FindWindow(img, &window, &why))
	{
		OrbisLog("[border] %s: %s", label.c_str(), why.c_str());
		return nullptr;
	}
	if (!FitToAspect(&img, &window, aspect, &why))
		OrbisLog("[border] %s: not fitted to %s, drawn as it is: %s", label.c_str(), ps5video::AspectName(aspect), why.c_str());
	auto border = BuildBorder(img, window, &why);
	if (!border)
		OrbisLog("[border] %s: %s", label.c_str(), why.c_str());
	return border;
}

std::shared_ptr<const ps5video::Border> LoadFile(const std::string& label, const std::string& path, const Ini& ini,
	ps5video::Aspect aspect)
{
	std::vector<uint8_t> bytes;
	std::string why;
	if (!ReadFile(path, &bytes, &why))
	{
		OrbisLog("[border] %s: %s", label.c_str(), why.c_str());
		return nullptr;
	}
	return Build(label, bytes.data(), bytes.size(), ini, aspect);
}
} // namespace

Pick Choose(const std::string& pack, int setting, const GameInfo& game, ps5video::Aspect aspect)
{
	Pick pick;
	const std::string games_dir = pack + "/games";
	if (setting == 1)
	{
		// This game has no border, whatever its picture is called (the ROM's own name; the picture's, below).
		if (ReadIni(games_dir + "/" + game.file_base + ".ini").off)
		{
			pick.source = "off";
			return pick;
		}
		const std::vector<PackFile> files = ListPack(games_dir);
		auto try_stem = [&](const PackFile& f, const char* how) {
			const Ini ini = ReadIni(games_dir + "/" + f.stem + ".ini");
			if (ini.off)
			{
				pick.source = "off";
				pick.file = f.file;
				return true;
			}
			pick.border = LoadFile("games/" + f.file, games_dir + "/" + f.file, ini, aspect);
			if (!pick.border)
				return false;
			pick.source = how;
			pick.file = f.file;
			return true;
		};
		const auto own = std::find_if(files.begin(), files.end(), [&](const PackFile& f) { return f.stem == game.file_base; });
		if (own != files.end() && try_stem(*own, "own file"))
			return pick;
		std::string matched;
		if (!files.empty())
		{
			std::vector<std::string> stems;
			for (const PackFile& f : files)
				stems.push_back(f.stem);
			matched = MatchPackName(stems, game);
			const auto hit = matched.empty() || (own != files.end() && matched == own->stem)
				? files.end()
				: std::find_if(files.begin(), files.end(), [&](const PackFile& f) { return f.stem == matched; });
			if (hit != files.end() && try_stem(*hit, "matched"))
				return pick;
		}
		// The Bezel Project's name for the game (the index built into the app): a border downloaded earlier is found by it
		// even when the matcher, which sees only the files in the folder, would pick another name or none.
		const std::string bezel = BezelNameFor(game);
		if (!bezel.empty() && bezel != matched && !(own != files.end() && bezel == own->stem))
		{
			const auto hit = std::find_if(files.begin(), files.end(), [&](const PackFile& f) { return f.stem == bezel; });
			if (hit != files.end() && try_stem(*hit, "bezel name"))
				return pick;
		}
	}
	if (IsFile(pack + "/default.png"))
	{
		pick.border = LoadFile("default.png", pack + "/default.png", ReadIni(pack + "/default.ini"), aspect);
		if (pick.border)
		{
			pick.source = "default.png";
			pick.file = "default.png";
			return pick;
		}
	}
	pick.border = Build("the built-in frame", s9x_border_default_begin, size_t(s9x_border_default_end - s9x_border_default_begin), Ini(), aspect);
	if (pick.border)
	{
		pick.source = "built-in";
		pick.file = "built-in";
	}
	return pick;
}

namespace
{
// The game that is loaded, to pick again when the setting changes. Used on the thread that loads games, except `shown`.
struct Current
{
	bool game = false;
	std::string rom_path;
	uint32_t crc = 0;
	int setting = 0; // the setting the border on screen was picked for
	int aspect = 0; // and the aspect ratio it was fitted to (ps5video::Aspect)
	bool shown = false; // SetBorder has been given a border (s_lock)
} s_cur;

// s_gen changes whenever the border is picked again or released: the watcher for an older pick gives up. s_lock guards it and
// `shown`; the cv wakes the watcher at once.
std::mutex s_lock;
std::condition_variable s_cv;
uint64_t s_gen = 0;
ps5::BigThread s_watch; // started and joined by the thread that loads games

constexpr std::chrono::milliseconds kWatchPoll(500);
constexpr std::chrono::minutes kWatchMax(30); // the helper may be offline for a while; after this the next game start asks again

void Show(std::shared_ptr<const ps5video::Border> border)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (!border && !s_cur.shown)
		return; // nothing on screen, nothing to take away: the renderer is not disturbed
	s_cur.shown = border != nullptr;
	ps5video::SetBorder(std::move(border));
}

// The watcher puts a border up only while its pick is still the current one.
bool ShowIfCurrent(uint64_t gen, std::shared_ptr<const ps5video::Border> border)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_gen != gen)
		return false;
	s_cur.shown = true;
	ps5video::SetBorder(std::move(border));
	return true;
}

void StopWatch()
{
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_gen++;
	}
	s_cv.notify_all();
	s_watch.join();
}

// The helper is downloading `file` into the pack for the game that was just loaded: waits for it (a stat twice a second, on
// this thread), loads it and puts it up in place of the frame the game started with. Never on the emulation thread.
void Watch(uint64_t gen, std::string pack, std::string file, std::string game_name, ps5video::Aspect aspect)
{
	try
	{
		const std::string path = pack + "/games/" + file;
		const std::string stem = file.substr(0, file.size() - 4);
		const time_t since = time(nullptr);
		const auto deadline = std::chrono::steady_clock::now() + kWatchMax;
		struct stat st = {};
		for (;;)
		{
			if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
				break;
			if (stat((pack + "/games/" + stem + ".missing").c_str(), &st) == 0 && st.st_mtime >= since)
			{
				OrbisLog("[border] %s: The Bezel Project has no picture named %s", game_name.c_str(), file.c_str());
				return;
			}
			std::unique_lock<std::mutex> lock(s_lock);
			if (s_cv.wait_for(lock, kWatchPoll, [&] { return s_gen != gen; }) || std::chrono::steady_clock::now() >= deadline)
				return;
		}
		const auto t0 = std::chrono::steady_clock::now();
		const Ini ini = ReadIni(pack + "/games/" + stem + ".ini");
		if (ini.off)
			return;
		auto border = LoadFile("games/" + file, path, ini, aspect);
		if (!border)
			return;
		const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
		const ps5video::Rect& w = border->window;
		const size_t rim = border->rim.size();
		if (ShowIfCurrent(gen, std::move(border)))
			OrbisLog("[border] %s -> %s (downloaded): window %d,%d %dx%d, rim %zu pixels, %ld ms", game_name.c_str(), file.c_str(), w.x,
				w.y, w.w, w.h, rim, ms);
	}
	catch (const std::exception& e)
	{
		OrbisLog("[border] watching for the download failed: %s", e.what());
	}
	catch (...)
	{
		OrbisLog("[border] watching for the download failed");
	}
}

// Auto, and the game has only the default or the built-in frame: asks the helper for The Bezel Project's border (borders/
// priority.txt, fe_borderfetch.h) and starts watching for it.
void RequestDownload(const std::string& pack, const GameInfo& game, ps5video::Aspect aspect)
{
	WantedCover want;
	if (!BorderWanted(pack, game, &want))
		return;
	WritePriorityBorders(pack, {want});
	OrbisLog("[border] %s: asking the helper for %s", game.file_base.c_str(), want.file.c_str());
	uint64_t gen;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		gen = s_gen;
	}
	s_watch = ps5::BigThread([gen, pack, file = want.file, name = game.file_base, aspect] { Watch(gen, pack, file, name, aspect); }, 1024 * 1024);
}

void Apply()
{
	StopWatch();
	s_cur.setting = Config().border;
	s_cur.aspect = Config().aspect;
	const ps5video::Aspect aspect = ps5video::Aspect(s_cur.aspect);
	if (s_cur.setting <= 0)
	{
		Show(nullptr);
		return;
	}
	try
	{
		const auto t0 = std::chrono::steady_clock::now();
		const GameInfo game = DescribeGame(s_cur.rom_path, s_cur.crc);
		const std::string pack = OrbisDir("borders");
		Pick pick = Choose(pack, s_cur.setting, game, aspect);
		const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
		if (pick.border)
		{
			const ps5video::Rect& w = pick.border->window;
			OrbisLog("[border] %s -> %s (%s): window %d,%d %dx%d, rim %zu pixels, %ld ms, aspect ratio %s", game.file_base.c_str(),
				pick.file.c_str(), pick.source.c_str(), w.x, w.y, w.w, w.h, pick.border->rim.size(), ms, ps5video::AspectName(aspect));
		}
		else if (pick.source == "off")
			OrbisLog("[border] %s: none, an .ini says off=1", game.file_base.c_str());
		else
			OrbisLog("[border] %s: none usable", game.file_base.c_str());
		const bool no_own = pick.source != "own file" && pick.source != "matched" && pick.source != "bezel name" && pick.source != "off";
		Show(std::move(pick.border));
		if (s_cur.setting == 1 && no_own && Config().border_download)
			RequestDownload(pack, game, aspect);
	}
	catch (const std::exception& e)
	{
		OrbisLog("[border] failed, the game runs without: %s", e.what());
		Show(nullptr);
	}
	catch (...)
	{
		OrbisLog("[border] failed, the game runs without");
		Show(nullptr);
	}
}
} // namespace

void Select(const std::string& rom_path, uint32_t rom_crc)
{
	s_cur.game = true;
	s_cur.rom_path = rom_path;
	s_cur.crc = rom_crc;
	Apply();
}

void Reselect()
{
	// the aspect ratio counts too: the window is stretched to its shape (an Off border has nothing to fit)
	if (!s_cur.game || (Config().border == s_cur.setting && (s_cur.setting <= 0 || Config().aspect == s_cur.aspect)))
		return;
	Apply();
}

void Release()
{
	StopWatch();
	s_cur.game = false;
	s_cur.rom_path.clear();
	Show(nullptr);
}
} // namespace fe::border

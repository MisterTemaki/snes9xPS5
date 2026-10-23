// Snes9x PS5 frontend: the game list and the menus.
//
// Everything is drawn on the CPU into the 1920x1080 surface with Snes9x's 8x10 font, scaled 3x-5x, and
// shown with ps5video::Present (which waits for vsync, so the menus run at 60 Hz).
//
// SPDX-License-Identifier: MIT

#include "fe_menu.h"

#include "fe_coverworker.h"
#include "fe_emu.h"
#include "fe_games.h"
#include "fe_i18n.h"
#include "fe_shelf.h"
#include "fe_settings.h"
#include "fe_shortcuts.h"
#include "fe_text.h"
#include "fe_ui.h"

#include "netfs.h"

#include "OrbisPaths.h"
#include "ProsperoJailbreak.h"
#include "ProsperoUpdate.h"
#include "ProsperoCrt.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "snes9x.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

#ifndef SNES9X_PS5_VERSION
#define SNES9X_PS5_VERSION "dev"
#endif

namespace fe
{
using namespace ui;

namespace
{
using ps5video::Rgb;
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

bool HasRomExtension(const char* name)
{
	static const char* const exts[] = {".sfc", ".smc", ".swc", ".fig", ".bs", ".st", ".zip", ".gz", ".7z", ".bin", ".mgd"};
	const size_t n = strlen(name);
	for (const char* e : exts)
	{
		const size_t m = strlen(e);
		if (n > m && strcasecmp(name + n - m, e) == 0)
			return true;
	}
	return false;
}

struct Entry
{
	std::string name; // shown
	std::string path; // full path
	bool dir = false;
};

std::vector<Entry> ListDir(const std::string& dir)
{
	std::vector<Entry> out;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		Entry en;
		en.path = dir + "/" + e->d_name;
		en.dir = OrbisIsDir(en.path);
		if (!en.dir && !HasRomExtension(e->d_name))
			continue;
		en.name = e->d_name;
		if (!en.dir)
		{
			const size_t dot = en.name.find_last_of('.');
			if (dot != std::string::npos && dot > 0)
				en.name.resize(dot);
		}
		out.push_back(std::move(en));
	}
	closedir(d);
	std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
		if (a.dir != b.dir)
			return a.dir;
		return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
	});
	return out;
}

std::string RootLabel(const std::string& root)
{
	if (root == OrbisDir("roms"))
		return std::string(T("Console")) + "  (" + root + ")";
	if (root.rfind("/mnt/usb", 0) == 0)
		return "USB " + root.substr(8, 1) + "  (" + root + ")";
	if (root.rfind("/smb/", 0) == 0 || root.rfind("/nfs/", 0) == 0)
		return Fmt(T("Network: %s"), root.substr(5).c_str()) + (root[1] == 's' ? "  (SMB)" : "  (NFS)");
	return root;
}

const char* YesNo(bool b)
{
	return T(b ? "On" : "Off");
}

// Settings::border: nothing, the game's own art from /data/snes9x/borders (else the pack's default, else the built-in
// frame), or the default frame only.
const char* BorderName(int b)
{
	switch (b)
	{
		case 1: return "Auto";
		case 2: return "Default frame";
		default: return "Off";
	}
}

// The settings shared by the game list (Triangle) and the pause menu. Returns true if 'idx' was a setting
// and 'dir' (-1/+1) changed it.
enum SettingRow
{
	S_SHADER,
	S_ASPECT,
	S_BORDER,
	S_SCANLINES,
	S_SMOOTH,
	S_FPS,
	S_AUDIO,
	S_TRANSPARENCY,
	S_SUPERFX,
	S_COVERS,
	S_LANGUAGE,
	S_DEBUGLOGS,
	S_UPDATES,
	S_COUNT
};

// ---- updates (ProsperoUpdate.h): the helper checks, downloads and installs; this screen shows it ----------------
struct UpdateView
{
	update::Status st;
	bool helper = false;
	double polled = -1;
};
UpdateView g_upd;

void PollUpdate(bool force)
{
	if (!force && Now() - g_upd.polled < 0.5)
		return;
	g_upd.polled = Now();
	std::string reply;
	g_upd.helper = jailbreak::UpdateRequest("status", reply) && update::Decode(reply, g_upd.st);
}

std::string UpdateValue()
{
	char buf[96];
	if (!g_upd.helper)
		return T("Needs the helper");
	const update::Status& st = g_upd.st;
	switch (st.phase)
	{
		case update::Phase::Checking: return T("Checking...");
		case update::Phase::UpToDate: return T("Up to date");
		case update::Phase::Available:
			snprintf(buf, sizeof(buf), T("Install %s (%.1f MB)"), st.latest.c_str(), st.size / 1048576.0);
			return buf;
		case update::Phase::Downloading:
			snprintf(buf, sizeof(buf), T("Downloading %d%%"), st.total ? int(st.done * 100 / st.total) : 0);
			return buf;
		case update::Phase::Installing:
			snprintf(buf, sizeof(buf), T("Installing %d/%d"), int(st.done), int(st.total));
			return buf;
		case update::Phase::Done: return T("Installed: restart");
		case update::Phase::Failed: return T("Failed: try again");
		default: return T("Check now");
	}
}

void UpdateAction()
{
	std::string reply;
	switch (g_upd.st.phase)
	{
		case update::Phase::Available: jailbreak::UpdateRequest("install", reply); break;
		case update::Phase::Checking:
		case update::Phase::Downloading:
		case update::Phase::Installing: break;
		case update::Phase::Done:
		{
			// the new build, opened in this one's place
			const char* path = "/data/homebrew/" SNES9X_TITLE_ID "/eboot.bin";
			OrbisLog("[update] restarting %s", path);
			Config().Save();
			OrbisLogClose();
			const int rc = sceSystemServiceLoadExec(path, nullptr);
			if (rc == 0)
				for (int i = 0; i < 100; i++)
					usleep(100 * 1000);
			OrbisLogOpen("boot-after-update");
			OrbisLog("[update] sceSystemServiceLoadExec(%s) -> %x: close the app and open it again", path, unsigned(rc));
			break;
		}
		default: jailbreak::UpdateRequest("check", reply); break;
	}
	PollUpdate(true);
}

// The Network shares row: how many shares are set up, and how many of them are connected.
std::string NetworkSummary()
{
	const std::vector<NetfsShareInfo> shares = netfs_shares();
	if (shares.empty())
		return T("None");
	int ready = 0;
	for (const NetfsShareInfo& i : shares)
		ready += i.state == NetfsState::Ready;
	return Fmt(T("%d of %d connected"), ready, int(shares.size()));
}

// The Download all borders row: what the helper is doing (borders/progress.txt), or what the last press asked for.
struct BorderAsk
{
	double at = -1000;
	int queued = 0;
} g_border_ask;

std::string BorderDownloadValue()
{
	const CoverProgress p = ReadBorderProgress();
	const double age = Now() - g_border_ask.at;
	if (p.valid && p.left > 0)
		return Fmt(T("%d left"), int(p.left));
	if (age < 6.0)
		return g_border_ask.queued > 0 ? Fmt(T("%d queued"), g_border_ask.queued) : std::string(T("Nothing to fetch"));
	if (g_border_ask.queued > 0 && !p.valid && age < 120.0)
		return T("Waiting for the helper");
	if (p.valid && p.fetched > 0)
		return Fmt(T("%d fetched"), int(p.fetched));
	return "";
}

std::string SettingLabel(int s)
{
	switch (s)
	{
		case S_SHADER: return T("Shader");
		case S_ASPECT: return T("Aspect ratio");
		case S_BORDER: return T("Border");
		case S_SCANLINES: return T("Scanlines (shader Off)");
		case S_SMOOTH: return T("Smooth picture (shader Off)");
		case S_FPS: return T("Show FPS");
		case S_AUDIO: return T("Sound");
		case S_TRANSPARENCY: return T("Transparency");
		case S_SUPERFX: return T("Super FX clock");
		case S_COVERS: return T("Download covers");
		case S_LANGUAGE: return T("Language");
		case S_DEBUGLOGS: return T("Debug logs");
		case S_UPDATES: return Fmt(T("Updates (this is %s)"), SNES9X_PS5_VERSION);
		default: return "";
	}
}

std::string SettingValue(int s)
{
	const Settings& c = Config();
	char buf[32];
	switch (s)
	{
		case S_SHADER: return T(ps5crt::Name(ps5crt::Shader(c.shader)));
		case S_ASPECT: return T(ps5video::AspectName(ps5video::Aspect(c.aspect)));
		case S_BORDER: return T(BorderName(c.border));
		case S_SCANLINES: return YesNo(c.scanlines);
		case S_SMOOTH: return YesNo(c.smooth);
		case S_FPS: return YesNo(c.show_fps);
		case S_AUDIO: return YesNo(c.audio);
		case S_TRANSPARENCY: return YesNo(c.transparency);
		case S_SUPERFX: snprintf(buf, sizeof(buf), "%d%%", c.superfx_clock); return buf;
		case S_COVERS: return YesNo(c.covers_download);
		case S_LANGUAGE: return LanguageName(c.language);
		case S_DEBUGLOGS: return YesNo(c.debug_logs);
		case S_UPDATES: return UpdateValue();
		default: return "";
	}
}

void ChangeSetting(int s, int dir)
{
	Settings& c = Config();
	const int n = int(ps5video::Aspect::Count);
	switch (s)
	{
		case S_SHADER:
		{
			const int ns = int(ps5crt::Shader::Count);
			c.shader = (c.shader + dir + ns) % ns;
			break;
		}
		case S_ASPECT: c.aspect = (c.aspect + dir + n) % n; break;
		case S_BORDER: c.border = (c.border + dir + 3) % 3; break;
		case S_SCANLINES: c.scanlines = !c.scanlines; break;
		case S_SMOOTH: c.smooth = !c.smooth; break;
		case S_FPS: c.show_fps = !c.show_fps; break;
		case S_AUDIO: c.audio = !c.audio; break;
		case S_TRANSPARENCY: c.transparency = !c.transparency; break;
		case S_COVERS: c.covers_download = !c.covers_download; break;
		case S_LANGUAGE:
		{
			const int nl = int(Language::Count);
			c.language = (c.language + dir + nl) % nl;
			break;
		}
		case S_DEBUGLOGS:
			c.debug_logs = !c.debug_logs;
			OrbisLogSetEnabled(c.debug_logs); // at once: the line saying so is the last (or first) one written
			break;
		case S_SUPERFX:
		{
			static const int steps[] = {50, 75, 100, 150, 200, 250, 300, 400};
			int i = 0;
			while (i < 7 && steps[i] < c.superfx_clock)
				i++;
			i = std::max(0, std::min(7, i + dir));
			c.superfx_clock = steps[i];
			break;
		}
	}
	emu::ApplySettings();
}

} // namespace

void SettingsMenu()
{
	NavReader nav;
	int sel = 0;
	for (;;)
	{
		PollUpdate(false);
		std::vector<Row> rows;
		for (int s = 0; s < S_COUNT; s++)
			rows.push_back({SettingLabel(s), SettingValue(s)});
		rows.push_back({T("Network shares"), NetworkSummary(), true, true});
		rows.push_back({T("Controller shortcuts"), ""});
		rows.push_back({T("Repair missing covers"), ""});
		rows.push_back({T("Download all borders"), BorderDownloadValue(), true, true});
		rows.push_back({T("Back"), ""});
		Header(T("Settings"));
		DrawOptionBox(T("Settings"), rows, sel, false);
		if (sel == S_UPDATES && g_upd.st.phase == update::Phase::Failed && !g_upd.st.error.empty())
			DrawText(80, H - 130, FitText(g_upd.st.error, 2, W - 160).c_str(), 2, kAccent);
		else if (sel == S_UPDATES && !g_upd.helper)
			DrawText(80, H - 130, FitText(T("Updates are downloaded by the helper: send the payload, then open Settings again."), 2, W - 160).c_str(), 2, kDim);
		Footer((std::string(icon::Cross) + " " + T("Toggle") + "     " + icon::DpadLeftRight + " " + T("Adjust") + "     " + icon::Circle + " " + T("Back")).c_str());
		Present();

		const Nav n = nav.Read();
		const int count = int(rows.size());
		if (n.up)
			sel = (sel + count - 1) % count;
		if (n.down)
			sel = (sel + 1) % count;
		if (sel == S_UPDATES)
		{
			if (n.ok)
				UpdateAction();
		}
		else if (sel < S_COUNT && (n.left || n.right || n.ok))
			ChangeSetting(sel, n.left ? -1 : 1);
		if (n.ok && sel == S_COUNT)
		{
			NetworkMenu();
			nav = NavReader();
		}
		if (n.ok && sel == S_COUNT + 1)
		{
			ShortcutMenu();
			nav = NavReader();
		}
		if (n.ok && sel == S_COUNT + 2)
		{
			const int count = RepairMissingCovers();
			OrbisLog("[settings] queued %d missing covers for retry", count);
			nav = NavReader();
		}
		if (n.ok && sel == S_COUNT + 3)
		{
			g_border_ask.queued = DownloadBorders();
			g_border_ask.at = Now();
		}
		if (n.back || n.options || (n.ok && sel == S_COUNT + 4))
		{
			Config().Save();
			return;
		}
	}
}

namespace
{
std::string DirTitle(const std::string& dir)
{
	return dir.empty() ? T("Choose where your games are") : dir;
}
} // namespace

// =========================================================================================================
std::string RomBrowser()
{
	Settings& cfg = Config();
	NavReader nav;

	std::vector<std::string> roots = RomRoots();
	std::string dir;
	// Reopen the last folder if it is still there and under one of the roots.
	if (!cfg.last_dir.empty() && OrbisIsDir(cfg.last_dir))
	{
		for (const std::string& r : roots)
			if (cfg.last_dir.rfind(r, 0) == 0)
				dir = cfg.last_dir;
	}
	if (dir.empty() && roots.size() == 1)
		dir = roots[0];

	std::vector<Entry> list;
	int sel = 0, top = 0;
	auto reload = [&](const std::string& select_path) {
		roots = RomRoots();
		if (dir.empty())
		{
			list.clear();
			for (const std::string& r : roots)
				list.push_back({RootLabel(r), r, true});
		}
		else
			list = ListDir(dir);
		sel = 0;
		for (size_t i = 0; i < list.size(); i++)
			if (list[i].path == select_path)
				sel = int(i);
		top = 0;
	};
	reload(cfg.last_rom);

	const int scale = 3;
	const int row_h = 42;
	const int list_y = 190;
	const int rows_visible = (H - 100 - list_y) / row_h;
	double next_rescan = Now() + 3.0;

	for (;;)
	{
		// USB drives come and go: look again every few seconds while on the root list.
		if (dir.empty() && Now() >= next_rescan)
		{
			const std::string keep = list.empty() ? "" : list[sel].path;
			reload(keep);
			next_rescan = Now() + 3.0;
		}

		Header(DirTitle(dir).c_str());
		if (list.empty())
		{
			const char* lines[] = {
				T("No games found here."),
				"",
				T("Copy your SNES ROMs (.sfc .smc .swc .fig .bs .zip .7z) to:"),
				"    /data/snes9x/roms",
				T("or, on a USB drive, to the folder  snes9x/roms"),
				"",
				T("Games on a NAS or PC?  Triangle (Settings), then Network shares."),
			};
			int y = 330;
			for (const char* l : lines)
			{
				DrawText(160, y, FitText(l, 4, W - 320).c_str(), 4, kText);
				y += 60;
			}
		}
		else
		{
			if (sel < top)
				top = sel;
			if (sel >= top + rows_visible)
				top = sel - rows_visible + 1;
			for (int i = 0; i < rows_visible && top + i < int(list.size()); i++)
			{
				const Entry& e = list[top + i];
				const int y = list_y + i * row_h;
				if (top + i == sel)
					ps5video::FillRect(60, y - 6, W - 140, row_h, kSel);
				const std::string label = (e.dir ? "> " : "  ") + e.name;
				DrawText(80, y, FitText(label, scale, W - 260).c_str(), scale, e.dir ? kFolder : kText);
			}
			// scrollbar
			if (int(list.size()) > rows_visible)
			{
				const int track = rows_visible * row_h;
				const int thumb = std::max(30, track * rows_visible / int(list.size()));
				const int ty = list_y + (track - thumb) * top / std::max(1, int(list.size()) - rows_visible);
				ps5video::FillRect(W - 60, list_y, 8, track, kPanel);
				ps5video::FillRect(W - 60, ty, 8, thumb, kAccent);
			}
			char count[64];
			snprintf(count, sizeof(count), "%d / %d", sel + 1, int(list.size()));
			DrawText(W - 80 - TextWidth(count, 3), 104, count, 3, kDim);
		}
		Footer((std::string(icon::Cross) + " " + T("Open") + "     " + icon::Circle + " " + T("Back") + "     " + icon::Triangle + " " + T("Settings") + "     " + icon::Options + " " + T("Quit") + "     " + icon::L1 + " " + icon::R1 + " " + T("Page")).c_str());
		Present();

		const Nav n = nav.Read();
		const int count = int(list.size());
		if (count > 0)
		{
			if (n.up)
				sel = (sel + count - 1) % count;
			if (n.down)
				sel = (sel + 1) % count;
			if (n.pgup || n.left)
				sel = std::max(0, sel - rows_visible);
			if (n.pgdn || n.right)
				sel = std::min(count - 1, sel + rows_visible);
		}
		if (n.triangle)
		{
			SettingsMenu();
			nav = NavReader();
		}
		if (n.options)
			return "";
		if (n.back && !dir.empty())
		{
			// up one level, or back to the list of drives
			bool at_root = false;
			for (const std::string& r : roots)
				at_root |= (dir == r);
			const std::string from = dir;
			if (at_root)
				dir = roots.size() == 1 ? dir : "";
			else
				dir = dir.substr(0, dir.find_last_of('/'));
			if (dir != from)
				reload(from);
		}
		if (n.ok && count > 0)
		{
			const Entry e = list[sel];
			if (e.dir)
			{
				dir = e.path;
				reload("");
			}
			else
			{
				cfg.last_dir = dir;
				cfg.last_rom = e.path;
				cfg.Save();
				return e.path;
			}
		}
	}
}

PauseAction PauseMenu()
{
	Settings& cfg = Config();
	NavReader nav;
	int sel = 0;
	enum Item
	{
		I_RESUME,
		I_SAVE,
		I_LOAD,
		I_SLOT,
		I_CHEATS,
		I_SHORTCUTS,
		I_SET0, // the S_* settings follow (not Updates: Settings on the game list has it)
		I_RESET = I_SET0 + S_UPDATES,
		I_LIST,
		I_QUIT,
		I_COUNT
	};
	std::string toast;
	double toast_until = 0;

	for (;;)
	{
		// the paused game, darkened, under the box
		ps5video::FillRect(0, 0, W, H, Rgb(0, 0, 0));
		emu::RedrawLastFrame();
		ps5video::DarkenRect(0, 0, W, H);
		ps5video::DarkenRect(0, 0, W, H);

		char slot[64];
		std::vector<Row> rows(I_COUNT);
		rows[I_RESUME] = {T("Resume"), ""};
		snprintf(slot, sizeof(slot), T("Save state (slot %d)"), cfg.state_slot);
		rows[I_SAVE] = {slot, ""};
		snprintf(slot, sizeof(slot), T("Load state (slot %d)"), cfg.state_slot);
		rows[I_LOAD] = {slot, "", emu::StateExists(cfg.state_slot)};
		snprintf(slot, sizeof(slot), T(emu::StateExists(cfg.state_slot) ? "%d (used)" : "%d (empty)"), cfg.state_slot);
		rows[I_SLOT] = {T("State slot"), slot};
		rows[I_CHEATS] = {T("Cheat manager"), ""};
		rows[I_SHORTCUTS] = {T("Controller shortcuts"), ""};
		for (int s = 0; s < S_UPDATES; s++)
			rows[I_SET0 + s] = {SettingLabel(s), SettingValue(s)};
		rows[I_RESET] = {T("Reset game"), ""};
		rows[I_LIST] = {T("Back to the game list"), ""};
		rows[I_QUIT] = {T("Quit Snes9x"), ""};

		DrawOptionBox(FitText(emu::GameName(), 5, 900).c_str(), rows, sel, true);
		if (!toast.empty() && Now() < toast_until)
		{
			toast = FitText(toast, 3, W - 200);
			const int tw = TextWidth(toast.c_str(), 3);
			ps5video::FillRect((W - tw) / 2 - 30, H - 110, tw + 60, 70, kSel);
			DrawText((W - tw) / 2, H - 90, toast.c_str(), 3, kText);
		}
		Present();

		const Nav n = nav.Read();
		if (n.up)
			sel = (sel + I_COUNT - 1) % I_COUNT;
		if (n.down)
			sel = (sel + 1) % I_COUNT;
		if (n.back || n.options || n.menu)
			return PauseAction::Resume;

		if (sel == I_SLOT && (n.left || n.right || n.ok))
			cfg.state_slot = (cfg.state_slot + (n.left ? 9 : 1)) % 10;
		else if (sel >= I_SET0 && sel < I_SET0 + S_UPDATES && (n.left || n.right || n.ok))
		{
			ChangeSetting(sel - I_SET0, n.left ? -1 : 1);
			cfg.Save();
		}
		else if (n.ok)
		{
			switch (sel)
			{
				case I_RESUME: return PauseAction::Resume;
				case I_SAVE:
					toast = T(emu::SaveState(cfg.state_slot) ? "State saved." : "Could not save the state.");
					toast_until = Now() + 2.0;
					cfg.Save();
					break;
				case I_LOAD:
					if (emu::LoadState(cfg.state_slot))
					{
						cfg.Save();
						emu::Osd(T("State loaded from slot %d"), cfg.state_slot);
						return PauseAction::Resume;
					}
					toast = T("This slot is empty.");
					toast_until = Now() + 2.0;
					break;
				case I_CHEATS:
					CheatMenu();
					nav = NavReader();
					break;
				case I_SHORTCUTS:
					ShortcutMenu();
					nav = NavReader();
					break;
				case I_RESET:
					emu::Reset();
					return PauseAction::Resume;
				case I_LIST: return PauseAction::BackToList;
				case I_QUIT: return PauseAction::Quit;
				default: break;
			}
		}
	}
}


// Cheat groups are read from the game already loaded by emu::LoadGame, and again whenever the cheat file changes
// (a code added over FTP shows up here by itself).
// A scrolled list is essential: RetroArch files can contain hundreds of codes.
void CheatMenu()
{
	NavReader nav;
	int sel = 0;
	int top = 0;
	constexpr int kVisible = 14;
	bool first = true;
	for (;;)
	{
		emu::RefreshCheats(first);
		first = false;
		const int count = emu::CheatCount();
		const int visible = std::min(count, kVisible);
		// the list may have become shorter under the selection
		sel = std::max(0, std::min(sel, count - 1));
		top = std::max(0, std::min(top, count - visible));
		if (count == 0)
		{
			Header(T("Cheats"));
			DrawOptionBox(T("No cheats found"), {{T("Place a matching .cht file in /data/snes9x/cheats"), ""}, {T("Back"), ""}}, 1, false);
			DrawText(150, H - 184, FitText(emu::CheatStatus(), 3, W - 300).c_str(), 3, kText);
			Footer(T("Copy .cht file over FTP, it loads by itself     Circle: Back"));
		}
		else
		{
			if (sel < top) top = sel;
			if (sel >= top + visible) top = sel - visible + 1;
			std::vector<Row> rows;
			rows.reserve(size_t(visible));
			for (int i = 0; i < visible; ++i)
			{
				const int index = top + i;
				rows.push_back({FitText(emu::CheatName(index), 3, 670), T(emu::CheatEnabled(index) ? "ON" : "OFF")});
			}
			Header(emu::GameName().c_str());
			char caption[120];
			snprintf(caption, sizeof(caption), T("Cheats %d-%d / %d"), top + 1, top + visible, count);
			DrawOptionBox(caption, rows, sel - top, true);
			Footer(T("Cross: Toggle   L1/R1: Page   Triangle: All ON   Square: All OFF   Circle: Back"));
		}
		Present();
		const Nav n = nav.Read();
		if (n.back || n.menu) return;
		if (count == 0) continue;
		if (n.up) sel = (sel + count - 1) % count;
		if (n.down) sel = (sel + 1) % count;
		if (n.pgup) sel = std::max(0, sel - kVisible);
		if (n.pgdn) sel = std::min(count - 1, sel + kVisible);
		// The navigation arrows auto-repeat; a toggle must react only to the edge,
		// otherwise holding Left/Right flips the cheat repeatedly and hammers storage.
		const uint32_t pressed_lr = ps5input::Pressed() & (SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT);
		if (n.ok || pressed_lr)
		{
			if (!emu::ToggleCheat(sel))
				OrbisLog("[cheats] could not persist toggle for group %d", sel);
		}
		if (n.triangle) emu::SetAllCheats(true);
		if (ps5input::Pressed() & SCE_PAD_BUTTON_SQUARE) emu::SetAllCheats(false);
	}
}


void ShortcutMenu()
{
	NavReader nav;
	int sel = 0;
	for (;;)
	{
		Settings& cfg = Config();
		int* items[] = {&cfg.shortcut_pause, &cfg.shortcut_list, &cfg.shortcut_cheats};
		std::vector<Row> rows = {
			{T("Pause menu"), T(shortcuts::Label(*items[0]))},
			{T("Return to game shelf"), T(shortcuts::Label(*items[1]))},
			{T("Cheat manager"), T(shortcuts::Label(*items[2]))},
			{T("Back"), ""}
		};
		Header(T("Controller shortcuts"));
		DrawOptionBox(T("Button shortcuts"), rows, sel, false);
		Footer(T("Left / Right: Reassign     Cross: Next option     Circle: Back"));
		Present();
		const Nav n = nav.Read();
		if (n.back || n.options || (n.ok && sel == 3)) { cfg.Save(); return; }
		if (n.up) sel = (sel + 3) % 4;
		if (n.down) sel = (sel + 1) % 4;
		if (sel < 3 && (n.left || n.right || n.ok))
		{
			// Never let two actions share a chord; skip existing bindings.
			const int dir = n.left ? -1 : 1;
			for (int i = 0; i < shortcuts::kCount; i++)
			{
				const int proposed = (*items[sel] + dir + shortcuts::kCount) % shortcuts::kCount;
				*items[sel] = proposed;
				if (proposed != *items[(sel + 1) % 3] && proposed != *items[(sel + 2) % 3])
					break;
			}
			cfg.Save();
		}
	}
}

void MessageBox(const std::string& title, const std::string& text)
{
	NavReader nav;
	for (;;)
	{
		Header("");
		const int bw = 1400, bh = 300;
		const int bx = (W - bw) / 2, by = (H - bh) / 2;
		ps5video::FillRect(bx - 4, by - 4, bw + 8, bh + 8, kAccent);
		ps5video::FillRect(bx, by, bw, bh, kPanel);
		DrawText(bx + 40, by + 30, FitText(title, 5, bw - 80).c_str(), 5, kAccent);
		if (text.find('\n') == std::string::npos)
			DrawText(bx + 40, by + 130, FitText(text, 3, bw - 80).c_str(), 3, kText);
		else
		{
			// a few lines (a ROM's path, then why it did not open), skipping the blank ones
			int y = by + 100;
			for (size_t i = 0, e; i < text.size() && y < by + bh - 80; i = e + 1)
			{
				e = text.find('\n', i);
				if (e == std::string::npos)
					e = text.size();
				if (e > i)
				{
					DrawText(bx + 40, y, FitText(text.substr(i, e - i), 3, bw - 80).c_str(), 3, kText);
					y += 46;
				}
			}
		}
		DrawText(bx + 40, by + bh - 70, (std::string(icon::Cross) + " " + T("OK")).c_str(), 3, kDim);
		Present();
		const Nav n = nav.Read();
		if (n.ok || n.back || n.options)
			return;
	}
}
} // namespace fe

// Snes9x PS5 frontend: the game list and the menus.
//
// Everything is drawn on the CPU into the 1920x1080 surface with Snes9x's 8x10 font, scaled 3x-5x, and
// shown with ps5video::Present (which waits for vsync, so the menus run at 60 Hz).
//
// SPDX-License-Identifier: MIT

#include "fe_menu.h"

#include "fe_emu.h"
#include "fe_games.h"
#include "fe_shelf.h"
#include "fe_settings.h"
#include "fe_shortcuts.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoCrt.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "snes9x.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#ifndef SNES9X_PS5_VERSION
#define SNES9X_PS5_VERSION "dev"
#endif

namespace fe
{
namespace
{
using ps5video::Rgb;
constexpr int W = ps5video::kWidth;
constexpr int H = ps5video::kHeight;

const uint32_t kBg = Rgb(14, 14, 28);
const uint32_t kPanel = Rgb(26, 24, 48);
const uint32_t kAccent = Rgb(150, 125, 255);
const uint32_t kSel = Rgb(78, 62, 170);
const uint32_t kText = Rgb(235, 235, 245);
const uint32_t kDim = Rgb(150, 150, 175);
const uint32_t kFolder = Rgb(255, 210, 110);

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---- input with key repeat ---------------------------------------------------------------------------
struct Nav
{
	bool up = false, down = false, left = false, right = false;
	bool pgup = false, pgdn = false;
	bool ok = false, back = false, options = false, triangle = false, menu = false;
};

class NavReader
{
public:
	NavReader()
	{
		ps5input::Poll();
		m_prev = ps5input::Pad(0).buttons; // buttons still held from before don't count
	}
	Nav Read()
	{
		ps5input::Poll();
		const uint32_t cur = ps5input::Pad(0).buttons;
		const uint32_t down = cur & ~m_prev;
		m_prev = cur;
		const double now = Now();
		Nav n;
		n.up = Repeat(cur, down, SCE_PAD_BUTTON_UP, now, 0);
		n.down = Repeat(cur, down, SCE_PAD_BUTTON_DOWN, now, 1);
		n.left = Repeat(cur, down, SCE_PAD_BUTTON_LEFT, now, 2);
		n.right = Repeat(cur, down, SCE_PAD_BUTTON_RIGHT, now, 3);
		n.pgup = Repeat(cur, down, SCE_PAD_BUTTON_L1, now, 4);
		n.pgdn = Repeat(cur, down, SCE_PAD_BUTTON_R1, now, 5);
		n.ok = down & SCE_PAD_BUTTON_CROSS;
		n.back = down & SCE_PAD_BUTTON_CIRCLE;
		n.options = down & SCE_PAD_BUTTON_OPTIONS;
		n.triangle = down & SCE_PAD_BUTTON_TRIANGLE;
		const uint32_t combo = shortcuts::kChoices[Config().shortcut_pause].buttons;
		n.menu = (cur & combo) == combo && (down & combo);
		return n;
	}

private:
	bool Repeat(uint32_t cur, uint32_t down, uint32_t bit, double now, int i)
	{
		if (down & bit)
		{
			m_next[i] = now + 0.40;
			return true;
		}
		if ((cur & bit) && now >= m_next[i])
		{
			m_next[i] = now + 0.07;
			return true;
		}
		return false;
	}
	uint32_t m_prev = 0;
	double m_next[6] = {};
};

void Present()
{
	ps5video::Present(0, 0, 0, 0, true);
}

void Header(const char* subtitle)
{
	ps5video::FillRect(0, 0, W, H, kBg);
	ps5video::FillRect(0, 0, W, 150, kPanel);
	ps5video::FillRect(0, 150, W, 4, kAccent);
	DrawText(80, 34, "Snes9x PS5", 6, kAccent);
	const char* ver = "Snes9x " VERSION " - port PS5 " SNES9X_PS5_VERSION;
	DrawText(W - 80 - TextWidth(ver, 2), 40, ver, 2, kDim);
	if (subtitle && *subtitle)
		DrawText(84, 104, FitText(subtitle, 3, W - 168).c_str(), 3, kDim);
}

void Footer(const char* help)
{
	ps5video::FillRect(0, H - 80, W, 80, kPanel);
	DrawText(80, H - 60, help, 3, kDim);
}

bool HasRomExtension(const char* name)
{
	static const char* const exts[] = {".sfc", ".smc", ".swc", ".fig", ".bs", ".st", ".zip", ".gz", ".bin", ".mgd"};
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
		return "Console  (" + root + ")";
	if (root.rfind("/mnt/usb", 0) == 0)
		return "USB " + root.substr(8, 1) + "  (" + root + ")";
	return root;
}

// A box of label/value rows (pause menu, settings).
struct Row
{
	std::string label;
	std::string value; // "" = an action
	bool enabled = true;
};

void DrawOptionBox(const char* title, const std::vector<Row>& rows, int sel, bool over_game)
{
	const int scale = 3;
	const int row_h = 44;
	const int bw = 1050;
	const int visible = std::min(16, int(rows.size()));
	const int start = std::min(std::max(0, sel - visible + 1), std::max(0, int(rows.size()) - visible));
	const int bh = 120 + visible * row_h + 30;
	const int bx = (W - bw) / 2;
	const int by = std::max(20, (H - bh) / 2);
	if (!over_game)
		ps5video::FillRect(bx - 4, by - 4, bw + 8, bh + 8, kAccent);
	ps5video::FillRect(bx, by, bw, bh, kPanel);
	ps5video::FillRect(bx, by + 86, bw, 3, kAccent);
	DrawText(bx + 40, by + 24, title, 5, kAccent);
	for (int i = start; i < start + visible; i++)
	{
		const int y = by + 110 + (i - start) * row_h;
		if (int(i) == sel)
			ps5video::FillRect(bx + 16, y - 6, bw - 32, row_h, kSel);
		const uint32_t col = rows[i].enabled ? kText : kDim;
		DrawText(bx + 40, y, FitText(rows[i].label, scale, bw - 320).c_str(), scale, col);
		if (!rows[i].value.empty())
		{
			const std::string v = (int(i) == sel ? "< " + rows[i].value + " >" : rows[i].value);
			DrawText(bx + bw - 40 - TextWidth(v.c_str(), scale), y, v.c_str(), scale, int(i) == sel ? kText : kAccent);
		}
	}
}

const char* YesNo(bool b)
{
	return b ? "On" : "Off";
}

// The settings shared by the game list (Triangle) and the pause menu. Returns true if 'idx' was a setting
// and 'dir' (-1/+1) changed it.
enum SettingRow
{
	S_SHADER,
	S_ASPECT,
	S_SCANLINES,
	S_FPS,
	S_AUDIO,
	S_TRANSPARENCY,
	S_SUPERFX,
	S_COVERS,
	S_DEBUGLOGS,
	S_COUNT
};

std::string SettingLabel(int s)
{
	switch (s)
	{
		case S_SHADER: return "Shader";
		case S_ASPECT: return "Aspect ratio";
		case S_SCANLINES: return "Scanlines (shader Off)";
		case S_FPS: return "Show FPS";
		case S_AUDIO: return "Sound";
		case S_TRANSPARENCY: return "Transparency";
		case S_SUPERFX: return "Super FX clock";
		case S_COVERS: return "Download covers";
		case S_DEBUGLOGS: return "Debug logs";
		default: return "";
	}
}

std::string SettingValue(int s)
{
	const Settings& c = Config();
	char buf[32];
	switch (s)
	{
		case S_SHADER: return ps5crt::Name(ps5crt::Shader(c.shader));
		case S_ASPECT: return ps5video::AspectName(ps5video::Aspect(c.aspect));
		case S_SCANLINES: return YesNo(c.scanlines);
		case S_FPS: return YesNo(c.show_fps);
		case S_AUDIO: return YesNo(c.audio);
		case S_TRANSPARENCY: return YesNo(c.transparency);
		case S_SUPERFX: snprintf(buf, sizeof(buf), "%d%%", c.superfx_clock); return buf;
		case S_COVERS: return YesNo(c.covers_download);
		case S_DEBUGLOGS: return YesNo(c.debug_logs);
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
		case S_SCANLINES: c.scanlines = !c.scanlines; break;
		case S_FPS: c.show_fps = !c.show_fps; break;
		case S_AUDIO: c.audio = !c.audio; break;
		case S_TRANSPARENCY: c.transparency = !c.transparency; break;
		case S_COVERS: c.covers_download = !c.covers_download; break;
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
		std::vector<Row> rows;
		for (int s = 0; s < S_COUNT; s++)
			rows.push_back({SettingLabel(s), SettingValue(s)});
		rows.push_back({"Controller shortcuts", ""});
		rows.push_back({"Repair missing covers", ""});
		rows.push_back({"Back", ""});
		Header("Settings");
		DrawOptionBox("Settings", rows, sel, false);
		Footer((std::string(icon::Cross) + " Toggle     " + icon::DpadLeftRight + " Adjust     " + icon::Circle + " Back").c_str());
		Present();

		const Nav n = nav.Read();
		const int count = int(rows.size());
		if (n.up)
			sel = (sel + count - 1) % count;
		if (n.down)
			sel = (sel + 1) % count;
		if (sel < S_COUNT && (n.left || n.right || n.ok))
			ChangeSetting(sel, n.left ? -1 : 1);
		if (n.ok && sel == S_COUNT)
		{
			ShortcutMenu();
			nav = NavReader();
		}
		if (n.ok && sel == S_COUNT + 1)
		{
			const int count = RepairMissingCovers();
			OrbisLog("[settings] queued %d missing covers for retry", count);
			nav = NavReader();
		}
		if (n.back || n.options || (n.ok && sel == S_COUNT + 2))
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
	return dir.empty() ? "Choose where your games are" : dir;
}
} // namespace

// =========================================================================================================
std::string RomBrowser()
{
	Settings& cfg = Config();
	NavReader nav;

	std::vector<std::string> roots = OrbisRomRoots();
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
		roots = OrbisRomRoots();
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
				"No games found here.",
				"",
				"Copy your SNES ROMs (.sfc .smc .swc .fig .bs .zip) to:",
				"    /data/snes9x/roms",
				"or, on a USB drive, to the folder  snes9x/roms",
			};
			int y = 330;
			for (const char* l : lines)
			{
				DrawText(160, y, l, 4, kText);
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
		Footer((std::string(icon::Cross) + " Open     " + icon::Circle + " Back     " + icon::Triangle + " Settings     " + icon::Options + " Quit     " + icon::L1 + " " + icon::R1 + " Page").c_str());
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
		I_SET0, // the S_* settings follow
		I_RESET = I_SET0 + S_COUNT,
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
		rows[I_RESUME] = {"Resume", ""};
		snprintf(slot, sizeof(slot), "Save state (slot %d)", cfg.state_slot);
		rows[I_SAVE] = {slot, ""};
		snprintf(slot, sizeof(slot), "Load state (slot %d)", cfg.state_slot);
		rows[I_LOAD] = {slot, "", emu::StateExists(cfg.state_slot)};
		snprintf(slot, sizeof(slot), "%d%s", cfg.state_slot, emu::StateExists(cfg.state_slot) ? " (used)" : " (empty)");
		rows[I_SLOT] = {"State slot", slot};
		rows[I_CHEATS] = {"Cheat manager", ""};
		rows[I_SHORTCUTS] = {"Controller shortcuts", ""};
		for (int s = 0; s < S_COUNT; s++)
			rows[I_SET0 + s] = {SettingLabel(s), SettingValue(s)};
		rows[I_RESET] = {"Reset game", ""};
		rows[I_LIST] = {"Back to the game list", ""};
		rows[I_QUIT] = {"Quit Snes9x", ""};

		DrawOptionBox(FitText(emu::GameName(), 5, 900).c_str(), rows, sel, true);
		if (!toast.empty() && Now() < toast_until)
		{
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
		else if (sel >= I_SET0 && sel < I_SET0 + S_COUNT && (n.left || n.right || n.ok))
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
					toast = emu::SaveState(cfg.state_slot) ? "State saved." : "Could not save the state.";
					toast_until = Now() + 2.0;
					cfg.Save();
					break;
				case I_LOAD:
					if (emu::LoadState(cfg.state_slot))
					{
						cfg.Save();
						emu::Osd("State loaded from slot %d", cfg.state_slot);
						return PauseAction::Resume;
					}
					toast = "This slot is empty.";
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


// Cheat groups are read from the game already loaded by emu::LoadGame.
// A scrolled list is essential: RetroArch files can contain hundreds of codes.
void CheatMenu()
{
	NavReader nav;
	int sel = 0;
	int top = 0;
	constexpr int kVisible = 14;
	for (;;)
	{
		if (emu::CheatCount() == 0) emu::ReloadDownloadedCheats();
		const int count = emu::CheatCount();
		const int visible = std::min(count, kVisible);
		if (count == 0)
		{
			Header("Cheats");
			DrawOptionBox("No cheats found", {{"Place a matching .cht file in /data/snes9x/cheats", ""}, {"Back", ""}}, 1, false);
			DrawText(150, H - 184, FitText(emu::CheatStatus(), 3, W - 300).c_str(), 3, kText);
			Footer("Copy .cht file over FTP     Circle: Back");
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
				rows.push_back({FitText(emu::CheatName(index), 3, 670), emu::CheatEnabled(index) ? "ON" : "OFF"});
			}
			Header(emu::GameName().c_str());
			char caption[120];
			snprintf(caption, sizeof(caption), "Cheats %d-%d / %d", top + 1, top + visible, count);
			DrawOptionBox(caption, rows, sel - top, true);
			Footer("Cross: Toggle   L1/R1: Page   Triangle: All ON   Square: All OFF   Circle: Back");
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
			{"Pause menu", shortcuts::Label(*items[0])},
			{"Return to game shelf", shortcuts::Label(*items[1])},
			{"Cheat manager", shortcuts::Label(*items[2])},
			{"Back", ""}
		};
		Header("Controller shortcuts");
		DrawOptionBox("Button shortcuts", rows, sel, false);
		Footer("Left / Right: Reassign     Cross: Next option     Circle: Back");
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
		DrawText(bx + 40, by + 130, FitText(text, 3, bw - 80).c_str(), 3, kText);
		DrawText(bx + 40, by + bh - 70, (std::string(icon::Cross) + " OK").c_str(), 3, kDim);
		Present();
		const Nav n = nav.Read();
		if (n.ok || n.back || n.options)
			return;
	}
}
} // namespace fe

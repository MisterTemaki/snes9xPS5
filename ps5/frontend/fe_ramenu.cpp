// Snes9x PS5 (2.11): the RetroAchievements screens (fe_menu.h): Settings, RetroAchievements (log in, hardcore mode, log
// out) and the pause menu's Achievements list (read-only).
// SPDX-License-Identifier: MIT

#include "fe_menu.h"

#include "fe_emu.h"
#include "fe_i18n.h"
#include "fe_osk.h"
#include "fe_ra.h"
#include "fe_settings.h"
#include "fe_text.h"
#include "fe_ui.h"

#include "OrbisPaths.h"
#include "ProsperoVideo.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace fe
{
using namespace ui;

namespace
{
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

std::string g_last_user; // the name typed last (this session only), offered again
} // namespace

std::string RetroAchievementsValue()
{
	switch (ra::LoginState())
	{
		case ra::Login::LoggedIn: return Fmt(T("%s (%u points)"), ra::UserName().c_str(), ra::Points());
		case ra::Login::Pending: return T("Logging in...");
		default: return T("Not logged in");
	}
}

void RetroAchievementsMenu()
{
	NavReader nav;
	int sel = 0;
	for (;;)
	{
		ra::Pump();
		const ra::Login login = ra::LoginState();
		const bool in = login == ra::Login::LoggedIn;
		enum Item
		{
			I_ACCOUNT,
			I_LOGIN, // Log in, or Log out
			I_HARDCORE,
			I_BACK,
			I_COUNT
		};
		std::vector<Row> rows(I_COUNT);
		std::string account;
		switch (login)
		{
			case ra::Login::LoggedIn: account = Fmt(T("%s (%u points)"), ra::UserName().c_str(), ra::Points()); break;
			case ra::Login::Pending: account = T("Logging in..."); break;
			default: account = T("Not logged in"); break;
		}
		// what the screen says, in the log whenever it changes (the host tests read it; nothing secret is in it)
		static std::string logged;
		const std::string said = account + " | " + ra::Problem();
		if (said != logged)
		{
			logged = said;
			OrbisLog("[ra] Settings, RetroAchievements: %s", said.c_str());
		}
		rows[I_ACCOUNT] = {T("Account"), account, true, true};
		rows[I_LOGIN] = {T(in ? "Log out" : "Log in"), "", login != ra::Login::Pending};
		rows[I_HARDCORE] = {T("Hardcore mode"), T(ra::HardcoreSetting() ? "On" : "Off")};
		rows[I_BACK] = {T("Back"), ""};
		Header("RetroAchievements");
		DrawOptionBox("RetroAchievements", rows, sel, false);
		// under the box (DrawOptionBox centres it: 4 rows end at y 703): what is wrong, what hardcore does, what is kept
		int y = 740;
		const std::string problem = ra::Problem();
		if (!problem.empty())
		{
			DrawText(160, y, FitText(problem, 3, W - 320).c_str(), 3, kBad);
			y += 56;
		}
		const char* lines[] = {
			T("Hardcore mode: no loading of states and no cheats while a game has achievements; turning it on resets the game."),
			T("Your password is sent once, through the helper, and never stored: only the server's token is kept,"),
			T("in /data/snes9x/retroachievements/login.txt. Log out deletes it."),
		};
		for (const char* l : lines)
		{
			DrawText(160, y, FitText(l, 2, W - 320).c_str(), 2, kDim);
			y += 36;
		}
		Footer((std::string(icon::Cross) + " " + T("Select") + "     " + icon::DpadLeftRight + " " + T("Adjust") + "     " +
			icon::Circle + " " + T("Back")).c_str());
		Present();

		const Nav n = nav.Read();
		if (n.up)
			sel = (sel + I_COUNT - 1) % I_COUNT;
		if (n.down)
			sel = (sel + 1) % I_COUNT;
		if (n.back || n.options || (n.ok && sel == I_BACK))
			return;
		if (sel == I_HARDCORE && (n.ok || n.left || n.right))
			ra::SetHardcore(!ra::HardcoreSetting());
		if (n.ok && sel == I_LOGIN && login != ra::Login::Pending)
		{
			if (in)
				ra::Logout();
			else
			{
				std::string user = g_last_user;
				if (EditText(T("RetroAchievements user name"), user, false, 64) && !user.empty())
				{
					g_last_user = user;
					std::string password;
					if (EditText(T("RetroAchievements password"), password, true, 128) && !password.empty())
						ra::BeginLogin(user, password); // wipes it
					for (char& c : password)
						*(volatile char*)&c = 0;
				}
			}
			nav = NavReader();
		}
	}
}

void AchievementsMenu()
{
	NavReader nav;
	int sel = 0, top = 0;
	const int row_h = 82, list_y = 250, visible = (H - 110 - list_y) / row_h;
	for (;;)
	{
		ra::Pump();
		const std::vector<ra::Item> items = ra::AchievementItems();
		const int count = int(items.size());
		sel = std::max(0, std::min(sel, count - 1));
		if (sel < top)
			top = sel;
		if (sel >= top + visible)
			top = sel - visible + 1;

		ps5video::FillRect(0, 0, W, H, ps5video::Rgb(0, 0, 0));
		emu::RedrawLastFrame();
		ps5video::DarkenRect(0, 0, W, H);
		ps5video::DarkenRect(0, 0, W, H);
		const int bx = 160, bw = W - 320;
		ps5video::FillRect(bx, 60, bw, H - 140, kPanel);
		ps5video::FillRect(bx, 60, bw, 3, kAccent);
		DrawText(bx + 40, 84, FitText(T("Achievements"), 5, bw / 2).c_str(), 5, kAccent);
		const std::string summary = ra::GameSummary() + "  -  " + T(ra::HardcoreSetting() ? "Hardcore mode" : "Softcore mode");
		DrawText(bx + bw - 40 - TextWidth(FitText(summary, 3, bw / 2).c_str(), 3), 100, FitText(summary, 3, bw / 2).c_str(), 3, kDim);
		const std::string title = ra::GameTitle().empty() ? emu::GameName() : ra::GameTitle();
		DrawText(bx + 40, 170, FitText(title, 3, bw - 80).c_str(), 3, kText);
		if (count == 0)
			DrawText(bx + 40, list_y + 20, FitText(T("No achievements to show for this game."), 3, bw - 80).c_str(), 3, kDim);
		for (int i = 0; i < visible && top + i < count; i++)
		{
			const ra::Item& it = items[size_t(top + i)];
			const int y = list_y + i * row_h;
			if (top + i == sel)
				ps5video::FillRect(bx + 16, y - 6, bw - 32, row_h - 6, kSel);
			if (it.section)
			{
				DrawText(bx + 40, y + 20, FitText(it.title, 3, bw - 80).c_str(), 3, kAccent);
				continue;
			}
			const std::string mark = T(it.unlocked ? "Unlocked" : "Locked");
			const std::string pts = Fmt(T("%u points"), it.points);
			const int right = TextWidth(pts.c_str(), 3) + 40;
			DrawText(bx + 40, y, FitText(it.title, 3, bw - 120 - right).c_str(), 3, it.unlocked ? kGood : kText);
			DrawText(bx + bw - 40 - TextWidth(pts.c_str(), 3), y, pts.c_str(), 3, it.unlocked ? kGood : kFolder);
			std::string second = it.description;
			const std::string state = it.unlocked ? mark : (it.progress.empty() ? mark : mark + "  " + it.progress);
			const int state_w = TextWidth(state.c_str(), 2) + 40;
			DrawText(bx + 40, y + 40, FitText(second, 2, bw - 120 - state_w).c_str(), 2, kDim);
			DrawText(bx + bw - 40 - TextWidth(state.c_str(), 2), y + 40, state.c_str(), 2, it.unlocked ? kGood : kDim);
		}
		if (count > visible)
		{
			const int track = visible * row_h;
			const int thumb = std::max(30, track * visible / count);
			const int ty = list_y + (track - thumb) * top / std::max(1, count - visible);
			ps5video::FillRect(bx + bw - 14, list_y, 6, track, kBg);
			ps5video::FillRect(bx + bw - 14, ty, 6, thumb, kAccent);
		}
		Footer((std::string(icon::DpadUpDown) + " " + T("Scroll") + "     " + icon::L1 + " " + icon::R1 + " " + T("Page") +
			"     " + icon::Circle + " " + T("Back")).c_str());
		Present();

		const Nav n = nav.Read();
		if (n.back || n.options || n.menu)
			return;
		if (count > 0)
		{
			if (n.up)
				sel = std::max(0, sel - 1);
			if (n.down)
				sel = std::min(count - 1, sel + 1);
			if (n.pgup)
				sel = std::max(0, sel - visible);
			if (n.pgdn)
				sel = std::min(count - 1, sel + visible);
		}
	}
}
} // namespace fe

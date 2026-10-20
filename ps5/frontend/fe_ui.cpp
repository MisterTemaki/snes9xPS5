// Snes9x PS5 frontend: what the menu screens have in common (fe_ui.h).
// SPDX-License-Identifier: MIT

#include "fe_ui.h"

#include "fe_settings.h"
#include "fe_shortcuts.h"
#include "fe_text.h"

#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "snes9x.h"

#include <algorithm>
#include <ctime>

#ifndef SNES9X_PS5_VERSION
#define SNES9X_PS5_VERSION "dev"
#endif

namespace fe
{
namespace ui
{
double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---- input with key repeat ---------------------------------------------------------------------------
NavReader::NavReader()
{
	ps5input::Poll();
	m_prev = ps5input::Pad(0).buttons; // buttons still held from before don't count
}

Nav NavReader::Read()
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
	n.square = down & SCE_PAD_BUTTON_SQUARE;
	n.l2 = down & SCE_PAD_BUTTON_L2;
	n.r2 = down & SCE_PAD_BUTTON_R2;
	const uint32_t combo = shortcuts::kChoices[Config().shortcut_pause].buttons;
	n.menu = (cur & combo) == combo && (down & combo);
	return n;
}

bool NavReader::Repeat(uint32_t cur, uint32_t down, uint32_t bit, double now, int i)
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
	DrawText(80, H - 60, FitText(help, 3, W - 160).c_str(), 3, kDim);
}

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
	DrawText(bx + 40, by + 24, FitText(title, 5, bw - 80).c_str(), 5, kAccent);
	for (int i = start; i < start + visible; i++)
	{
		const int y = by + 110 + (i - start) * row_h;
		if (int(i) == sel)
			ps5video::FillRect(bx + 16, y - 6, bw - 32, row_h, kSel);
		const uint32_t col = rows[i].enabled ? kText : kDim;
		const std::string label = FitText(rows[i].label, scale, bw - 320);
		DrawText(bx + 40, y, label.c_str(), scale, col);
		if (!rows[i].value.empty())
		{
			// a long (translated) value is cut before it reaches the label
			const int room = bw - 80 - TextWidth(label.c_str(), scale) - 30;
			const bool arrows = int(i) == sel && !rows[i].plain;
			const std::string value = FitText(rows[i].value, scale, room - (arrows ? TextWidth("<  >", scale) : 0));
			const std::string v = arrows ? "< " + value + " >" : value;
			DrawText(bx + bw - 40 - TextWidth(v.c_str(), scale), y, v.c_str(), scale, int(i) == sel ? kText : kAccent);
		}
	}
}
} // namespace ui
} // namespace fe

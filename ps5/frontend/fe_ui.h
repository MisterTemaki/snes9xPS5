// Snes9x PS5 frontend: what the menu screens have in common -- the colours, the pad with key repeat, the header and
// footer bars and the box of label/value rows -- drawn on the 1920x1080 surface (fe_menu.cpp, fe_netscreen.cpp, fe_osk.cpp).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ProsperoVideo.h"

namespace fe
{
namespace ui
{
constexpr int W = ps5video::kWidth;
constexpr int H = ps5video::kHeight;

inline const uint32_t kBg = ps5video::Rgb(14, 14, 28);
inline const uint32_t kPanel = ps5video::Rgb(26, 24, 48);
inline const uint32_t kAccent = ps5video::Rgb(150, 125, 255);
inline const uint32_t kSel = ps5video::Rgb(78, 62, 170);
inline const uint32_t kText = ps5video::Rgb(235, 235, 245);
inline const uint32_t kDim = ps5video::Rgb(150, 150, 175);
inline const uint32_t kFolder = ps5video::Rgb(255, 210, 110);
inline const uint32_t kGood = ps5video::Rgb(120, 230, 150);
inline const uint32_t kBad = ps5video::Rgb(255, 130, 130);

double Now(); // seconds, monotonic

// The pad as menu presses: one per button press, with repeat for the directions and L1/R1.
struct Nav
{
	bool up = false, down = false, left = false, right = false;
	bool pgup = false, pgdn = false;
	bool ok = false, back = false, options = false, triangle = false, menu = false;
	bool square = false, l2 = false, r2 = false;
};

class NavReader
{
public:
	NavReader(); // buttons still held from before don't count
	Nav Read();

private:
	bool Repeat(uint32_t cur, uint32_t down, uint32_t bit, double now, int i);
	uint32_t m_prev = 0;
	double m_next[6] = {};
};

void Present();
void Header(const char* subtitle);
void Footer(const char* help);

// A box of label/value rows (pause menu, settings).
struct Row
{
	std::string label;
	std::string value; // "" = an action
	bool enabled = true;
	bool plain = false; // the value only informs: no < > around it when selected
};
void DrawOptionBox(const char* title, const std::vector<Row>& rows, int sel, bool over_game);
} // namespace ui
} // namespace fe

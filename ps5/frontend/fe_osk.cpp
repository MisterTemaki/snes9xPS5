// Snes9x PS5 frontend: the on-screen keyboard (fe_osk.h).
//
// Three pages of keys (letters, capitals, symbols) and a row of function keys, moved over with the D-pad. The
// shortcuts follow the PlayStation keyboards: Square deletes, Triangle types a space, L1/R1 move the cursor,
// L2 is Shift (for one letter), R2 the symbols page, OPTIONS is Done and Circle cancels.
//
// SPDX-License-Identifier: MIT

#include "fe_osk.h"

#include "fe_i18n.h"
#include "fe_text.h"
#include "fe_ui.h"

#include "ProsperoVideo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace fe
{
using namespace ui;

namespace
{
const char* const kLower[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"};
const char* const kUpper[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@"};
const char* const kSymbols[4] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "~`"};

enum Fn
{
	FnShift,
	FnSymbols,
	FnSpace,
	FnDelete,
	FnCancel,
	FnDone,
	FnCount
};
const int kFnWidth[FnCount] = {190, 190, 400, 190, 130, 150};

constexpr int kKeyW = 120;
constexpr int kKeyH = 92;
constexpr int kGap = 10;
constexpr int kKeysY = 360;
constexpr int kPitchY = kKeyH + kGap;
constexpr int kFnRow = 4;

struct KeyBox
{
	int x, w;
};

// The keys of one row of the page, centred.
std::vector<KeyBox> RowBoxes(int row, const char* const* page)
{
	std::vector<KeyBox> out;
	int n, total;
	if (row == kFnRow)
	{
		n = FnCount;
		total = (n - 1) * kGap;
		for (int i = 0; i < n; i++)
			total += kFnWidth[i];
	}
	else
	{
		n = int(strlen(page[row]));
		total = n * kKeyW + (n - 1) * kGap;
	}
	int x = (W - total) / 2;
	for (int i = 0; i < n; i++)
	{
		const int w = row == kFnRow ? kFnWidth[i] : kKeyW;
		out.push_back({x, w});
		x += w + kGap;
	}
	return out;
}

// ---- text, by UTF-8 character ----
size_t PrevChar(const std::string& s, size_t pos)
{
	if (pos == 0)
		return 0;
	pos--;
	while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
		pos--;
	return pos;
}

size_t NextChar(const std::string& s, size_t pos)
{
	if (pos >= s.size())
		return s.size();
	pos++;
	while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
		pos++;
	return pos;
}

size_t CountChars(const std::string& s)
{
	size_t n = 0;
	for (unsigned char c : s)
		n += (c & 0xC0) != 0x80;
	return n;
}

// What the field shows: the text, or a * for each character.
std::string Shown(const std::string& s, bool secret)
{
	return secret ? std::string(CountChars(s), '*') : s;
}

// The shown text cut to what fits `max_px` around the cursor (`cursor` is a byte index into the shown text).
void Window(const std::string& shown, size_t cursor, int scale, int max_px, std::string& left, std::string& right)
{
	size_t start = 0;
	while (start < cursor && TextWidth(shown.substr(start, cursor - start).c_str(), scale) > max_px - 40)
		start = NextChar(shown, start);
	left = shown.substr(start, cursor - start);
	right = shown.substr(cursor);
	while (!right.empty() && TextWidth((left + right).c_str(), scale) > max_px)
		right.erase(PrevChar(right, right.size()));
}

void DrawKey(const KeyBox& b, int row, const std::string& label, bool selected, uint32_t color, int scale)
{
	const int y = kKeysY + row * kPitchY;
	// a translated function key that is too wide for its key: a size smaller, then cut
	std::string text = label;
	while (scale > 2 && TextWidth(text.c_str(), scale) > b.w - 16)
		scale--;
	if (TextWidth(text.c_str(), scale) > b.w - 16)
		text = FitText(text, scale, b.w - 16);
	if (selected)
		ps5video::FillRect(b.x - 4, y - 4, b.w + 8, kKeyH + 8, kAccent);
	ps5video::FillRect(b.x, y, b.w, kKeyH, selected ? kSel : kPanel);
	DrawText(b.x + (b.w - TextWidth(text.c_str(), scale)) / 2, y + (kKeyH - kGlyphH * scale) / 2, text.c_str(), scale, color, false);
}
} // namespace

bool EditText(const std::string& title, std::string& text, bool secret, size_t max_len)
{
	NavReader nav;
	std::string work = text;
	size_t cursor = work.size();
	bool shift = false, symbols = false;
	int row = 0, col = 0;

	for (;;)
	{
		const char* const* page = symbols ? kSymbols : shift ? kUpper : kLower;
		if (row != kFnRow && col >= int(strlen(page[row])))
			col = int(strlen(page[row])) - 1; // the symbols page has a short last row

		// ---- draw
		Header(title.c_str());
		const int fx = 160, fy = 190, fw = W - 320, fh = 110;
		ps5video::FillRect(fx - 4, fy - 4, fw + 8, fh + 8, kAccent);
		ps5video::FillRect(fx, fy, fw, fh, kBg);
		const std::string shown = Shown(work, secret);
		const size_t shown_cursor = secret ? CountChars(work.substr(0, cursor)) : cursor;
		std::string left, right;
		Window(shown, shown_cursor, 5, fw - 60, left, right);
		const int tx = fx + 30, ty = fy + (fh - kGlyphH * 5) / 2;
		DrawText(tx, ty, left.c_str(), 5, kText, false);
		const int cx = tx + TextWidth(left.c_str(), 5);
		DrawText(cx, ty, right.c_str(), 5, kText, false);
		if (std::fmod(Now(), 1.0) < 0.6)
			ps5video::FillRect(cx, ty - 4, 4, kGlyphH * 5 + 8, kAccent);
		char info[64];
		snprintf(info, sizeof(info), T(CountChars(work) == 1 ? "%zu character" : "%zu characters"), CountChars(work));
		DrawText(fx, fy + fh + 18, secret ? T("Hidden as you type.") : "", 2, kDim, false);
		DrawText(fx + fw - TextWidth(info, 2), fy + fh + 18, info, 2, kDim, false);

		for (int r = 0; r <= kFnRow; r++)
		{
			const std::vector<KeyBox> boxes = RowBoxes(r, page);
			for (int c = 0; c < int(boxes.size()); c++)
			{
				const bool sel = r == row && c == col;
				if (r == kFnRow)
				{
					static const char* const kNames[FnCount] = {"Shift", "#+=", "Space", "Delete", "Cancel", "Done"};
					std::string label = c == FnSymbols ? kNames[c] : T(kNames[c]); // #+= is no word
					if (c == FnSymbols && symbols)
						label = "abc";
					const bool lit = (c == FnShift && shift) || (c == FnSymbols && symbols);
					DrawKey(boxes[c], r, label, sel, lit ? kAccent : kText, 3);
				}
				else
					DrawKey(boxes[c], r, std::string(1, page[r][c]), sel, kText, 4);
			}
		}
		Footer((std::string(icon::Cross) + " " + T("Type") + "   " + icon::Square + " " + T("Delete") + "   " + icon::Triangle + " " + T("Space") +
				"   " + icon::L2 + " " + T("Shift") + "   " + icon::R2 + " #+=   " + icon::L1 + " " + icon::R1 + " " + T("Cursor") + "   " +
				icon::Options + " " + T("Done") + "   " + icon::Circle + " " + T("Cancel"))
				   .c_str());
		Present();

		// ---- input
		const Nav n = nav.Read();
		auto move_row = [&](int to) {
			const std::vector<KeyBox> from = RowBoxes(row, page);
			const int centre = from[size_t(col)].x + from[size_t(col)].w / 2;
			const std::vector<KeyBox> boxes = RowBoxes(to, page);
			int best = 0, best_d = 1 << 30;
			for (int c = 0; c < int(boxes.size()); c++)
			{
				const int d = std::abs(boxes[size_t(c)].x + boxes[size_t(c)].w / 2 - centre);
				if (d < best_d)
				{
					best_d = d;
					best = c;
				}
			}
			row = to;
			col = best;
		};
		if (n.up)
			move_row((row + kFnRow) % (kFnRow + 1));
		if (n.down)
			move_row((row + 1) % (kFnRow + 1));
		// after the vertical move: Down+Right in one poll must wrap within the row it landed on
		const int row_len = row == kFnRow ? int(FnCount) : int(strlen(page[row]));
		if (n.left)
			col = (col + row_len - 1) % row_len;
		if (n.right)
			col = (col + 1) % row_len;
		if (n.pgup)
			cursor = PrevChar(work, cursor);
		if (n.pgdn)
			cursor = NextChar(work, cursor);
		if (n.l2)
			shift = !shift;
		if (n.r2)
			symbols = !symbols;
		if (n.square && cursor > 0)
		{
			const size_t from = PrevChar(work, cursor);
			work.erase(from, cursor - from);
			cursor = from;
		}
		if (n.triangle && work.size() < max_len)
			work.insert(cursor++, 1, ' ');
		if (n.options)
		{
			text = work;
			return true;
		}
		if (n.back)
			return false;
		if (n.ok)
		{
			if (row != kFnRow)
			{
				if (work.size() < max_len)
				{
					work.insert(cursor++, 1, page[row][col]);
					shift = false;
				}
			}
			else
				switch (col)
				{
					case FnShift: shift = !shift; break;
					case FnSymbols: symbols = !symbols; break;
					case FnSpace:
						if (work.size() < max_len)
							work.insert(cursor++, 1, ' ');
						break;
					case FnDelete:
						if (cursor > 0)
						{
							const size_t from = PrevChar(work, cursor);
							work.erase(from, cursor - from);
							cursor = from;
						}
						break;
					case FnCancel: return false;
					case FnDone:
						text = work;
						return true;
				}
		}
	}
}
} // namespace fe

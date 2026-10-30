// Snes9x PS5 frontend (2.10): the shelf's search by name -- Circle on the shelf opens the on-screen keyboard, the player
// types part of a name, and the shelf shows only the games that match (fe_shelf.cpp).
//
// The rule: the text typed, without the spaces at its ends, is looked for inside the name the shelf shows and inside the
// ROM's file name (without its folder), both folded the same way: capitals made small and accents taken off (é -> e,
// ß -> ss, Æ -> ae), so "pokemon" finds "Pokémon" and "MARIO" finds "Super Mario World". Nothing typed: every game.
// The keyboard has no accented keys, so folding the names is what lets an accented title be found at all.
//
// Header only, as fe_tabs.h: tests/test_search.cpp checks it without the app.
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_games.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
namespace search
{
namespace detail
{
// U+00C0..U+00FF and U+0100..U+017F to their base letters. '0' keeps the character (x, division sign); digits 1-5 stand
// for two letters: 1 ij, 2 oe, 3 ae, 4 th, 5 ss.
constexpr const char kLatin1[] = "aaaaaa3ceeeeiiiidnooooo0ouuuuy45aaaaaa3ceeeeiiiidnooooo0ouuuuy4y";
constexpr const char kLatinA[] = "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiii11jjkkkllllllllllnnnnnnnnnoooooo22"
								 "rrrrrrsssssssstttttuuuuuuuuuuuuuwwyyyzzzzzzs";
static_assert(sizeof(kLatin1) == 64 + 1, "U+00C0..U+00FF");
static_assert(sizeof(kLatinA) == 128 + 1, "U+0100..U+017F");

inline void PutBase(std::string& out, char c)
{
	switch (c)
	{
		case '1': out += "ij"; break;
		case '2': out += "oe"; break;
		case '3': out += "ae"; break;
		case '4': out += "th"; break;
		case '5': out += "ss"; break;
		default: out += c; break;
	}
}

constexpr uint32_t kNotUtf8 = 0xFFFFFFFFu;

// One UTF-8 character at s[i]: its code point and length; a byte that starts no valid character: kNotUtf8, n = 1 (the
// byte is kept as it is, never read as Latin-1).
inline uint32_t Decode(const std::string& s, size_t i, size_t& n)
{
	const unsigned char c = static_cast<unsigned char>(s[i]);
	n = 1;
	if (c < 0x80)
		return c;
	int extra;
	if (c >= 0xC2 && c <= 0xDF)
		extra = 1;
	else if (c >= 0xE0 && c <= 0xEF)
		extra = 2;
	else if (c >= 0xF0 && c <= 0xF4)
		extra = 3;
	else
		return kNotUtf8;
	if (i + size_t(extra) >= s.size())
		return kNotUtf8;
	uint32_t cp = c & (0x3F >> extra);
	for (int k = 1; k <= extra; k++)
	{
		const unsigned char t = static_cast<unsigned char>(s[i + size_t(k)]);
		if ((t & 0xC0) != 0x80)
			return kNotUtf8;
		cp = (cp << 6) | (t & 0x3F);
	}
	n = size_t(extra) + 1;
	return cp;
}
} // namespace detail

// The text as it is compared: ASCII capitals small, Latin accents off (above), the typographic apostrophes as ', every
// other character as it is.
inline std::string Fold(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();)
	{
		size_t n = 1;
		const uint32_t cp = detail::Decode(s, i, n);
		if (cp >= 'A' && cp <= 'Z')
			out += char(cp - 'A' + 'a');
		else if (cp >= 0xC0 && cp <= 0xFF && detail::kLatin1[cp - 0xC0] != '0')
			detail::PutBase(out, detail::kLatin1[cp - 0xC0]);
		else if (cp >= 0x100 && cp <= 0x17F)
			detail::PutBase(out, detail::kLatinA[cp - 0x100]);
		else if (cp == 0x2018 || cp == 0x2019)
			out += '\'';
		else
			out.append(s, i, n);
		i += n;
	}
	return out;
}

// The text typed, ready to look for: folded, without the spaces at its ends. "" means no search (every game).
inline std::string Query(const std::string& typed)
{
	const size_t a = typed.find_first_not_of(' ');
	if (a == std::string::npos)
		return "";
	const size_t b = typed.find_last_not_of(' ');
	return Fold(typed.substr(a, b - a + 1));
}

// The ROM's file name as the player sees it in a file list: without its folder, with its extension.
inline std::string FileName(const GameInfo& g)
{
	const size_t slash = g.path.find_last_of('/');
	return slash == std::string::npos ? g.path : g.path.substr(slash + 1);
}

// A game matches a query (made by Query): it is found in the shelf's name or in the file name.
inline bool Matches(const GameInfo& g, const std::string& query)
{
	if (query.empty())
		return true;
	return Fold(g.title).find(query) != std::string::npos || Fold(FileName(g)).find(query) != std::string::npos;
}

// The shelf's view (indices into games, in shelf order: a tab, fe_tabs.h) cut to the games that match what was typed,
// in the same order. Nothing typed: the view as it is.
inline std::vector<int> Filter(const std::vector<GameInfo>& games, const std::vector<int>& view, const std::string& typed)
{
	const std::string q = Query(typed);
	if (q.empty())
		return view;
	std::vector<int> out;
	for (int i : view)
		if (i >= 0 && size_t(i) < games.size() && Matches(games[size_t(i)], q))
			out.push_back(i);
	return out;
}

// Where a game is in a view: its position, or -1 when the view doesn't show it.
inline int PositionOf(const std::vector<int>& view, int game)
{
	for (size_t p = 0; p < view.size(); p++)
		if (view[p] == game)
			return int(p);
	return -1;
}

// The order the cover service loads covers in around the selection (fe_covers.h): the shelf's view, so the covers that
// stand next to the selection are loaded and kept, wherever they are in the library. Up to 2.9 that was the library's
// own order (a tab's games are side by side in it, fe_tabs.h); with a search the games beside the selection can be far
// apart there. Not set (Clear): the library's order, as before.
class ViewOrder
{
public:
	void Clear(size_t count)
	{
		m_count = count;
		m_order.clear();
		m_rank.clear();
	}
	void Set(const std::vector<int>& order, size_t count)
	{
		m_count = count;
		m_order.clear();
		m_rank.assign(count, -1);
		for (int i : order)
			if (i >= 0 && size_t(i) < count && m_rank[size_t(i)] < 0)
			{
				m_rank[size_t(i)] = int(m_order.size());
				m_order.push_back(i);
			}
	}
	bool IsSet() const { return !m_rank.empty(); }
	// The games within `radius` places of game `focus`, nearest first (the focus, then left before right at each
	// distance). A focus the order doesn't hold: none.
	std::vector<int> Around(int focus, int radius) const
	{
		std::vector<int> out;
		const long at = Place(focus);
		if (at < 0)
			return out;
		const long n = IsSet() ? long(m_order.size()) : long(m_count);
		for (long d = 0; d <= radius; d++)
			for (int sgn = -1; sgn <= 1; sgn += 2)
			{
				if (d == 0 && sgn > 0)
					continue;
				const long p = at + sgn * d;
				if (p >= 0 && p < n)
					out.push_back(IsSet() ? m_order[size_t(p)] : int(p));
			}
		return out;
	}
	// Game `game` stands within `radius` places of game `focus` (false when the order holds either not).
	bool Near(int game, int focus, int radius) const
	{
		const long a = Place(game), b = Place(focus);
		return a >= 0 && b >= 0 && (a > b ? a - b : b - a) <= radius;
	}

private:
	long Place(int game) const
	{
		if (game < 0 || size_t(game) >= m_count)
			return -1;
		return IsSet() ? m_rank[size_t(game)] : long(game);
	}
	size_t m_count = 0;
	std::vector<int> m_order; // the games, in shelf order
	std::vector<int> m_rank;  // game -> its place in m_order, -1 when not there; empty: the library's order
};
} // namespace search
} // namespace fe

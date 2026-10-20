// Snes9x PS5 frontend: the shelf's tabs -- Internal, USB and Network, as PSFlycast's library has -- and which game
// stands on which. The shelf keeps one game list; a tab is the indices of its games (docs: ADR-004 in .context).
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_games.h"

#include <algorithm>
#include <vector>

namespace fe
{
namespace tabs
{
enum Tab
{
	Internal = 0, // the console's drive (/data/snes9x/roms and the like)
	Usb = 1,      // /mnt/... ; shown only while a game is on a USB drive
	Network = 2,  // a share; always shown, so a new player finds how to add one
	kCount = 3,
};

inline const char* Name(int t)
{
	return t == Usb ? "USB" : t == Network ? "Network" : "Internal";
}

inline int Of(const GameInfo& g)
{
	return g.on_network ? Network : g.on_usb ? Usb : Internal;
}

inline void Count(const std::vector<GameInfo>& games, int counts[kCount])
{
	for (int t = 0; t < kCount; t++)
		counts[t] = 0;
	for (const GameInfo& g : games)
		counts[Of(g)]++;
}

inline bool Shown(const int counts[kCount], int t)
{
	return t != Usb || counts[Usb] > 0;
}

// The tab to show when `want` was asked for. `keep_empty`: stay on `want` even with no games (the player chose it, or
// it is the Network tab while a share is still being reached). Otherwise an empty or hidden tab hands over to the
// first tab that has games. With no games anywhere: Network when a share is set up (its tab says what the share
// answered, which "no games found" would not), else `want`, or Internal when that is hidden.
inline int Resolve(const int counts[kCount], int want, bool keep_empty, bool share_set_up)
{
	if (want < 0 || want >= kCount)
		want = Internal;
	const bool shown = Shown(counts, want);
	if (shown && (counts[want] > 0 || keep_empty))
		return want;
	for (int t = 0; t < kCount; t++)
		if (counts[t] > 0)
			return t;
	if (share_set_up)
		return Network;
	return shown ? want : int(Internal);
}

// The next shown tab in a direction (+1 / -1), round and round.
inline int Step(const int counts[kCount], int from, int dir)
{
	int t = from;
	for (int i = 0; i < kCount; i++)
	{
		t = (t + dir + kCount) % kCount;
		if (Shown(counts, t))
			return t;
	}
	return from;
}

// The shelf list in tab order (Internal, USB, Network), each tab by title as ScanGames sorted it: a tab's games are then
// side by side, so the cover service's window around the selection (fe_covers.h) covers the tab's neighbours.
inline void OrderByTab(std::vector<GameInfo>& games)
{
	std::stable_sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) { return Of(a) < Of(b); });
}

// Indices into `games` of the games on tab t, in list order.
inline std::vector<int> View(const std::vector<GameInfo>& games, int t)
{
	std::vector<int> v;
	for (size_t i = 0; i < games.size(); i++)
		if (Of(games[i]) == t)
			v.push_back(int(i));
	return v;
}
} // namespace tabs
} // namespace fe

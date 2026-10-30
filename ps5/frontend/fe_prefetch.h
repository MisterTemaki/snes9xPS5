// Snes9x PS5: the cover prefetch, as PS5SX2 does it (its fe_ps5.cpp, orbis_frontend_prefetch_covers).
//
// 2.2: when the helper downloads the covers itself (fe_coverworker.h), none of this runs: the app asks for the
// list (which tells it so) and starts at once. What follows is for a helper that doesn't (etaHEN's...).
//
// On the console the app downloads covers in one step at the very start, before it asks the helper for
// /data, with a 30 s budget; after that the shelf downloads nothing (PS5SX2: "the rest came from the
// prefetch, before the jailbreak"). The 1.6.1 console log showed why: on the shelf, after that request, every
// HTTPS GET failed at once (-1 in 23 ms) although the network was up.
//
// Before /data is visible the app can't read its game list, so the list of covers to fetch comes from the
// helper: covers/wanted.txt, which the app writes after each scan (fe_covers.h, MissingCovers). The covers are
// kept in memory and written to /data/snes9x/covers once the app can see it. When a scan finds covers that
// this start didn't try (new games), the app writes the list and starts itself again so the prefetch gets
// them, as PS5SX2 re-executes its own eboot (main-boot.cpp, ReturnForCovers).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "fe_games.h"

namespace fe
{
struct PrefetchItem
{
	std::string file; // in /data/snes9x/covers
	std::string url;
	int status = -1; // HTTP status; 200 = data is the image
	std::vector<uint8_t> data;
};

struct PrefetchResult
{
	bool helper_ok = false; // the helper answered (so a restart can prefetch again)
	bool background = false; // the helper downloads the covers itself, while the app runs (fe_coverworker.h)
	std::vector<PrefetchItem> items;
	std::set<std::string> attempted; // files tried in this start
	double seconds = 0;
};

// Before asking for /data: get the wanted list from the helper and download it (budget in seconds).
PrefetchResult PrefetchCovers(double budget_s);

// Once /data is visible: write what came, and remember the 404s (.missing) as the shelf does.
void SavePrefetched(const PrefetchResult& result);

// True when the console reports an IP address (the network is up).
bool NetConnected();

// Keeps this start's prefetch outcome for CoversRestartIfNeeded.
void RememberPrefetch(const PrefetchResult& result);
// The helper downloads the covers in the background (this start's prefetch said so).
bool BackgroundCovers();

// The shelf, after a scan (console only): writes covers/wanted.txt for `games`. When it holds covers this
// start didn't try (or `force`: Square on a game), the network is up and a helper answered, shows `text` with
// `show` and starts the app again (sceSystemServiceLoadExec, as PS5SX2 re-executes its eboot), so the
// prefetch fetches them. Returns only when it didn't restart.
void CoversRestartIfNeeded(const std::vector<GameInfo>& games, bool downloads_on, bool force,
	void (*show)(const char* text));

// 2.10, the check in the background (fe_shelf.cpp; console only): writes covers/wanted.txt for `games` and never
// restarts the app (a restart would take the player off the shelf they are using): a helper that downloads in the
// background fetches the new games' covers now; otherwise the next start's prefetch does. Any thread.
void CoversWantedLater(const std::vector<GameInfo>& games, bool downloads_on);
} // namespace fe

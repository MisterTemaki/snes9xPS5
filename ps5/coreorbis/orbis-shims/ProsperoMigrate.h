// Moving the app to a new title ID (ProsperoMigrate.cpp).
//
// A title ID is the app's identity on the console: /data/homebrew/<ID>, /user/appmeta/<ID> and the home-screen
// registration. Our first IDs (PPSA99009..PPSA99012) turned out to be taken in the homebrew catalog
// (homebrew.page) by other apps, so the app moved to its own ID (MIG_NEW_TITLE, ProsperoMigrateConfig.h). The
// payload and the app bring an install made under an old ID over to the new one:
//
//   1. the old folder, /data/homebrew/<old ID>, if it is ours (its param.json carries our content ID), is renamed
//      to /data/homebrew/<new ID> (or removed, when the new one is already there); the installer then brings
//      the files up to date as usual. A folder of another app under that ID is never touched.
//   2. the old home-screen registration is removed through ShadowMountPlus's API (127.0.0.1:10101,
//      /api/v1/games/uninstall), only after /api/v1/games/info says that title's content ID is ours; then a
//      rescan is asked for, so the new ID shows up at once. ShadowMountPlus refuses while a game or app is in
//      front (EBUSY), so what can't be done at once is remembered (<root>/migrate/<old ID>.unregister) and the
//      helper tries again every 30 seconds.
//
// The data folder (/data/<app>/: games, saves, states, covers, settings) doesn't depend on the title ID and is
// not moved.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace migrate
{
struct Result
{
	int moved = 0;        // old folders renamed to the new ID
	int removed = 0;      // old folders removed (the new one was already there)
	int unregistered = 0; // old home-screen registrations removed through ShadowMountPlus
	int pending = 0;      // registrations still to remove (ShadowMountPlus busy or not reachable)
	bool no_api = false;  // ShadowMountPlus's API didn't answer
	std::string old_ids;  // the old IDs that were found, "PPSA99011"
};

// Step 1 (the folders) and, with network = true, one try at step 2. The installer calls it before installing;
// the app calls it after it leaves the sandbox (network = false: ShadowMountPlus refuses while the app runs).
Result Run(bool network);

// Step 2 again every 30 s while something is pending (the helper's thread); returns at once otherwise.
void StartRetryThread();

// Test hooks: the old IDs and our content ID label.
const char* const* OldTitleIds();
std::string ContentIdFor(const char* title_id);
} // namespace migrate

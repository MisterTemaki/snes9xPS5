// In-console Libretro cheat downloads through the unsandboxed HTTPS helper.
// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>

namespace fe {
struct CheatRequestGame {
    std::string basename; // ROM stem, used for /data/snes9x/cheats/<stem>.cht
    std::string nointro; // official title used for cheat database matching
};
struct CheatDownloadStatus {
    int done = 0, total = 0, found = 0, failed = 0;
    std::string state = "idle"; // queued, downloading, completed, offline, failed, idle
    bool valid = false;
};
// Queue work for the background helper. Never call HTTPS from the app after jailbreak.
bool RequestGameCheats(const CheatRequestGame& game);
bool RequestLibraryCheats(const std::vector<CheatRequestGame>& games);
bool RequestAllSnesCheats();
CheatDownloadStatus ReadCheatDownloadStatus();
std::string CheatDownloadPath(const std::string& basename);
// If bulk collection was downloaded, put the matching file at the expected ROM filename.
bool InstallCachedCheat(const CheatRequestGame& game);
// A complete manifest of the Libretro SNES .cht repository is embedded in the app and helper.
size_t CheatDatabaseCount();
std::string BestCheatSourceFile(const CheatRequestGame& game);
// Called only by the helper process.
void StartCheatDownloadWorker();
} // namespace fe

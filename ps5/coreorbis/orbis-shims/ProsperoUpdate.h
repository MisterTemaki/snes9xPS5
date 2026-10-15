// Updating the app from its GitHub releases (ProsperoUpdate.cpp), as Porpoise does (github.com/elripalda/Porpoise,
// src/porpoise_update.cpp), and as this port's installer payload already installs: files replaced one by one,
// eboot.bin last.
//
//   1. Check: GitHub's answer for the newest release (api.github.com/repos/<repo>/releases/latest) names the app's
//      zip (the asset ending in "-<TITLE_ID>.zip", the same zip the homebrew catalog lists), its size and SHA-256.
//      Newer than this version (by the tag, "v1.8" > "1.7"): it can be installed.
//   2. Install: the zip is downloaded (into memory), its size and SHA-256 checked; it holds the app folder
//      (<TITLE_ID>/ ...) stored without compression and a manifest (<TITLE_ID>/manifest.sha256: "<sha-256> *<path>"
//      lines); every file is checked against the manifest. Only then is anything written: each file beside the old
//      one as <name>.new, then all renamed over the old ones, eboot.bin and the manifest last; files the old
//      manifest had and the new one hasn't are removed; the home screen's copy of the art (/user/appmeta/<ID>) is
//      brought up to date. If anything fails before the renames, the .new files go and nothing has changed.
//   3. The running app is the old build: it has to be opened again.
//
// The data folder (/data/<app>/: games, saves, settings) isn't touched. Everything runs on a thread of its own;
// Status() says how far it is. The transport (Fetch) is the caller's: the emulators' helper uses its own HTTPS
// (Mbed TLS), Media Player Classic PS5 FFmpeg's.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace update
{
struct Config
{
	std::string repo;        // "MisterTemaki/genplusgxPS5"
	std::string title_id;    // "PPSA99921"
	std::string version;     // this build's, "1.7"
	std::string app_name;    // "Genesis Plus GX PS5"
	std::string app_dir;     // /data/homebrew/<title_id>
	std::string appmeta_dir; // /user/appmeta/<title_id>
	std::string api_url;     // "" -> https://api.github.com/repos/<repo>/releases/latest
};

// GET url into out (at most max bytes); progress (when not null) follows the bytes read. The HTTP status, or -1.
using Fetch = std::function<int(const std::string& url, std::vector<uint8_t>& out, size_t max,
	std::atomic<size_t>* progress)>;

enum class Phase
{
	Idle,
	Checking,
	UpToDate,
	Available,   // latest/size/notes say what
	Downloading, // done/total in bytes
	Installing,  // done/total in files
	Done,        // installed: the app must be opened again
	Failed,      // error says why; nothing was changed
};

struct Status
{
	Phase phase = Phase::Idle;
	std::string latest; // "1.8"
	uint64_t size = 0;  // the zip's
	uint64_t done = 0, total = 0;
	std::string error;
};

void Init(const Config& config, Fetch fetch);
// The version installed (the app says which when it asks: a helper of an older build may be the one running).
void SetCurrentVersion(const std::string& version);
const Config& CurrentConfig();
// Start a check / an install of the release the last check found; false when one is running (or nothing to install).
// notify: a check of the app's own (at start, once a day): on_newer is told when it finds a newer version.
bool StartCheck(bool notify = false);
void SetOnNewer(std::function<void(const std::string& version)> on_newer);
bool StartInstall();
Status GetStatus();
// Back to Idle (after the screen showed a result), unless something runs.
void Acknowledge();

// "phase|latest|size|done|total|error" and back, for the app <-> helper requests.
std::string Encode(const Status& s);
bool Decode(const std::string& text, Status& s);
const char* PhaseName(Phase p);

// ---- the parts, for the tests -------------------------------------------------------------------------------------
struct Release
{
	std::string tag, version; // "v1.8", "1.8"
	std::string zip_url, zip_name;
	uint64_t size = 0;
	std::string sha256; // hex, from the asset's digest ("" when GitHub gave none)
};
// One release object of GitHub's answer; the asset ending in "-<title_id>.zip".
bool ParseRelease(const std::string& json, const std::string& title_id, Release& out, std::string* why = nullptr);
// "1.10" > "1.9"; a leading "v" is ignored. <0, 0, >0.
int CompareVersions(const std::string& a, const std::string& b);
std::string Sha256Hex(const uint8_t* data, size_t size);
// Checks and installs the zip into config.app_dir (see above). "" when done, else why not (nothing changed then,
// unless the error says otherwise). files_done follows the files written.
std::string InstallZip(const std::vector<uint8_t>& zip, const std::string& expect_sha256, const Config& config,
	std::atomic<size_t>* files_done = nullptr, std::atomic<size_t>* files_total = nullptr);
} // namespace update

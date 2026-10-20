// Snes9x PS5: the dashboard app asks to be let out of its sandbox (ProsperoJailbreak.cpp), and the payload
// that does it (ProsperoHelper.cpp).
//
// The app (eboot.bin, a native title like PS5SX2's) starts inside the console's sandbox: no /data, no USB
// drives. PS5SX2 asks a jailbreak daemon for full rights when it starts (its main-boot.cpp, orbis_try_jailbreak:
// the PS5SX2 Helper or etaHEN, 127.0.0.1:9028/9069, a 0xDEADBEEF command 5 with the app's pid). Snes9x does the
// same, first with its own helper -- Snes9xPS5.elf stays running as one after it installs the app -- then with
// those two. When nobody answers, the app sends the helper built into it (Snes9xPS5-helper.elf) to the
// console's ELF loader (127.0.0.1:9021) and asks again.
//
// The helper only lets out the Snes9x PS5 title (PPSA99922 since 2.6; PPSA99009 before): it gives the process root's folder view
// (/data, /mnt/usbN) and uid 0. It never touches another title.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace jailbreak
{
// Snes9x PS5's helper (SNES9X_HELPER_PORT on the host). Up to 2.1 the helper listened on 9075, 2.2's (the first
// to download the covers) on 9080; 2.3's, with its own HTTPS, listens on 9083, so an older helper still running
// (until the console restarts) is left alone and the app starts its own.
// 2.6 moved the app to a new title ID (PPSA99922): older helpers only let PPSA99009 out, so 2.6's listens on 9089.
// The helper that also downloads the borders (fe_coverworker.h) listens on 9090: an older one still running would take the
// request and never fetch a border, so the app starts its own.
constexpr int kHelperPort = 9090;
// the ELF loader the helper is sent to when it isn't running (SNES9X_ELFLDR_PORT on the host)
constexpr int kElfLoaderPort = 9021;

// The request, PS5SX2's/etaHEN's layout (cmd 5 = jailbreak the process pid); ret is 0 when done.
struct Request
{
	uint32_t magic;
	int32_t cmd;
	int32_t pid;
	int32_t ret;
	char msg1[0x500];
	char msg2[0x500];
};
constexpr uint32_t kMagic = 0xDEADBEEF;
constexpr int32_t kCmdJailbreak = 5;
// Snes9x's own: the helper answers with covers/wanted.txt (ret = its length, then the bytes), so the app can
// prefetch covers before it asks for /data, as PS5SX2 does (fe_prefetch.h).
constexpr int32_t kCmdWantedCovers = 6;
// In the answer's msg2: the helper downloads the wanted covers itself, in the background (fe_coverworker.h).
constexpr const char* kBackgroundCovers = "covers: background";
constexpr int32_t kMaxWantedBytes = 1 << 20;
constexpr int32_t kRetUntouched = -1337;
// Our own (2.6): the app's updates (ProsperoUpdate.h), done by the helper (its own HTTPS; it writes the app folder).
// msg1 asks "status", "check", "autocheck" (a notification when there is a newer version), "install" or "ack";
// the answer's msg1 is the updater's status (update::Encode).
constexpr int32_t kCmdUpdate = 7;

// The app side: ask for this process. Returns true when a helper answered yes; `how` names it.
// Every step goes to OrbisLog (kept in memory until boot.log can be opened).
bool RequestForSelf(std::string& how);

// The app side, before the jailbreak: the wanted-covers list from the helper (starting the helper through the
// ELF loader if none answers). False when no helper could be reached; `text` may be empty (nothing wanted).
// `background`: the helper downloads them itself (the app downloads nothing at start).
bool FetchWantedCovers(std::string& text, bool* background = nullptr);

// The app side: one updater request to our own helper (kCmdUpdate). False when no helper of ours answered.
bool UpdateRequest(const char* what, std::string& reply);

// The helper ELF built into the app (helper_data.cpp in the native eboot; nothing elsewhere).
struct Blob
{
	const unsigned char* data;
	size_t size;
};
Blob EmbeddedHelper();

// The payload side: serve requests on 127.0.0.1:kHelperPort until the process ends. Returns false at once
// when the port is taken (a helper is already running). `on_ready` runs once the port is bound.
bool ServeHelper(void (*on_ready)());

// Lets one process out (payload only; the host build fakes it). False with a reason when it refuses.
bool JailbreakProcess(int pid, std::string& why);
} // namespace jailbreak

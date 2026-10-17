// Snes9x PS5 netfs: network shares as folders. The frontend's view of it.
//
// Shares come from shares.txt (netfs_share.h). Each appears under a mount point such as
// /smb/192.168.1.50/games/snes; every libc file call on a path under a mount point is answered by netfs (the
// linker's --wrap list, netfs_wrap.cpp), so the Snes9x core and minizip need no change. Paths that are not
// under a mount point go to the real call.
//
// Threads: netfs_load* and netfs_stop are called while no file or folder of netfs is open and no other netfs
// call runs (at start, or between tests). Everything else may be called from any thread.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "netfs_backend.h"
#include "netfs_share.h"

enum class NetfsState
{
	Idle,       // loaded, not mounted yet
	Connecting, // the mount job is running
	Ready,
	Failed,     // the mount failed; netfs_last_error() says why
	Dead,       // was ready; the watchdog cut it (a request ran over its limit)
};

struct NetfsShareInfo
{
	std::string mount_point; // /smb/host/share/folder
	std::string label;       // the redacted address, safe to show
	NetfsState state = NetfsState::Idle;
	std::string error;       // one plain sentence, empty when none
};

// ---- setup ----

// Reads the shares file. When it does not exist, writes the commented template (atomically, through .part) and
// loads nothing. A file the app did not write is never rewritten. Returns the number of shares loaded; the
// notices (netfs_notices) say what was skipped.
int netfs_load(const std::string& shares_file);
// The same from text (host tests).
int netfs_load_text(const std::string& text);
// Mounts every share on its own worker thread and starts the watchdog. Returns at once.
void netfs_start();
// Mounts every share one after the other on the calling thread (host tests); no watchdog thread.
void netfs_mount_all_blocking();
// Aborts every connection, joins the workers and forgets the shares. Files and folders still open fail with EIO.
void netfs_stop();

// Stops everything and loads the file netfs_load was given again, then mounts. Returns the number of shares (0
// when netfs_load was never called). The Network screen calls it after it changed the file.
int netfs_reload();

// ---- what the frontend asks ----

// The virtual folders to scan for ROMs: the shares that are Ready. In memory only: no stat, no network.
std::vector<std::string> netfs_mount_points();
std::vector<NetfsShareInfo> netfs_shares();
// True when every share is Ready, Failed or Dead (none is waiting to mount or connecting). True with no shares.
bool netfs_mount_finished();
// The newest failure sentence of any share ("" if none). Never has a password in it.
std::string netfs_last_error();
// What happened since the last call, oldest first: "Connected to ...", the sentence of a failed mount, "The
// connection to ... was lost." For the frontend to show as notices; never a password. At most 32 are kept.
std::vector<std::string> netfs_take_events();
// Problems found in shares.txt: line numbers and reasons, never a line's text.
std::vector<std::string> netfs_notices();
// True for /smb/... and /nfs/... paths (whether or not a share is mounted there).
bool netfs_is_network_path(const char* path);

// The frontend calls this with true after LoadGame returns and with false in CloseGame. While it is true nothing
// in netfs waits on the network for the calling thread: stat and open answer from memory, a .pcm read that is not
// in memory is silence, and no connection call is made on the emulation thread. The one exception is a miss
// inside an MSU-1 data file (<rom>.msu, msu1.rom) that did not fit the prefetch budget; see netfs_cache.h.
void netfs_set_realtime(bool on);
bool netfs_realtime();

// ---- the Network screen: editing the shares file ----

// An address line of the shares file, read again at each call (the file may have been edited over FTP).
struct NetfsFileShare
{
	int line = 0;           // 1-based line in the file
	netfs::ShareConfig cfg; // WITH the password: for the edit form only, never for display or a log
	std::string mount_point;
	bool used = true;       // false: the loader ignores the line (same folder as an earlier line, or more than 8)
	std::string note;       // why it is ignored
};
struct NetfsFileView
{
	std::string error; // one sentence when the file cannot be used ("" = fine)
	std::vector<NetfsFileShare> shares;
	int invalid = 0;   // lines that are not valid addresses (netfs_notices says which; they are left alone)
};
NetfsFileView netfs_file_shares();
// Saves `cfg` over the share that was shown at `line` (its mount point, `old_mount_point`, must still be there),
// or as a new line at the end when line <= 0. Every other byte of the file stays. Does not reload. Returns ""
// or one sentence about why not (the file changed meanwhile, the folder is used by another share, 8 are in
// use, the file cannot be written). Never has a password in it.
std::string netfs_file_save_share(int line, const std::string& old_mount_point, const netfs::ShareConfig& cfg);
std::string netfs_file_remove_share(int line, const std::string& old_mount_point);

// A one-off check of a share that is not necessarily in the file: connects on its own thread, lists the
// configured folder, disconnects. Call Poll() every frame; it enforces kNetfsConnectLimitMs.
struct NetfsTestResult
{
	bool ok = false;
	std::string text; // one or two sentences for the player; never a password
};
class NetfsConnectionTest
{
public:
	explicit NetfsConnectionTest(const netfs::ShareConfig& cfg);
	~NetfsConnectionTest(); // aborts a test that still runs and waits for its thread
	NetfsConnectionTest(const NetfsConnectionTest&) = delete;
	NetfsConnectionTest& operator=(const NetfsConnectionTest&) = delete;
	bool Poll();   // true once the result is there
	void Cancel(); // stop now; Result() then holds nothing
	const NetfsTestResult& Result() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};

// ---- cache ----

// Before LoadGame, for a ROM on a share: lists the ROM's folder, fetches the MSU-1 data files whole (up to 32 MiB)
// and the first 128 KiB of up to 64 tracks. Waits for the data, and for the track heads up to head_wait_ms.
// Does nothing for other paths and for a share that is not ready.
void netfs_prepare_game(const std::string& rom_path, int head_wait_ms = 3000);
// After CloseGame: lets go of what netfs_prepare_game kept.
void netfs_end_game();
// Forget the cached folder listings (the frontend's rescan). They expire after 60 s on their own.
void netfs_rescan();

struct NetfsStats
{
	uint64_t blocks_fetched = 0;
	uint64_t bytes_fetched = 0;
	uint64_t underruns = 0;   // .pcm reads answered with silence
	uint64_t data_stalls = 0; // MSU-1 data reads that waited for the network
};
NetfsStats netfs_stats();
// Where netfs's own lines go (one line per call, safe to show or log). Empty: nowhere.
void netfs_set_log(std::function<void(const std::string&)> log);

// ---- test and backend hooks ----

using NetfsBackendFactory = std::function<std::unique_ptr<netfs::Backend>()>;
// Replaces the factory of a scheme (the default calls netfs::MakeSmbBackend / MakeNfsBackend). Empty restores it.
void netfs_set_backend_factory(netfs::Scheme scheme, NetfsBackendFactory factory);
// The watchdog's clock in milliseconds. Empty restores the steady clock.
void netfs_set_clock(std::function<int64_t()> now_ms);
// One watchdog pass (the watchdog thread calls it every 250 ms): a connect running over 15 s, or any other
// request over 5 s, aborts that share's connection and marks it Dead (or Failed, while connecting).
void netfs_watchdog_tick();
// Returns when no share has read-ahead or handle-close work queued or running (deterministic waits in tests).
void netfs_cache_wait_idle();
constexpr int64_t kNetfsRequestLimitMs = 5000;
constexpr int64_t kNetfsConnectLimitMs = 15000;

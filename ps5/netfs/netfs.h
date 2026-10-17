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

// ---- what the frontend asks ----

// The virtual folders to scan for ROMs: the shares that are Ready. In memory only: no stat, no network.
std::vector<std::string> netfs_mount_points();
std::vector<NetfsShareInfo> netfs_shares();
// True when every share is Ready, Failed or Dead (none is waiting to mount or connecting). True with no shares.
bool netfs_mount_finished();
// The newest failure sentence of any share ("" if none). Never has a password in it.
std::string netfs_last_error();
// Problems found in shares.txt: line numbers and reasons, never a line's text.
std::vector<std::string> netfs_notices();
// True for /smb/... and /nfs/... paths (whether or not a share is mounted there).
bool netfs_is_network_path(const char* path);

// The frontend calls this with true after LoadGame returns and with false in CloseGame. The read-ahead cache
// (a later phase) uses it to decide what may wait on the network. It has no other effect yet.
void netfs_set_realtime(bool on);
bool netfs_realtime();

// ---- test and backend hooks ----

using NetfsBackendFactory = std::function<std::unique_ptr<netfs::Backend>()>;
// Replaces the factory of a scheme (the default calls netfs::MakeSmbBackend / MakeNfsBackend). Empty restores it.
void netfs_set_backend_factory(netfs::Scheme scheme, NetfsBackendFactory factory);
// The watchdog's clock in milliseconds. Empty restores the steady clock.
void netfs_set_clock(std::function<int64_t()> now_ms);
// One watchdog pass (the watchdog thread calls it every 250 ms): a connect running over 15 s, or any other
// request over 5 s, aborts that share's connection and marks it Dead (or Failed, while connecting).
void netfs_watchdog_tick();
constexpr int64_t kNetfsRequestLimitMs = 5000;
constexpr int64_t kNetfsConnectLimitMs = 15000;

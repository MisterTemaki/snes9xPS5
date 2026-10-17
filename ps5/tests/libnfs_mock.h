// An in-memory stand-in for libnfs, for the NFS backend's unit test (nfs_backend_test.cpp). It implements
// exactly the libnfs calls backend_nfs.cpp makes. Calls complete when the backend services the context
// (nfs_service), so hangs, refusals and dropped connections are scripted without a network or a sleep.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mocknfs
{
struct Node
{
	bool dir = false;
	std::string data;       // file contents
	int64_t mtime = 0;
	std::string link;       // non-empty: a symlink to this absolute server path
	bool no_attrs = false;  // READDIRPLUS gave no attributes for this entry
};

struct Server
{
	std::map<std::string, Node> nodes; // absolute server path ("/export/snes/a.sfc") -> node
	std::vector<std::string> exports;  // what mountd lists
	std::set<std::string> accepts;     // paths a MOUNT is accepted for; empty = every existing folder
	bool refuse = false;               // the connection is refused
	bool hang = false;                 // no call ever completes
	bool exports_denied = false;       // MOUNT of an existing path fails with EACCES
	int short_read = 0;                // > 0: a read returns at most this many bytes
	size_t rtmax = 1u << 20;           // the server's largest READ (FSINFO)
	int fail_service_at = -1;          // the n-th nfs_service call reports a dead socket (-1)

	// Observations
	std::vector<std::string> mount_paths;
	int version_set = 0;
	int uid_set = -1, gid_set = -1, nfsport_set = 0, mountport_set = 0;
	std::string mount_host;
	int services = 0;          // nfs_service calls
	int calls_after_death = 0; // libnfs calls made after a service failure (the backend must make none)
	int last_read_len = 0;
	int max_read_len = 0;   // the largest single READ asked for
	int in_flight = 0;      // READs queued and not yet completed
	int max_in_flight = 0;
	int leaked_handles = 0; // files still open when a context was destroyed (libnfs does not free them)
	std::function<void()> on_service; // runs at the start of every nfs_service
};

Server& S();
void Reset();
void AddDir(const std::string& path);
void AddFile(const std::string& path, const std::string& data, int64_t mtime = 1700000000);
} // namespace mocknfs

// Snes9x PS5 netfs: the shares and their connections. Internal; the frontend uses netfs.h.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "netfs.h"

namespace netfs
{
class Cache;

// One configured share and its one connection. All backend calls go through here, under the connection lock,
// timed for the watchdog.
class Share
{
public:
	explicit Share(const ShareConfig& c);
	~Share();

	const ShareConfig cfg;
	const std::string mount_point;
	const std::string label; // netfs_redact(cfg)
	const std::vector<std::string> segs; // the mount point's path segments

	NetfsState State() const { return m_state.load(); }
	std::string Error() const;
	// errno for a call on a share that is not ready: EIO once dead, ENOTCONN before and after a failed mount.
	int StateErrno() const;

	// The mount job: creates the backend and connects (blocking; a worker thread, or the test's thread).
	void Mount();
	// The watchdog's verdict (connect over its limit: Failed; a request over its limit: Dead) and netfs_stop.
	void Kill(bool stopping);
	// Milliseconds on the netfs clock when the running request began; 0 when none runs.
	int64_t RequestStart() const { return m_req_start.load(); }
	// Listings, file blocks and the read-ahead worker for this share (netfs_cache.h). Always there.
	Cache& cache() { return *m_cache; }

	Status Stat(const std::string& rel, Attr& out);
	Status List(const std::string& rel, std::vector<DirEntry>& out);
	Status Open(const std::string& rel, Handle& out);
	// Reads len bytes at off, asking again after short reads. Returns the bytes read (fewer only at the end of
	// the file or after an error that came after some bytes), or -errno.
	int64_t Read(Handle h, uint64_t off, void* buf, size_t len);
	void CloseHandle(Handle h);

private:
	friend class Timed;
	int LockConn(std::unique_lock<std::mutex>& lk);
	void Fail(NetfsState to, const Status& st);

	std::atomic<NetfsState> m_state{NetfsState::Idle};
	mutable std::mutex m_state_mu; // transitions, m_error and the backend pointer's assignment
	std::string m_error;
	std::unique_ptr<Backend> m_backend;
	std::mutex m_conn_mu; // serialises backend calls
	std::atomic<int64_t> m_req_start{0};
	std::unique_ptr<Cache> m_cache; // last: goes first, while the connection it closes handles on is still there
};

struct Route
{
	std::shared_ptr<Share> share;
	std::string rel; // relative to the share's location ("" = the location itself)
};

// A path under a configured mount point -> its share and relative path. False for every other path (also
// for /smb/... paths that no share covers, and for ones whose ".." leaves the mount point).
bool Resolve(const char* path, Route& out);

int64_t NowMs();

// For netfs_edit.cpp: the backend factory in use (netfs_set_backend_factory), the shares file netfs_load was
// given ("" before that), and whole-file read and write (the write goes through <path>.part).
std::unique_ptr<Backend> MakeBackendFor(Scheme scheme);
std::string SharesFilePath();
bool ReadWholeFile(const std::string& path, std::string& out, int& err);
bool WriteFileAtomic(const std::string& path, const std::string& text);
void SetLastError(const std::string& text);
// One line for the frontend's log (netfs_set_log); a no-op when none is set. Never has a password in it.
void Log(const std::string& line);
// A line for netfs_take_events (a share connected, could not connect, or was lost).
void PushEvent(const std::string& text);
} // namespace netfs

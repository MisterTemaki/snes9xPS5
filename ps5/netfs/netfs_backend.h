// Snes9x PS5 netfs: what a protocol backend (SMB, NFS, or a test double) implements.
//
// netfs makes a network share look like a folder to the whole process (netfs_vfs.cpp answers the libc file
// calls for /smb/... and /nfs/... paths). A Backend does the protocol work for ONE share over ONE connection
// and knows nothing about libc, mount points or the frontend. This header is the contract between netfs and
// the SMB (libsmb2) and NFS (libnfs) backends. Keep it stable: change it only with a note in the PR.
//
// Rules for an implementation:
//  1. Read-only. There is no write, create, rename or delete.
//  2. Calls are serialised. netfs holds one lock per Backend, so an implementation needs no locking of its
//     own and may use a single-threaded library. The one exception is Abort().
//  3. Calls may block (that is why they run on worker threads, or on a caller that is allowed to wait). A
//     call that has been running for 5 s is cut by Abort() (the watchdog), so do not retry internally for
//     longer than that. Set your library's own timeouts at or below it where you can.
//  4. Paths are relative to the configured location (ShareConfig, "location"): '/'-separated UTF-8, no
//     leading or trailing '/', "" is the location itself. netfs has already rejected "." and ".." segments.
//     The backend adds whatever its protocol needs in front (SMB: the folder inside the share; NFS: the
//     export and the part of the path inside it).
//  5. Failures: file-level calls return Status::Errno(e) with an errno value (ENOENT, EACCES, EIO, ENOTDIR,
//     ...). Connect() returns Status::Kind(kind, text) with the nearest ErrKind. `text` is shown to the
//     player: library wording is fine, a password or the whole address line never is.
//  6. Symlinks are followed by the server or by you; Stat/List report the target's type and size.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace netfs
{
enum class Scheme
{
	Smb,
	Nfs,
};

// One configured share, parsed from a line of shares.txt (netfs_share.h). All strings are percent-decoded.
//
// Location:
//   SMB  `share` is the share name, `path` the folder inside it ("" = the share's root).
//   NFS  `share` is empty; `path` is everything after the host, "export" or "export/folder". The export is
//        not known in advance: the backend tries the whole path, then one folder higher, and so on.
struct ShareConfig
{
	Scheme scheme = Scheme::Smb;
	std::string host;     // name or IP, lower case; "[::1]" style for IPv6
	int port = 0;         // 0 = the protocol's default
	std::string share;    // SMB share name; empty for NFS
	std::string path;     // see above; no leading or trailing '/'
	std::string user;     // empty = guest / anonymous
	std::string domain;   // SMB only
	std::string password; // NEVER log, display or copy this anywhere but into the library call
	// SMB. `vers` is libsmb2's name: "" (negotiate), "2", "3", "2.02", "2.10", "3.00", "3.02" or "3.1.1".
	std::string vers;
	bool sign = false;
	bool seal = false;
	// NFS
	int nfs_version = 3; // 3 or 4
	int nfsport = 0;     // 0 = ask the portmapper
	int mountport = 0;   // 0 = ask the portmapper
	int uid = -1;        // -1 = the library's default
	int gid = -1;
};

// Why a Connect() (or, rarely, a later call) failed. One player-visible sentence each (netfs_share.cpp).
enum class ErrKind
{
	None,
	NoAnswer,        // the server did not answer in time
	Refused,         // connection refused
	Closed,          // the server closed the connection
	NameNotFound,    // the host name does not resolve
	AccessDenied,    // access denied, or wrong user or password
	ShareNotFound,   // no such SMB share or NFS export; fill Status::available when you can
	FolderNotFound,  // the share is there but the folder is not
	Smb1Only,        // the server only speaks SMB1
	SigningRequired, // the server demands signing and the config did not allow it
	NotBuilt,        // this protocol is not compiled into this build
	Aborted,         // Abort() was called
	Other,           // anything else; put the library's text in Status::text
};

struct Status
{
	ErrKind kind = ErrKind::None;
	int err = 0;                        // errno value for file-level failures
	std::string text;                   // credential-free detail; may be empty
	std::vector<std::string> available; // ShareNotFound: the shares or exports the server does offer

	bool ok() const { return kind == ErrKind::None && err == 0; }
	static Status Ok() { return Status(); }
	static Status Errno(int e)
	{
		Status s;
		s.err = e;
		return s;
	}
	static Status Kind(ErrKind k, std::string text = std::string())
	{
		Status s;
		s.kind = k;
		s.text = std::move(text);
		return s;
	}
};

struct Attr
{
	bool is_dir = false;
	uint64_t size = 0; // bytes; 0 for folders
	int64_t mtime = 0; // seconds since 1970-01-01 UTC
};

struct DirEntry
{
	std::string name; // one path segment; never "." or ".."
	Attr attr;        // filled by the listing itself; no extra round trip per entry
};

// An open file, owned by the backend. Opaque to netfs.
using Handle = void*;

class Backend
{
public:
	virtual ~Backend() = default;

	// Connects, signs in, and checks that the configured location exists and is a folder. Called once, from
	// a worker thread, before any other call. `cfg` stays valid until the backend is destroyed.
	virtual Status Connect(const ShareConfig& cfg) = 0;

	// Closes the connection and frees any handle still open. Not called concurrently with any other call; the
	// destructor must do the same and be safe after a failed Connect() or after Abort().
	virtual void Disconnect() = 0;

	// Size, type and mtime of one file or folder. "" is the location itself.
	virtual Status Stat(const std::string& path, Attr& out) = 0;

	// All entries of a folder, names and attributes together, without "." and "..". Order is free.
	virtual Status List(const std::string& path, std::vector<DirEntry>& out) = 0;

	// Opens a file for reading. Fails with EISDIR for a folder.
	virtual Status Open(const std::string& path, Handle& out) = 0;

	// Reads up to `len` bytes at `offset`. Returns the byte count (0 at or past the end of the file), or a
	// negative errno value. A short count before the end is allowed; netfs asks again. `len` is at most
	// 1 MiB, split it if your protocol's limit is lower.
	virtual int64_t Pread(Handle h, void* buf, size_t len, uint64_t offset) = 0;

	// Closes a handle from Open(). Never fails. Not called once the share is dead (Abort() was called): the
	// handle is freed by Disconnect() or the destructor.
	virtual void Close(Handle h) = 0;

	// The watchdog's kill switch. Callable from ANY thread at ANY time, WITHOUT the serialising lock, while
	// another call is in flight. Make that call return promptly (shut the socket down is enough) and make
	// every later call fail with Status::Kind(ErrKind::Aborted) or Errno(EIO). Abort is final: netfs does
	// not reuse the object; a reconnect creates a new one. It must not free anything the in-flight call
	// still uses. Do not block.
	virtual void Abort() = 0;

	// Optional, for error text: the names of the shares (SMB) or exports (NFS) the server offers. Called
	// only after a Connect() that failed with ShareNotFound, on the same connection if it is still up.
	virtual Status Enumerate(std::vector<std::string>& out)
	{
		(void)out;
		return Status::Errno(ENOTSUP);
	}
};

// Each protocol file defines its factory. The stubs (backend_smb_stub.cpp, backend_nfs_stub.cpp) return a
// backend whose Connect() fails with ErrKind::NotBuilt; the real files replace the stubs in the Makefile
// (NETFS_SMB_SRC, NETFS_NFS_SRC). Tests can override a scheme with netfs_set_backend_factory (netfs.h).
std::unique_ptr<Backend> MakeSmbBackend();
std::unique_ptr<Backend> MakeNfsBackend();
} // namespace netfs

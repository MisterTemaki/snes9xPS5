// Snes9x PS5 netfs: the NFS backend, on libnfs (LGPL-2.1, vendored in third_party/libnfs). One object is one
// connection to one export. Written from the design in .context/projects/snes9xps5 (swanstation-nfs-reference.md,
// network-shares-spec.md); no SwanStationPS5 code is used.
//
// Every libnfs call goes through its async API and our own wait loop (Wait) instead of the library's sync
// wrappers. The sync wrappers cannot be interrupted and wait up to 60 s for a vanished server; our loop checks
// Abort() and a deadline (kCallTimeoutMs, under the 5 s watchdog) every wait step. A call that is given up on
// leaves libnfs holding a pointer into it (and, for a read, into the caller's buffer), so a given-up call kills
// the connection: no libnfs call is made after it except destroying the context.
//
// SPDX-License-Identifier: MIT
#include "backend_nfs.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>

#include <nfsc/libnfs.h> // first: it defines EXTERN for the raw headers
#include <nfsc/libnfs-raw.h>

#include "libnfs-raw-mount.h"
#include "netfs_backend.h"

namespace netfs
{
namespace
{
constexpr int kCallTimeoutMs = 4500;    // one metadata call or read; the watchdog cuts at 5 s
constexpr int kConnectTimeoutMs = 4500; // the whole Connect(), every mount attempt together
constexpr int kExportsTimeoutMs = 2000; // asking the server which exports it has (blocking, error path only)
constexpr size_t kMaxRead = 1u << 20;   // the Backend contract's limit for one Pread
// One READ request is at most this long, and a Pread keeps up to kMaxRead / kReadChunk of them in flight.
// libnfs 6.0.2 sends a single request per call (it clamps to the negotiated size) and cannot reassemble an
// RPC reply the server split into fragments, which servers on libtirpc (unfs3) do above 256 KiB: a smaller
// request keeps those servers working and the pipelining keeps the wire busy.
constexpr size_t kReadChunk = 128u << 10;
constexpr int kMaxMountTries = 8;       // export, then one folder higher each time

NfsTestHooks g_hooks;

int64_t NowMs()
{
	if (g_hooks.now_ms)
		return g_hooks.now_ms();
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::vector<std::string> Split(const std::string& path)
{
	std::vector<std::string> parts;
	size_t i = 0;
	while (i < path.size())
	{
		size_t j = path.find('/', i);
		if (j == std::string::npos)
			j = path.size();
		if (j > i)
			parts.push_back(path.substr(i, j - i));
		i = j + 1;
	}
	return parts;
}

std::string Join(const std::vector<std::string>& parts, size_t from, size_t to)
{
	std::string s;
	for (size_t i = from; i < to; i++)
	{
		if (!s.empty())
			s += '/';
		s += parts[i];
	}
	return s;
}

bool Has(const std::string& lower, const char* needle)
{
	return lower.find(needle) != std::string::npos;
}

// The library's last error text -> a failure of the connection itself (as opposed to "the server answered
// and said no"). None means the server answered.
ErrKind ClassifyTransport(const std::string& text)
{
	std::string t = text;
	std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	if (Has(t, "refused"))
		return ErrKind::Refused;
	if (Has(t, "timed out") || Has(t, "timeout"))
		return ErrKind::NoAnswer;
	if (Has(t, "invalid address") || Has(t, "resolv") || Has(t, "getaddrinfo") || Has(t, "name or service"))
		return ErrKind::NameNotFound;
	if (Has(t, "closed connection") || Has(t, "pollhup") || Has(t, "read from socket") || Has(t, "reset by peer") ||
	    Has(t, "not connected") || Has(t, "socket error"))
		return ErrKind::Closed;
	return ErrKind::None;
}

// One libnfs call in flight. Heap allocated and owned by the backend until libnfs is gone: libnfs holds the
// pointer until the callback has run.
struct Call
{
	bool done = false;
	int status = 0;     // >= 0 success (read: byte count), < 0 -errno
	void* data = nullptr; // open: nfsfh*, opendir: nfsdir*
	nfs_stat_64 st = {};
	bool want_st = false;
	bool want_ptr = false;
};

void OnDone(int status, nfs_context*, void* data, void* priv)
{
	Call* c = static_cast<Call*>(priv);
	c->done = true;
	c->status = status;
	if (status >= 0 && data)
	{
		if (c->want_st)
			std::memcpy(&c->st, data, sizeof(c->st));
		else if (c->want_ptr)
			c->data = data;
	}
}

std::unique_ptr<Call> NewCall(bool want_st = false, bool want_ptr = false)
{
	auto c = std::make_unique<Call>();
	c->want_st = want_st;
	c->want_ptr = want_ptr;
	return c;
}

class NfsBackend final : public Backend
{
public:
	~NfsBackend() override { Disconnect(); }

	Status Connect(const ShareConfig& cfg) override;
	void Disconnect() override;
	Status Stat(const std::string& path, Attr& out) override;
	Status List(const std::string& path, std::vector<DirEntry>& out) override;
	Status Open(const std::string& path, Handle& out) override;
	int64_t Pread(Handle h, void* buf, size_t len, uint64_t offset) override;
	void Close(Handle h) override;
	void Abort() override;
	Status Enumerate(std::vector<std::string>& out) override;

private:
	bool NewContext();
	int Wait(std::unique_ptr<Call>& c, int64_t deadline);
	int WaitAll(std::vector<std::unique_ptr<Call>>& calls, int64_t deadline);
	std::string Full(const std::string& rel) const;
	std::string LastError() const;
	const std::vector<std::string>& Exports();

	const ShareConfig* m_cfg = nullptr;
	std::string m_host;
	std::string m_base; // folder inside the mounted export, no leading or trailing '/'
	nfs_context* m_ctx = nullptr;
	std::vector<std::unique_ptr<Call>> m_orphans; // given-up calls; libnfs may still point at them
	std::vector<std::string> m_exports;
	bool m_exports_fetched = false;
	bool Dead() const { return m_aborted.load(std::memory_order_acquire) || m_dead.load(std::memory_order_acquire); }

	std::atomic<bool> m_aborted{false}; // Abort() was called
	std::atomic<bool> m_dead{false};    // a call was given up on (deadline, socket error): the connection is finished
	std::atomic<int> m_fd{-1};          // the socket, only while a Wait is running (for Abort)
	int m_sockerr = 0;                  // the socket's error (SO_ERROR) when the last Wait saw it fail
};

// libnfs keeps two error strings: the NFS layer's, and the RPC layer's (socket failures land only there).
std::string NfsBackend::LastError() const
{
	std::string text;
	if (!m_ctx)
		return text;
	if (const char* e = nfs_get_error(m_ctx))
		text = e;
	if (const char* r = rpc_get_error(nfs_get_rpc_context(m_ctx)))
		if (*r && text.find(r) == std::string::npos)
			text += text.empty() ? r : std::string("; ") + r;
	return text;
}

std::string NfsBackend::Full(const std::string& rel) const
{
	std::string s = "/" + m_base;
	if (!rel.empty())
	{
		if (s.size() > 1)
			s += '/';
		s += rel;
	}
	return s;
}

bool NfsBackend::NewContext()
{
	if (m_ctx)
	{
		nfs_destroy_context(m_ctx);
		m_ctx = nullptr;
	}
	m_ctx = nfs_init_context();
	if (!m_ctx)
		return false;
	nfs_set_version(m_ctx, m_cfg->nfs_version == 4 ? 4 : 3);
	nfs_set_autoreconnect(m_ctx, 0); // a lost connection is final: netfs builds a new backend
	nfs_set_dircache(m_ctx, 0);      // netfs has its own listing cache
	nfs_set_readmax(m_ctx, kReadChunk);
	const int nfsport = m_cfg->nfsport > 0 ? m_cfg->nfsport : m_cfg->port;
	if (nfsport > 0)
		nfs_set_nfsport(m_ctx, nfsport);
	if (m_cfg->mountport > 0)
		nfs_set_mountport(m_ctx, m_cfg->mountport);
	if (m_cfg->uid >= 0)
		nfs_set_uid(m_ctx, m_cfg->uid);
	if (m_cfg->gid >= 0)
		nfs_set_gid(m_ctx, m_cfg->gid);
	return true;
}

// Services the connection until every call has finished. Returns 0 then. Otherwise an errno (ECANCELED:
// Abort(); ETIMEDOUT: deadline; EIO: the socket failed), the calls are parked in m_orphans and the
// connection is dead.
int NfsBackend::Wait(std::unique_ptr<Call>& c, int64_t deadline)
{
	std::vector<std::unique_ptr<Call>> one;
	one.push_back(std::move(c));
	const int err = WaitAll(one, deadline);
	if (!err)
		c = std::move(one[0]);
	return err;
}

int NfsBackend::WaitAll(std::vector<std::unique_ptr<Call>>& calls, int64_t deadline)
{
	int err = 0;
	const auto pending = [&calls] {
		for (const auto& c : calls)
			if (!c->done)
				return true;
		return false;
	};
	while (pending())
	{
		if (m_aborted.load(std::memory_order_acquire))
		{
			err = ECANCELED;
			break;
		}
		if (NowMs() >= deadline)
		{
			err = ETIMEDOUT;
			break;
		}
		pollfd pfd = {};
		pfd.fd = nfs_get_fd(m_ctx);
		pfd.events = static_cast<short>(nfs_which_events(m_ctx));
		m_fd.store(pfd.fd, std::memory_order_relaxed);
		const int n = poll(&pfd, 1, g_hooks.poll_ms);
		if (n < 0 && errno != EINTR)
		{
			err = EIO;
			break;
		}
		if (n > 0 && (pfd.revents & (POLLERR | POLLHUP)) && pfd.fd >= 0)
		{
			// libnfs reports a refused connection and a closed one alike ("Socket failed with POLLHUP"): the
			// socket's own error tells them apart.
			int e = 0;
			socklen_t len = sizeof e;
			if (getsockopt(pfd.fd, SOL_SOCKET, SO_ERROR, &e, &len) == 0 && e)
				m_sockerr = e;
		}
		if (nfs_service(m_ctx, n > 0 ? pfd.revents : 0) < 0)
		{
			err = EIO;
			break;
		}
	}
	m_fd.store(-1, std::memory_order_relaxed);
	if (err)
	{
		m_dead.store(true, std::memory_order_release);
		for (auto& c : calls)
			m_orphans.push_back(std::move(c));
		calls.clear();
	}
	return err;
}

void NfsBackend::Abort()
{
	m_aborted.store(true, std::memory_order_release);
	const int fd = m_fd.load(std::memory_order_relaxed);
	if (fd >= 0)
		shutdown(fd, SHUT_RDWR); // also frees a blocked send(); the wait loop sees the flag within one step
}

void NfsBackend::Disconnect()
{
	m_fd.store(-1, std::memory_order_relaxed);
	if (m_ctx)
	{
		nfs_destroy_context(m_ctx);
		m_ctx = nullptr;
	}
	m_orphans.clear(); // after the context: destroying it can still run their callbacks
}

const std::vector<std::string>& NfsBackend::Exports()
{
	if (!m_exports_fetched)
	{
		m_exports_fetched = true;
		if (m_cfg && m_cfg->nfs_version != 4 && !Dead())
		{
			exportnode* ex = mount_getexports_timeout(m_host.c_str(), kExportsTimeoutMs);
			for (exportnode* e = ex; e; e = e->ex_next)
				if (e->ex_dir)
					m_exports.emplace_back(e->ex_dir);
			mount_free_export_list(ex);
		}
	}
	return m_exports;
}

Status NfsBackend::Enumerate(std::vector<std::string>& out)
{
	if (!m_cfg || m_cfg->nfs_version == 4)
		return Status::Errno(ENOTSUP);
	out = Exports();
	return out.empty() ? Status::Errno(ENOENT) : Status::Ok();
}

Status NfsBackend::Connect(const ShareConfig& cfg)
{
	if (cfg.scheme != Scheme::Nfs)
		return Status::Kind(ErrKind::Other, "not an NFS share");
	if (Dead())
		return Status::Kind(ErrKind::Aborted);
	m_cfg = &cfg;
	m_host = cfg.host;
	if (m_host.size() > 2 && m_host.front() == '[' && m_host.back() == ']')
		m_host = m_host.substr(1, m_host.size() - 2);

	const std::vector<std::string> parts = Split(cfg.path);
	const int64_t deadline = NowMs() + kConnectTimeoutMs;
	bool all_denied = true; // every refusal was "permission denied"
	size_t keep = parts.size();
	bool mounted = false;

	// The export is not known in advance: try the whole path, then one folder higher, and so on. A server that
	// says "no" is asked again with a shorter path; a connection that fails ends it at once.
	for (int tries = 0; tries < kMaxMountTries; tries++)
	{
		if (!NewContext())
			return Status::Kind(ErrKind::Other, "out of memory");
		const std::string exp = "/" + Join(parts, 0, keep);
		auto c = NewCall();
		if (nfs_mount_async(m_ctx, m_host.c_str(), exp.c_str(), OnDone, c.get()) != 0)
		{
			const std::string text = LastError();
			const ErrKind k = ClassifyTransport(text);
			return Status::Kind(k == ErrKind::None ? ErrKind::Other : k, text);
		}
		const int err = Wait(c, deadline);
		if (err)
		{
			// Abort() shuts the socket down, so the loop may see the socket error first: the flag decides.
			if (m_aborted.load())
				return Status::Kind(ErrKind::Aborted);
			if (err == ETIMEDOUT)
				return Status::Kind(ErrKind::NoAnswer, "the server did not answer");
			const std::string text = LastError();
			ErrKind k = ClassifyTransport(text);
			if (m_sockerr == ECONNREFUSED)
				k = ErrKind::Refused;
			else if (m_sockerr == ETIMEDOUT || m_sockerr == EHOSTUNREACH || m_sockerr == ENETUNREACH || m_sockerr == EHOSTDOWN)
				k = ErrKind::NoAnswer;
			return Status::Kind(k == ErrKind::None ? ErrKind::Closed : k, text);
		}
		if (c->status == 0)
		{
			mounted = true;
			break;
		}
		const std::string text = LastError();
		const ErrKind k = ClassifyTransport(text);
		if (k != ErrKind::None)
			return Status::Kind(k, text);
		if (c->status != -EACCES && c->status != -EPERM)
			all_denied = false;
		if (keep == 0)
			break;
		keep--;
	}

	if (!mounted)
	{
		// The server answered but would not mount any prefix of the path.
		const std::vector<std::string>& exports = Exports();
		if (m_aborted.load())
			return Status::Kind(ErrKind::Aborted);
		const std::string want = "/" + cfg.path;
		for (const std::string& e : exports)
		{
			const bool prefix = e == "/" || want == e || (want.size() > e.size() && want.compare(0, e.size(), e) == 0 && want[e.size()] == '/');
			if (prefix)
				return Status::Kind(ErrKind::AccessDenied, "the server has this export but refused to mount it (is it exported to this console's address?)");
		}
		Status s = Status::Kind(all_denied && exports.empty() ? ErrKind::AccessDenied : ErrKind::ShareNotFound,
		                        all_denied && exports.empty() ? "the server refused to mount it (not exported to this console, or no such export)"
		                                                      : "the server does not export this path");
		s.available = exports;
		return s;
	}

	m_base = Join(parts, keep, parts.size());
	if (!m_base.empty())
	{
		// The export is higher than the configured folder: the folder itself still has to be there.
		Attr a;
		const Status s = Stat(std::string(), a);
		if (m_aborted.load())
			return Status::Kind(ErrKind::Aborted);
		if (m_dead.load())
			return Status::Kind(ErrKind::NoAnswer, "the server stopped answering");
		if (!s.ok() || !a.is_dir)
			return Status::Kind(ErrKind::FolderNotFound, "folder not found: " + cfg.path);
	}
	return Status::Ok();
}

Status NfsBackend::Stat(const std::string& path, Attr& out)
{
	if (!m_ctx || Dead())
		return Status::Errno(EIO);
	auto c = NewCall(true);
	const std::string full = Full(path);
	if (nfs_stat64_async(m_ctx, full.c_str(), OnDone, c.get()) != 0)
		return Status::Errno(EIO);
	if (Wait(c, NowMs() + kCallTimeoutMs))
		return Status::Errno(EIO);
	if (c->status < 0)
		return Status::Errno(-c->status);
	out.is_dir = S_ISDIR(c->st.nfs_mode);
	out.size = out.is_dir ? 0 : c->st.nfs_size;
	out.mtime = static_cast<int64_t>(c->st.nfs_mtime);
	return Status::Ok();
}

Status NfsBackend::List(const std::string& path, std::vector<DirEntry>& out)
{
	if (!m_ctx || Dead())
		return Status::Errno(EIO);
	auto c = NewCall(false, true);
	const std::string full = Full(path);
	if (nfs_opendir_async(m_ctx, full.c_str(), OnDone, c.get()) != 0)
		return Status::Errno(EIO);
	if (Wait(c, NowMs() + kCallTimeoutMs))
		return Status::Errno(EIO);
	if (c->status < 0)
		return Status::Errno(-c->status);

	nfsdir* dir = static_cast<nfsdir*>(c->data);
	std::vector<std::string> follow; // symlinks and entries the server gave no attributes for
	std::vector<DirEntry> entries;
	for (nfsdirent* e = nfs_readdir(m_ctx, dir); e; e = nfs_readdir(m_ctx, dir))
	{
		if (!e->name || !std::strcmp(e->name, ".") || !std::strcmp(e->name, ".."))
			continue;
		DirEntry d;
		d.name = e->name;
		const uint32_t type = e->mode & S_IFMT;
		if (type == S_IFDIR || type == S_IFREG)
		{
			d.attr.is_dir = type == S_IFDIR;
			d.attr.size = d.attr.is_dir ? 0 : e->size;
			d.attr.mtime = static_cast<int64_t>(e->mtime.tv_sec);
			entries.push_back(std::move(d));
		}
		else if (type == S_IFLNK || type == 0)
			follow.push_back(e->name);
		// sockets, devices and pipes are not ROMs
	}
	nfs_closedir(m_ctx, dir);

	// The target's type and size (Backend rule 6). A dangling link, or a stat that fails, drops the entry.
	for (const std::string& name : follow)
	{
		DirEntry d;
		d.name = name;
		const Status s = Stat(path.empty() ? name : path + "/" + name, d.attr);
		if (Dead())
			return Status::Errno(EIO);
		if (s.ok())
			entries.push_back(std::move(d));
	}
	out = std::move(entries);
	return Status::Ok();
}

Status NfsBackend::Open(const std::string& path, Handle& out)
{
	Attr a;
	const Status s = Stat(path, a);
	if (!s.ok())
		return s;
	if (a.is_dir)
		return Status::Errno(EISDIR);
	auto c = NewCall(false, true);
	const std::string full = Full(path);
	if (nfs_open_async(m_ctx, full.c_str(), O_RDONLY, OnDone, c.get()) != 0)
		return Status::Errno(EIO);
	if (Wait(c, NowMs() + kCallTimeoutMs))
		return Status::Errno(EIO);
	if (c->status < 0)
		return Status::Errno(-c->status);
	out = c->data;
	return Status::Ok();
}

int64_t NfsBackend::Pread(Handle h, void* buf, size_t len, uint64_t offset)
{
	if (!m_ctx || Dead())
		return -EIO;
	if (len == 0)
		return 0;
	len = std::min(len, kMaxRead);
	const size_t chunk = std::max<size_t>(1, std::min(kReadChunk, nfs_get_readmax(m_ctx)));

	std::vector<std::unique_ptr<Call>> calls;
	std::vector<size_t> lens;
	for (size_t done = 0; done < len; done += chunk)
	{
		const size_t n = std::min(chunk, len - done);
		calls.push_back(NewCall());
		lens.push_back(n);
		if (nfs_pread_async(m_ctx, static_cast<nfsfh*>(h), static_cast<char*>(buf) + done, n, offset + done, OnDone,
		                    calls.back().get()) != 0)
		{
			// The ones already queued are still in flight: wait for them before giving up.
			calls.pop_back();
			lens.pop_back();
			break;
		}
	}
	if (calls.empty())
		return -EIO;
	if (WaitAll(calls, NowMs() + kCallTimeoutMs))
		return -EIO;

	// The bytes from the start that arrived whole; a short or failed chunk ends them (netfs asks again).
	int64_t total = 0;
	for (size_t i = 0; i < calls.size(); i++)
	{
		if (calls[i]->status < 0)
			return total > 0 ? total : calls[i]->status;
		total += calls[i]->status;
		if (static_cast<size_t>(calls[i]->status) < lens[i])
			break;
	}
	return total;
}

void NfsBackend::Close(Handle h)
{
	if (!h || !m_ctx || Dead())
		return; // a dead connection: the handle goes with the context
	auto c = NewCall();
	if (nfs_close_async(m_ctx, static_cast<nfsfh*>(h), OnDone, c.get()) != 0)
		return;
	Wait(c, NowMs() + kCallTimeoutMs);
}
} // namespace

void NfsSetTestHooks(const NfsTestHooks& hooks)
{
	g_hooks = hooks;
}

std::unique_ptr<Backend> MakeNfsBackend()
{
	return std::make_unique<NfsBackend>();
}
} // namespace netfs

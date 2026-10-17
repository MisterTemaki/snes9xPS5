// Snes9x PS5 netfs: the SMB backend, on libsmb2 (SMB 2 and 3, read only). Contract: netfs_backend.h.
//
// One Backend = one TCP connection = one libsmb2 context. netfs serialises the calls, so there is no lock here.
// Every call runs libsmb2's async API under our own wait loop (Pump) instead of its sync API: the loop wakes
// every kTickMs to look at Abort(), and gives up when the server has said nothing for kIdleMs, which keeps a
// call under the watchdog's 5 s. Once a call is cut off, by Abort() or by silence, the connection is dead: no
// later call touches the context except Disconnect() destroying it.
//
// Reads are pipelined: a Pread() splits into up to kDepth requests in flight, so a server limited to 64 KiB
// per request (SMB 2.02, many routers) still fills the wire.
//
// Console behaviour of this file is UNVERIFIED (no console in the cloud). It was tested against Samba on Linux.
//
// SPDX-License-Identifier: MIT

#include "netfs_backend.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <memory>
#include <poll.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "smb2/smb2.h"
#include "smb2/libsmb2.h"
#include "smb2/libsmb2-raw.h"
#include "smb2/libsmb2-dcerpc.h"
#include "smb2/libsmb2-dcerpc-srvsvc.h"
#include "smb2/smb2-errors.h"

namespace netfs
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr int kTickMs = 100;                // how often a waiting call looks at Abort()
constexpr uint32_t kChunkMax = 256 * 1024;  // one read request, at most
constexpr uint32_t kChunkMin = 64 * 1024;   // SMB 2.02's limit
constexpr size_t kReadMax = 1024 * 1024;    // netfs_backend.h: Pread() len is at most this
constexpr size_t kDepth = 8;                // read requests in flight
constexpr int kDefaultPort = 445;
#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0; // the app ignores SIGPIPE (ProsperoCrash.cpp)
#endif

// The silence after which a call gives up. Under the watchdog's 5 s. A test hook shortens it.
std::atomic<int> g_idle_ms{4000};

// What one libsmb2 call reports. Heap-allocated and kept alive until the context is destroyed, because
// libsmb2 calls the callback of every request still queued when it is destroyed (with SMB2_STATUS_SHUTDOWN).
struct Req
{
	bool done = false;
	int status = 0;
	void* data = nullptr;
	smb2_stat_64 st = {};
	// pread slot
	size_t pos = 0;
	size_t want = 0;
	size_t got = 0;
	int empty = 0; // replies of 0 bytes inside the file
	bool finished = false;
};

void OnDone(smb2_context*, int status, void* command_data, void* private_data)
{
	Req* r = static_cast<Req*>(private_data);
	r->done = true;
	r->status = status;
	r->data = command_data;
}

int Errno(int status) // libsmb2 reports -errno; a few paths report +errno
{
	if (uint32_t(status) == SMB2_STATUS_SHUTDOWN) // the connection went away under a queued request
		return EIO;
	return status < 0 ? -status : status;
}

// An open file: libsmb2's handle plus the size the server gave when it opened it.
struct File
{
	smb2fh* fh;
	uint64_t size;
};

bool IsLogonFailure(uint32_t nt)
{
	switch (nt)
	{
	case SMB2_STATUS_LOGON_FAILURE:
	case SMB2_STATUS_ACCESS_DENIED:
	case SMB2_STATUS_ACCOUNT_DISABLED:
	case SMB2_STATUS_ACCOUNT_LOCKED_OUT:
	case SMB2_STATUS_ACCOUNT_RESTRICTION:
	case SMB2_STATUS_PASSWORD_EXPIRED:
	case SMB2_STATUS_NETWORK_ACCESS_DENIED:
		return true;
	default:
		return false;
	}
}

enum smb2_negotiate_version Version(const std::string& v)
{
	if (v == "2")
		return SMB2_VERSION_ANY2;
	if (v == "3")
		return SMB2_VERSION_ANY3;
	if (v == "2.02")
		return SMB2_VERSION_0202;
	if (v == "2.10")
		return SMB2_VERSION_0210;
	if (v == "3.00")
		return SMB2_VERSION_0300;
	if (v == "3.02")
		return SMB2_VERSION_0302;
	if (v == "3.1.1")
		return SMB2_VERSION_0311;
	return SMB2_VERSION_ANY;
}

std::string Join(const std::string& root, const std::string& path)
{
	if (root.empty())
		return path;
	return path.empty() ? root : root + "/" + path;
}

// The IPv4 address of a dotted string or a name. getaddrinfo is the console's own when netfs wraps it
// (netfs_shims_ps5.cpp: libSceNet's resolver), the system's on the host.
bool ResolveV4(const std::string& name, in_addr& out)
{
	if (inet_pton(AF_INET, name.c_str(), &out) == 1)
		return true;
	addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	bool ok = false;
	if (getaddrinfo(name.c_str(), nullptr, &hints, &res) == 0)
	{
		for (const addrinfo* a = res; a; a = a->ai_next)
			if (a->ai_family == AF_INET && a->ai_addrlen >= sizeof(sockaddr_in))
			{
				out = reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr;
				ok = true;
				break;
			}
		freeaddrinfo(res);
	}
	return ok;
}

// A server that only speaks SMB1 never answers libsmb2's SMB 2 negotiate. Ask it in SMB 1 to be sure.
bool ServerSpeaksSmb1(const in_addr& ip, int port)
{
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return false;
	fcntl(s, F_SETFL, O_NONBLOCK); // SO_NBIO on the console (netfs_shims_ps5.cpp wraps fcntl)
	sockaddr_in sa = {};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(uint16_t(port));
	sa.sin_addr = ip;
	bool yes = false;
	const int rc = connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
	pollfd p = {s, POLLOUT, 0};
	if ((rc == 0 || errno == EINPROGRESS) && poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT))
	{
		int err = 0;
		socklen_t len = sizeof(err);
		getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
		if (err == 0)
		{
			// NetBIOS length, then the SMB1 header (command 0x72 NEGOTIATE) and one dialect, "NT LM 0.12"
			unsigned char req[4 + 32 + 3 + 12] = {};
			req[3] = 47; // the SMB message: header 32 + word count 1 + byte count 2 + dialect 12
			unsigned char* h = req + 4;
			h[0] = 0xff, h[1] = 'S', h[2] = 'M', h[3] = 'B', h[4] = 0x72;
			h[9] = 0x18;                  // flags
			h[10] = 0x43, h[11] = 0xc8;   // flags2: long names, NT status, unicode
			h[24] = h[25] = 0xff;         // tree id
			h[26] = 0xfe, h[27] = 0xff;   // process id
			unsigned char* body = h + 32; // word count 0, then the byte count
			body[1] = 12;
			body[3] = 0x02;
			std::memcpy(body + 4, "NT LM 0.12", 11); // with its NUL
			if (send(s, req, sizeof(req), kNoSignal) == ssize_t(sizeof(req)))
			{
				p.events = POLLIN;
				p.revents = 0;
				unsigned char resp[8] = {};
				if (poll(&p, 1, 2000) == 1 && recv(s, resp, sizeof(resp), 0) >= 8)
					yes = resp[4] == 0xff && resp[5] == 'S' && resp[6] == 'M' && resp[7] == 'B';
			}
		}
	}
	close(s);
	return yes;
}

class SmbBackend final : public Backend
{
public:
	~SmbBackend() override { Disconnect(); }

	Status Connect(const ShareConfig& cfg) override
	{
		m_cfg = &cfg;
		m_root = cfg.path;
		if (cfg.host.empty() || cfg.share.empty())
			return Status::Kind(ErrKind::Other, "the SMB address needs a server and a share");
		if (cfg.host[0] == '[')
			return Status::Kind(ErrKind::Other, "IPv6 addresses are not supported");
		if (!ResolveV4(cfg.host, m_ip))
			return Status::Kind(ErrKind::NameNotFound, "could not find " + cfg.host);
		char dotted[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &m_ip, dotted, sizeof(dotted));
		m_port = cfg.port ? cfg.port : kDefaultPort;
		m_server = std::string(dotted) + ":" + std::to_string(m_port);

		Status st;
		if (cfg.user.empty())
		{
			// Guest: the Windows and Samba way ("Guest", empty password), then the anonymous session some NAS boxes want
			st = Attempt(m_server, cfg.share, "Guest", "");
			if (st.kind == ErrKind::AccessDenied && !m_aborted)
			{
				Status anon = Attempt(m_server, cfg.share, "", nullptr);
				if (anon.ok() || anon.kind != ErrKind::AccessDenied)
					st = anon;
			}
		}
		else
		{
			st = Attempt(m_server, cfg.share, cfg.user.c_str(), cfg.password.c_str());
		}
		if (!st.ok())
			return st;

		const uint32_t max_read = smb2_get_max_read_size(m_ctx);
		m_chunk = std::max(kChunkMin, std::min(kChunkMax, max_read / kChunkMin * kChunkMin));

		Attr root;
		const Status f = Stat("", root);
		Status result = Status::Ok();
		if (!f.ok())
			result = Status::Kind(f.err == EIO ? ErrKind::Closed : ErrKind::FolderNotFound,
				f.err == EIO ? "the server stopped answering" : "the folder is not on the share");
		else if (!root.is_dir)
			result = Status::Kind(ErrKind::FolderNotFound, "the path is a file, not a folder");
		if (!result.ok())
			Disconnect(); // a failed Connect leaves no session behind
		return result;
	}

	void Disconnect() override
	{
		if (m_ctx)
		{
			if (m_connected && !m_dead && !m_aborted)
			{
				// leave politely (tree disconnect and log off); a server that does not answer is not waited for long
				std::unique_ptr<Req> r(new Req);
				const int q = smb2_disconnect_share_async(m_ctx, OnDone, r.get());
				if (q == 0 && Pump([&] { return r->done; }, 500) != 0)
					m_graveyard.push_back(std::move(r));
			}
			smb2_destroy_context(m_ctx); // also frees the handles and directories still open
			m_ctx = nullptr;
		}
		for (File* f : m_files)
			delete f;
		m_files.clear();
		m_graveyard.clear();
		m_connected = false;
		m_dead = true;
	}

	Status Stat(const std::string& path, Attr& out) override
	{
		if (!Usable())
			return Status::Errno(EIO);
		std::unique_ptr<Req> r(new Req);
		const int rc = Await(r, smb2_stat_async(m_ctx, Join(m_root, path).c_str(), &r->st, OnDone, r.get()));
		if (rc < 0)
			return Status::Errno(-rc);
		out.is_dir = r->st.smb2_type == SMB2_TYPE_DIRECTORY;
		out.size = out.is_dir ? 0 : r->st.smb2_size;
		out.mtime = int64_t(r->st.smb2_mtime);
		return Status::Ok();
	}

	Status List(const std::string& path, std::vector<DirEntry>& out) override
	{
		if (!Usable())
			return Status::Errno(EIO);
		std::unique_ptr<Req> r(new Req);
		const int rc = Await(r, smb2_opendir_async(m_ctx, Join(m_root, path).c_str(), OnDone, r.get()));
		if (rc < 0)
			return Status::Errno(-rc);
		smb2dir* dir = static_cast<smb2dir*>(r->data);
		if (!dir)
			return Status::Errno(EIO);
		out.clear();
		while (const smb2dirent* e = smb2_readdir(m_ctx, dir))
		{
			if (!e->name || !strcmp(e->name, ".") || !strcmp(e->name, ".."))
				continue;
			DirEntry d;
			d.name = e->name;
			d.attr.is_dir = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
			d.attr.size = d.attr.is_dir ? 0 : e->st.smb2_size;
			d.attr.mtime = int64_t(e->st.smb2_mtime);
			out.push_back(std::move(d));
		}
		smb2_closedir(m_ctx, dir);
		return Status::Ok();
	}

	Status Open(const std::string& path, Handle& out) override
	{
		if (!Usable())
			return Status::Errno(EIO);
		std::unique_ptr<Req> r(new Req);
		const int rc = Await(r, smb2_open_async(m_ctx, Join(m_root, path).c_str(), O_RDONLY, OnDone, r.get()));
		if (rc < 0)
			return Status::Errno(-rc);
		smb2fh* fh = static_cast<smb2fh*>(r->data);
		if (!fh)
			return Status::Errno(EIO);
		uint64_t end = 0; // the size from the open reply: no round trip
		const int64_t size = smb2_lseek(m_ctx, fh, 0, SEEK_END, &end);
		File* f = new File{fh, size < 0 ? 0 : uint64_t(size)};
		m_files.insert(f);
		out = f;
		return Status::Ok();
	}

	int64_t Pread(Handle h, void* buf, size_t len, uint64_t offset) override
	{
		File* f = static_cast<File*>(h);
		if (!Usable() || !f || !m_files.count(f))
			return -EIO;
		if (offset >= f->size)
			return 0;
		len = std::min<uint64_t>(std::min(len, kReadMax), f->size - offset);
		if (len == 0)
			return 0;
		smb2fh* fh = f->fh;
		uint8_t* const base = static_cast<uint8_t*>(buf);

		std::vector<std::unique_ptr<Req>> slots;
		for (size_t pos = 0; pos < len; pos += m_chunk)
		{
			slots.emplace_back(new Req);
			slots.back()->pos = pos;
			slots.back()->want = std::min<size_t>(m_chunk, len - pos);
		}
		size_t next = 0;      // the next slot not yet started
		size_t in_flight = 0;
		size_t finished = 0;
		int err = 0;

		auto start = [&](Req* r) {
			r->done = false;
			const int q = smb2_pread_async(m_ctx, fh, base + r->pos + r->got, uint32_t(r->want - r->got), offset + r->pos + r->got,
				OnDone, r);
			if (q < 0)
			{
				err = err ? err : -q;
				r->finished = true;
				finished++;
				return false;
			}
			in_flight++;
			return true;
		};

		while (finished < slots.size() && (in_flight > 0 || !err))
		{
			while (!err && in_flight < kDepth && next < slots.size())
				start(slots[next++].get());
			if (in_flight == 0)
				break;
			if (Pump([&] {
					for (const auto& s : slots)
						if (s->done)
							return true;
					return false;
				}) != 0)
			{
				for (auto& s : slots)
					m_graveyard.push_back(std::move(s)); // libsmb2 may still call them back while it is destroyed
				return -EIO;
			}
			for (auto& s : slots)
			{
				if (!s->done)
					continue;
				s->done = false;
				in_flight--;
				if (s->status < 0)
				{
					err = err ? err : Errno(s->status);
					s->finished = true;
					finished++;
				}
				else if (s->status == 0)
				{
					// The range is inside the file, so this is not the end: libsmb2 asked for 0 bytes because the
					// credits ran out, or the server is not serving. Ask again, a few times.
					if (++s->empty > 8)
					{
						err = err ? err : EIO;
						s->finished = true;
						finished++;
					}
					else if (err)
					{
						s->finished = true;
						finished++;
					}
					else
						start(s.get());
				}
				else
				{
					s->got += size_t(s->status);
					if (s->got >= s->want)
					{
						s->finished = true;
						finished++;
					}
					else if (!err)
						start(s.get()); // the server (or the credits) gave less than asked: ask for the rest
					else
					{
						s->finished = true;
						finished++;
					}
				}
			}
		}

		size_t total = 0; // the bytes that are contiguous from `offset`
		for (const auto& s : slots)
		{
			total += s->got;
			if (s->got < s->want)
				break;
		}
		if (total == 0 && err)
			return -err;
		return int64_t(total);
	}

	void Close(Handle h) override
	{
		File* f = static_cast<File*>(h);
		if (!f || !m_files.erase(f))
			return;
		if (Usable()) // a dead connection frees its handles when it is destroyed
		{
			std::unique_ptr<Req> r(new Req);
			const int q = smb2_close_async(m_ctx, f->fh, OnDone, r.get());
			if (q == 0 && Pump([&] { return r->done; }, 1000) != 0)
				m_graveyard.push_back(std::move(r));
		}
		delete f;
	}

	// Any thread, no lock: only flips a flag. A waiting call sees it within kTickMs.
	void Abort() override { m_aborted.store(true); }

	Status Enumerate(std::vector<std::string>& out) override
	{
		// A share that does not exist leaves the session useless, so ask over a new connection to IPC$
		if (!m_cfg || m_aborted)
			return Status::Errno(EIO);
		SmbBackend ipc;
		ipc.m_cfg = m_cfg;
		ipc.m_ip = m_ip;
		ipc.m_port = m_port;
		const ShareConfig& c = *m_cfg;
		Status st = c.user.empty() ? ipc.Attempt(m_server, "IPC$", "Guest", "") : ipc.Attempt(m_server, "IPC$", c.user.c_str(), c.password.c_str());
		if (!st.ok() && c.user.empty())
			st = ipc.Attempt(m_server, "IPC$", "", nullptr);
		if (!st.ok())
			return Status::Errno(EACCES);
		std::unique_ptr<Req> r(new Req);
		const int rc = ipc.Await(r, smb2_share_enum_async(ipc.m_ctx, SHARE_INFO_1, OnDone, r.get()));
		if (rc < 0)
			return Status::Errno(-rc);
		auto* rep = static_cast<srvsvc_NetrShareEnum_rep*>(r->data);
		if (!rep)
			return Status::Errno(EIO);
		out.clear();
		const auto& l1 = rep->ses.ShareInfo.Level1;
		for (uint32_t i = 0; l1.Buffer && i < l1.EntriesRead; i++)
		{
			const auto& si = l1.Buffer->share_info_1[i];
			const bool disk = (si.type & 3) == SHARE_TYPE_DISKTREE;
			const bool hidden = (si.type & SHARE_TYPE_HIDDEN) != 0;
			if (disk && !hidden && si.netname.utf8)
				out.push_back(si.netname.utf8);
		}
		smb2_free_data(ipc.m_ctx, rep);
		return Status::Ok();
	}

private:
	bool Usable() const { return m_ctx && m_connected && !m_dead && !m_aborted; }

	// Services the socket until done() holds. 0, or -1 when the connection is dead (aborted, silent for the
	// idle limit, or closed by the server).
	template <class Done> int Pump(Done done, int idle_ms = -1)
	{
		const int limit = idle_ms >= 0 ? idle_ms : g_idle_ms.load();
		auto quiet_since = Clock::now();
		while (!done())
		{
			if (m_aborted.load())
				return Die();
			pollfd p = {smb2_get_fd(m_ctx), short(smb2_which_events(m_ctx)), 0};
			const int n = poll(&p, 1, kTickMs);
			if (n < 0 && errno != EINTR)
				return Die();
			if (n > 0)
			{
				quiet_since = Clock::now();
				if (smb2_service(m_ctx, p.revents) < 0)
				{
					// libsmb2 has already failed every waiting request through its callback
					m_dead = true;
					return done() ? 0 : -1;
				}
			}
			else if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - quiet_since).count() >= limit)
			{
				m_timed_out = true;
				return Die();
			}
		}
		return 0;
	}

	int Die()
	{
		m_dead = true;
		return -1;
	}

	// Waits for the reply of an async call libsmb2 accepted (queued == 0). Returns its status: 0 or -errno.
	int Await(std::unique_ptr<Req>& r, int queued)
	{
		if (queued < 0)
			return queued;
		if (Pump([&] { return r->done; }) != 0)
		{
			m_graveyard.push_back(std::move(r));
			return -EIO;
		}
		if (uint32_t(r->status) == SMB2_STATUS_SHUTDOWN)
			return -EIO;
		return r->status < 0 ? r->status : (r->status > 0 ? -EIO : 0);
	}

	// One try at connecting with one identity. On failure the context is gone again.
	Status Attempt(const std::string& server, const std::string& share, const char* user, const char* password)
	{
		const ShareConfig& cfg = *m_cfg;
		m_dead = false;
		m_timed_out = false;
		m_ctx = smb2_init_context();
		if (!m_ctx)
			return Status::Kind(ErrKind::Other, "out of memory");
		smb2_set_version(m_ctx, Version(cfg.vers));
		smb2_set_security_mode(m_ctx, cfg.sign ? (SMB2_NEGOTIATE_SIGNING_ENABLED | SMB2_NEGOTIATE_SIGNING_REQUIRED) : SMB2_NEGOTIATE_SIGNING_ENABLED);
		if (cfg.sign)
			smb2_set_sign(m_ctx, 1);
		if (cfg.seal)
			smb2_set_seal(m_ctx, 1);
		if (!cfg.domain.empty())
			smb2_set_domain(m_ctx, cfg.domain.c_str());
		if (password)
			smb2_set_password(m_ctx, password);
		if (user && *user)
			smb2_set_user(m_ctx, user);

		std::unique_ptr<Req> r(new Req);
		int rc = smb2_connect_share_async(m_ctx, server.c_str(), share.c_str(), user && *user ? user : nullptr, OnDone, r.get());
		if (rc == 0)
			rc = Pump([&] { return r->done; }) == 0 ? r->status : -EIO;
		if (uint32_t(rc) == SMB2_STATUS_SHUTDOWN)
			rc = -ECONNRESET;
		if (rc == 0)
		{
			m_connected = true;
			return Status::Ok();
		}
		const Status st = Classify(rc);
		smb2_destroy_context(m_ctx); // calls back `r` (still alive here) if a request was left queued
		m_ctx = nullptr;
		m_connected = false;
		return st;
	}

	// Turns a failed connect into the player-visible kind.
	Status Classify(int rc)
	{
		const uint32_t nt = uint32_t(smb2_get_nterror(m_ctx));
		const std::string text = Scrub(smb2_get_error(m_ctx));
		const int e = Errno(rc);
		if (m_aborted)
			return Status::Kind(ErrKind::Aborted, "cancelled");
		if (nt == SMB2_STATUS_BAD_NETWORK_NAME)
		{
			return Status::Kind(ErrKind::ShareNotFound, "no share with that name");
		}
		if (IsLogonFailure(nt))
			return Status::Kind(ErrKind::AccessDenied, "access denied or wrong password");
		if (text.find("Signing") != std::string::npos || text.find("signing") != std::string::npos)
			return Status::Kind(ErrKind::SigningRequired, "signing could not be set up (a guest session cannot sign, and the sign option needs a server that does)");
		if (nt == 0)
		{
			switch (e)
			{
			case ECONNREFUSED:
				return Status::Kind(ErrKind::Refused, "connection refused");
			case ETIMEDOUT:
			case EHOSTUNREACH:
			case ENETUNREACH:
			case ENETDOWN:
			case EHOSTDOWN:
				return Status::Kind(ErrKind::NoAnswer, "no answer from the server");
			default:
				break;
			}
			if (m_timed_out)
				return Status::Kind(ErrKind::NoAnswer, "no answer from the server");
			// The server took the connection, then said nothing SMB 2 can use: it may only speak SMB 1
			if (ServerSpeaksSmb1(m_ip, m_port))
				return Status::Kind(ErrKind::Smb1Only, "the server only speaks SMB 1");
			if (e == ECONNRESET || e == EPIPE || e == ECONNABORTED || e == EIO || e == ENOTCONN)
				return Status::Kind(ErrKind::Closed, "the server closed the connection");
		}
		return Status::Kind(ErrKind::Other, text.empty() ? "unknown error" : text);
	}

	// Library text must never carry the password out.
	std::string Scrub(const char* text) const
	{
		std::string t = text ? text : "";
		const std::string& pw = m_cfg ? m_cfg->password : std::string();
		if (!pw.empty())
			for (size_t at = t.find(pw); at != std::string::npos; at = t.find(pw, at + 3))
				t.replace(at, pw.size(), "***");
		while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
			t.pop_back();
		return t;
	}

	smb2_context* m_ctx = nullptr;
	const ShareConfig* m_cfg = nullptr;
	std::string m_root;   // the folder inside the share
	std::string m_server; // "a.b.c.d:port"
	in_addr m_ip = {};
	int m_port = kDefaultPort;
	uint32_t m_chunk = kChunkMin;
	bool m_connected = false;
	bool m_dead = false;
	bool m_timed_out = false;
	std::atomic<bool> m_aborted{false};
	std::set<File*> m_files; // open, not yet closed
	std::vector<std::unique_ptr<Req>> m_graveyard;
};
} // namespace

// Test hook (declared where it is used): the silence after which a call gives up, in ms.
void SmbSetIdleTimeoutMs(int ms)
{
	g_idle_ms.store(ms);
}

std::unique_ptr<Backend> MakeSmbBackend()
{
	return std::make_unique<SmbBackend>();
}
} // namespace netfs

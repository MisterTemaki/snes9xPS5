// Snes9x PS5: tests of the SMB backend (netfs/backend_smb.cpp) against a real Samba server, with a TCP proxy
// in between that can hang, cut or slow the link. Run by tests/smb/run.sh, which starts Samba.
// SPDX-License-Identifier: MIT

#include "netfs_backend.h"

#include <arpa/inet.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace netfs
{
void SmbSetIdleTimeoutMs(int ms);
}
using namespace netfs;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
static void Ok(bool c, const char* what)
{
	std::printf("%s: %s\n", c ? "ok" : "FAIL", what);
	if (!c)
		g_fail++;
}
#define CHECK(cond, what) Ok((cond), what)
static void Why(const Status& st)
{
	if (!st.ok())
		std::printf("info: kind %d, errno %d, text '%s'\n", int(st.kind), st.err, st.text.c_str());
}

static long Ms(Clock::time_point since)
{
	return long(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

// A TCP proxy to the real server on the loopback. Hang() stops forwarding without closing anything (a server
// that went silent), Cut() closes every connection that is open now (a cable pull or a crash), Latency() delays
// every chunk in each direction.
class Proxy
{
public:
	explicit Proxy(int upstream_port) : m_upstream(upstream_port)
	{
		m_listen = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(m_listen, reinterpret_cast<sockaddr*>(&a), sizeof(a));
		socklen_t len = sizeof(a);
		getsockname(m_listen, reinterpret_cast<sockaddr*>(&a), &len);
		m_port = ntohs(a.sin_port);
		listen(m_listen, 4);
		m_thread = std::thread([this] { Run(); });
	}
	~Proxy()
	{
		m_stop = true;
		m_thread.join();
		for (auto& t : m_pipes)
			t.join();
		close(m_listen);
	}
	int port() const { return m_port; }
	void Hang(bool on) { m_hang = on; }
	void Cut() { m_cut_gen++; }
	void Latency(int ms) { m_latency = ms; }
	void Reset()
	{
		m_hang = false;
		m_latency = 0;
	}

private:
	struct Chunk
	{
		Clock::time_point ready;
		std::vector<char> data;
	};
	void Run()
	{
		while (!m_stop)
		{
			pollfd l = {m_listen, POLLIN, 0};
			if (poll(&l, 1, 20) <= 0)
				continue;
			int c = accept(m_listen, nullptr, nullptr);
			int s = socket(AF_INET, SOCK_STREAM, 0);
			sockaddr_in a = {};
			a.sin_family = AF_INET;
			a.sin_port = htons(uint16_t(m_upstream));
			a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (c < 0 || s < 0 || connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
			{
				if (c >= 0)
					close(c);
				if (s >= 0)
					close(s);
				continue;
			}
			int one = 1;
			setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			const int gen = m_cut_gen.load();
			m_pipes.emplace_back([this, c, s, gen] {
				Pipe(c, s, gen);
				close(c);
				close(s);
			});
		}
	}
	void Pipe(int c, int s, int gen)
	{
		std::deque<Chunk> to_s, to_c;
		int fd[2] = {c, s};
		std::deque<Chunk>* q[2] = {&to_s, &to_c}; // q[i] is filled from fd[i] and drained to fd[1 - i]
		while (!m_stop && m_cut_gen.load() == gen)
		{
			pollfd p[2] = {{fd[0], POLLIN, 0}, {fd[1], POLLIN, 0}};
			if (poll(p, 2, 5) < 0)
				return;
			for (int i = 0; i < 2; i++)
			{
				if (!m_hang && (p[i].revents & (POLLIN | POLLHUP)))
				{
					char buf[65536];
					const ssize_t n = recv(fd[i], buf, sizeof(buf), 0);
					if (n <= 0)
						return;
					q[i]->push_back({Clock::now() + std::chrono::milliseconds(m_latency.load()), std::vector<char>(buf, buf + n)});
				}
				while (!m_hang && !q[i]->empty() && q[i]->front().ready <= Clock::now())
				{
					const auto& d = q[i]->front().data;
					size_t off = 0;
					while (off < d.size())
					{
						const ssize_t w = send(fd[1 - i], d.data() + off, d.size() - off, MSG_NOSIGNAL);
						if (w <= 0)
							return;
						off += size_t(w);
					}
					q[i]->pop_front();
				}
			}
			if (m_hang)
				usleep(2000); // a hung link has nothing to read; do not spin
		}
	}
	int m_upstream, m_listen = -1, m_port = 0;
	std::atomic<bool> m_stop{false}, m_hang{false};
	std::atomic<int> m_latency{0}, m_cut_gen{0};
	std::thread m_thread;
	std::vector<std::thread> m_pipes;
};

static std::string Env(const char* n, const char* d = "")
{
	const char* v = getenv(n);
	return v ? v : d;
}

static std::vector<char> ReadLocal(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static ShareConfig Cfg(int port, const char* share = "games", const char* path = "snes")
{
	ShareConfig c;
	c.scheme = Scheme::Smb;
	c.host = "127.0.0.1";
	c.port = port;
	c.share = share;
	c.path = path;
	return c;
}

// Reads a whole remote file the way netfs does: blocks of 512 KiB, asking again after a short count.
static bool ReadAll(Backend& b, const std::string& path, std::vector<char>& out, uint64_t* reads = nullptr)
{
	Handle h = nullptr;
	if (!b.Open(path, h).ok())
		return false;
	Attr a;
	b.Stat(path, a);
	out.assign(a.size, 0);
	uint64_t off = 0;
	while (off < a.size)
	{
		const int64_t n = b.Pread(h, out.data() + off, std::min<uint64_t>(512 * 1024, a.size - off), off);
		if (reads)
			(*reads)++;
		if (n <= 0)
		{
			b.Close(h);
			return false;
		}
		off += uint64_t(n);
	}
	b.Close(h);
	return true;
}

int main()
{
	const int port = atoi(Env("SMB_PORT", "0").c_str());
	const std::string root = Env("SMB_ROOT"); // the folder behind the share "games"
	const std::string user = Env("SMB_USER"), pass = Env("SMB_PASS");
	const std::string mode = Env("SMB_MODE", "main");
	if (!port || root.empty())
	{
		std::fprintf(stderr, "SMB_PORT and SMB_ROOT are required\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);

	if (mode == "smb1")
	{
		// the server only speaks SMB 1: a player-visible reason, not "unknown error"
		ShareConfig c = Cfg(port);
		auto b = MakeSmbBackend();
		Status st = b->Connect(c);
		Why(st);
		CHECK(st.kind == ErrKind::Smb1Only, "an SMB 1 only server is named as such");
		return g_fail ? 1 : 0;
	}
	if (mode == "sign" || mode == "seal")
	{
		ShareConfig c = Cfg(port);
		c.user = user;
		c.password = pass;
		c.seal = mode == "seal";
		auto b = MakeSmbBackend();
		Status st = b->Connect(c);
		CHECK(st.ok(), mode == "sign" ? "connects to a server that requires signing" : "connects with encryption (seal)");
		std::vector<char> got;
		CHECK(ReadAll(*b, "big.sfc", got) && got == ReadLocal(root + "/snes/big.sfc"), "a 6 MiB ROM reads identically on that link");
		auto g = MakeSmbBackend();
		ShareConfig guest = Cfg(port);
		st = g->Connect(guest);
		CHECK(!st.ok(), "guest access to that server is refused");
		return g_fail ? 1 : 0;
	}

	Proxy proxy(port);

	// ---- guest: connect, list, stat, read
	{
		ShareConfig c = Cfg(proxy.port());
		auto b = MakeSmbBackend();
		Status st = b->Connect(c);
		Why(st);
		CHECK(st.ok(), "guest connect to share/folder");

		std::vector<DirEntry> list;
		CHECK(b->List("", list).ok(), "list the folder");
		bool has_big = false, has_utf8 = false, has_dir = false, dots = false;
		for (const auto& e : list)
		{
			has_big |= e.name == "big.sfc" && !e.attr.is_dir && e.attr.size == 6 * 1024 * 1024;
			has_utf8 |= e.name == "Pok\xc3\xa9mon Test.sfc";
			has_dir |= e.name == "sub" && e.attr.is_dir && e.attr.size == 0;
			dots |= e.name == "." || e.name == "..";
		}
		CHECK(has_big && has_utf8 && has_dir && !dots, "list gives names, types and sizes (UTF-8 name, folder, no dots)");
		CHECK(list.size() >= 300, "a folder of 300+ entries lists completely");

		Attr a;
		CHECK(b->Stat("big.sfc", a).ok() && !a.is_dir && a.size == 6 * 1024 * 1024 && a.mtime > 1000000000, "stat a file: size and mtime");
		CHECK(b->Stat("sub/deep", a).ok() && a.is_dir, "stat a nested folder");
		CHECK(b->Stat("", a).ok() && a.is_dir, "stat the location itself");
		CHECK(b->Stat("nothere.sfc", a).err == ENOENT, "stat of a missing file is ENOENT");
		CHECK(b->List("nothere", list).err == ENOENT || b->List("nothere", list).err == ENOTDIR, "list of a missing folder fails");

		Handle h = nullptr;
		CHECK(b->Open("sub", h).err == EISDIR || !b->Open("sub", h).ok(), "open of a folder fails");
		CHECK(b->Open("nothere.sfc", h).err == ENOENT, "open of a missing file is ENOENT");

		std::vector<char> got;
		uint64_t reads = 0;
		const auto t0 = Clock::now();
		CHECK(ReadAll(*b, "big.sfc", got, &reads), "read a 6 MiB ROM in 512 KiB blocks");
		CHECK(got == ReadLocal(root + "/snes/big.sfc"), "the 6 MiB ROM is byte-identical");
		std::printf("info: 6 MiB in %ld ms (%llu reads)\n", Ms(t0), (unsigned long long)reads);
		{
			std::vector<char> again;
			const auto t1 = Clock::now();
			ReadAll(*b, "big.sfc", again);
			std::printf("info: the same file again: %ld ms\n", Ms(t1));
		}

		// edges
		CHECK(b->Open("big.sfc", h).ok(), "open big.sfc");
		std::vector<char> ref = ReadLocal(root + "/snes/big.sfc");
		std::vector<char> buf(1024 * 1024);
		int64_t n = b->Pread(h, buf.data(), 1024 * 1024, 100);
		CHECK(n > 0 && std::memcmp(buf.data(), ref.data() + 100, size_t(n)) == 0, "unaligned 1 MiB read matches");
		n = b->Pread(h, buf.data(), 4096, ref.size() - 1000);
		CHECK(n == 1000 && std::memcmp(buf.data(), ref.data() + ref.size() - 1000, 1000) == 0, "a read across the end returns the tail");
		CHECK(b->Pread(h, buf.data(), 4096, ref.size()) == 0, "a read at the end returns 0");
		CHECK(b->Pread(h, buf.data(), 4096, ref.size() + 5000) == 0, "a read past the end returns 0");
		CHECK(b->Pread(h, buf.data(), 0, 0) == 0, "a zero-length read returns 0");
		b->Close(h);
		CHECK(b->Open("empty.bin", h).ok() && b->Pread(h, buf.data(), 100, 0) == 0, "an empty file reads as empty");
		b->Close(h);
		CHECK(b->Open("one.bin", h).ok() && b->Pread(h, buf.data(), 100, 0) == 1 && buf[0] == 'x', "a one-byte file reads one byte");
		b->Close(h);
		for (int i = 0; i < 200; i++)
		{
			Handle t = nullptr;
			if (!b->Open("one.bin", t).ok())
				break;
			b->Close(t);
		}
		CHECK(b->Stat("one.bin", a).ok(), "200 opens and closes leave the connection healthy");
		b->Disconnect();
		CHECK(b->Stat("one.bin", a).err == EIO, "after Disconnect calls fail");
	}

	// ---- latency: a pipelined read does not pay a round trip per 64 KiB
	{
		ShareConfig c = Cfg(proxy.port());
		auto b = MakeSmbBackend();
		CHECK(b->Connect(c).ok(), "connect for the latency test");
		proxy.Latency(25); // 50 ms round trip
		Handle h = nullptr;
		b->Open("big.sfc", h);
		std::vector<char> buf(1024 * 1024);
		const auto t0 = Clock::now();
		const int64_t n = b->Pread(h, buf.data(), buf.size(), 0);
		const long ms = Ms(t0);
		proxy.Latency(0);
		CHECK(n == int64_t(buf.size()), "1 MiB comes back whole over a 50 ms link");
		std::printf("info: 1 MiB over a 50 ms round trip: %ld ms\n", ms);
		CHECK(ms < 600, "and in a few round trips, not one per request");
		b->Close(h);
	}

	// ---- SMB 2.02 (64 KiB per request)
	{
		ShareConfig c = Cfg(proxy.port());
		c.vers = "2.02";
		auto b = MakeSmbBackend();
		Status st = b->Connect(c);
		CHECK(st.ok(), "connect forcing SMB 2.02");
		std::vector<char> got;
		CHECK(ReadAll(*b, "big.sfc", got) && got == ReadLocal(root + "/snes/big.sfc"), "SMB 2.02: the 6 MiB ROM is byte-identical");
	}

	// ---- credentials
	if (!user.empty())
	{
		ShareConfig c = Cfg(proxy.port(), "private", "");
		c.user = user;
		c.password = pass;
		auto b = MakeSmbBackend();
		Status st = b->Connect(c);
		CHECK(st.ok(), "user and password (special characters in the password)");
		std::vector<DirEntry> l;
		CHECK(b->List("", l).ok() && l.size() == 1, "list the private share");

		ShareConfig bad = c;
		bad.password = "wrong" + pass;
		auto b2 = MakeSmbBackend();
		st = b2->Connect(bad);
		Why(st);
		CHECK(st.kind == ErrKind::AccessDenied, "a wrong password is AccessDenied");
		CHECK(st.text.find(bad.password) == std::string::npos && st.text.find(pass) == std::string::npos, "and the message does not contain the password");

		ShareConfig guest = Cfg(proxy.port(), "private", "");
		auto b3 = MakeSmbBackend();
		st = b3->Connect(guest);
		Why(st);
		CHECK(st.kind == ErrKind::AccessDenied, "guest on a private share is AccessDenied");
	}

	// ---- not found
	{
		ShareConfig c = Cfg(proxy.port(), "gamez", "");
		auto b = MakeSmbBackend();
		Status st = b->Connect(c);
		CHECK(st.kind == ErrKind::ShareNotFound, "a wrong share name is ShareNotFound");
		std::vector<std::string> names;
		CHECK(b->Enumerate(names).ok(), "the shares can be listed");
		bool games = false;
		for (auto& n : names)
			games |= n == "games";
		CHECK(games, "the list names the real share");

		c = Cfg(proxy.port(), "games", "no/such/folder");
		auto b2 = MakeSmbBackend();
		CHECK(b2->Connect(c).kind == ErrKind::FolderNotFound, "a wrong folder is FolderNotFound");
		c = Cfg(proxy.port(), "games", "snes/big.sfc");
		auto b3 = MakeSmbBackend();
		st = b3->Connect(c);
		Why(st);
		CHECK(st.kind == ErrKind::FolderNotFound, "a file as the folder is FolderNotFound");
	}

	// ---- no server
	{
		SmbSetIdleTimeoutMs(500);
		ShareConfig c = Cfg(1); // nothing listens on port 1
		auto b = MakeSmbBackend();
		CHECK(b->Connect(c).kind == ErrKind::Refused, "a closed port is Refused");
		c = Cfg(445);
		c.host = "no-such-host.invalid";
		auto b2 = MakeSmbBackend();
		CHECK(b2->Connect(c).kind == ErrKind::NameNotFound, "an unknown name is NameNotFound");
		c = Cfg(445);
		c.host = "198.51.100.1"; // TEST-NET-2: nobody answers
		auto b3 = MakeSmbBackend();
		const auto t0 = Clock::now();
		Status st = b3->Connect(c);
		CHECK(st.kind == ErrKind::NoAnswer || st.kind == ErrKind::Other, "a silent address is NoAnswer");
		CHECK(Ms(t0) < 3000, "and gives up inside the idle limit");
		SmbSetIdleTimeoutMs(4000);
	}

	// ---- the link dies
	{
		SmbSetIdleTimeoutMs(400);
		ShareConfig c = Cfg(proxy.port());
		auto b = MakeSmbBackend();
		CHECK(b->Connect(c).ok(), "connect for the hang test");
		Handle h = nullptr;
		b->Open("big.sfc", h);
		std::vector<char> buf(512 * 1024);
		CHECK(b->Pread(h, buf.data(), buf.size(), 0) == int64_t(buf.size()), "read before the hang");
		proxy.Hang(true);
		auto t0 = Clock::now();
		int64_t n = b->Pread(h, buf.data(), buf.size(), 512 * 1024);
		CHECK(n == -EIO, "a read on a silent link fails with EIO");
		CHECK(Ms(t0) < 2500, "after the idle limit, not a minute");
		t0 = Clock::now();
		Attr a;
		CHECK(b->Stat("big.sfc", a).err == EIO && Ms(t0) < 50, "later calls fail at once");
		b->Close(h);
		b->Disconnect();
		proxy.Hang(false);
		proxy.Cut();
		usleep(100000);
		proxy.Reset();

		// Abort() from another thread cuts a call that is waiting
		SmbSetIdleTimeoutMs(30000);
		auto b2 = MakeSmbBackend();
		CHECK(b2->Connect(c).ok(), "connect for the abort test");
		Handle h2 = nullptr;
		b2->Open("big.sfc", h2);
		proxy.Hang(true);
		std::thread killer([&] {
			usleep(300000);
			b2->Abort();
		});
		t0 = Clock::now();
		n = b2->Pread(h2, buf.data(), buf.size(), 0);
		killer.join();
		CHECK(n == -EIO, "Abort() makes the waiting read fail");
		CHECK(Ms(t0) >= 250 && Ms(t0) < 1500, "and promptly (inside a poll tick of the call)");
		CHECK(b2->Open("big.sfc", h2).err == EIO, "after Abort() every call fails");
		b2->Disconnect();
		proxy.Hang(false);
		proxy.Cut();
		usleep(100000);
		proxy.Reset();

		// the server closes the connection
		SmbSetIdleTimeoutMs(4000);
		auto b3 = MakeSmbBackend();
		CHECK(b3->Connect(c).ok(), "connect for the cut test");
		Handle h3 = nullptr;
		b3->Open("big.sfc", h3);
		proxy.Cut();
		usleep(200000);
		t0 = Clock::now();
		n = b3->Pread(h3, buf.data(), buf.size(), 0);
		CHECK(n == -EIO && Ms(t0) < 2000, "a closed connection fails reads quickly with EIO");
		b3->Disconnect();
	}

	std::printf("%s\n", g_fail ? "SMB backend tests: FAILED" : "SMB backend tests: all passed");
	return g_fail ? 1 : 0;
}

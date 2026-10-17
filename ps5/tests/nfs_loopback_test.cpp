// The NFS backend on REAL libnfs, against sockets on the loopback interface that misbehave on purpose: nothing
// listening, a server that hangs up, one that never answers, an Abort() from another thread. It checks the
// parts a mock cannot: what libnfs reports for each failure and that the wait loop reacts to it. A working NFS
// server is the optional integration job in the plan.
//
// SPDX-License-Identifier: MIT
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "../netfs/backend_nfs.h"
#include "../netfs/netfs_backend.h"

using namespace netfs;

static int g_fail = 0;

#define CHECK(cond)                                                                \
	do                                                                             \
	{                                                                              \
		if (!(cond))                                                               \
		{                                                                          \
			std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_fail++;                                                              \
		}                                                                          \
	} while (0)

static int Listen(int* port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	int on = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
	listen(s, 4);
	socklen_t l = sizeof a;
	getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
	*port = ntohs(a.sin_port);
	return s;
}

static int64_t RealMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int64_t FastMs() // 100 times faster than real time: the 4.5 s deadline passes in 45 ms
{
	return RealMs() * 100;
}

static ShareConfig Cfg(int port)
{
	ShareConfig c;
	c.scheme = Scheme::Nfs;
	c.host = "127.0.0.1";
	c.path = "export";
	c.nfs_version = 4; // straight to nfsport, no portmapper
	c.nfsport = port;
	return c;
}

// accepts one connection and holds it open until `stop`
struct Silent
{
	int listener = -1;
	int port = 0;
	std::atomic<bool> stop{false};
	std::thread t;
	Silent()
	{
		listener = Listen(&port);
		t = std::thread([this] {
			int c = accept(listener, nullptr, nullptr);
			while (!stop.load())
				usleep(5000);
			close(c);
		});
	}
	~Silent()
	{
		stop = true;
		shutdown(listener, SHUT_RDWR); // frees an accept() nobody connected to
		t.join();
		close(listener);
	}
};

static void RefusedPortIsRefused()
{
	int port;
	close(Listen(&port)); // nothing listens on it now
	ShareConfig cfg = Cfg(port);
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::Refused);
}

static void HangUpIsClosed()
{
	int port;
	int l = Listen(&port);
	std::thread t([l] { close(accept(l, nullptr, nullptr)); });
	ShareConfig cfg = Cfg(port);
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::Closed);
	t.join();
	close(l);
}

static void SilentServerIsNoAnswer()
{
	Silent srv;
	NfsTestHooks h;
	h.now_ms = FastMs;
	NfsSetTestHooks(h);
	ShareConfig cfg = Cfg(srv.port);
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::NoAnswer);
	NfsSetTestHooks(NfsTestHooks());
}

static void AbortFromAnotherThreadEndsConnectPromptly()
{
	Silent srv;
	ShareConfig cfg = Cfg(srv.port);
	auto b = MakeNfsBackend();
	Backend* raw = b.get();
	std::thread killer([raw] {
		usleep(150000);
		raw->Abort();
	});
	const int64_t t0 = RealMs();
	CHECK(b->Connect(cfg).kind == ErrKind::Aborted);
	CHECK(RealMs() - t0 < 2000); // not the 4.5 s deadline
	killer.join();
}

int main()
{
	RefusedPortIsRefused();
	HangUpIsClosed();
	SilentServerIsNoAnswer();
	AbortFromAnotherThreadEndsConnectPromptly();
	std::printf("%s (4 loopback checks)\n", g_fail ? "FAILED" : "ok");
	return g_fail ? 1 : 0;
}

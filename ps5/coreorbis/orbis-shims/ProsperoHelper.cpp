// Snes9x PS5: the helper payload's side of the jailbreak (ProsperoJailbreak.h).
//
// Runs in Snes9xPS5.elf (after it installs the app) and in Snes9xPS5-helper.elf (which the app sends to the
// ELF loader when no helper answers). One request at a time on 127.0.0.1:9083 only (9075 before 2.2, 9080 in 2.2).
//
// What the jailbreak changes in the Snes9x PS5 process, with the payload SDK's kernel access (the same calls
// ps5-payload-dev's elfldr makes for the payloads it starts): the root and jail folders become the kernel's
// root (so /data and /mnt/usbN are visible), uid/gid 0, and every SCE capability. The authid stays the
// app's own, so the system keeps treating it as the foreground title (pads, video, sound).
//
// SPDX-License-Identifier: MIT

#include "ProsperoJailbreak.h"

#include "OrbisPaths.h"
#include "fe_coverworker.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef SNES9X_TITLE_ID
#define SNES9X_TITLE_ID "PPSA99009"
#endif

#ifdef __PROSPERO__
#include <ps5/kernel.h>
extern "C" int sceKernelGetAppInfo(int pid, void* info);
#endif

namespace jailbreak
{
namespace
{
// The first PPSA/CUSA-style title id (4 letters + 5 digits) in a buffer, or "".
std::string FindTitleId(const unsigned char* p, size_t n)
{
	for (size_t i = 0; i + 9 <= n; i++)
	{
		bool ok = true;
		for (size_t k = 0; k < 4 && ok; k++)
			ok = p[i + k] >= 'A' && p[i + k] <= 'Z';
		for (size_t k = 4; k < 9 && ok; k++)
			ok = p[i + k] >= '0' && p[i + k] <= '9';
		if (ok)
			return std::string(reinterpret_cast<const char*>(p + i), 9);
	}
	return "";
}

// The title the process belongs to: sceKernelGetAppInfo's title id. "" when it can't be told.
std::string TitleOf(int pid)
{
#ifdef __PROSPERO__
	unsigned char info[512];
	memset(info, 0, sizeof(info));
	const int rc = sceKernelGetAppInfo(pid, info);
	if (rc != 0)
	{
		OrbisLog("[helper] sceKernelGetAppInfo(%d) -> %x", pid, unsigned(rc));
		return "";
	}
	return FindTitleId(info, sizeof(info));
#else
	// host: the test says which title the asking process is
	if (kill(pid, 0) != 0)
		return "";
	const char* t = getenv("SNES9X_HOST_JB_TITLE");
	const std::string title = t ? t : SNES9X_TITLE_ID;
	return FindTitleId(reinterpret_cast<const unsigned char*>(title.data()), title.size());
#endif
}
} // namespace

bool JailbreakProcess(int pid, std::string& why)
{
	if (pid <= 0)
	{
		why = "bad pid";
		return false;
	}
	// Fail closed: a process whose title can't be read (a payload, a daemon, an exited pid) is refused, as is
	// any other title. The requester can claim any pid, so the title is the only thing that decides.
	const std::string title = TitleOf(pid);
	if (title != SNES9X_TITLE_ID)
	{
		why = title.empty() ? "title unknown: not Snes9x PS5" : "title " + title + " is not Snes9x PS5";
		return false;
	}
	OrbisLog("[helper] pid %d (title %s): letting it out", pid, title.c_str());
#ifdef __PROSPERO__
	if (kernel_get_proc(pid) == 0)
	{
		why = "no such process";
		return false;
	}
	uint8_t caps[16];
	memset(caps, 0xff, sizeof(caps));
	const intptr_t root = kernel_get_root_vnode();
	const int r_caps = kernel_set_ucred_caps(pid, caps);
	const int r_uid = kernel_set_ucred_uid(pid, 0);
	const int r_ruid = kernel_set_ucred_ruid(pid, 0);
	const int r_svuid = kernel_set_ucred_svuid(pid, 0);
	const int r_rgid = kernel_set_ucred_rgid(pid, 0);
	const int r_svgid = kernel_set_ucred_svgid(pid, 0);
	const int r_root = root ? kernel_set_proc_rootdir(pid, root) : -1;
	const int r_jail = root ? kernel_set_proc_jaildir(pid, root) : -1;
	OrbisLog("[helper]   caps %d, uid %d/%d/%d, gid %d/%d, root vnode %lx: rootdir %d, jaildir %d", r_caps, r_uid,
		r_ruid, r_svuid, r_rgid, r_svgid, (unsigned long)root, r_root, r_jail);
	if (r_root != 0)
	{
		why = "could not change the process's root folder";
		return false;
	}
	if (r_caps != 0 || r_uid != 0 || r_ruid != 0 || r_svuid != 0 || r_rgid != 0 || r_svgid != 0 || r_jail != 0)
		why = "ok, but some rights were not changed (see the helper's log)"; // reported, not fatal: the root folder is
#endif
	return true;
}

namespace
{
// Every connection gets this long in total, however slowly the other side sends: one slow client can't hold the
// (single) serving loop.
constexpr int kConnectionMs = 2000;

int64_t NowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Waits for fd to be readable (events = POLLIN) or writable (POLLOUT) before the deadline.
bool WaitFd(int fd, short events, int64_t deadline)
{
	for (;;)
	{
		const int64_t left = deadline - NowMs();
		if (left <= 0)
			return false;
		pollfd p = {fd, events, 0};
		const int r = poll(&p, 1, int(left));
		if (r > 0)
			return true;
		if (r == 0 || errno != EINTR)
			return false;
	}
}

size_t RecvUntil(int fd, void* buf, size_t size, int64_t deadline)
{
	size_t got = 0;
	while (got < size && WaitFd(fd, POLLIN, deadline))
	{
		const ssize_t n = recv(fd, static_cast<char*>(buf) + got, size - got, 0);
		if (n <= 0)
			break;
		got += size_t(n);
	}
	return got;
}

bool SendUntil(int fd, const void* buf, size_t size, int64_t deadline)
{
	const char* p = static_cast<const char*>(buf);
	while (size > 0)
	{
		if (!WaitFd(fd, POLLOUT, deadline))
			return false;
		const ssize_t n = send(fd, p, size, 0);
		if (n <= 0)
			return false;
		p += n;
		size -= size_t(n);
	}
	return true;
}

// covers/wanted.txt: a regular file of ours (symbolic links refused), at most kMaxWantedBytes, cut at a line end.
std::string ReadWantedList()
{
	std::string text;
	const int fd = open((OrbisDir("covers") + "/wanted.txt").c_str(), O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return text;
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
	{
		char buf[16384];
		ssize_t n;
		while (text.size() <= size_t(kMaxWantedBytes) && (n = read(fd, buf, sizeof(buf))) > 0)
			text.append(buf, size_t(n));
		if (text.size() > size_t(kMaxWantedBytes))
		{
			const size_t cut = text.rfind('\n', size_t(kMaxWantedBytes) - 1);
			text.resize(cut == std::string::npos ? 0 : cut + 1); // whole lines only
		}
	}
	close(fd);
	return text;
}
} // namespace

bool ServeHelper(void (*on_ready)())
{
	signal(SIGPIPE, SIG_IGN);
	const char* env = getenv("SNES9X_HELPER_PORT");
	const int port = (env && *env) ? atoi(env) : kHelperPort;
	const int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0)
	{
		OrbisLog("[helper] socket: errno %d", errno);
		return false;
	}
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
#ifdef __FreeBSD__
	sa.sin_len = sizeof(sa);
#endif
	sa.sin_family = AF_INET;
	sa.sin_port = htons(uint16_t(port));
	sa.sin_addr.s_addr = htonl(0x7F000001); // this console only
	if (bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || listen(srv, 4) != 0)
	{
		OrbisLog("[helper] port %d is taken (errno %d): a helper is already running", port, errno);
		close(srv);
		return false;
	}
	OrbisLog("[helper] listening on 127.0.0.1:%d, pid %d", port, int(getpid()));
	if (on_ready)
		on_ready();
	// the covers the app wants, downloaded while it runs (fe_coverworker.h)
	fe::StartCoverWorker();
	for (;;)
	{
		const int c = accept(srv, nullptr, nullptr);
		if (c < 0)
		{
			if (errno == EINTR)
				continue;
			OrbisLog("[helper] accept: errno %d", errno);
			usleep(200 * 1000);
			continue;
		}
		OrbisLogRefresh(); // the "Debug logs" setting may have changed since the helper started
		const int64_t deadline = NowMs() + kConnectionMs;
		Request req;
		memset(&req, 0, sizeof(req));
		const size_t got = RecvUntil(c, &req, sizeof(req), deadline);
		if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdWantedCovers)
		{
			// covers/wanted.txt, written by the app after its last scan: a fixed file of ours, nothing else
			const std::string text = ReadWantedList();
			req.ret = int32_t(text.size());
			// this helper downloads them itself, in the background: the app has nothing to wait for
			memset(req.msg2, 0, sizeof(req.msg2));
			snprintf(req.msg2, sizeof(req.msg2), "%s", kBackgroundCovers);
			OrbisLog("[helper] pid %d: wanted-covers list, %zu bytes", req.pid, text.size());
			if (SendUntil(c, &req, sizeof(req), deadline))
				SendUntil(c, text.data(), text.size(), deadline);
		}
		else if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdJailbreak)
		{
			std::string why;
			const bool ok = JailbreakProcess(req.pid, why);
			req.ret = ok ? 0 : -1;
			memset(req.msg1, 0, sizeof(req.msg1));
			snprintf(req.msg1, sizeof(req.msg1), "%s", ok ? "ok" : why.c_str());
			OrbisLog("[helper] pid %d: %s", req.pid, req.msg1);
			SendUntil(c, &req, sizeof(req), deadline);
		}
		else
			OrbisLog("[helper] ignored a request of %zu bytes (magic %x, cmd %d)", got, req.magic, req.cmd);
		close(c);
	}
}
} // namespace jailbreak

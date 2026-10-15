// Snes9x PS5: the app's side of the jailbreak (ProsperoJailbreak.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoJailbreak.h"

#include "OrbisPaths.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace jailbreak
{
__attribute__((weak)) Blob EmbeddedHelper()
{
	return {nullptr, 0};
}

namespace
{
int PortFromEnv(const char* name, int fallback)
{
	const char* v = getenv(name);
	return (v && *v) ? atoi(v) : fallback;
}

// A TCP connection to 127.0.0.1:port with 5 s send/receive timeouts, or -1 (errno kept).
int ConnectLocal(int port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	timeval tv = {5, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
#ifdef __FreeBSD__
	sa.sin_len = sizeof(sa);
#endif
	sa.sin_family = AF_INET;
	sa.sin_port = htons(uint16_t(port));
	sa.sin_addr.s_addr = htonl(0x7F000001);
	if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0)
	{
		const int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

bool WriteAll(int fd, const void* data, size_t size)
{
	const char* p = static_cast<const char*>(data);
	while (size > 0)
	{
		const ssize_t n = send(fd, p, size > 65536 ? 65536 : size, 0);
		if (n <= 0)
			return false;
		p += n;
		size -= size_t(n);
	}
	return true;
}

// One request to one daemon. 1 = yes, 0 = no answer on that port, -1 = it answered no.
// strict (our own helper): only a whole reply with our magic and ret 0 counts. The other daemons get PS5SX2's
// leniency (etaHEN's legacy server can answer without touching ret).
int Ask(int port, const char* who, bool strict)
{
	const int fd = ConnectLocal(port);
	if (fd < 0)
	{
		OrbisLog("[jailbreak] %s (port %d): nobody there (errno %d)", who, port, errno);
		return 0;
	}
	Request req;
	memset(&req, 0, sizeof(req));
	req.magic = kMagic;
	req.cmd = kCmdJailbreak;
	req.pid = int32_t(getpid());
	req.ret = kRetUntouched;
	if (!WriteAll(fd, &req, sizeof(req)))
	{
		OrbisLog("[jailbreak] %s (port %d): send failed (errno %d)", who, port, errno);
		close(fd);
		return 0;
	}
	size_t got = 0;
	while (got < sizeof(req))
	{
		const ssize_t n = recv(fd, reinterpret_cast<char*>(&req) + got, sizeof(req) - got, 0);
		if (n <= 0)
			break;
		got += size_t(n);
	}
	close(fd);
	req.msg1[sizeof(req.msg1) - 1] = 0;
	OrbisLog("[jailbreak] %s (port %d): ret %d, %zu bytes back%s%s", who, port, req.ret, got, req.msg1[0] ? ": " : "",
		req.msg1);
	if (strict)
		return got == sizeof(req) && req.magic == kMagic && req.ret == 0 ? 1 : -1;
	// PS5SX2 accepts an untouched ret from etaHEN's legacy server too (orbis_try_jailbreak)
	if (req.ret == 0 || (req.ret == kRetUntouched && got > 0))
		return 1;
	return -1;
}

bool g_started_helper = false; // this process sent the helper to the ELF loader

// Hands the built-in helper to the ELF loader. True when every byte went out.
bool SendHelper()
{
	const Blob blob = EmbeddedHelper();
	if (!blob.data || blob.size == 0)
	{
		OrbisLog("[jailbreak] no helper built into this binary");
		return false;
	}
	const int port = PortFromEnv("SNES9X_ELFLDR_PORT", kElfLoaderPort);
	const int fd = ConnectLocal(port);
	if (fd < 0)
	{
		OrbisLog("[jailbreak] ELF loader (port %d) not there (errno %d)", port, errno);
		return false;
	}
	const bool ok = WriteAll(fd, blob.data, blob.size);
	close(fd);
	g_started_helper = g_started_helper || ok;
	OrbisLog("[jailbreak] helper (%zu bytes) sent to the ELF loader on port %d: %s", blob.size, port, ok ? "ok" : "failed");
	return ok;
}
} // namespace

namespace
{
// 1: the list came back (maybe empty), 0: nobody on the port, -1: a helper answered but not with a list.
int AskWanted(int port, std::string& text, bool* background)
{
	text.clear();
	*background = false;
	const int fd = ConnectLocal(port);
	if (fd < 0)
		return 0;
	Request req;
	memset(&req, 0, sizeof(req));
	req.magic = kMagic;
	req.cmd = kCmdWantedCovers;
	req.pid = int32_t(getpid());
	req.ret = kRetUntouched;
	if (!WriteAll(fd, &req, sizeof(req)))
	{
		close(fd);
		return 0;
	}
	size_t got = 0;
	while (got < sizeof(req))
	{
		const ssize_t n = recv(fd, reinterpret_cast<char*>(&req) + got, sizeof(req) - got, 0);
		if (n <= 0)
			break;
		got += size_t(n);
	}
	if (got != sizeof(req) || req.magic != kMagic || req.ret < 0 || req.ret > kMaxWantedBytes)
	{
		close(fd);
		OrbisLog("[prefetch] helper answered %zu bytes, ret %d: no list", got, req.ret);
		return -1;
	}
	req.msg2[sizeof(req.msg2) - 1] = 0;
	*background = strcmp(req.msg2, kBackgroundCovers) == 0;
	text.resize(size_t(req.ret));
	size_t have = 0;
	while (have < text.size())
	{
		const ssize_t n = recv(fd, &text[have], text.size() - have, 0);
		if (n <= 0)
			break;
		have += size_t(n);
	}
	close(fd);
	if (have != text.size())
	{
		// a cut list could end inside a URL (and turn into a 404 -> a cover marked missing for 30 days)
		OrbisLog("[prefetch] the list was cut short (%zu of %zu bytes): not used", have, text.size());
		text.clear();
		return -1;
	}
	return 1;
}
} // namespace

bool FetchWantedCovers(std::string& text, bool* background)
{
	bool bg = false;
	const int own = PortFromEnv("SNES9X_HELPER_PORT", kHelperPort);
	int r = AskWanted(own, text, &bg);
	if (r == 0 && SendHelper())
		for (int i = 0; i < 16 && r == 0; i++)
		{
			usleep(500 * 1000);
			r = AskWanted(own, text, &bg);
		}
	OrbisLog("[prefetch] wanted list from the helper: %s, %zu bytes%s", r == 1 ? "ok" : "unavailable", text.size(),
		r == 1 && bg ? " (the helper downloads in the background)" : "");
	if (background)
		*background = r == 1 && bg;
	return r == 1;
}

bool UpdateRequest(const char* what, std::string& reply)
{
	reply.clear();
	const int fd = ConnectLocal(PortFromEnv("SNES9X_HELPER_PORT", kHelperPort));
	if (fd < 0)
		return false;
	Request req;
	memset(&req, 0, sizeof(req));
	req.magic = kMagic;
	req.cmd = kCmdUpdate;
	req.pid = int32_t(getpid());
	req.ret = kRetUntouched;
	snprintf(req.msg1, sizeof(req.msg1), "%s", what);
	snprintf(req.msg2, sizeof(req.msg2), "%s", SNES9X_PS5_VERSION); // the version installed: this app's
	if (!WriteAll(fd, &req, sizeof(req)))
	{
		close(fd);
		return false;
	}
	size_t got = 0;
	while (got < sizeof(req))
	{
		const ssize_t n = recv(fd, reinterpret_cast<char*>(&req) + got, sizeof(req) - got, 0);
		if (n <= 0)
			break;
		got += size_t(n);
	}
	close(fd);
	if (got != sizeof(req) || req.magic != kMagic || req.ret != 0)
		return false;
	req.msg1[sizeof(req.msg1) - 1] = 0;
	reply = req.msg1;
	return true;
}

bool RequestForSelf(std::string& how)
{
	const int own = PortFromEnv("SNES9X_HELPER_PORT", kHelperPort);
	OrbisLog("[jailbreak] pid %d asks to leave the sandbox", int(getpid()));
	if (Ask(own, "Snes9x helper", true) == 1)
	{
		how = g_started_helper ? "Snes9x helper (started by the app)" : "Snes9x helper";
		return true;
	}
	// PS5SX2's daemons (main-boot.cpp, orbis_try_jailbreak): etaHEN's legacy server, then 9069
	if (getenv("SNES9X_JB_NO_OTHERS") == nullptr)
	{
		if (Ask(9028, "etaHEN", false) == 1)
		{
			how = "etaHEN";
			return true;
		}
		if (Ask(9069, "jailbreak daemon 9069", false) == 1)
		{
			how = "jailbreak daemon 9069";
			return true;
		}
	}
	// nobody: start our own helper through the ELF loader, then ask it for ~8 s
	if (SendHelper())
	{
		for (int i = 0; i < 16; i++)
		{
			usleep(500 * 1000);
			const int r = Ask(own, "Snes9x helper", true);
			if (r == 1)
			{
				how = "Snes9x helper (started by the app)";
				return true;
			}
			if (r < 0)
				break;
		}
	}
	return false;
}
} // namespace jailbreak

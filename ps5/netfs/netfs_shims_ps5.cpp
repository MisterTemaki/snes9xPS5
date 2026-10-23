// Snes9x PS5 netfs: what libsmb2 and libnfs need from the console's libc and a native title cannot give them.
// Linked in with --wrap (NETFS_WRAPPED_PS5 in the Makefile), PS5 build only:
//
//  - fcntl(fd, F_SETFL/F_GETFL) on a socket: the console's own fcntl first (it works in SwanStationPS5's libnfs on
//    a console); if it refuses, setsockopt(SO_NBIO), the console's own switch, and getsockopt to read it back.
//  - getaddrinfo jumps to address 0 in a native title (the module that holds it is not loaded). This one takes
//    IPv4 literals itself and asks libSceNet's resolver for names, as fe_tlshttp.cpp does in the helper.
//    IPv4 only, one result. Its results are freed by the freeaddrinfo below.
//
// UNVERIFIED on a console (spike S2 in the plan): SO_NBIO's value, and that a native title may open the
// resolver. Both are Sony's documented names; the number is taken from the SDK headers of other homebrew.
//
// SPDX-License-Identifier: MIT
#ifdef __PROSPERO__

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ProsperoSce.h"

#ifndef SO_NBIO
#define SO_NBIO 0x1200
#endif

extern "C"
{
int sceNetResolverCreate(const char* name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char* hostname, uint32_t* addr, int timeout_us, int retries, int flags);
int sceNetResolverDestroy(int resolver);

int __real_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	long arg = va_arg(ap, long); // an int or a pointer; both travel in the same register
	va_end(ap);
	// The console's own fcntl first (SwanStationPS5's libnfs sets O_NONBLOCK on its sockets with it, and works);
	// SO_NBIO only if that is refused.
	const int r = __real_fcntl(fd, cmd, arg);
	if (r != -1 || (cmd != F_SETFL && cmd != F_GETFL))
		return r;
	if (cmd == F_SETFL)
	{
		int on = (arg & O_NONBLOCK) != 0;
		if (setsockopt(fd, SOL_SOCKET, SO_NBIO, &on, sizeof on) == 0)
			return 0;
	}
	else
	{
		int on = 0;
		socklen_t len = sizeof on;
		if (getsockopt(fd, SOL_SOCKET, SO_NBIO, &on, &len) == 0)
			return O_RDWR | (on ? O_NONBLOCK : 0);
	}
	return r;
}
}

namespace
{
std::mutex g_net_mu;
int g_pool = -1;

bool ResolveName(const char* name, in_addr& out)
{
	int pool;
	{
		std::lock_guard<std::mutex> lk(g_net_mu);
		if (g_pool < 0)
		{
			sceNetInit();
			g_pool = sceNetPoolCreate("snes9x-netfs", 16 * 1024, 0);
		}
		pool = g_pool;
	}
	if (pool < 0)
		return false;
	int r = sceNetResolverCreate("snes9x-netfs", pool, 0);
	if (r < 0)
		return false;
	uint32_t a = 0;
	int rc = sceNetResolverStartNtoa(r, name, &a, 5 * 1000 * 1000, 2, 0);
	sceNetResolverDestroy(r);
	if (rc != 0 || a == 0)
		return false;
	out.s_addr = a;
	return true;
}

struct Result
{
	addrinfo ai;
	sockaddr_in sa;
};
} // namespace

extern "C"
{
int __wrap_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res)
{
	*res = nullptr;
	int flags = hints ? hints->ai_flags : 0;
	if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
		return EAI_FAMILY;
	uint16_t port = 0;
	if (service && *service)
	{
		char* end = nullptr;
		long p = strtol(service, &end, 10);
		if (*end || p < 0 || p > 65535)
			return EAI_SERVICE; // service names (getservbyname) are not known here
		port = (uint16_t)p;
	}
	in_addr addr = {};
	if (!node || !*node)
		addr.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
	else if (inet_pton(AF_INET, node, &addr) != 1)
	{
		if (flags & AI_NUMERICHOST)
			return EAI_NONAME;
		if (!ResolveName(node, addr))
			return EAI_NONAME;
	}
	Result* r = (Result*)calloc(1, sizeof(Result));
	if (!r)
		return EAI_MEMORY;
	r->sa.sin_len = sizeof r->sa; // FreeBSD's sockaddr_in has a length
	r->sa.sin_family = AF_INET;
	r->sa.sin_port = htons(port);
	r->sa.sin_addr = addr;
	r->ai.ai_family = AF_INET;
	r->ai.ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
	r->ai.ai_protocol = hints ? hints->ai_protocol : 0;
	r->ai.ai_addrlen = sizeof r->sa;
	r->ai.ai_addr = (sockaddr*)&r->sa;
	if ((flags & AI_CANONNAME) && node)
		r->ai.ai_canonname = strdup(node);
	*res = &r->ai;
	return 0;
}

void __wrap_freeaddrinfo(addrinfo* ai)
{
	while (ai)
	{
		addrinfo* next = ai->ai_next;
		free(ai->ai_canonname);
		free(ai); // ai is the first member of Result
		ai = next;
	}
}
}

#endif // __PROSPERO__

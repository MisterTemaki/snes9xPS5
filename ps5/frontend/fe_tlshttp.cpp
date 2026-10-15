// Snes9x PS5: the helper's own HTTPS client (fe_tlshttp.h).
// SPDX-License-Identifier: MIT

#include "fe_tlshttp.h"

#include "fe_coverworker.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#else
#include <sys/sysctl.h>
#endif

// Mozilla's CA list (data/cacert.pem), built in
#define SNES9X_TLS_INCBIN(sym, path)                                                                              \
	__asm__(".section .rodata\n"                                                                                   \
			".global " #sym "_begin\n"                                                                             \
			".balign 16\n" #sym "_begin:\n"                                                                        \
			".incbin \"" path "\"\n"                                                                               \
			".global " #sym "_end\n" #sym "_end:\n"                                                                \
			".previous\n");                                                                                        \
	extern "C" const char sym##_begin[];                                                                           \
	extern "C" const char sym##_end[];
SNES9X_TLS_INCBIN(snes9x_cacert, CACERT_PEM)

#ifdef __PROSPERO__
extern "C" {
int sceNetResolverCreate(const char* name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char* hostname, uint32_t* addr, int timeout_us, int retries, int flags);
int sceNetResolverDestroy(int resolver);
}
#endif

// Mbed TLS's randomness (MBEDTLS_ENTROPY_HARDWARE_ALT, fe_mbedtls_config.h): the kernel's
extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t len, size_t* olen)
{
	*olen = 0;
#if defined(__linux__)
	while (*olen < len)
	{
		const ssize_t n = getrandom(output + *olen, len - *olen, 0);
		if (n <= 0)
			break;
		*olen += size_t(n);
	}
#else
	// FreeBSD's kern.arandom (what arc4random reads), at most 256 bytes a call
	while (*olen < len)
	{
		int mib[2] = {CTL_KERN, KERN_ARND};
		size_t n = len - *olen > 256 ? 256 : len - *olen;
		if (sysctl(mib, 2, output + *olen, &n, nullptr, 0) != 0 || n == 0)
			break;
		*olen += n;
	}
#endif
	if (*olen < len)
	{
		const int fd = open("/dev/urandom", O_RDONLY);
		if (fd >= 0)
		{
			while (*olen < len)
			{
				const ssize_t n = read(fd, output + *olen, len - *olen);
				if (n <= 0)
					break;
				*olen += size_t(n);
			}
			close(fd);
		}
	}
	if (*olen < len)
	{
		static bool logged = false;
		if (!logged)
			OrbisLog("[https] no randomness from the kernel (kern.arandom, /dev/urandom): no TLS");
		logged = true;
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	}
	return 0;
}

namespace fe
{
namespace
{
constexpr int kConnectTimeoutMs = 10000;
constexpr int kIoTimeoutSeconds = 15;
constexpr size_t kMaxHeaderBytes = 64 * 1024;

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string TlsError(int err)
{
	char buf[160] = {};
	mbedtls_strerror(err, buf, sizeof(buf));
	char out[200];
	snprintf(out, sizeof(out), "-0x%04x %s", unsigned(-err), buf);
	return out;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	return s;
}

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t'))
		a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
		b--;
	return s.substr(a, b - a);
}

struct Url
{
	bool tls = false;
	std::string host;
	int port = 0;
	std::string path; // "/..." with its query
};

bool ParseUrl(const std::string& url, Url& u)
{
	size_t p;
	if (url.compare(0, 8, "https://") == 0)
	{
		u.tls = true;
		p = 8;
	}
	else if (url.compare(0, 7, "http://") == 0)
	{
		u.tls = false;
		p = 7;
	}
	else
		return false;
	const size_t slash = url.find('/', p);
	std::string hostport = url.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
	u.path = slash == std::string::npos ? "/" : url.substr(slash);
	const size_t frag = u.path.find('#');
	if (frag != std::string::npos)
		u.path.resize(frag);
	if (hostport.find('@') != std::string::npos || hostport.empty())
		return false;
	u.port = u.tls ? 443 : 80;
	const size_t colon = hostport.rfind(':');
	if (colon != std::string::npos && hostport.find(']') == std::string::npos)
	{
		const std::string ps = hostport.substr(colon + 1);
		char* end = nullptr;
		const long port = strtol(ps.c_str(), &end, 10);
		if (ps.empty() || *end || port <= 0 || port > 65535)
			return false;
		u.port = int(port);
		hostport.resize(colon);
	}
	u.host = hostport;
	for (char c : u.host)
		if (!(isalnum(uint8_t(c)) || c == '.' || c == '-'))
			return false;
	return !u.host.empty();
}

// Where a redirect points, from the address it came from.
std::string Resolve(const Url& from, const std::string& location)
{
	if (location.compare(0, 8, "https://") == 0 || location.compare(0, 7, "http://") == 0)
		return location;
	std::string base = std::string(from.tls ? "https://" : "http://") + from.host;
	if (from.port != (from.tls ? 443 : 80))
		base += ":" + std::to_string(from.port);
	if (!location.empty() && location[0] == '/')
		return base + location;
	const size_t q = from.path.find('?');
	const std::string dir = from.path.substr(0, from.path.rfind('/', q == std::string::npos ? std::string::npos : q) + 1);
	return base + dir + location;
}
} // namespace

struct TlsHttp::Impl
{
	// set up once
	bool tls_tried = false, tls_ok = false;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt ca;
	mbedtls_ssl_config conf;

	std::atomic<size_t>* progress = nullptr;
	// the network state, asked again after Term
	bool net_tried = false, net_ok = false;
	int pool = -1;

	// the connection kept between requests
	int fd = -1;
	bool tls = false, ssl_live = false;
	mbedtls_ssl_context ssl;
	std::string conn_host;
	int conn_port = 0;
	int conn_requests = 0;
	std::string inbuf; // read but not used yet
	size_t inpos = 0;

	std::map<std::string, std::vector<sockaddr_in>> addresses; // host -> its addresses (the first that worked first)

	Impl()
	{
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		mbedtls_x509_crt_init(&ca);
		mbedtls_ssl_config_init(&conf);
		mbedtls_ssl_init(&ssl);
	}

	~Impl()
	{
		Close();
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_x509_crt_free(&ca);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
#ifdef __PROSPERO__
		if (pool >= 0)
			sceNetPoolDestroy(pool);
#endif
	}

	bool SetUpTls()
	{
		if (tls_tried)
			return tls_ok;
		tls_tried = true;
		const psa_status_t ps = psa_crypto_init();
		int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
			reinterpret_cast<const unsigned char*>("snes9x-ps5 covers"), 17);
		if (ps != PSA_SUCCESS || rc != 0)
		{
			OrbisLog("[https] Mbed TLS: psa %d, random generator %s: no TLS", int(ps), TlsError(rc).c_str());
			return false;
		}
		// the built-in CA list (NUL-terminated for the PEM parser), and the tests' own CA
		std::string pem(snes9x_cacert_begin, size_t(snes9x_cacert_end - snes9x_cacert_begin));
		const int bad = mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1);
		int extra = 0;
		if (const char* f = getenv("SNES9X_EXTRA_CA"))
		{
			if (FILE* fp = fopen(f, "rb"))
			{
				std::string more;
				char buf[4096];
				size_t n;
				while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
					more.append(buf, n);
				fclose(fp);
				extra = mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char*>(more.c_str()), more.size() + 1);
			}
		}
		int count = 0;
		for (const mbedtls_x509_crt* c = &ca; c && c->raw.len; c = c->next)
			count++;
		OrbisLog("[https] %s: %d CA certificate(s)%s%s", MBEDTLS_VERSION_STRING_FULL, count,
			bad > 0 ? " (some not parsed)" : bad < 0 ? (" (list: " + TlsError(bad) + ")").c_str() : "",
			getenv("SNES9X_EXTRA_CA") ? (extra == 0 ? ", + the extra CA" : ", the extra CA not parsed") : "");
		if (count == 0)
			return false;
		rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
		if (rc != 0)
		{
			OrbisLog("[https] TLS settings: %s", TlsError(rc).c_str());
			return false;
		}
		mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
		mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
		mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
		mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		tls_ok = true;
		return true;
	}

	// libSceNetCtl: is there an IP address? (an offline console never waits on the resolver)
	bool NetUp()
	{
		if (net_tried)
			return net_ok;
		net_tried = true;
		const int nc = sceNetCtlInit();
		int state[4] = {-1, 0, 0, 0};
		const int gs = sceNetCtlGetState(state);
		if (nc == 0)
			sceNetCtlTerm();
		net_ok = !(gs == 0 && state[0] >= 0 && state[0] < 3);
		OrbisLog("[https] netctl init %x, state %d (%x)%s", unsigned(nc), state[0], unsigned(gs),
			net_ok ? "" : ": the console is not connected");
		return net_ok;
	}

	std::vector<sockaddr_in> Lookup(const std::string& host, int port)
	{
		auto it = addresses.find(host);
		std::vector<sockaddr_in> out;
		if (it != addresses.end())
			out = it->second;
		const char* how = "cached";
		if (out.empty())
		{
			addrinfo hints = {};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo* res = nullptr;
			const int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
			for (addrinfo* a = res; gai == 0 && a; a = a->ai_next)
				if (a->ai_family == AF_INET && a->ai_addrlen >= sizeof(sockaddr_in))
					out.push_back(*reinterpret_cast<sockaddr_in*>(a->ai_addr));
			if (res)
				freeaddrinfo(res);
			how = "getaddrinfo";
			if (out.empty())
				OrbisLog("[https] getaddrinfo(%s) -> %d (%s)", host.c_str(), gai, gai ? gai_strerror(gai) : "no IPv4 address");
#ifdef __PROSPERO__
			if (out.empty())
			{
				if (pool < 0)
				{
					sceNetInit();
					pool = sceNetPoolCreate("snes9x-dns", 16 * 1024, 0);
				}
				const int r = pool >= 0 ? sceNetResolverCreate("snes9x-dns", pool, 0) : -1;
				uint32_t a = 0;
				const int rc = r >= 0 ? sceNetResolverStartNtoa(r, host.c_str(), &a, 5 * 1000 * 1000, 2, 0) : -1;
				if (r >= 0)
					sceNetResolverDestroy(r);
				OrbisLog("[https] libSceNet resolver(%s): pool %x, resolver %x -> %x", host.c_str(), unsigned(pool),
					unsigned(r), unsigned(rc));
				if (rc == 0 && a != 0)
				{
					sockaddr_in sa = {};
					sa.sin_family = AF_INET;
					sa.sin_addr.s_addr = a;
					out.push_back(sa);
					how = "libSceNet's resolver";
				}
			}
#endif
			if (out.empty() && host == "raw.githubusercontent.com")
			{
				// GitHub's published addresses for it (api.github.com/meta, "web"/"pages": 185.199.108-111.153/133);
				// the certificate is still checked against the name
				for (const char* ip : {"185.199.108.133", "185.199.109.133", "185.199.110.133", "185.199.111.133"})
				{
					sockaddr_in sa = {};
					sa.sin_family = AF_INET;
					inet_pton(AF_INET, ip, &sa.sin_addr);
					out.push_back(sa);
				}
				how = "GitHub's published addresses";
			}
			if (!out.empty())
			{
				addresses[host] = out;
				char ip[INET_ADDRSTRLEN] = {};
				inet_ntop(AF_INET, &out[0].sin_addr, ip, sizeof(ip));
				OrbisLog("[https] %s -> %s (%s, %zu address(es))", host.c_str(), ip, how, out.size());
			}
		}
		for (sockaddr_in& sa : out)
			sa.sin_port = htons(uint16_t(port));
		return out;
	}

	static int Send(void* ctx, const unsigned char* buf, size_t len)
	{
		const int fd = *static_cast<int*>(ctx);
		for (;;)
		{
			const ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
			if (n >= 0)
				return int(n);
			if (errno == EINTR)
				continue;
			return MBEDTLS_ERR_NET_SEND_FAILED_COMPAT;
		}
	}

	static int Recv(void* ctx, unsigned char* buf, size_t len)
	{
		const int fd = *static_cast<int*>(ctx);
		for (;;)
		{
			const ssize_t n = recv(fd, buf, len, 0);
			if (n >= 0)
				return int(n);
			if (errno == EINTR)
				continue;
			return MBEDTLS_ERR_NET_RECV_FAILED_COMPAT;
		}
	}
	// MBEDTLS_NET_C is off: its two error codes, as the TLS layer reports them
	static constexpr int MBEDTLS_ERR_NET_SEND_FAILED_COMPAT = -0x004E;
	static constexpr int MBEDTLS_ERR_NET_RECV_FAILED_COMPAT = -0x004C;

	void Close()
	{
		if (ssl_live)
		{
			mbedtls_ssl_close_notify(&ssl);
			mbedtls_ssl_session_reset(&ssl);
		}
		if (fd >= 0)
			close(fd);
		fd = -1;
		ssl_live = false;
		conn_host.clear();
		conn_port = 0;
		conn_requests = 0;
		inbuf.clear();
		inpos = 0;
	}

	// A TCP connection (and the TLS handshake). "" or what went wrong.
	std::string Connect(const Url& u)
	{
		Close();
		const std::vector<sockaddr_in> addrs = Lookup(u.host, u.port);
		if (addrs.empty())
			return "no address for " + u.host;
		std::string why;
		for (const sockaddr_in& sa : addrs)
		{
			const int s = socket(AF_INET, SOCK_STREAM, 0);
			if (s < 0)
				return "socket: errno " + std::to_string(errno);
			const int flags = fcntl(s, F_GETFL, 0);
			fcntl(s, F_SETFL, flags | O_NONBLOCK);
			int rc = connect(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
			int err = rc == 0 ? 0 : errno;
			if (rc != 0 && err == EINPROGRESS)
			{
				pollfd p = {s, POLLOUT, 0};
				const int pr = poll(&p, 1, kConnectTimeoutMs);
				socklen_t el = sizeof(err);
				if (pr == 1)
					getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
				else
					err = pr == 0 ? ETIMEDOUT : errno;
			}
			char ip[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
			if (err != 0)
			{
				close(s);
				why = "connect " + std::string(ip) + ":" + std::to_string(u.port) + ": errno " + std::to_string(err);
				continue;
			}
			fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
			timeval tv = {kIoTimeoutSeconds, 0};
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			int one = 1;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			fd = s;
			why.clear();
			break;
		}
		if (fd < 0)
			return why;
		tls = u.tls;
		if (tls)
		{
			int rc = mbedtls_ssl_setup(&ssl, &conf);
			if (rc == MBEDTLS_ERR_SSL_BAD_INPUT_DATA) // set up already: a fresh session on it
				rc = mbedtls_ssl_session_reset(&ssl);
			if (rc == 0)
				rc = mbedtls_ssl_set_hostname(&ssl, u.host.c_str());
			if (rc != 0)
			{
				Close();
				return "TLS setup: " + TlsError(rc);
			}
			mbedtls_ssl_set_bio(&ssl, &fd, Send, Recv, nullptr);
			ssl_live = true;
			do
				rc = mbedtls_ssl_handshake(&ssl);
			while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
			if (rc != 0)
			{
				std::string what = "TLS handshake with " + u.host + ": " + TlsError(rc);
				const uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
				if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0 && flags != uint32_t(-1))
				{
					char info[512] = {};
					mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
					for (char& c : info)
						if (c == '\n')
							c = ';';
					what += " (certificate: " + Trim(info) + ")";
				}
				Close();
				return what;
			}
		}
		conn_host = u.host;
		conn_port = u.port;
		return "";
	}

	// >0 bytes, 0 the other side closed, <0 an error
	int ReadSome(unsigned char* buf, size_t len)
	{
		if (!tls)
		{
			for (;;)
			{
				const ssize_t n = recv(fd, buf, len, 0);
				if (n >= 0)
					return int(n);
				if (errno != EINTR)
					return -1;
			}
		}
		for (;;)
		{
			const int n = mbedtls_ssl_read(&ssl, buf, len);
			if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
				n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
				continue;
			if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
				return 0;
			return n;
		}
	}

	bool WriteAll(const std::string& data)
	{
		size_t off = 0;
		while (off < data.size())
		{
			int n;
			if (tls)
			{
				n = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(data.data() + off), data.size() - off);
				if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
					continue;
			}
			else
			{
				const ssize_t s = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
				if (s < 0 && errno == EINTR)
					continue;
				n = int(s);
			}
			if (n <= 0)
				return false;
			off += size_t(n);
		}
		return true;
	}

	// More bytes into inbuf. False when the connection ended (or failed).
	bool Fill()
	{
		if (inpos > 0 && inpos == inbuf.size())
		{
			inbuf.clear();
			inpos = 0;
		}
		unsigned char buf[16384];
		const int n = ReadSome(buf, sizeof(buf));
		if (n <= 0)
			return false;
		inbuf.append(reinterpret_cast<char*>(buf), size_t(n));
		return true;
	}

	// One line without its CRLF; false at the end of the connection or past max bytes.
	bool ReadLine(std::string& line, size_t max)
	{
		for (;;)
		{
			const size_t nl = inbuf.find('\n', inpos);
			if (nl != std::string::npos)
			{
				line = inbuf.substr(inpos, nl - inpos);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				inpos = nl + 1;
				return true;
			}
			if (inbuf.size() - inpos > max || !Fill())
				return false;
		}
	}

	// Exactly n bytes into out (or dropped when out is null). False when the connection ended first.
	bool ReadBytes(size_t n, std::vector<uint8_t>* out)
	{
		while (n > 0)
		{
			if (inpos == inbuf.size() && !Fill())
				return false;
			const size_t take = std::min(n, inbuf.size() - inpos);
			if (out)
			{
				out->insert(out->end(), inbuf.begin() + long(inpos), inbuf.begin() + long(inpos + take));
				if (progress)
					progress->store(out->size(), std::memory_order_relaxed);
			}
			inpos += take;
			n -= take;
		}
		return true;
	}

	// One GET on the open connection: the status, -2 (too big), or -1 (the connection broke; nothing_back: before
	// any answer, as a kept connection the server closed meanwhile does).
	int Exchange(const Url& u, std::vector<uint8_t>& out, size_t max_bytes, std::string& location, std::string& why,
		bool* nothing_back)
	{
		*nothing_back = false;
		const std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
			(u.port != (u.tls ? 443 : 80) ? ":" + std::to_string(u.port) : std::string()) +
			"\r\nUser-Agent: Snes9xPS5/" SNES9X_PS5_VERSION "\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n";
		if (!WriteAll(req))
		{
			*nothing_back = true;
			why = "send failed";
			return -1;
		}
		conn_requests++;
		std::string line;
		int major = 0, minor = 0, status = 0;
		bool keep = false, chunked = false;
		long long length = -1;
		bool first = true;
		do // an interim (1xx) answer is followed by the real one
		{
			if (!ReadLine(line, kMaxHeaderBytes))
			{
				*nothing_back = first && inbuf.size() == inpos;
				why = "no answer";
				return -1;
			}
			first = false;
			if (sscanf(line.c_str(), "HTTP/%d.%d %d", &major, &minor, &status) != 3 || status < 100 || status > 999)
			{
				why = "not an HTTP answer: " + line.substr(0, 60);
				return -1;
			}
			keep = major > 1 || (major == 1 && minor >= 1);
			chunked = false;
			length = -1;
			size_t header_bytes = line.size();
			for (;;)
			{
				if (!ReadLine(line, kMaxHeaderBytes))
				{
					why = "the headers stopped";
					return -1;
				}
				header_bytes += line.size();
				if (header_bytes > kMaxHeaderBytes)
				{
					why = "headers too long";
					return -1;
				}
				if (line.empty())
					break;
				const size_t colon = line.find(':');
				if (colon == std::string::npos)
					continue;
				const std::string name = Lower(Trim(line.substr(0, colon)));
				const std::string value = Trim(line.substr(colon + 1));
				if (name == "content-length")
					length = strtoll(value.c_str(), nullptr, 10);
				else if (name == "transfer-encoding")
					chunked = Lower(value).find("chunked") != std::string::npos;
				else if (name == "connection")
				{
					const std::string v = Lower(value);
					if (v.find("close") != std::string::npos)
						keep = false;
					else if (v.find("keep-alive") != std::string::npos)
						keep = true;
				}
				else if (name == "location")
					location = value;
			}
		} while (status >= 100 && status < 200);
		// the body: kept for a 200, read and dropped otherwise (so the connection can serve the next request)
		out.clear();
		const size_t limit = status == 200 ? max_bytes : 1u << 20;
		std::vector<uint8_t>* sink = status == 200 ? &out : nullptr;
		size_t got = 0;
		if (chunked)
		{
			for (;;)
			{
				if (!ReadLine(line, 1024))
				{
					why = "a chunk stopped";
					return -1;
				}
				const unsigned long long n = strtoull(line.c_str(), nullptr, 16);
				if (n == 0)
				{
					while (ReadLine(line, kMaxHeaderBytes) && !line.empty())
					{
					}
					break;
				}
				if (got + n > limit)
				{
					Close();
					return status == 200 ? -2 : status;
				}
				if (!ReadBytes(size_t(n), sink) || !ReadLine(line, 16))
				{
					why = "a chunk stopped";
					return -1;
				}
				got += size_t(n);
			}
		}
		else if (length >= 0)
		{
			if (size_t(length) > limit)
			{
				Close();
				return status == 200 ? -2 : status;
			}
			if (!ReadBytes(size_t(length), sink))
			{
				why = "the answer stopped after " + std::to_string(out.size()) + " of " + std::to_string(length) + " bytes";
				return -1;
			}
		}
		else if (status != 204 && status != 304)
		{
			// to the end of the connection
			keep = false;
			for (;;)
			{
				const size_t have = inbuf.size() - inpos;
				if (got + have > limit)
				{
					Close();
					return status == 200 ? -2 : status;
				}
				ReadBytes(have, sink);
				got += have;
				if (!Fill())
					break;
			}
		}
		if (!keep)
			Close();
		return status;
	}
};

TlsHttp::TlsHttp() : m(new Impl)
{
}

TlsHttp::~TlsHttp() = default;

bool TlsHttp::Offline() const
{
	return m->net_tried && !m->net_ok;
}

void TlsHttp::SetProgress(std::atomic<size_t>* bytes)
{
	m->progress = bytes;
}

void TlsHttp::Term()
{
	m->Close();
	m->net_tried = m->net_ok = false;
}

int TlsHttp::Get(const std::string& start_url, std::vector<uint8_t>& out, size_t max_bytes)
{
	out.clear();
	if (!m->NetUp())
		return -1;
	if (!m->SetUpTls())
		return -1;
	int status = -1;
	for (int attempt = 1; attempt <= 3; attempt++)
	{
		std::string url = start_url;
		const double t0 = Now();
		int connections = 0;
		std::string why;
		for (int hop = 0; hop <= 5; hop++)
		{
			Url u;
			if (!ParseUrl(url, u))
			{
				OrbisLog("[https] not an address I can fetch: %s", url.c_str());
				return -1;
			}
			std::string location;
			bool reused = false, nothing_back = false;
			status = -1;
			why.clear();
			for (int conn_try = 0; conn_try < 2; conn_try++)
			{
				reused = m->fd >= 0 && m->conn_host == u.host && m->conn_port == u.port && m->tls == u.tls;
				if (!reused)
				{
					connections++;
					why = m->Connect(u);
					if (!why.empty())
						break;
				}
				status = m->Exchange(u, out, max_bytes, location, why, &nothing_back);
				if (status == -1)
					m->Close();
				// a kept connection the server had closed in the meantime: once more on a new one
				if (!(status == -1 && reused && nothing_back))
					break;
			}
			if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !location.empty())
			{
				url = Resolve(u, location);
				out.clear();
				continue;
			}
			break;
		}
		OrbisLog("[https] try %d: GET %s -> %d, %zu bytes, %.0f ms%s%s%s", attempt, start_url.c_str(), status, out.size(),
			(Now() - t0) * 1000.0, connections ? "" : " (kept connection)", why.empty() ? "" : ": ", why.c_str());
		if (status != -1)
			break;
		for (int i = 0; i < 15; i++)
			usleep(100 * 1000);
	}
	if (status != 200)
		out.clear();
	return status;
}

std::unique_ptr<HttpClient> MakeCoverWorkerHttp()
{
	return std::make_unique<TlsHttp>();
}
} // namespace fe

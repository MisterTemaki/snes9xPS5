// Snes9x PS5 (2.11): the helper's side of the app's RetroAchievements requests (ProsperoRaProxy.h).
// SPDX-License-Identifier: MIT

#include "ProsperoRaProxy.h"

#include "OrbisPaths.h"
#include "ProsperoThread.h"
#include "fe_tlshttp.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace raproxy
{
namespace
{
constexpr const char* kSite = "https://retroachievements.org/dorequest.php";
constexpr const char* kMedia = "https://media.retroachievements.org/";
// requests waiting for the worker; more than this and the app is told to try again later (it retries by itself)
constexpr size_t kMaxQueued = 16;

int64_t NowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

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

bool SendAll(int fd, const void* data, size_t size, int64_t deadline)
{
	const char* p = static_cast<const char*>(data);
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

std::string TestHost()
{
#ifdef __PROSPERO__
	return std::string();
#else
	const char* h = getenv("SNES9X_RA_TEST_HOST");
	return h && *h ? h : "";
#endif
}

bool StartsWith(const std::string& s, const std::string& prefix)
{
	return !prefix.empty() && s.compare(0, prefix.size(), prefix) == 0;
}

// A printable header value: no control characters (a CR or LF would start another header)
bool Printable(const char* s)
{
	for (; *s; s++)
		if (static_cast<unsigned char>(*s) < 0x20 || static_cast<unsigned char>(*s) > 0x7e)
			return false;
	return true;
}

int HexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

std::string UrlDecode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '%' && i + 2 < s.size() && HexValue(s[i + 1]) >= 0 && HexValue(s[i + 2]) >= 0)
		{
			out += char(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2]));
			i += 2;
		}
		else
			out += s[i] == '+' ? ' ' : s[i];
	}
	return out;
}

// The value of field `name` in a form body ("" when absent)
std::string Field(const std::string& post, const char* name)
{
	const std::string key = std::string(name) + "=";
	for (size_t at = 0; at < post.size();)
	{
		const size_t end = post.find('&', at);
		const size_t stop = end == std::string::npos ? post.size() : end;
		if (post.compare(at, key.size(), key) == 0)
			return post.substr(at + key.size(), stop - at - key.size());
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return "";
}

struct Job
{
	int fd = -1;
	std::string url, agent, post, what, password, token;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
ps5::BigThread* g_thread = nullptr;

void Answer(int fd, int32_t ret, int http, const std::vector<uint8_t>& body, const char* why)
{
	jailbreak::Request a;
	memset(&a, 0, sizeof(a));
	a.magic = jailbreak::kMagic;
	a.cmd = jailbreak::kCmdRetroAchievements;
	a.pid = int32_t(getpid());
	a.ret = ret;
	if (ret == jailbreak::kRaOk)
		snprintf(a.msg1, sizeof(a.msg1), "%d %zu", http, body.size());
	if (why)
		snprintf(a.msg2, sizeof(a.msg2), "%s", why);
	const int64_t deadline = NowMs() + 10000;
	if (SendAll(fd, &a, sizeof(a), deadline) && ret == jailbreak::kRaOk && !body.empty())
		SendAll(fd, body.data(), body.size(), deadline);
}

void Worker()
{
	// one connection kept per server (the site and the pictures' host)
	std::map<std::string, std::unique_ptr<fe::TlsHttp>> http;
	for (;;)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(g_mu);
			g_cv.wait(lock, [] { return !g_jobs.empty(); });
			job = std::move(g_jobs.front());
			g_jobs.pop_front();
		}
		const size_t scheme = job.url.find("://");
		const size_t slash = scheme == std::string::npos ? std::string::npos : job.url.find('/', scheme + 3);
		const std::string host = job.url.substr(0, slash);
		std::unique_ptr<fe::TlsHttp>& client = http[host];
		if (!client)
			client.reset(new fe::TlsHttp);
		std::vector<uint8_t> body;
		// 2.11 review: a redirect is followed only to an address of the same allow-list
		const int status = client->Send(job.url, job.post, job.agent, job.what.c_str(), body, jailbreak::kRaMaxAnswer, Allowed);
		if (status >= 100)
			Answer(job.fd, jailbreak::kRaOk, status, body, nullptr);
		else if (status == -2)
			Answer(job.fd, jailbreak::kRaTooBig, 0, body, "the answer is too big");
		else if (status == -4)
			Answer(job.fd, jailbreak::kRaRefused, 0, body, "a redirect to an address that isn't RetroAchievements'");
		else
		{
			Answer(job.fd, jailbreak::kRaNetwork, 0, body, client->Offline() ? "no network" : "the server did not answer");
			client->Term(); // the next request asks the console for its network again (it may be back)
		}
		OrbisLog("[ra] %s -> %d", job.what.c_str(), status);
		close(job.fd);
		// the password was only needed while its request ran; the token stays redacted for good
		if (!job.password.empty())
		{
			OrbisLogUnredact(job.password);
			OrbisLogUnredact(UrlDecode(job.password));
			for (char& c : job.password)
				*(volatile char*)&c = 0;
		}
		for (char& c : job.post)
			*(volatile char*)&c = 0;
	}
}
} // namespace

bool Allowed(const std::string& url)
{
	const std::string test = TestHost();
	if (url == kSite || (!test.empty() && url == test + "/dorequest.php"))
		return true;
	std::string path;
	if (StartsWith(url, kMedia))
		path = url.substr(strlen(kMedia));
	else if (!test.empty() && StartsWith(url, test + "/"))
		path = url.substr(test.size() + 1);
	else
		return false;
	// a picture: letters, digits, '/', '_', '-' and one ".png" at the end, nothing that leaves the folder or asks more
	if (path.empty() || path.size() > 200 || path.find("..") != std::string::npos ||
		path.size() < 4 || path.compare(path.size() - 4, 4, ".png") != 0)
		return false;
	for (size_t i = 0; i + 4 < path.size(); i++)
	{
		const char c = path[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-'))
			return false;
	}
	return true;
}

std::string Describe(const std::string& url, const std::string& post)
{
	if (post.empty())
	{
		const size_t scheme = url.find("://");
		const size_t slash = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
		return "picture " + (slash == std::string::npos ? url : url.substr(slash));
	}
	std::string r = Field(post, "r");
	for (char& c : r)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
			c = '?';
	return "r=" + r.substr(0, 32);
}

void SecretsOf(const std::string& post, std::string* password, std::string* token)
{
	if (password)
		*password = Field(post, "p");
	if (token)
		*token = Field(post, "t");
}

void Refuse(int fd, const jailbreak::Request& req, int64_t deadline_ms, const char* why)
{
	// the body read and dropped first (at most kRaMaxPost, until the deadline): closing with it unread would reset the
	// connection, and the answer could be lost with it
	char sink[4096];
	size_t left = req.ret > 0 && uint32_t(req.ret) <= jailbreak::kRaMaxPost ? size_t(req.ret) : 0;
	while (left > 0 && WaitFd(fd, POLLIN, deadline_ms))
	{
		const ssize_t n = recv(fd, sink, std::min(left, sizeof(sink)), 0);
		if (n <= 0)
			break;
		left -= size_t(n);
	}
	const std::vector<uint8_t> none;
	Answer(fd, jailbreak::kRaRefused, 0, none, why);
	close(fd);
}

void Take(int fd, const jailbreak::Request& req_in, int64_t deadline_ms)
{
	jailbreak::Request req = req_in;
	req.msg1[sizeof(req.msg1) - 1] = 0;
	req.msg2[sizeof(req.msg2) - 1] = 0;
	const std::vector<uint8_t> none;
	if (req.ret < 0 || uint32_t(req.ret) > jailbreak::kRaMaxPost)
	{
		OrbisLog("[ra] pid %d: a body of %d bytes refused", req.pid, req.ret);
		Answer(fd, jailbreak::kRaRefused, 0, none, "request too big");
		close(fd);
		return;
	}
	Job job;
	job.fd = fd;
	job.url = req.msg1;
	job.agent = req.msg2;
	job.post.resize(size_t(req.ret));
	size_t got = 0;
	while (got < job.post.size() && WaitFd(fd, POLLIN, deadline_ms))
	{
		const ssize_t n = recv(fd, &job.post[got], job.post.size() - got, 0);
		if (n <= 0)
			break;
		got += size_t(n);
	}
	// the secrets first: nothing below may log them
	SecretsOf(job.post, &job.password, &job.token);
	for (const std::string* s : {&job.password, &job.token})
		if (!s->empty())
		{
			OrbisLogRedact(*s);
			OrbisLogRedact(UrlDecode(*s));
		}
	job.what = Describe(job.url, job.post);
#ifndef __PROSPERO__
	// the host tests: the body logged on purpose, to show the password and the token never reach a log line
	if (getenv("SNES9X_HOST_LOG_SECRETS"))
		OrbisLog("[ra] (test) body of %s: %s", job.what.c_str(), job.post.c_str());
#endif
	if (got != job.post.size())
	{
		OrbisLog("[ra] pid %d: %s cut short (%zu of %zu bytes)", req.pid, job.what.c_str(), got, job.post.size());
		// 2.11 second review: answered all the same (a connection closed with nothing back means a 2.10 helper to the app)
		Answer(fd, jailbreak::kRaNetwork, 0, none, "the request was cut short");
		close(fd);
		return;
	}
	if (!Allowed(job.url) || job.agent.empty() || job.agent.size() > 200 || !Printable(job.agent.c_str()))
	{
		OrbisLog("[ra] pid %d: %s refused (not a RetroAchievements address, or no User-Agent)", req.pid, job.what.c_str());
		Answer(fd, jailbreak::kRaRefused, 0, none, "not a RetroAchievements address");
		close(fd);
		return;
	}
	std::unique_lock<std::mutex> lock(g_mu);
	if (g_jobs.size() >= kMaxQueued)
	{
		lock.unlock();
		OrbisLog("[ra] pid %d: %s: %zu requests waiting already", req.pid, job.what.c_str(), kMaxQueued);
		Answer(fd, jailbreak::kRaBusy, 0, none, "busy");
		close(fd);
		return;
	}
	if (!g_thread)
	{
		g_thread = new ps5::BigThread(Worker, 512 * 1024); // the TLS handshake wants room
		if (!g_thread->joinable())
		{
			delete g_thread;
			g_thread = nullptr;
			lock.unlock();
			OrbisLog("[ra] no thread for the requests");
			Answer(fd, jailbreak::kRaNetwork, 0, none, "the helper could not start a thread");
			close(fd);
			return;
		}
		OrbisLog("[ra] RetroAchievements requests: thread started");
	}
	OrbisLog("[ra] pid %d: %s queued", req.pid, job.what.c_str());
	g_jobs.push_back(std::move(job));
	g_cv.notify_one();
}
} // namespace raproxy

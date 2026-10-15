// Moving the app to a new title ID (ProsperoMigrate.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoMigrate.h"

#include "OrbisPaths.h"
#include "ProsperoMigrateConfig.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace migrate
{
namespace
{
std::string Env(const char* name)
{
	const char* v = getenv((std::string(MIG_ENV_PREFIX) + name).c_str());
	return v ? v : "";
}

std::string HomebrewRoot()
{
	const std::string v = Env("PS5_HOMEBREW");
	return v.empty() ? "/data/homebrew" : v;
}

std::vector<std::string> OldIds()
{
	std::vector<std::string> out;
	std::string s = MIG_OLD_TITLES;
	size_t a = 0;
	while (a <= s.size())
	{
		size_t b = s.find(',', a);
		if (b == std::string::npos)
			b = s.size();
		std::string id = s.substr(a, b - a);
		while (!id.empty() && id[0] == ' ')
			id.erase(0, 1);
		if (id.size() == 9 && id != MIG_NEW_TITLE)
			out.push_back(id);
		a = b + 1;
	}
	return out;
}

std::string ReadSmall(const std::string& path, size_t max = 1 << 16)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return "";
	std::string s(max, '\0');
	const size_t n = fread(&s[0], 1, max, f);
	fclose(f);
	s.resize(n);
	return s;
}

// The value of "key": "..." in a small JSON text ("" when it isn't there).
std::string JsonString(const std::string& json, const char* key)
{
	const std::string k = std::string("\"") + key + "\"";
	size_t p = json.find(k);
	if (p == std::string::npos)
		return "";
	p = json.find(':', p + k.size());
	if (p == std::string::npos)
		return "";
	p = json.find('"', p + 1);
	if (p == std::string::npos)
		return "";
	const size_t e = json.find('"', p + 1);
	return e == std::string::npos ? "" : json.substr(p + 1, e - p - 1);
}

// Our app's folder under that ID: its param.json says that title ID and our content ID.
bool IsOurFolder(const std::string& dir, const std::string& id)
{
	const std::string param = ReadSmall(dir + "/sce_sys/param.json");
	return !param.empty() && JsonString(param, "titleId") == id && JsonString(param, "contentId") == ContentIdFor(id.c_str());
}

// rm -r, without following links (a link is removed, not what it points to).
bool RemoveTree(const std::string& path)
{
	struct stat st = {};
	if (lstat(path.c_str(), &st) != 0)
		return errno == ENOENT;
	if (!S_ISDIR(st.st_mode))
		return unlink(path.c_str()) == 0;
	DIR* d = opendir(path.c_str());
	if (!d)
		return false;
	bool ok = true;
	while (dirent* e = readdir(d))
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		ok = RemoveTree(path + "/" + e->d_name) && ok;
	}
	closedir(d);
	return rmdir(path.c_str()) == 0 && ok;
}

std::string PendingDir()
{
	return OrbisRoot() + "/migrate";
}

std::string PendingFile(const std::string& id)
{
	return PendingDir() + "/" + id + ".unregister";
}

// ShadowMountPlus's API port: api_port in its config.ini, else 10101.
int SmpPort()
{
	const std::string env = Env("SMP_PORT");
	if (!env.empty())
		return atoi(env.c_str());
	std::string cfg = Env("SMP_CONFIG");
	if (cfg.empty())
		cfg = "/data/shadowmount/config.ini";
	const std::string text = ReadSmall(cfg, 1 << 18);
	size_t p = 0;
	while (p < text.size())
	{
		size_t e = text.find('\n', p);
		if (e == std::string::npos)
			e = text.size();
		std::string line = text.substr(p, e - p);
		p = e + 1;
		while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
			line.erase(0, 1);
		if (line.compare(0, 8, "api_port") == 0)
		{
			const size_t eq = line.find('=');
			if (eq != std::string::npos)
			{
				const int port = atoi(line.c_str() + eq + 1);
				if (port > 0 && port < 65536)
					return port;
			}
		}
	}
	return 10101;
}

// One POST to ShadowMountPlus (127.0.0.1 only). The HTTP status, or -1 when nobody answered.
int Post(const char* path, const std::string& body, std::string* reply)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(uint16_t(SmpPort()));
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	int rc = connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
	if (rc != 0 && errno == EINPROGRESS)
	{
		pollfd p = {fd, POLLOUT, 0};
		int err = 0;
		socklen_t len = sizeof(err);
		rc = (poll(&p, 1, 2000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) ? 0 : -1;
	}
	fcntl(fd, F_SETFL, flags);
	if (rc != 0)
	{
		close(fd);
		return -1;
	}
	timeval tv = {10, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	char head[256];
	snprintf(head, sizeof(head),
		"POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
		"Connection: close\r\n\r\n",
		path, body.size());
	const std::string req = head + body;
	size_t off = 0;
	while (off < req.size())
	{
		const ssize_t n = send(fd, req.data() + off, req.size() - off, 0);
		if (n <= 0)
		{
			close(fd);
			return -1;
		}
		off += size_t(n);
	}
	std::string in;
	char buf[4096];
	while (in.size() < (1 << 18))
	{
		const ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		in.append(buf, size_t(n));
	}
	close(fd);
	int status = -1;
	if (in.compare(0, 5, "HTTP/") == 0)
	{
		const size_t sp = in.find(' ');
		if (sp != std::string::npos)
			status = atoi(in.c_str() + sp + 1);
	}
	if (reply)
	{
		const size_t hdr = in.find("\r\n\r\n");
		*reply = hdr == std::string::npos ? "" : in.substr(hdr + 4);
	}
	return status;
}

enum class Unreg
{
	Done,     // removed, or nothing (of ours) to remove
	Busy,     // try again later
	NoApi,    // ShadowMountPlus didn't answer
};

Unreg Unregister(const std::string& id, bool* removed)
{
	*removed = false;
	const std::string body = std::string("{\"title_id\":\"") + id + "\"}";
	std::string reply;
	const int info = Post("/api/v1/games/info", body, &reply);
	if (info < 0)
	{
		OrbisLog("[migrate] ShadowMountPlus's API (127.0.0.1:%d) doesn't answer", SmpPort());
		return Unreg::NoApi;
	}
	if (info == 404)
	{
		OrbisLog("[migrate] %s isn't registered (anymore)", id.c_str());
		return Unreg::Done;
	}
	if (info != 200)
	{
		OrbisLog("[migrate] info on %s: HTTP %d %.200s", id.c_str(), info, reply.c_str());
		return Unreg::Busy;
	}
	const std::string content = JsonString(reply, "content_id");
	if (content != ContentIdFor(id.c_str()))
	{
		OrbisLog("[migrate] %s belongs to another app (%s): left alone", id.c_str(), content.c_str());
		return Unreg::Done;
	}
	const int un = Post("/api/v1/games/uninstall", body, &reply);
	if (un == 200)
	{
		OrbisLog("[migrate] the old registration %s was removed", id.c_str());
		*removed = true;
		return Unreg::Done;
	}
	if (un == 404)
		return Unreg::Done;
	OrbisLog("[migrate] uninstall %s: HTTP %d %.200s (tried again later)", id.c_str(), un, reply.c_str());
	return un < 0 ? Unreg::NoApi : Unreg::Busy;
}

// Step 2 for every pending ID. Fills r.unregistered / r.pending / r.no_api.
void TryPending(Result& r)
{
	int removed_now = 0;
	for (const std::string& id : OldIds())
	{
		const std::string pf = PendingFile(id);
		if (!OrbisIsFile(pf))
			continue;
		bool removed = false;
		const Unreg u = Unregister(id, &removed);
		if (u == Unreg::Done)
		{
			unlink(pf.c_str());
			if (removed)
			{
				r.unregistered++;
				removed_now++;
			}
		}
		else
		{
			r.pending++;
			if (u == Unreg::NoApi)
				r.no_api = true;
		}
	}
	if (removed_now > 0)
	{
		std::string reply;
		const int s = Post("/api/v1/scan", "{\"reset_attempts\":false}", &reply);
		OrbisLog("[migrate] rescan asked for: HTTP %d", s);
	}
}

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void* RetryMain(void*)
{
	const std::string env = Env("MIGRATE_RETRY_MS");
	const long ms = env.empty() ? 30000 : atol(env.c_str());
	for (;;)
	{
		usleep(useconds_t(ms) * 1000);
		bool any = false;
		for (const std::string& id : OldIds())
			any = any || OrbisIsFile(PendingFile(id));
		if (!any)
			continue; // the app may leave new work (an old payload reinstalled the old folder)
		pthread_mutex_lock(&g_mu);
		Result r;
		TryPending(r);
		pthread_mutex_unlock(&g_mu);
		if (r.unregistered > 0)
			OrbisLog("[migrate] retry: %d old registration(s) removed", r.unregistered);
	}
	return nullptr;
}
} // namespace

const char* const* OldTitleIds()
{
	static std::vector<std::string> ids = OldIds();
	static std::vector<const char*> ptrs;
	if (ptrs.empty())
	{
		for (const std::string& s : ids)
			ptrs.push_back(s.c_str());
		ptrs.push_back(nullptr);
	}
	return ptrs.data();
}

std::string ContentIdFor(const char* title_id)
{
	return std::string("UP9000-") + title_id + "_00-" MIG_CONTENT_LABEL;
}

Result Run(bool network)
{
	pthread_mutex_lock(&g_mu);
	Result r;
	const std::string root = HomebrewRoot();
	const std::string now = root + "/" MIG_NEW_TITLE;
	for (const std::string& id : OldIds())
	{
		const std::string old = root + "/" + id;
		if (!OrbisIsDir(old))
			continue;
		if (!IsOurFolder(old, id))
		{
			OrbisLog("[migrate] %s is another app's folder: left alone", old.c_str());
			continue;
		}
		if (!r.old_ids.empty())
			r.old_ids += ", ";
		r.old_ids += id;
		// remember the registration first: if the move is cut short, it is still removed later
		if (OrbisMkdirs(PendingDir()))
		{
			FILE* f = fopen(PendingFile(id).c_str(), "w");
			if (f)
			{
				fprintf(f, "%s -> %s\n", id.c_str(), MIG_NEW_TITLE);
				fclose(f);
			}
		}
		if (!OrbisIsFile(now + "/eboot.bin") && !OrbisIsDir(now) && rename(old.c_str(), now.c_str()) == 0)
		{
			OrbisLog("[migrate] %s renamed to %s", old.c_str(), now.c_str());
			r.moved++;
		}
		else if (RemoveTree(old))
		{
			OrbisLog("[migrate] %s removed (%s is the app now)", old.c_str(), now.c_str());
			r.removed++;
		}
		else
			OrbisLog("[migrate] can't remove %s: %s", old.c_str(), strerror(errno));
	}
	if (network)
		TryPending(r);
	else
		for (const std::string& id : OldIds())
			if (OrbisIsFile(PendingFile(id)))
				r.pending++;
	pthread_mutex_unlock(&g_mu);
	if (r.moved || r.removed || r.unregistered || r.pending)
		OrbisLog("[migrate] title %s: %d folder(s) moved, %d removed, %d registration(s) removed, %d pending%s",
			MIG_NEW_TITLE, r.moved, r.removed, r.unregistered, r.pending, r.no_api ? " (ShadowMountPlus's API not reachable)" : "");
	return r;
}

void StartRetryThread()
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	pthread_t t;
	if (pthread_create(&t, &attr, RetryMain, nullptr) != 0)
		OrbisLog("[migrate] can't start the retry thread");
	pthread_attr_destroy(&attr);
}
} // namespace migrate

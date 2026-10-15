// Snes9x PS5: the covers downloaded in the background by the helper (fe_coverworker.h).
// SPDX-License-Identifier: MIT

#include "fe_coverworker.h"

#include "fe_coverfetch.h"
#include "fe_http.h"

#include "OrbisPaths.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace fe
{
__attribute__((weak)) std::unique_ptr<HttpClient> MakeCoverWorkerHttp()
{
	return std::make_unique<Http>();
}

namespace
{
constexpr long kMissingRetrySeconds = 30L * 24 * 3600; // as the shelf: a 404 isn't asked again for 30 days
constexpr long kFailRetrySeconds = 10L * 60;           // another failure (a timeout...): not before 10 minutes
constexpr long kOfflinePauseSeconds = 30;              // no network: look again after this
constexpr size_t kMaxListBytes = 32u << 20;

std::atomic<bool> g_started{false};

std::string CoversDir()
{
	return OrbisDir("covers");
}

// A regular file of ours (symbolic links refused), at most kMaxListBytes.
bool ReadList(const std::string& path, std::string* text, time_t* mtime, off_t* size)
{
	text->clear();
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return false;
	struct stat st;
	bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && size_t(st.st_size) <= kMaxListBytes;
	if (ok)
	{
		*mtime = st.st_mtime;
		*size = st.st_size;
		char buf[65536];
		ssize_t n;
		while ((n = read(fd, buf, sizeof(buf))) > 0 && text->size() <= kMaxListBytes)
			text->append(buf, size_t(n));
	}
	close(fd);
	return ok;
}

bool Changed(const std::string& path, time_t mtime, off_t size)
{
	struct stat st;
	if (lstat(path.c_str(), &st) != 0)
		return mtime != 0 || size != 0;
	return st.st_mtime != mtime || st.st_size != size;
}

bool NonEmptyFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool RecentlyMissing(const std::string& marker, const std::string& url)
{
    struct stat st = {};
    if (stat(marker.c_str(), &st) != 0 || time(nullptr) - st.st_mtime >= kMissingRetrySeconds)
        return false;
    FILE* f = fopen(marker.c_str(), "r");
    if (!f) return false;
    char line[8192] = {};
    const bool read = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    if (!read) return false;
    std::string old(line);
    while (!old.empty() && (old.back() == '\n' || old.back() == '\r')) old.pop_back();
    return old == url;
}

struct Worker
{
	std::vector<WantedCover> wanted, priority;
	time_t wanted_mtime = 0, priority_mtime = 0;
	off_t wanted_size = 0, priority_size = 0;
	size_t cursor = 0;
	int left = 0, fetched = 0;
	std::map<std::string, time_t> tried; // file -> when it last failed (not a 404)
	std::string last_progress;
	std::unique_ptr<HttpClient> http = MakeCoverWorkerHttp();

	bool Needs(const WantedCover& w)
	{
		auto t = tried.find(w.file);
		if (t != tried.end() && time(nullptr) - t->second < kFailRetrySeconds)
			return false;
		const std::string path = CoversDir() + "/" + w.file;
		const std::string base = path.substr(0, path.size() - 4);
		struct stat st = {};
		if (stat((base + ".refetch").c_str(), &st) == 0)
			return true; // Square on the shelf: asked for again
		return !NonEmptyFile(path) && !RecentlyMissing(base + ".missing", w.url);
	}

	void Reload()
	{
		const std::string wpath = CoversDir() + "/wanted.txt", ppath = PriorityListPath();
		if (Changed(wpath, wanted_mtime, wanted_size))
		{
			std::string text;
			wanted_mtime = 0;
			wanted_size = 0;
			ReadList(wpath, &text, &wanted_mtime, &wanted_size);
			wanted = ParseWantedList(text);
			cursor = 0;
			left = 0;
			for (const WantedCover& w : wanted)
				left += Needs(w) ? 1 : 0;
			OrbisLog("[covers] wanted list: %zu cover(s), %d to fetch", wanted.size(), left);
		}
		if (Changed(ppath, priority_mtime, priority_size))
		{
			std::string text;
			priority_mtime = 0;
			priority_size = 0;
			ReadList(ppath, &text, &priority_mtime, &priority_size);
			priority = ParseWantedList(text);
		}
	}

	void Progress(const char* state)
	{
		char line[96];
		snprintf(line, sizeof(line), "%d %d %s\n", left, fetched, state);
		// written when it changes, and at least every 20 s while there is work (so the shelf sees it's alive)
		static time_t last_write = 0;
		const time_t now = time(nullptr);
		if (line == last_progress && (left == 0 || now - last_write < 20))
			return;
		last_progress = line;
		last_write = now;
		WriteFileAtomicTo(CoversDir() + "/progress.txt", std::vector<uint8_t>(line, line + strlen(line)));
	}

	const WantedCover* Next()
	{
		for (const WantedCover& w : priority)
			if (Needs(w))
				return &w;
		while (cursor < wanted.size())
		{
			if (Needs(wanted[cursor]))
				return &wanted[cursor];
			cursor++;
		}
		return nullptr;
	}

	void Run()
	{
		OrbisLog("[covers] background downloads ready (%s)", CoversDir().c_str());
		time_t offline_until = 0;
		for (;;)
		{
			Reload();
			if (time(nullptr) < offline_until)
			{
				Progress("offline");
				sleep(1);
				continue;
			}
			const WantedCover* next = Next();
			if (!next)
			{
				left = 0;
				Progress("idle");
				usleep(500 * 1000);
				continue;
			}
			const WantedCover w = *next; // the lists can be reloaded below
			Progress("busy");
			std::vector<uint8_t> data;
			const int status = FetchCoverUrl(*http, w.url, w.file, data);
			const int saved = SaveCoverResult(CoversDir(), w, status, data);
			OrbisLog("[covers] %s -> %d (%zu bytes)%s", w.file.c_str(), status, data.size(),
				saved == 1 ? "" : saved == 0 ? ": not on the server" : ": not saved");
			if (saved == 1)
				fetched++;
			if (saved < 0)
				tried[w.file] = time(nullptr);
			if (left > 0)
				left--;
			if (status == -1 && http->Offline())
			{
				OrbisLog("[covers] the console is offline: trying again in %ld s", kOfflinePauseSeconds);
				http->Term();
				offline_until = time(nullptr) + kOfflinePauseSeconds;
			}
		}
	}
};

void* WorkerMain(void*)
{
	static Worker worker; // lives as long as the helper
	worker.Run();
	return nullptr;
}
} // namespace

std::string PriorityListPath()
{
	return OrbisDir("covers") + "/priority.txt";
}

void StartCoverWorker()
{
	if (g_started.exchange(true))
		return;
	OrbisMkdirs(OrbisDir("covers"));
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t t;
	const int rc = pthread_create(&t, &attr, WorkerMain, nullptr);
	pthread_attr_destroy(&attr);
	if (rc != 0)
	{
		OrbisLog("[covers] can't start the download thread (%d)", rc);
		g_started = false;
	}
}

CoverProgress ReadCoverProgress()
{
	CoverProgress p;
	const std::string path = OrbisDir("covers") + "/progress.txt";
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || time(nullptr) - st.st_mtime > 60)
		return p;
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return p;
	char state[32] = {};
	p.valid = fscanf(f, "%d %d %31s", &p.left, &p.fetched, state) == 3;
	fclose(f);
	p.state = state;
	return p;
}
} // namespace fe

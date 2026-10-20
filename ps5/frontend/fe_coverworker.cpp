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

// One kind of download: the files wanted (the app writes <lists>/wanted.txt, and <lists>/priority.txt with the ones it needs
// first), where they go, and how far the worker got (<lists>/progress.txt).
struct Lane
{
	const char* tag; // for the log: "covers", "borders"
	std::string files_dir, lists_dir;
	std::vector<WantedCover> wanted, priority;
	time_t wanted_mtime = 0;
	off_t wanted_size = 0;
	std::string priority_text; // priority.txt is small: it is read every round and compared as text (a new list of the same size
	                           // written within the same second is still seen; the mtime would not tell)
	size_t cursor = 0;
	int left = 0, fetched = 0;
	std::map<std::string, time_t> tried; // file -> when it last failed (not a 404)
	std::string last_progress;
	time_t last_write = 0;

	Lane(const char* t, std::string files, std::string lists) : tag(t), files_dir(std::move(files)), lists_dir(std::move(lists)) {}

	bool Needs(const WantedCover& w)
	{
		auto t = tried.find(w.file);
		if (t != tried.end() && time(nullptr) - t->second < kFailRetrySeconds)
			return false;
		const std::string path = files_dir + "/" + w.file;
		const std::string base = path.substr(0, path.size() - 4);
		struct stat st = {};
		if (stat((base + ".refetch").c_str(), &st) == 0)
			return true; // Square on the shelf: asked for again
		return !NonEmptyFile(path) && !RecentlyMissing(base + ".missing", w.url);
	}

	void Reload()
	{
		const std::string wpath = lists_dir + "/wanted.txt", ppath = lists_dir + "/priority.txt";
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
			OrbisLog("[%s] wanted list: %zu file(s), %d to fetch", tag, wanted.size(), left);
		}
		std::string text;
		time_t mtime = 0;
		off_t size = 0;
		if (!ReadList(ppath, &text, &mtime, &size))
			text.clear();
		if (text != priority_text)
		{
			priority_text = text;
			priority = ParseWantedList(text);
		}
	}

	void Progress(const char* state)
	{
		char line[96];
		snprintf(line, sizeof(line), "%d %d %s\n", left, fetched, state);
		// written when it changes, and at least every 20 s while there is work (so the shelf sees it's alive)
		const time_t now = time(nullptr);
		if (line == last_progress && (left == 0 || now - last_write < 20))
			return;
		last_progress = line;
		last_write = now;
		WriteFileAtomicTo(lists_dir + "/progress.txt", std::vector<uint8_t>(line, line + strlen(line)));
	}

	const WantedCover* NextPriority()
	{
		for (const WantedCover& w : priority)
			if (Needs(w))
				return &w;
		return nullptr;
	}

	const WantedCover* NextWanted()
	{
		while (cursor < wanted.size())
		{
			if (Needs(wanted[cursor]))
				return &wanted[cursor];
			cursor++;
		}
		return nullptr;
	}
};
} // namespace

struct DownloadWorker::Impl
{
	Lane covers, borders;
	std::unique_ptr<HttpClient> http;
	time_t offline_until = 0;

	Impl(std::unique_ptr<HttpClient> h, const std::string& covers_dir, const std::string& borders_dir)
		: covers("covers", covers_dir, covers_dir), borders("borders", borders_dir + "/games", borders_dir), http(std::move(h))
	{
	}

	// The border the player waits for, the covers around the selection, the other covers, the borders of the library.
	Lane* Pick(const WantedCover** next)
	{
		if ((*next = borders.NextPriority()))
			return &borders;
		if ((*next = covers.NextPriority()))
			return &covers;
		if ((*next = covers.NextWanted()))
			return &covers;
		if ((*next = borders.NextWanted()))
			return &borders;
		return nullptr;
	}

	int Step()
	{
		covers.Reload();
		borders.Reload();
		if (time(nullptr) < offline_until)
		{
			covers.Progress("offline");
			borders.Progress("offline");
			return 1000;
		}
		const WantedCover* next = nullptr;
		Lane* lane = Pick(&next);
		if (!lane)
		{
			covers.left = 0;
			borders.left = 0;
			covers.Progress("idle");
			borders.Progress("idle");
			return 500;
		}
		const WantedCover w = *next; // the lists can be reloaded below
		covers.Progress(lane == &covers || covers.left > 0 ? "busy" : "idle");
		borders.Progress(lane == &borders || borders.left > 0 ? "busy" : "idle");
		std::vector<uint8_t> data;
		const int status = FetchCoverUrl(*http, w.url, w.file, data);
		const int saved = SaveCoverResult(lane->files_dir, w, status, data);
		OrbisLog("[%s] %s -> %d (%zu bytes)%s", lane->tag, w.file.c_str(), status, data.size(),
			saved == 1 ? "" : saved == 0 ? ": not on the server" : ": not saved");
		if (saved == 1)
			lane->fetched++;
		if (saved < 0)
			lane->tried[w.file] = time(nullptr);
		if (lane->left > 0)
			lane->left--;
		if (status == -1 && http->Offline())
		{
			OrbisLog("[%s] the console is offline: trying again in %ld s", lane->tag, kOfflinePauseSeconds);
			http->Term();
			offline_until = time(nullptr) + kOfflinePauseSeconds;
		}
		return 0;
	}
};

DownloadWorker::DownloadWorker(std::unique_ptr<HttpClient> http, const std::string& covers_dir, const std::string& borders_dir)
	: m_i(new Impl(std::move(http), covers_dir, borders_dir))
{
}

DownloadWorker::~DownloadWorker() = default;

int DownloadWorker::Step()
{
	return m_i->Step();
}

void DownloadWorker::Run()
{
	OrbisLog("[covers] background downloads ready (%s, %s)", m_i->covers.files_dir.c_str(), m_i->borders.files_dir.c_str());
	for (;;)
	{
		const int wait_ms = Step();
		if (wait_ms > 0)
			usleep(useconds_t(wait_ms) * 1000);
	}
}

namespace
{
void* WorkerMain(void*)
{
	static DownloadWorker worker(MakeCoverWorkerHttp(), OrbisDir("covers"), OrbisDir("borders")); // lives as long as the helper
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
	OrbisMkdirs(OrbisDir("borders/games"));
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

namespace
{
CoverProgress ReadProgress(const std::string& dir)
{
	CoverProgress p;
	const std::string path = dir + "/progress.txt";
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
} // namespace

CoverProgress ReadCoverProgress()
{
	return ReadProgress(OrbisDir("covers"));
}

CoverProgress ReadBorderProgress()
{
	return ReadProgress(OrbisDir("borders"));
}
} // namespace fe

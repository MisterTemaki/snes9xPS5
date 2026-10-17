// The helper's download loop with two lanes, covers and borders (frontend/fe_coverworker.cpp), against a fake HTTP client:
// the order files are fetched in, where they land, the 404 marker, the progress files, and the offline pause.
// SPDX-License-Identifier: MIT

#include "fe_coverworker.h"
#include "fe_http.h"

#include "OrbisPaths.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
int g_failed = 0;
int g_checked = 0;
#define CHECK(cond, what)                                                                          \
	do                                                                                             \
	{                                                                                              \
		g_checked++;                                                                               \
		if (!(cond))                                                                               \
		{                                                                                          \
			g_failed++;                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what);                                  \
		}                                                                                          \
	} while (0)

// Answers from a table: address -> status. 200 gives a PNG's first bytes.
struct FakeHttp : fe::HttpClient
{
	std::map<std::string, int> status;
	std::vector<std::string> asked;
	bool offline = false;
	int Get(const std::string& url, std::vector<uint8_t>& out, size_t) override
	{
		asked.push_back(url);
		if (offline)
			return -1;
		const auto it = status.find(url);
		const int s = it == status.end() ? 200 : it->second;
		out.clear();
		if (s == 200)
			out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 'x', 'y'};
		return s;
	}
	bool Offline() const override { return offline; }
	void Term() override {}
};

std::string Read(const std::string& path)
{
	std::string out;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		char buf[1024];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			out.append(buf, n);
		fclose(f);
	}
	return out;
}

void Write(const std::string& path, const std::string& text)
{
	if (FILE* f = fopen(path.c_str(), "wb"))
	{
		fwrite(text.data(), 1, text.size(), f);
		fclose(f);
	}
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

// Steps until the worker has nothing to do (returns a wait), at most `max` rounds.
int Drain(fe::DownloadWorker& w, int max = 50)
{
	int rounds = 0;
	while (rounds < max && w.Step() == 0)
		rounds++;
	return rounds;
}
} // namespace

int main()
{
	char dir[] = "/tmp/download-worker-test-XXXXXX";
	if (!mkdtemp(dir))
	{
		perror("mkdtemp");
		return 2;
	}
	const std::string root = dir, covers = root + "/covers", borders = root + "/borders";
	mkdir(covers.c_str(), 0777);
	mkdir(borders.c_str(), 0777);
	mkdir((borders + "/games").c_str(), 0777);
	setenv("SNES9X_PS5_ROOT", dir, 1);

	// priority first (the border of the game being played, then the covers around the selection), then the rest
	Write(covers + "/wanted.txt", "C1.png\thttp://h/c1\nC2.png\thttp://h/c2\n");
	Write(covers + "/priority.txt", "C2.png\thttp://h/c2\n");
	Write(borders + "/wanted.txt", "B1 (USA).png\thttp://h/b1\nB2 (USA).png\thttp://h/b2\n");
	Write(borders + "/priority.txt", "B3 (USA).png\thttp://h/b3\n");
	auto http = std::make_unique<FakeHttp>();
	FakeHttp* fake = http.get();
	fe::DownloadWorker worker(std::move(http), covers, borders);
	const int rounds = Drain(worker);
	CHECK(rounds == 5, "five files fetched, then the worker waits");
	const std::vector<std::string> want = {"http://h/b3", "http://h/c2", "http://h/c1", "http://h/b1", "http://h/b2"};
	CHECK(fake->asked == want, "order: the playing game's border, covers around the selection, other covers, the library's borders");
	CHECK(Exists(borders + "/games/B3 (USA).png") && Exists(borders + "/games/B1 (USA).png") && Exists(borders + "/games/B2 (USA).png"),
		"borders land in borders/games/");
	CHECK(Exists(covers + "/C1.png") && Exists(covers + "/C2.png"), "covers land in covers/");
	CHECK(!Exists(covers + "/B1 (USA).png") && !Exists(borders + "/C1.png"), "and nowhere else");

	const fe::CoverProgress bp = fe::ReadBorderProgress(), cp = fe::ReadCoverProgress();
	CHECK(bp.valid && bp.left == 0 && bp.fetched == 3 && bp.state == "idle", "borders/progress.txt: nothing left, 3 fetched");
	CHECK(cp.valid && cp.left == 0 && cp.fetched == 2 && cp.state == "idle", "covers/progress.txt: nothing left, 2 fetched");

	// nothing is asked twice
	fake->asked.clear();
	CHECK(worker.Step() > 0 && fake->asked.empty(), "a file already there is not fetched again");

	// a 404: the marker holds the address, no second request
	Write(borders + "/priority.txt", "Gone (USA).png\thttp://h/gone\n");
	fake->status["http://h/gone"] = 404;
	Drain(worker);
	CHECK(fake->asked.size() == 1 && Read(borders + "/games/Gone (USA).missing") == "http://h/gone\n", "a 404 leaves <name>.missing with the address");
	fake->asked.clear();
	Write(borders + "/priority.txt", "Gone (USA).png\thttp://h/gone\n# changed\n");
	Drain(worker);
	CHECK(fake->asked.empty(), "and the border is not asked for again");

	// a server error: tried once, not again within ten minutes, no marker
	Write(borders + "/priority.txt", "Sick (USA).png\thttp://h/sick\n");
	fake->status["http://h/sick"] = 500;
	fake->asked.clear();
	Drain(worker);
	CHECK(fake->asked.size() == 1 && !Exists(borders + "/games/Sick (USA).missing") && !Exists(borders + "/games/Sick (USA).png"), "a 500 leaves nothing");
	fake->asked.clear();
	worker.Step();
	CHECK(fake->asked.empty(), "and is not retried at once");

	// offline: a pause, nothing marked, the file comes later
	Write(borders + "/priority.txt", "Late (USA).png\thttp://h/late\n");
	fake->offline = true;
	fake->asked.clear();
	worker.Step();
	CHECK(fake->asked.size() == 1 && !Exists(borders + "/games/Late (USA).missing"), "offline: asked once, no 404 marker");
	CHECK(worker.Step() >= 500 && fake->asked.size() == 1, "and the worker pauses instead of asking again");
	CHECK(fe::ReadBorderProgress().state == "offline", "borders/progress.txt says offline");

	// names that could leave the folder are refused by the list parser
	Write(borders + "/priority.txt", "../escape.png\thttp://h/e\n");
	Write(borders + "/wanted.txt", "sub/dir.png\thttp://h/s\n");
	fake->offline = false;
	fake->asked.clear();
	for (int i = 0; i < 3; i++)
		worker.Step();
	CHECK(fake->asked.empty() && !Exists(root + "/escape.png"), "a name with a folder part is never fetched or written");

	if (FILE* p = popen((std::string("rm -rf '") + dir + "'").c_str(), "r"))
		pclose(p);
	printf("%d checks, %d failed\n", g_checked, g_failed);
	return g_failed ? 1 : 0;
}

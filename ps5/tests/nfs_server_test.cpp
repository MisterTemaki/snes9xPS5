// The NFS backend on real libnfs against a real NFSv3 server (unfs3, started by nfs_server_test.sh). Not part of
// `make test`: it needs rpcbind and unfsd. Without NFS_TEST_HOST it does nothing and passes.
//
// Environment: NFS_TEST_HOST (server), NFS_TEST_EXPORT (exported folder as the share path, no leading '/'),
// NFS_TEST_DIR (the same folder on this machine, to compare bytes), NFS_TEST_DENIED (a path the server lists but
// refuses to this client), NFS_TEST_SERVER_PID (the server, for the stall test).
//
// SPDX-License-Identifier: MIT
#include <signal.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../netfs/netfs_backend.h"

using namespace netfs;

static int g_fail = 0;
static int g_tests = 0;

#define CHECK(cond)                                                                \
	do                                                                             \
	{                                                                              \
		if (!(cond))                                                               \
		{                                                                          \
			std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_fail++;                                                              \
		}                                                                          \
	} while (0)

static std::string Env(const char* name)
{
	const char* v = std::getenv(name);
	return v ? v : "";
}

static ShareConfig Cfg(const std::string& path)
{
	ShareConfig c;
	c.scheme = Scheme::Nfs;
	c.host = Env("NFS_TEST_HOST");
	c.path = path;
	return c;
}

static std::string ReadLocal(const std::string& rel)
{
	std::ifstream f(Env("NFS_TEST_DIR") + "/" + rel, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static const DirEntry* Find(const std::vector<DirEntry>& v, const char* name)
{
	for (const DirEntry& e : v)
		if (e.name == name)
			return &e;
	return nullptr;
}

static void Report(const char* name, int before)
{
	g_tests++;
	std::printf("%s %s\n", g_fail == before ? "  ok:" : "  FAILED:", name);
}

static void ListingHasSizesTypesAndFollowedLinks()
{
	const ShareConfig cfg = Cfg(Env("NFS_TEST_EXPORT"));
	auto b = MakeNfsBackend();
	const Status s = b->Connect(cfg);
	CHECK(s.ok());
	std::vector<DirEntry> e;
	CHECK(b->List("", e).ok());
	const std::string game = ReadLocal("Game.sfc");
	const DirEntry* g = Find(e, "Game.sfc");
	CHECK(g && !g->attr.is_dir && g->attr.size == game.size() && g->attr.mtime > 0);
	const DirEntry* h = Find(e, "hacks");
	CHECK(h && h->attr.is_dir);
	CHECK(Find(e, "dir with space") != nullptr);
	const DirEntry* l = Find(e, "link.sfc");
	CHECK(l && !l->attr.is_dir && l->attr.size == game.size()); // a symlink reports its target
	CHECK(Find(e, "dead.sfc") == nullptr);                     // a dangling one is dropped
}

static void WholeFileReadsBackByteForByte()
{
	const ShareConfig cfg = Cfg(Env("NFS_TEST_EXPORT"));
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	Handle h = nullptr;
	CHECK(b->Open("Game.sfc", h).ok());
	std::string got;
	std::vector<char> buf(1u << 20);
	for (;;)
	{
		const int64_t n = b->Pread(h, buf.data(), buf.size(), got.size());
		CHECK(n >= 0);
		if (n <= 0)
			break;
		got.append(buf.data(), static_cast<size_t>(n));
	}
	CHECK(got == ReadLocal("Game.sfc"));
	b->Close(h);
}

static void FolderInsideTheExportMountsAndPathsAreRelativeToIt()
{
	const ShareConfig cfg = Cfg(Env("NFS_TEST_EXPORT") + "/hacks");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	std::vector<DirEntry> e;
	CHECK(b->List("", e).ok());
	CHECK(e.size() == 1 && e[0].name == "Hack.sfc");
	Attr a;
	CHECK(b->Stat("Hack.sfc", a).ok() && a.size == ReadLocal("hacks/Hack.sfc").size());
}

static void NamesWithSpacesWork()
{
	const ShareConfig cfg = Cfg(Env("NFS_TEST_EXPORT") + "/dir with space");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	Attr a;
	CHECK(b->Stat("a.sfc", a).ok() && !a.is_dir);
}

static void MissingFolderIsFolderNotFound()
{
	const ShareConfig cfg = Cfg(Env("NFS_TEST_EXPORT") + "/nope");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::FolderNotFound);
}

static void UnexportedPathListsTheExports()
{
	const ShareConfig cfg = Cfg("tmp/definitely-not-exported");
	auto b = MakeNfsBackend();
	const Status s = b->Connect(cfg);
	CHECK(s.kind == ErrKind::ShareNotFound);
	bool listed = false;
	for (const std::string& x : s.available)
		listed = listed || ("/" + Env("NFS_TEST_EXPORT")) == x;
	CHECK(listed);
}

static void RefusedExportIsAccessDenied()
{
	if (Env("NFS_TEST_DENIED").empty())
		return;
	const ShareConfig cfg = Cfg(Env("NFS_TEST_DENIED"));
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::AccessDenied);
}

static void MissingFileAndFolderOpenFail()
{
	const ShareConfig cfg = Cfg(Env("NFS_TEST_EXPORT"));
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	Handle h = nullptr;
	CHECK(b->Open("missing.sfc", h).err == ENOENT);
	CHECK(b->Open("hacks", h).err == EISDIR);
}

static void StoppedServerFailsTheReadInTimeAndTheConnectionStaysDead()
{
	const std::string pid = Env("NFS_TEST_SERVER_PID");
	if (pid.empty())
		return;
	const ShareConfig cfg = Cfg(Env("NFS_TEST_EXPORT"));
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	Handle h = nullptr;
	CHECK(b->Open("Game.sfc", h).ok());
	const pid_t server = static_cast<pid_t>(std::atoi(pid.c_str()));
	kill(server, SIGSTOP);
	std::vector<char> buf(1u << 20);
	const auto t0 = std::chrono::steady_clock::now();
	const int64_t n = b->Pread(h, buf.data(), buf.size(), 0);
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	kill(server, SIGCONT);
	CHECK(n == -EIO);
	CHECK(secs > 3.0 && secs < 5.0); // the 4.5 s deadline, under the 5 s watchdog
	Attr a;
	CHECK(b->Stat("Game.sfc", a).err == EIO); // nothing more is sent on a connection that lost a reply
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (Env("NFS_TEST_HOST").empty())
	{
		std::printf("skipped: NFS_TEST_HOST not set (see nfs_server_test.sh)\n");
		return 0;
	}
	struct
	{
		const char* name;
		void (*fn)();
	} tests[] = {
	    {"listing_has_sizes_types_and_followed_links", ListingHasSizesTypesAndFollowedLinks},
	    {"whole_file_reads_back_byte_for_byte", WholeFileReadsBackByteForByte},
	    {"folder_inside_the_export_mounts", FolderInsideTheExportMountsAndPathsAreRelativeToIt},
	    {"names_with_spaces_work", NamesWithSpacesWork},
	    {"missing_folder_is_folder_not_found", MissingFolderIsFolderNotFound},
	    {"unexported_path_lists_the_exports", UnexportedPathListsTheExports},
	    {"refused_export_is_access_denied", RefusedExportIsAccessDenied},
	    {"missing_file_and_folder_open_fail", MissingFileAndFolderOpenFail},
	    {"stopped_server_fails_the_read_in_time", StoppedServerFailsTheReadInTimeAndTheConnectionStaysDead},
	};
	for (const auto& t : tests)
	{
		const int before = g_fail;
		t.fn();
		Report(t.name, before);
	}
	std::printf("%d tests, %d failed checks\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}

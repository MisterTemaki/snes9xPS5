// Unit test of the NFS backend (netfs/backend_nfs.cpp) against an in-memory libnfs (libnfs_mock.cpp). No
// network, no sleeping: the backend's clock is a counter that the mock advances on every nfs_service call.
// A real-server run (a userspace NFS server) is the optional integration job in the plan; this test covers
// what the backend itself does: path handling, the mount fallback, error classification, listings, reads,
// Abort() and what happens to a connection that gives up.
//
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../netfs/backend_nfs.h"
#include "../netfs/netfs_backend.h"
#include "libnfs_mock.h"

using namespace netfs;
using mocknfs::S;

static int g_fail = 0;
static int g_tests = 0;
static int64_t g_clock = 0;

#define CHECK(cond)                                                                   \
	do                                                                                \
	{                                                                                 \
		if (!(cond))                                                                  \
		{                                                                             \
			std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			g_fail++;                                                                 \
		}                                                                             \
	} while (0)

static int64_t FakeNow()
{
	return g_clock;
}

// A server with /export/snes (Game.sfc, hacks/) and /export/other, listed by mountd.
static void Fixture()
{
	mocknfs::Reset();
	g_clock = 0;
	NfsTestHooks hooks;
	hooks.now_ms = FakeNow;
	hooks.poll_ms = 0;
	NfsSetTestHooks(hooks);
	S().on_service = [] { g_clock += 50; };
	for (const char* d : {"/", "/export", "/export/snes", "/export/snes/hacks", "/export/other"})
		mocknfs::AddDir(d);
	mocknfs::AddFile("/export/snes/Game.sfc", "0123456789abcdef", 1700000123);
	mocknfs::AddFile("/export/snes/hacks/Hack.sfc", "hack");
	S().exports = {"/export/snes", "/export/other"};
}

static ShareConfig Cfg(const char* path = "export/snes")
{
	ShareConfig c;
	c.scheme = Scheme::Nfs;
	c.host = "nas";
	c.path = path;
	return c;
}

static bool HasName(const std::vector<DirEntry>& v, const char* name)
{
	for (const DirEntry& e : v)
		if (e.name == name)
			return true;
	return false;
}

static const DirEntry* Find(const std::vector<DirEntry>& v, const char* name)
{
	for (const DirEntry& e : v)
		if (e.name == name)
			return &e;
	return nullptr;
}

#define TEST(name)                       \
	static void name();                  \
	static struct Reg_##name             \
	{                                    \
		Reg_##name() { Tests().push_back({#name, name}); } \
	} reg_##name;                        \
	static void name()

struct T
{
	const char* name;
	void (*fn)();
};
static std::vector<T>& Tests()
{
	static std::vector<T> t;
	return t;
}

// ---- connecting

TEST(connect_mounts_the_whole_path_first)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	CHECK((S().mount_paths == std::vector<std::string>{"/export/snes"}));
	CHECK(S().mount_host == "nas");
}

TEST(connect_with_an_empty_path_mounts_the_root)
{
	Fixture();
	S().accepts = {"/"};
	ShareConfig cfg = Cfg("");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	CHECK((S().mount_paths == std::vector<std::string>{"/"}));
	std::vector<DirEntry> e;
	CHECK(b->List("", e).ok());
	CHECK(HasName(e, "export"));
}

TEST(connect_goes_one_folder_higher_until_a_mount_works)
{
	Fixture();
	S().accepts = {"/export/snes"};
	ShareConfig cfg = Cfg("export/snes/hacks");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	CHECK((S().mount_paths == std::vector<std::string>{"/export/snes/hacks", "/export/snes"}));
	std::vector<DirEntry> e; // paths are now relative to the folder inside the export
	CHECK(b->List("", e).ok());
	CHECK(e.size() == 1 && e[0].name == "Hack.sfc");
}

TEST(connect_reports_a_missing_folder_inside_a_good_export)
{
	Fixture();
	S().accepts = {"/export/snes"};
	ShareConfig cfg = Cfg("export/snes/nope");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::FolderNotFound);
}

TEST(connect_reports_a_file_where_a_folder_is_expected)
{
	Fixture();
	S().accepts = {"/export/snes"};
	ShareConfig cfg = Cfg("export/snes/Game.sfc");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::FolderNotFound);
}

TEST(connect_lists_the_exports_when_the_path_is_not_exported)
{
	Fixture();
	ShareConfig cfg = Cfg("export/nothing");
	auto b = MakeNfsBackend();
	const Status s = b->Connect(cfg);
	CHECK(s.kind == ErrKind::ShareNotFound);
	CHECK((s.available == std::vector<std::string>{"/export/snes", "/export/other"}));
}

TEST(connect_tries_at_most_eight_paths)
{
	Fixture();
	ShareConfig cfg = Cfg("a/b/c/d/e/f/g/h/i/j");
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::ShareNotFound);
	CHECK(S().mount_paths.size() == 8);
}

TEST(connect_says_access_denied_when_the_export_exists_but_refuses)
{
	Fixture();
	S().exports_denied = true;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::AccessDenied);
}

TEST(connect_says_access_denied_when_every_refusal_is_a_permission_error_and_no_list_exists)
{
	Fixture();
	S().exports_denied = true;
	S().exports.clear();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::AccessDenied);
}

TEST(connect_stops_at_once_when_the_connection_is_refused)
{
	Fixture();
	S().refuse = true;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::Refused);
	CHECK(S().mount_paths.size() == 1);
}

TEST(connect_gives_up_on_a_silent_server_without_sleeping)
{
	Fixture();
	S().hang = true;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::NoAnswer);
	CHECK(g_clock >= 4500 && g_clock < 5000); // under the 5 s watchdog
}

TEST(abort_ends_a_connect_in_flight)
{
	Fixture();
	S().hang = true;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	Backend* raw = b.get();
	S().on_service = [raw] {
		g_clock += 50;
		if (S().services == 3)
			raw->Abort();
	};
	CHECK(b->Connect(cfg).kind == ErrKind::Aborted);
	CHECK(S().services == 3); // the wait loop saw the flag at its next step
}

TEST(connect_refuses_other_schemes)
{
	Fixture();
	ShareConfig cfg = Cfg();
	cfg.scheme = Scheme::Smb;
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).kind == ErrKind::Other);
}

TEST(connect_passes_the_options_to_libnfs)
{
	Fixture();
	ShareConfig cfg = Cfg();
	cfg.host = "[fe80::1]";
	cfg.nfs_version = 4;
	cfg.nfsport = 2050;
	cfg.mountport = 2051;
	cfg.uid = 1000;
	cfg.gid = 100;
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	CHECK(S().mount_host == "fe80::1");
	CHECK(S().version_set == 4);
	CHECK(S().nfsport_set == 2050 && S().mountport_set == 2051);
	CHECK(S().uid_set == 1000 && S().gid_set == 100);
}

TEST(connect_takes_the_url_port_as_the_nfs_port)
{
	Fixture();
	ShareConfig cfg = Cfg();
	cfg.port = 2049;
	auto b = MakeNfsBackend();
	CHECK(b->Connect(cfg).ok());
	CHECK(S().nfsport_set == 2049);
}

TEST(enumerate_gives_the_exports_and_nfs4_has_none)
{
	Fixture();
	ShareConfig cfg = Cfg("export/nothing");
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	std::vector<std::string> out;
	CHECK(b->Enumerate(out).ok());
	CHECK(out.size() == 2);

	ShareConfig v4 = Cfg("export/nothing");
	v4.nfs_version = 4;
	auto b4 = MakeNfsBackend();
	b4->Connect(v4);
	CHECK(b4->Enumerate(out).err == ENOTSUP);
}

// ---- files and folders

TEST(stat_reports_size_type_and_mtime)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Attr f, d;
	CHECK(b->Stat("Game.sfc", f).ok());
	CHECK(!f.is_dir && f.size == 16 && f.mtime == 1700000123);
	CHECK(b->Stat("hacks", d).ok());
	CHECK(d.is_dir && d.size == 0);
	CHECK(b->Stat("", d).ok() && d.is_dir);
	CHECK(b->Stat("missing.sfc", f).err == ENOENT);
}

TEST(list_gives_names_with_attributes_and_no_dot_entries)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	std::vector<DirEntry> e;
	CHECK(b->List("", e).ok());
	CHECK(e.size() == 2);
	CHECK(!HasName(e, ".") && !HasName(e, ".."));
	const DirEntry* g = Find(e, "Game.sfc");
	CHECK(g && !g->attr.is_dir && g->attr.size == 16 && g->attr.mtime == 1700000123);
	const DirEntry* h = Find(e, "hacks");
	CHECK(h && h->attr.is_dir);
}

TEST(list_follows_symlinks_and_drops_dangling_ones)
{
	Fixture();
	mocknfs::Node link;
	link.link = "/export/snes/Game.sfc";
	S().nodes["/export/snes/Link.sfc"] = link;
	mocknfs::Node dead;
	dead.link = "/nowhere";
	S().nodes["/export/snes/Dead.sfc"] = dead;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	std::vector<DirEntry> e;
	CHECK(b->List("", e).ok());
	const DirEntry* l = Find(e, "Link.sfc");
	CHECK(l && !l->attr.is_dir && l->attr.size == 16); // the target's size, not the link's
	CHECK(!HasName(e, "Dead.sfc"));
}

TEST(list_asks_for_the_attributes_the_server_left_out)
{
	Fixture();
	S().nodes["/export/snes/Game.sfc"].no_attrs = true;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	std::vector<DirEntry> e;
	CHECK(b->List("", e).ok());
	const DirEntry* g = Find(e, "Game.sfc");
	CHECK(g && g->attr.size == 16);
}

TEST(list_of_a_missing_or_plain_path_fails_with_errno)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	std::vector<DirEntry> e;
	CHECK(b->List("nope", e).err == ENOENT);
	CHECK(b->List("Game.sfc", e).err == ENOTDIR);
}

TEST(open_refuses_folders_and_missing_files)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	CHECK(b->Open("hacks", h).err == EISDIR);
	CHECK(b->Open("missing.sfc", h).err == ENOENT);
}

TEST(pread_returns_the_bytes_at_the_offset)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	CHECK(b->Open("Game.sfc", h).ok());
	char buf[8] = {};
	CHECK(b->Pread(h, buf, 4, 2) == 4);
	CHECK(std::memcmp(buf, "2345", 4) == 0);
	b->Close(h);
}

TEST(pread_at_or_past_the_end_returns_zero_and_a_tail_is_short)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	char buf[32];
	CHECK(b->Pread(h, buf, sizeof buf, 16) == 0);
	CHECK(b->Pread(h, buf, sizeof buf, 100) == 0);
	CHECK(b->Pread(h, buf, sizeof buf, 14) == 2);
	b->Close(h);
}

static std::string Pattern(size_t n)
{
	std::string s(n, '\0');
	for (size_t i = 0; i < n; i++)
		s[i] = static_cast<char>(i % 251);
	return s;
}

TEST(pread_splits_a_big_read_into_small_pipelined_requests)
{
	Fixture();
	const std::string data = Pattern(3u << 20);
	mocknfs::AddFile("/export/snes/Big.bin", data);
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Big.bin", h);
	std::vector<char> buf(2u << 20);
	CHECK(b->Pread(h, buf.data(), buf.size(), 5) == (1 << 20)); // the contract's 1 MiB limit
	CHECK(std::memcmp(buf.data(), data.data() + 5, 1 << 20) == 0);
	CHECK(S().max_read_len == (128 << 10)); // no READ above 128 KiB: libnfs cannot join a fragmented reply
	CHECK(S().max_in_flight == 8);          // all eight queued before the first is serviced
	b->Close(h);
}

TEST(pread_uses_the_servers_smaller_limit)
{
	Fixture();
	S().rtmax = 32 << 10;
	mocknfs::AddFile("/export/snes/Big.bin", Pattern(2u << 20));
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Big.bin", h);
	std::vector<char> buf(1u << 20);
	CHECK(b->Pread(h, buf.data(), buf.size(), 0) == (1 << 20));
	CHECK(S().max_read_len == (32 << 10));
	b->Close(h);
}

TEST(pread_near_the_end_of_a_big_file_returns_just_the_tail)
{
	Fixture();
	const std::string data = Pattern(3u << 20);
	mocknfs::AddFile("/export/snes/Big.bin", data);
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Big.bin", h);
	std::vector<char> buf(1u << 20);
	CHECK(b->Pread(h, buf.data(), buf.size(), data.size() - 10) == 10);
	CHECK(std::memcmp(buf.data(), data.data() + data.size() - 10, 10) == 0);
	CHECK(b->Pread(h, buf.data(), buf.size(), data.size()) == 0);
	b->Close(h);
}

TEST(pread_passes_a_short_count_through)
{
	Fixture();
	S().short_read = 5;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	char buf[16];
	CHECK(b->Pread(h, buf, sizeof buf, 0) == 5);
	b->Close(h);
}

// ---- a connection that goes away

TEST(abort_makes_every_later_call_fail_and_touches_libnfs_no_more)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	b->Abort();
	const int services = S().services;
	Attr a;
	std::vector<DirEntry> e;
	char buf[4];
	CHECK(b->Stat("Game.sfc", a).err == EIO);
	CHECK(b->List("", e).err == EIO);
	CHECK(b->Pread(h, buf, 4, 0) == -EIO);
	b->Close(h);
	CHECK(S().services == services);
}

TEST(a_read_the_server_never_answers_ends_the_connection)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	S().hang = true;
	char buf[4];
	CHECK(b->Pread(h, buf, 4, 0) == -EIO);
	const int services = S().services;
	Attr a;
	CHECK(b->Stat("Game.sfc", a).err == EIO);
	CHECK(S().services == services); // nothing more is sent on a connection with a lost reply
	b->Close(h);
}

TEST(a_socket_failure_ends_the_connection)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	S().fail_service_at = S().services + 1;
	char buf[4];
	CHECK(b->Pread(h, buf, 4, 0) == -EIO);
	Attr a;
	CHECK(b->Stat("Game.sfc", a).err == EIO);
	CHECK(S().calls_after_death == 0);
}

TEST(files_open_when_the_connection_is_aborted_are_freed)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr, h2 = nullptr;
	b->Open("Game.sfc", h);
	b->Open("Game.sfc", h2);
	b->Abort();
	b->Close(h);
	b->Disconnect();
	CHECK(S().leaked_handles == 0); // h2 was never closed: Disconnect frees it
}

TEST(files_open_when_the_socket_fails_are_freed)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	S().fail_service_at = S().services + 1;
	char buf[4];
	CHECK(b->Pread(h, buf, 4, 0) == -EIO);
	b->Close(h);
	b->Disconnect();
	CHECK(S().leaked_handles == 0);
}

TEST(files_open_when_a_read_never_answers_are_freed)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	S().hang = true;
	char buf[4];
	CHECK(b->Pread(h, buf, 4, 0) == -EIO);
	b->Disconnect(); // the caller never got to Close
	CHECK(S().leaked_handles == 0);
}

TEST(closed_files_and_a_double_close_leave_nothing_behind)
{
	Fixture();
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	b->Connect(cfg);
	Handle h = nullptr;
	b->Open("Game.sfc", h);
	b->Close(h);
	b->Close(h); // not an open handle any more: ignored
	b->Disconnect();
	CHECK(S().leaked_handles == 0);
}

TEST(disconnect_after_a_failed_connect_and_after_abort_is_safe)
{
	Fixture();
	S().refuse = true;
	ShareConfig cfg = Cfg();
	auto b = MakeNfsBackend();
	CHECK(!b->Connect(cfg).ok());
	b->Disconnect();
	b->Disconnect();
	auto c = MakeNfsBackend();
	c->Abort();
	c->Disconnect();
	CHECK(c->Connect(cfg).kind == ErrKind::Aborted);
}

int main()
{
	for (const T& t : Tests())
	{
		const int before = g_fail;
		t.fn();
		g_tests++;
		std::printf("%s %s\n", g_fail == before ? "  ok:" : "  FAILED:", t.name);
	}
	std::printf("%d tests, %d failed checks\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}

// Snes9x PS5 host test of netfs: share addresses, redaction, routing, the libc --wrap layer, minizip and
// zlib on a share, and the watchdog, against an in-memory server (netfs_fake.h). Linked with the same --wrap
// list as the app (Makefile, NETFS_WRAPPED), so a missing entry there fails here.
//
// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>

#include "ProsperoCrash.h"
#include "netfs.h"
#include "netfs_cache.h"
#include "netfs_fake.h"
#include "unzip.h"

using namespace netfs;

// BigThread arms the crash handler per thread; the host test has none.
namespace crashlog
{
void* ArmThread() { return nullptr; }
void DisarmThread(void*) {}
} // namespace crashlog

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, label)                                                  \
	do                                                                      \
	{                                                                       \
		if (cond)                                                           \
			g_pass++;                                                       \
		else                                                                \
		{                                                                   \
			g_fail++;                                                       \
			printf("  FAIL: %s (test_netfs.cpp:%d)\n", label, __LINE__);   \
		}                                                                   \
	} while (0)

static std::string Blob(size_t n, uint32_t seed)
{
	std::string s(n, 0);
	uint32_t x = seed * 2654435761u + 1;
	for (size_t i = 0; i < n; i++)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		s[i] = (char)x;
	}
	return s;
}

static std::string PctAll(const std::string& s)
{
	std::string out;
	char b[4];
	for (unsigned char c : s)
	{
		if (isalnum(c))
			out.push_back((char)c);
		else
		{
			snprintf(b, sizeof b, "%%%02X", c);
			out += b;
		}
	}
	return out;
}

static bool Has(const std::string& hay, const std::string& needle)
{
	return hay.find(needle) != std::string::npos;
}

// Shares mounted against fake servers: the nth mounted share gets the nth server.
struct Rig
{
	std::vector<std::shared_ptr<FakeServer>> srv;
	Rig(const std::string& shares_text, int servers = 1)
	{
		for (int i = 0; i < servers; i++)
			srv.push_back(std::make_shared<FakeServer>());
		auto next = std::make_shared<std::atomic<int>>(0);
		std::vector<std::shared_ptr<FakeServer>> list = srv;
		NetfsBackendFactory f = [list, next]() -> std::unique_ptr<Backend> {
			int i = next->fetch_add(1);
			return std::make_unique<FakeBackend>(list[(size_t)i % list.size()]);
		};
		netfs_set_backend_factory(Scheme::Smb, f);
		netfs_set_backend_factory(Scheme::Nfs, f);
		netfs_load_text(shares_text);
	}
	void Mount() { netfs_mount_all_blocking(); }
	~Rig()
	{
		netfs_stop();
		netfs_set_backend_factory(Scheme::Smb, nullptr);
		netfs_set_backend_factory(Scheme::Nfs, nullptr);
		netfs_set_clock(nullptr);
	}
};

// ---------------------------------------------------------------- addresses

static void TestParse()
{
	ShareConfig c;
	std::string err;
	CHECK(netfs_parse_share("smb://192.168.1.50/games/snes", c, err), "guest address parses");
	CHECK(c.scheme == Scheme::Smb && c.host == "192.168.1.50" && c.share == "games" && c.path == "snes" && c.user.empty() && c.port == 0,
		"guest address fields");
	CHECK(netfs_mount_point(c) == "/smb/192.168.1.50/games/snes", "mount point of a guest address");

	CHECK(netfs_parse_share("smb://WORKGROUP;jason@NAS:4455/games?password=p%40ss%26word%3D1&vers=3&sign", c, err), "full address parses");
	CHECK(c.domain == "WORKGROUP" && c.user == "jason" && c.host == "nas" && c.port == 4455 && c.share == "games" && c.path.empty(),
		"domain, user, host, port, share");
	CHECK(c.password == "p@ss&word=1" && c.vers == "3" && c.sign && !c.seal, "escaped password decodes; vers and sign set");
	CHECK(netfs_mount_point(c) == "/smb/nas:4455/games", "mount point keeps a non-default port, drops the folder-less tail");

	CHECK(netfs_parse_share("smb://h/s?password=a%26sign%3D1", c, err) && c.password == "a&sign=1" && !c.sign,
		"an encoded delimiter never splits an option");
	CHECK(netfs_parse_share("smb://h/s?password=a#b", c, err) && c.password == "a#b", "a # inside an address belongs to it");
	CHECK(netfs_parse_share("smb://h/s?password=abc   # a note", c, err) && c.password == "abc", "a trailing # note is allowed");
	CHECK(netfs_parse_share("smb://h/my%20games/sub%20dir/%E3%82%B2", c, err) && c.share == "my games" && c.path == "sub dir/\xE3\x82\xB2",
		"spaces and UTF-8 bytes in names");
	CHECK(netfs_parse_share("smb://h//s///a//", c, err) && c.share == "s" && c.path == "a", "empty path segments are dropped");

	CHECK(netfs_parse_share("nfs://nas/export/snes?version=4&uid=1000&gid=100&nfsport=2049&mountport=20048", c, err), "nfs address parses");
	CHECK(c.scheme == Scheme::Nfs && c.host == "nas" && c.share.empty() && c.path == "export/snes" && c.nfs_version == 4 && c.uid == 1000 &&
			  c.gid == 100 && c.nfsport == 2049 && c.mountport == 20048,
		"nfs fields");
	CHECK(netfs_mount_point(c) == "/nfs/nas/export/snes", "nfs mount point");
	CHECK(netfs_parse_share("NFS://[::1]:2049/x", c, err) && c.host == "[::1]" && c.port == 2049, "IPv6 literal and a upper-case scheme");

	static const char* bad[] = {
		"smb://h/s/../x?password=SECRETPW",
		"smb://h/s/%2e%2E/x?password=SECRETPW",
		"smb://h/s/a%2Fb?password=SECRETPW",
		"smb://h/s/a%5Cb?password=SECRETPW",
		"smb://u:SECRETPW@h/s",
		"smb://a@b@h/s?password=SECRETPW",
		"smb://h/s?password=SECRETPW&bogus=1",
		"smb://h/s?password=SECRETPW&password=SECRETPW",
		"smb://h/s?password=SECRETPW%zz",
		"smb://h/s?password=SECRETPW%4",
		"smb://h/s?password=SECRETPW%00",
		"smb://h/s?password=SECRETPW&vers=9",
		"smb://h/s?password=SECRETPW&sign=maybe",
		"smb://h:0/s?password=SECRETPW",
		"smb://h:99999/s?password=SECRETPW",
		"smb://h:x/s?password=SECRETPW",
		"smb://h/s?password=SECRETPW extra",
		"smb://h?password=SECRETPW",
		"smb:///s?password=SECRETPW",
		"smb://h%/s?password=SECRETPW",
		"nfs://u@h/x?password=SECRETPW",
		"nfs://h/x?password=SECRETPW",
		"nfs://h/x?version=5&uid=SECRETPW",
		"nfs://h?uid=SECRETPW",
		"http://h/s?password=SECRETPW",
		"smb//h/s?password=SECRETPW",
	};
	bool all_rejected = true, none_leaked = true, all_reasoned = true;
	for (const char* line : bad)
	{
		ShareConfig x;
		std::string e;
		if (netfs_parse_share(line, x, e))
		{
			all_rejected = false;
			printf("    accepted: %s\n", line);
		}
		if (e.empty())
			all_reasoned = false;
		if (Has(e, "SECRETPW"))
			none_leaked = false;
	}
	CHECK(all_rejected, "every malformed address is rejected");
	CHECK(all_reasoned && none_leaked, "every rejection has a reason that holds no part of the address");
}

static void TestFile()
{
	NetfsShareFile f = netfs_parse_shares(
		"\xEF\xBB\xBF# comment\r\n\r\n  smb://a/s1   \r\nsmb://a/s1\nsmb://b/s?password=SECRETPW%zz\nnfs://c/e # tail\n   # indented comment\n");
	CHECK(f.shares.size() == 2 && f.shares[0].host == "a" && f.shares[1].host == "c", "BOM, CRLF, comments and blanks; two shares");
	CHECK(f.notices.size() == 2, "two notices");
	bool lines = f.notices.size() == 2 && Has(f.notices[0], "line 4") && Has(f.notices[1], "line 5");
	CHECK(lines, "notices name the lines (a duplicate folder, a bad escape)");
	bool clean = true;
	for (const std::string& n : f.notices)
		clean = clean && !Has(n, "SECRETPW") && !Has(n, "smb://");
	CHECK(clean, "notices never quote the line");

	std::string many;
	for (int i = 0; i < 10; i++)
		many += "smb://h" + std::to_string(i) + "/s\n";
	f = netfs_parse_shares(many);
	CHECK((int)f.shares.size() == kNetfsMaxShares && f.notices.size() == 2, "at most eight shares are used");

	f = netfs_parse_shares(netfs_shares_template());
	CHECK(f.shares.empty() && f.notices.empty(), "the template holds comments only");
}

static void TestRedact()
{
	ShareConfig c;
	std::string err;
	CHECK(netfs_parse_share("smb://WORKGROUP;jason@nas:4455/games/snes?password=x&vers=3&sign", c, err), "sample parses");
	CHECK(netfs_redact(c) == "smb://WORKGROUP;jason@nas:4455/games/snes?vers=3&sign", "redaction rebuilds from fields and drops the password");
	CHECK(netfs_parse_share("nfs://nas/export/snes?version=4&uid=7", c, err) && netfs_redact(c) == "nfs://nas/export/snes?version=4&uid=7",
		"nfs redaction");

	static const char* passwords[] = {"p@ss&word=1", "a%b", "x y", "hunter2", "pa;ss/w?rd#", "\xC3\xBC\xC3\xB6", "100%"};
	bool redact_ok = true, text_ok = true;
	for (const char* pw : passwords)
	{
		ShareConfig x;
		std::string e;
		std::string line = "smb://u@h/s?password=" + PctAll(pw);
		if (!netfs_parse_share(line, x, e) || x.password != pw)
		{
			printf("    did not round-trip: %s\n", pw);
			redact_ok = false;
			continue;
		}
		std::string r = netfs_redact(x);
		if (Has(r, pw) || Has(r, PctAll(pw)))
			redact_ok = false;
		for (ErrKind k : {ErrKind::NoAnswer, ErrKind::Refused, ErrKind::Closed, ErrKind::NameNotFound, ErrKind::AccessDenied,
				 ErrKind::ShareNotFound, ErrKind::FolderNotFound, ErrKind::Smb1Only, ErrKind::SigningRequired, ErrKind::NotBuilt,
				 ErrKind::Aborted, ErrKind::Other})
		{
			Status st = Status::Kind(k, std::string("server said ") + pw + " and " + PctAll(pw));
			st.available.push_back(pw);
			std::string t = netfs_error_text(st, x);
			if (t.empty() || Has(t, pw) || Has(t, PctAll(pw)))
			{
				text_ok = false;
				printf("    error text %d leaks or is empty for %s\n", (int)k, pw);
			}
		}
	}
	CHECK(redact_ok, "the redacted address holds no password, raw or encoded");
	CHECK(text_ok, "no error text holds a password, raw or encoded, even when the library echoes it");

	Status st = Status::Kind(ErrKind::ShareNotFound);
	st.available = {"games", "isos"};
	CHECK(Has(netfs_error_text(st, c), "exports") || Has(netfs_error_text(st, c), "isos"), "a missing share lists what the server offers");
}

// ---------------------------------------------------------------- the virtual file system

static const char* kBase = "/smb/192.168.1.50/games/snes";

static void TestRouting()
{
	Rig rig("smb://192.168.1.50/games/snes\nsmb://192.168.1.50/games/snes2\nnfs://nas/export/snes\n", 3);
	rig.srv[0]->Put("a.sfc", "first");
	rig.srv[1]->Put("a.sfc", "second");
	rig.srv[2]->Put("a.sfc", "third!");
	rig.Mount();
	std::vector<std::string> mp = netfs_mount_points();
	CHECK(mp.size() == 3 && mp[0] == kBase && mp[1] == std::string(kBase) + "2" && mp[2] == "/nfs/nas/export/snes", "three mount points once mounted");

	struct stat st;
	CHECK(stat("/smb/192.168.1.50/games/snes/a.sfc", &st) == 0 && st.st_size == 5, "share 1 routes");
	CHECK(stat("/smb/192.168.1.50/games/snes2/a.sfc", &st) == 0 && st.st_size == 6, "snes2 is not a prefix match of snes");
	CHECK(stat("/nfs/nas/export/snes/a.sfc", &st) == 0 && st.st_size == 6, "nfs routes");
	CHECK(stat("/smb/192.168.1.50/games/snes/./x/../a.sfc", &st) == 0 && st.st_size == 5, "dot segments resolve inside the share");
	CHECK(stat("/smb/192.168.1.50/games/snes/../snes2/a.sfc", &st) == 0 && st.st_size == 6, ".. into a sibling share resolves");
	errno = 0;
	CHECK(stat("/smb/192.168.1.50/games/snes/../../etc/passwd", &st) == -1 && errno == ENOENT, ".. out of every share goes to the real call");
	CHECK(stat("/smb/other/x/a.sfc", &st) == -1 && errno == ENOENT, "an unmounted /smb path goes to the real call");
	CHECK(stat("/smbx/a", &st) == -1 && stat("/sm", &st) == -1 && stat("/", &st) == 0, "near-miss and short paths are safe");
	CHECK(netfs_is_network_path("/smb/x") && netfs_is_network_path("/nfs/x") && !netfs_is_network_path("/mnt/usb0/x") &&
			  !netfs_is_network_path("smb/x") && !netfs_is_network_path("") && !netfs_is_network_path(nullptr),
		"netfs_is_network_path");
	char* rp = realpath("/smb/192.168.1.50/games/snes/x/../a.sfc", nullptr);
	CHECK(rp && std::string(rp) == "/smb/192.168.1.50/games/snes/a.sfc", "realpath is lexical and checks the file exists");
	free(rp);
	CHECK(realpath("/smb/192.168.1.50/games/snes/nope", nullptr) == nullptr && errno == ENOENT, "realpath of a missing file fails");
}

static void TestFiles()
{
	Rig rig("smb://192.168.1.50/games/snes\n");
	std::string zelda = Blob(1536 * 1024 + 123, 7), notes = "line one\nline two\nlast";
	rig.srv[0]->Put("Zelda.sfc", zelda);
	rig.srv[0]->Put("sub/Mario.smc", Blob(100000, 9));
	rig.srv[0]->Put("notes.txt", notes);
	rig.Mount();
	FakeServer& s = *rig.srv[0];
	std::string zp = std::string(kBase) + "/Zelda.sfc", np = std::string(kBase) + "/notes.txt";

	struct stat st;
	CHECK(stat(zp.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == zelda.size() && st.st_mtime == 1700000000, "stat of a file");
	CHECK(stat((std::string(kBase) + "/sub").c_str(), &st) == 0 && S_ISDIR(st.st_mode), "stat of a folder");
	CHECK(lstat(kBase, &st) == 0 && S_ISDIR(st.st_mode), "lstat of the share root");
	errno = 0;
	CHECK(stat((std::string(kBase) + "/missing").c_str(), &st) == -1 && errno == ENOENT, "stat of a missing file is ENOENT");
	CHECK(access(zp.c_str(), R_OK) == 0 && access(zp.c_str(), F_OK) == 0, "access: readable");
	errno = 0;
	CHECK(access(zp.c_str(), W_OK) == -1 && errno == EROFS, "access: not writable");

	// a sequential pass in small reads: every byte arrives once
	FILE* f = fopen(zp.c_str(), "rb");
	CHECK(f != nullptr, "fopen");
	std::string got;
	char buf[100000];
	size_t n;
	while (f && (n = fread(buf, 1, sizeof buf, f)) > 0)
		got.append(buf, n);
	CHECK(got == zelda, "fread in 100 KB steps returns the file");
	CHECK(f && feof(f) && !ferror(f) && ftell(f) == (long)zelda.size(), "feof and ftell at the end");
	CHECK(s.bytes_read == zelda.size(), "a sequential read asks the server for each byte once");
	clearerr(f);
	CHECK(!feof(f), "clearerr");

	CHECK(fseek(f, 12345, SEEK_SET) == 0 && fread(buf, 1, 10, f) == 10 && memcmp(buf, zelda.data() + 12345, 10) == 0, "fseek and read");
	CHECK(fseeko(f, -100, SEEK_END) == 0 && ftello(f) == (off_t)zelda.size() - 100 && fread(buf, 1, 200, f) == 100 && memcmp(buf, zelda.data() + zelda.size() - 100, 100) == 0 &&
			  feof(f),
		"fseeko from the end, short read at the end");
	CHECK(fseeko64(f, 777, SEEK_SET) == 0 && ftello64(f) == 777 && fseeko64(f, 3, SEEK_CUR) == 0 && ftello64(f) == 780, "fseeko64, ftello64, SEEK_CUR");
	errno = 0;
	CHECK(fseek(f, -1, SEEK_SET) == -1 && errno == EINVAL, "seek before the start is EINVAL");
	rewind(f);
	CHECK(ftell(f) == 0 && !feof(f) && fgetc(f) == (unsigned char)zelda[0], "rewind and fgetc");
	CHECK(ungetc('Z', f) == 'Z' && ftell(f) == 0 && getc(f) == 'Z' && getc(f) == (unsigned char)zelda[1], "ungetc and getc");
	CHECK(fflush(f) == 0 && setvbuf(f, nullptr, _IONBF, 0) == 0, "fflush and setvbuf are accepted");
	flockfile(f);
	CHECK(fgetc(f) == (unsigned char)zelda[2], "read under flockfile");
	funlockfile(f);

	// a big read goes straight into the caller's buffer
	std::string big(zelda.size(), 0);
	rewind(f);
	CHECK(fread(&big[0], 1, big.size(), f) == big.size() && big == zelda, "one fread of the whole file");
	int fd = fileno(f);
	CHECK(fd >= 0x40000000, "fileno of a netfs FILE* is a netfs descriptor");
	CHECK(fclose(f) == 0, "fclose");
	CHECK(s.closes == 1, "the backend handle was closed");

	// text
	f = fopen(np.c_str(), "r");
	char line[64];
	CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, "line one\n") && fgets(line, sizeof line, f) && !strcmp(line, "line two\n") &&
			  fgets(line, sizeof line, f) && !strcmp(line, "last") && !fgets(line, sizeof line, f) && feof(f),
		"fgets lines to the end");
	if (f)
		fclose(f);

	// descriptors
	fd = open(zp.c_str(), O_RDONLY);
	CHECK(fd >= 0x40000000, "open returns a netfs descriptor");
	CHECK(read(fd, buf, 16) == 16 && memcmp(buf, zelda.data(), 16) == 0 && lseek(fd, 0, SEEK_CUR) == 16, "read and lseek");
	CHECK(pread(fd, buf, 16, 1000) == 16 && memcmp(buf, zelda.data() + 1000, 16) == 0 && lseek(fd, 0, SEEK_CUR) == 16, "pread leaves the position alone");
	CHECK(lseek(fd, 0, SEEK_END) == (off_t)zelda.size() && read(fd, buf, 16) == 0 && pread(fd, buf, 16, zelda.size() + 5) == 0, "end of file reads 0");
	CHECK(fstat(fd, &st) == 0 && (uint64_t)st.st_size == zelda.size() && S_ISREG(st.st_mode), "fstat");
	CHECK(close(fd) == 0, "close");
	errno = 0;
	CHECK(close(fd) == -1 && errno == EBADF, "a second close is EBADF");

	// read-only
	errno = 0;
	CHECK(fopen(zp.c_str(), "wb") == nullptr && errno == EROFS, "fopen for writing is EROFS");
	errno = 0;
	CHECK(fopen(zp.c_str(), "r+") == nullptr && errno == EROFS, "fopen r+ is EROFS");
	errno = 0;
	CHECK(open(zp.c_str(), O_WRONLY) == -1 && errno == EROFS, "open for writing is EROFS");
	errno = 0;
	CHECK(fopen((std::string(kBase) + "/missing").c_str(), "rb") == nullptr && errno == ENOENT, "fopen of a missing file is ENOENT");
	errno = 0;
	CHECK(fopen((std::string(kBase) + "/sub").c_str(), "rb") == nullptr && errno == EISDIR, "fopen of a folder is EISDIR");
	CHECK(fopen64(zp.c_str(), "rb") != nullptr, "fopen64");

	// folders
	DIR* d = opendir(kBase);
	std::set<std::string> names;
	bool types_ok = d != nullptr;
	while (d)
	{
		struct dirent* e = readdir(d);
		if (!e)
			break;
		names.insert(e->d_name);
		types_ok = types_ok && (std::string(e->d_name) == "sub" || !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ? e->d_type == DT_DIR : e->d_type == DT_REG);
	}
	CHECK(names == std::set<std::string>({".", "..", "Zelda.sfc", "notes.txt", "sub"}), "readdir lists the folder");
	CHECK(types_ok, "d_type tells folders from files");
	CHECK(d && closedir(d) == 0, "closedir");
	errno = 0;
	CHECK(opendir(zp.c_str()) == nullptr && errno == ENOTDIR, "opendir of a file is ENOTDIR");
	errno = 0;
	CHECK(opendir((std::string(kBase) + "/nope").c_str()) == nullptr && errno == ENOENT, "opendir of a missing folder is ENOENT");

	// the rest of the process is untouched
	char tmpl[] = "/tmp/netfs-test-XXXXXX";
	int lfd = mkstemp(tmpl);
	CHECK(lfd >= 0 && lfd < 0x40000000, "real descriptors stay small");
	FILE* lf = fdopen(lfd, "w+");
	fputs("hello", lf);
	fflush(lf);
	rewind(lf);
	CHECK(fread(buf, 1, 5, lf) == 5 && !memcmp(buf, "hello", 5) && !feof(lf) && fflush(nullptr) == 0, "a local FILE* passes through");
	fclose(lf);
	FILE* again = fopen(tmpl, "r");
	CHECK(again && fgetc(again) == 'h' && ftell(again) == 1, "a local fopen passes through");
	if (again)
		fclose(again);
	unlink(tmpl);
}

static std::string MakeStoredZip(const std::string& name, const std::string& data)
{
	auto le16 = [](std::string& s, unsigned v) { s.push_back((char)v), s.push_back((char)(v >> 8)); };
	auto le32 = [&](std::string& s, uint32_t v) { le16(s, v & 0xffff), le16(s, v >> 16); };
	uint32_t crc = (uint32_t)crc32(0, (const Bytef*)data.data(), (uInt)data.size());
	std::string z;
	le32(z, 0x04034b50), le16(z, 10), le16(z, 0), le16(z, 0), le16(z, 0), le16(z, 0x21);
	le32(z, crc), le32(z, (uint32_t)data.size()), le32(z, (uint32_t)data.size()), le16(z, (unsigned)name.size()), le16(z, 0);
	z += name + data;
	std::string cd;
	le32(cd, 0x02014b50), le16(cd, 20), le16(cd, 10), le16(cd, 0), le16(cd, 0), le16(cd, 0), le16(cd, 0x21);
	le32(cd, crc), le32(cd, (uint32_t)data.size()), le32(cd, (uint32_t)data.size()), le16(cd, (unsigned)name.size());
	le16(cd, 0), le16(cd, 0), le16(cd, 0), le16(cd, 0), le32(cd, 0), le32(cd, 0);
	cd += name;
	std::string eocd;
	le32(eocd, 0x06054b50), le16(eocd, 0), le16(eocd, 0), le16(eocd, 1), le16(eocd, 1);
	le32(eocd, (uint32_t)cd.size()), le32(eocd, (uint32_t)z.size()), le16(eocd, 0);
	return z + cd + eocd;
}

static void TestArchives()
{
	Rig rig("smb://192.168.1.50/games/snes\n");
	std::string rom = Blob(200000, 3);
	rig.srv[0]->Put("game.zip", MakeStoredZip("game.sfc", rom));

	char tmpl[] = "/tmp/netfs-gz-XXXXXX";
	int lfd = mkstemp(tmpl);
	close(lfd);
	gzFile w = gzopen(tmpl, "wb");
	gzwrite(w, rom.data(), (unsigned)rom.size());
	gzclose(w);
	std::string gz;
	if (FILE* r = fopen(tmpl, "rb"))
	{
		char b[4096];
		size_t n;
		while ((n = fread(b, 1, sizeof b, r)) > 0)
			gz.append(b, n);
		fclose(r);
	}
	unlink(tmpl);
	rig.srv[0]->Put("game.gz", gz);
	rig.Mount();

	for (int variant = 0; variant < 2; variant++)
	{
		std::string zp = std::string(kBase) + "/game.zip";
		unzFile z = variant ? unzOpen64(zp.c_str()) : unzOpen(zp.c_str());
		CHECK(z != nullptr, variant ? "unzOpen64 opens a zip on the share" : "unzOpen opens a zip on the share");
		if (!z)
			continue;
		unz_file_info info;
		char name[64];
		std::string out;
		if (unzGoToFirstFile(z) == UNZ_OK && unzGetCurrentFileInfo(z, &info, name, sizeof name, nullptr, 0, nullptr, 0) == UNZ_OK &&
			unzOpenCurrentFile(z) == UNZ_OK)
		{
			char b[8192];
			int n;
			while ((n = unzReadCurrentFile(z, b, sizeof b)) > 0)
				out.append(b, (size_t)n);
			unzCloseCurrentFile(z);
		}
		unzClose(z);
		CHECK(out == rom, "the zipped ROM comes out byte-identical");
	}

	gzFile g = gzopen((std::string(kBase) + "/game.gz").c_str(), "rb");
	std::string out;
	char b[8192];
	int n;
	while (g && (n = gzread(g, b, sizeof b)) > 0)
		out.append(b, (size_t)n);
	if (g)
		gzclose(g);
	CHECK(g != nullptr && out == rom, "gzopen/gzread of a .gz on the share is byte-identical");
}

// ---------------------------------------------------------------- failures

static void TestConnectFailures()
{
	Rig rig("smb://u@h/s?password=SECRETPW\n");
	rig.srv[0]->connect_status = Status::Kind(ErrKind::AccessDenied, "NT_STATUS_LOGON_FAILURE SECRETPW");
	rig.Mount();
	std::vector<NetfsShareInfo> info = netfs_shares();
	CHECK(info.size() == 1 && info[0].state == NetfsState::Failed, "a refused sign-in leaves the share Failed");
	CHECK(info.size() == 1 && Has(info[0].error, "denied") && !Has(info[0].error, "SECRETPW") && !Has(netfs_last_error(), "SECRETPW"),
		"the sentence says so and has no password");
	CHECK(info.size() == 1 && info[0].label == "smb://u@h/s", "the label is the redacted address");
	CHECK(netfs_mount_finished() && netfs_mount_points().empty(), "finished, and no mount point to scan");
	struct stat st;
	errno = 0;
	CHECK(stat("/smb/h/s/a", &st) == -1 && errno == ENOTCONN, "a call on a failed share is ENOTCONN");

	netfs_stop();
	rig.srv[0]->connect_status = Status::Kind(ErrKind::ShareNotFound);
	rig.srv[0]->offered = {"games", "isos"};
	netfs_load_text("smb://u@h/s\n");
	rig.Mount();
	info = netfs_shares();
	CHECK(info.size() == 1 && Has(info[0].error, "games") && Has(info[0].error, "isos"), "a wrong share name lists the shares the server has");

	netfs_stop();
	rig.srv[0]->connect_status = Status();
	netfs_load_text("smb://u@h/s\n");
	CHECK(!netfs_mount_finished(), "a share that is not mounted yet is not finished");
	netfs_mount_all_blocking();
	CHECK(netfs_mount_finished() && netfs_mount_points().size() == 1, "mounted");
	netfs_set_realtime(true);
	CHECK(netfs_realtime(), "realtime flag");
	netfs_set_realtime(false);
}

static void TestWatchdog()
{
	std::atomic<int64_t> now{1000};
	Rig rig("smb://192.168.1.50/games/snes\n");
	netfs_set_clock([&] { return now.load(); });
	rig.srv[0]->Put("a.sfc", Blob(2048, 5));
	rig.Mount();
	FakeServer& s = *rig.srv[0];
	std::string ap = std::string(kBase) + "/a.sfc";

	FILE* open_file = fopen(ap.c_str(), "rb");
	CHECK(open_file != nullptr, "a file opened before the outage");

	netfs_rescan(); // the opened file's folder listing would answer the stat from memory
	s.hang = true;
	std::atomic<int> rc{0}, err{0};
	std::thread t([&] {
		struct stat st;
		rc = stat(ap.c_str(), &st);
		err = errno;
	});
	s.WaitParked(1);
	netfs_watchdog_tick();
	CHECK(netfs_shares()[0].state == NetfsState::Ready, "a request that has just begun is left alone");
	now += kNetfsRequestLimitMs;
	netfs_watchdog_tick();
	CHECK(netfs_shares()[0].state == NetfsState::Ready, "five seconds exactly is still allowed");
	now += 1;
	netfs_watchdog_tick();
	t.join();
	CHECK(netfs_shares()[0].state == NetfsState::Dead, "a request over five seconds kills the share");
	CHECK(rc == -1 && err == EIO, "the stuck call returns EIO");
	CHECK(Has(netfs_shares()[0].error, "lost") && Has(netfs_last_error(), "lost"), "the sentence says the connection was lost");
	CHECK(netfs_mount_points().empty() && netfs_mount_finished(), "a dead share is no longer a root");

	int stats = s.stats;
	struct stat st;
	errno = 0;
	CHECK(stat(ap.c_str(), &st) == -1 && errno == EIO && s.stats == stats, "later calls fail at once without touching the backend");
	char b[16];
	errno = 0;
	CHECK(open_file && fread(b, 1, sizeof b, open_file) == 0 && ferror(open_file) && errno == EIO, "an open file reads EIO, it does not hang");
	CHECK(open_file && fclose(open_file) == 0, "and closes");
	s.hang = false;

	// netfs_stop releases a call that is stuck
	netfs_stop();
	netfs_load_text("smb://192.168.1.50/games/snes\n");
	rig.Mount();
	s.hang = true;
	s.waiting = 0;
	std::thread t2([&] {
		struct stat st2;
		stat(ap.c_str(), &st2);
	});
	s.WaitParked(1);
	netfs_stop();
	t2.join();
	CHECK(true, "netfs_stop released a stuck call");
}

static void TestThreads()
{
	std::atomic<int64_t> now{1000};
	Rig rig("smb://192.168.1.50/games/snes\n");
	netfs_set_clock([&] { return now.load(); });
	rig.srv[0]->hang = true;
	netfs_start();
	rig.srv[0]->WaitParked(1);
	CHECK(!netfs_mount_finished(), "connecting");
	now += kNetfsConnectLimitMs + 1;
	for (int i = 0; i < 4000000 && !netfs_mount_finished(); i++)
		std::this_thread::yield(); // the watchdog thread notices within its 250 ms period
	std::vector<NetfsShareInfo> info = netfs_shares();
	CHECK(netfs_mount_finished() && info.size() == 1 && info[0].state == NetfsState::Failed && Has(info[0].error, "did not answer"),
		"a connect that never answers fails after fifteen seconds");

	netfs_stop();
	rig.srv[0]->hang = false;
	netfs_load_text("smb://192.168.1.50/games/snes\n");
	netfs_start();
	for (int i = 0; i < 4000000 && !netfs_mount_finished(); i++)
		std::this_thread::yield();
	CHECK(netfs_mount_points().size() == 1, "netfs_start mounts on a worker thread");
}

static void TestLoad()
{
	char dir[] = "/tmp/netfs-load-XXXXXX";
	if (!mkdtemp(dir))
		return;
	std::string path = std::string(dir) + "/shares.txt";
	CHECK(netfs_load(path) == 0, "a missing shares.txt loads nothing");
	std::string text;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		char b[2048];
		size_t n;
		while ((n = fread(b, 1, sizeof b, f)) > 0)
			text.append(b, n);
		fclose(f);
	}
	CHECK(text == netfs_shares_template(), "and writes the template");
	CHECK(access((path + ".part").c_str(), F_OK) == -1, "without a .part left behind");
	CHECK(netfs_load(path) == 0 && netfs_notices().empty(), "the template loads as no shares and no notices");

	std::string mine = "smb://h/s\n";
	if (FILE* f = fopen(path.c_str(), "wb"))
	{
		fwrite(mine.data(), 1, mine.size(), f);
		fclose(f);
	}
	CHECK(netfs_load(path) == 1, "a file with a share loads it");
	std::string after;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		char b[64];
		size_t n = fread(b, 1, sizeof b, f);
		after.assign(b, n);
		fclose(f);
	}
	CHECK(after == mine, "a file the app did not write is never rewritten");
	netfs_stop();
	unlink(path.c_str());
	rmdir(dir);
}


// ---------------------------------------------------------------- cache

static std::string Msu1Track(size_t bytes, uint32_t loop_samples, uint32_t seed)
{
	std::string t = Blob(bytes, seed);
	memcpy(&t[0], "MSU1", 4);
	for (int i = 0; i < 4; i++)
		t[4 + i] = (char)(loop_samples >> (8 * i));
	return t;
}

static std::string Slurp(const std::string& path)
{
	std::string out;
	FILE* f = fopen(path.c_str(), "rb");
	char buf[65536];
	size_t n;
	while (f && (n = fread(buf, 1, sizeof buf, f)) > 0)
		out.append(buf, n);
	if (f)
		fclose(f);
	return out;
}

static void TestCacheListings()
{
	std::atomic<int64_t> now{1000};
	Rig rig("smb://192.168.1.50/games/snes\n");
	netfs_set_clock([&] { return now.load(); });
	FakeServer& s = *rig.srv[0];
	for (int d = 0; d < 20; d++)
		for (int i = 0; i < 100; i++)
			s.Put("folder" + std::to_string(d) + "/game" + std::to_string(i) + ".sfc", Blob(64, (uint32_t)(d * 100 + i)));
	s.Put("Zelda.sfc", Blob(4000, 3));
	rig.Mount();
	const std::string base = kBase;

	// a scan of 2,000 ROMs: one listing per folder, a stat per file from memory
	int roms = 0;
	DIR* root = opendir(base.c_str());
	for (struct dirent* e = root ? readdir(root) : nullptr; e; e = readdir(root))
	{
		std::string name = e->d_name;
		if (name.compare(0, 6, "folder") != 0)
			continue;
		DIR* d = opendir((base + "/" + name).c_str());
		for (struct dirent* g = d ? readdir(d) : nullptr; g; g = readdir(d))
		{
			struct stat st;
			if (g->d_name[0] != '.' && stat((base + "/" + name + "/" + g->d_name).c_str(), &st) == 0 && st.st_size == 64)
				roms++;
		}
		if (d)
			closedir(d);
	}
	if (root)
		closedir(root);
	CHECK(roms == 2000, "the scan found 2,000 ROMs with their sizes");
	CHECK(s.lists == 21 && s.stats <= 1 && s.opens == 0, "a scan of 2,000 ROMs costs one listing per folder");

	int lists = s.lists, stats = s.stats;
	struct stat st;
	errno = 0;
	CHECK(stat((base + "/folder3/nothere.sfc").c_str(), &st) == -1 && errno == ENOENT, "a name missing from a fresh listing is ENOENT");
	errno = 0;
	CHECK(access((base + "/folder3/nothere.msu").c_str(), F_OK) == -1 && errno == ENOENT, "access of a missing name is ENOENT");
	errno = 0;
	CHECK(fopen((base + "/folder3/nothere.pcm").c_str(), "rb") == nullptr && errno == ENOENT, "fopen of a missing name is ENOENT");
	CHECK(s.lists == lists && s.stats == stats && s.opens == 0, "and none of those asked the server");

	// case: SMB shares ignore it
	CHECK(Slurp(base + "/zelda.SFC") == Blob(4000, 3), "a name matches ignoring case, the server's spelling is used to open it");
	s.Put("Dup.sfc", "a");
	s.Put("dup.SFC", "b");
	netfs_rescan();
	errno = 0;
	CHECK(stat((base + "/DUP.sfc").c_str(), &st) == -1 && errno == ENOENT, "two names that differ only in case: no guess");
	CHECK(stat((base + "/Dup.sfc").c_str(), &st) == 0, "the exact spelling still works");

	// expiry and rescan
	lists = s.lists;
	CHECK(stat((base + "/Zelda.sfc").c_str(), &st) == 0 && s.lists == lists, "a fresh listing answers");
	now += 61 * 1000;
	CHECK(stat((base + "/Zelda.sfc").c_str(), &st) == 0 && s.lists == lists + 1, "a listing older than 60 s is fetched again");
	s.Put("Zelda.sfc", Blob(5000, 4));
	CHECK(stat((base + "/Zelda.sfc").c_str(), &st) == 0 && st.st_size == 4000, "until it expires the old size stands");
	netfs_rescan();
	CHECK(stat((base + "/Zelda.sfc").c_str(), &st) == 0 && st.st_size == 5000 && Slurp(base + "/Zelda.sfc") == Blob(5000, 4), "a rescan sees the change");
}

static void TestCacheReads()
{
	Rig rig("smb://192.168.1.50/games/snes\n");
	FakeServer& s = *rig.srv[0];
	std::string rom = Blob(6 * 1024 * 1024 + 77, 11);
	s.Put("Big.sfc", rom);
	rig.Mount();
	const std::string path = std::string(kBase) + "/Big.sfc";

	// a launch: the ROM crosses the wire once
	netfs_prepare_game(path);
	CHECK(Slurp(path) == rom, "the ROM reads back as it is");
	netfs_cache_wait_idle();
	CHECK(s.bytes_read == rom.size(), "a launch reads the ROM from the server exactly once");
	CHECK(s.opens == 1 && s.closes == 1, "one open, one close");

	// a zip reads its directory at the end: one block, no read-ahead
	uint64_t before = s.bytes_read;
	FILE* f = fopen(path.c_str(), "rb");
	char tail[22];
	CHECK(f && fseek(f, -22, SEEK_END) == 0 && fread(tail, 1, 22, f) == 22 && memcmp(tail, rom.data() + rom.size() - 22, 22) == 0, "reading the end of the file");
	netfs_cache_wait_idle();
	CHECK(s.bytes_read - before <= kBlockSize, "the end of the file costs one block");
	char b[100];
	CHECK(fseek(f, 5, SEEK_SET) == 0 && fread(b, 1, 100, f) == 100 && memcmp(b, rom.data() + 5, 100) == 0, "the start costs one more");
	if (f)
		fclose(f);

	// a file past 16 MiB is read through a window: same bytes, bounded read-ahead
	std::string huge = Blob(20 * 1024 * 1024 + 5, 12);
	s.Put("Huge.bin", huge);
	netfs_rescan();
	CHECK(Slurp(std::string(kBase) + "/Huge.bin") == huge, "a 20 MiB file reads back as it is");
	f = fopen((std::string(kBase) + "/Huge.bin").c_str(), "rb");
	CHECK(f && fseek(f, 3 * 1024 * 1024, SEEK_SET) == 0 && fread(b, 1, 100, f) == 100 && memcmp(b, huge.data() + 3 * 1024 * 1024, 100) == 0, "a seek back works after the window moved");
	if (f)
		fclose(f);
}

static void TestRealtime()
{
	Rig rig("smb://192.168.1.50/games/snes\n");
	FakeServer& s = *rig.srv[0];
	s.Put("g/Game.sfc", Blob(1000, 1));
	s.Put("g/Game-1.pcm", Msu1Track(3 * 1024 * 1024, 100, 2));
	s.Put("other/Other.sfc", Blob(1000, 3));
	rig.Mount();
	const std::string base = kBase;

	struct stat st;
	CHECK(stat((base + "/g/Game.sfc").c_str(), &st) == 0, "the game's folder is listed");
	netfs_set_realtime(true);
	int calls = s.connects + s.stats + s.lists + s.opens + s.reads + s.closes;
	int hits = 0;
	for (int frame = 0; frame < 600; frame++)
	{
		if (stat((base + "/g/Game.sfc").c_str(), &st) == 0)
			hits++;
		errno = 0;
		if (stat((base + "/g/Game-9.pcm").c_str(), &st) == -1 && errno == ENOENT)
			hits++;
		if (access((base + "/g/Game.msu").c_str(), F_OK) == -1)
			hits++;
	}
	CHECK(hits == 1800, "600 frames of lookups give the right answers");
	CHECK(s.connects + s.stats + s.lists + s.opens + s.reads + s.closes == calls, "no server call over 600 frames");

	errno = 0;
	CHECK(fopen((base + "/other/Other.sfc").c_str(), "rb") == nullptr && errno == ENOENT, "a folder that was never listed is unknown in realtime");
	CHECK(s.lists == 1, "and was not listed for it");

	errno = 0;
	CHECK(fopen((base + "/g/Game-1.pcm").c_str(), "rb") == nullptr && errno == ENOENT, "a track whose head is not here is not opened in realtime");
	netfs_cache_wait_idle();
	FILE* f = fopen((base + "/g/Game-1.pcm").c_str(), "rb");
	CHECK(f != nullptr, "after its head arrived it opens");
	int opens = s.opens;
	if (f)
		fclose(f);
	CHECK(s.opens == opens, "the open itself needed no round trip");
	netfs_set_realtime(false);
}

static void TestMsu1()
{
	Rig rig("smb://192.168.1.50/games/snes\n");
	FakeServer& s = *rig.srv[0];
	std::vector<std::string> captured;
	netfs_set_log([&](const std::string& l) { captured.push_back(l); });
	const std::string rom = "Hero.sfc";
	std::string data = Blob(300000, 21);
	std::string t1 = Msu1Track(3 * 1024 * 1024, 700000, 22), t2 = Msu1Track(2 * 1024 * 1024, 0, 23);
	s.Put("m/" + rom, Blob(1000, 20));
	s.Put("m/Hero.msu", data);
	s.Put("m/Hero-1.pcm", t1);
	s.Put("m/Hero-2.pcm", t2);
	s.Put("m/Hero-x.txt", "not a track");
	rig.Mount();
	const std::string base = std::string(kBase) + "/m";

	netfs_prepare_game(base + "/" + rom);
	netfs_cache_wait_idle(); // the loop points of the heads are on their way too
	NetfsStats st0 = netfs_stats();
	CHECK(st0.blocks_fetched >= 3, "the data file and both track heads are here before the first frame");
	netfs_set_realtime(true);

	// the data file: from memory, whatever the link does
	s.gate = true;
	int reads = s.reads;
	CHECK(Slurp(base + "/Hero.msu") == data, "MSU-1 data is complete and identical with a stalled link");
	CHECK(netfs_stats().data_stalls == 0 && s.reads == reads, "and was read without waiting");

	// a track: the head is here, the rest is silence until it arrives, never a short read
	FILE* f = fopen((base + "/Hero-1.pcm").c_str(), "rb");
	CHECK(f != nullptr, "the prefetched track opens in realtime");
	std::vector<char> buf(64 * 1024);
	CHECK(f && fread(buf.data(), 1, buf.size(), f) == buf.size() && memcmp(buf.data(), t1.data(), buf.size()) == 0, "the head reads real audio");
	CHECK(f && fseek(f, 1024 * 1024, SEEK_SET) == 0 && fread(buf.data(), 1, buf.size(), f) == buf.size(), "a read where nothing is here is a full-length read");
	bool zeros = true;
	for (char c : buf)
		zeros = zeros && c == 0;
	CHECK(zeros, "and it is silence");
	CHECK(netfs_stats().underruns >= 1 && netfs_stats().data_stalls == 0, "the underrun is counted, nothing waited");
	size_t log_underruns = 0;
	for (const std::string& l : captured)
		log_underruns += Has(l, "audio underrun") ? 1 : 0;
	CHECK(log_underruns == 1, "and logged once");

	// the link comes back: the same read now returns the audio
	s.Release();
	netfs_cache_wait_idle();
	CHECK(f && fseek(f, 1024 * 1024, SEEK_SET) == 0 && fread(buf.data(), 1, buf.size(), f) == buf.size() && memcmp(buf.data(), t1.data() + 1024 * 1024, buf.size()) == 0,
		"after the link answered the track reads its real audio");
	if (f)
		fclose(f);

	// the loop point's block is kept: 8 + 4 * 700000 = 2,800,008
	s.gate = true;
	f = fopen((base + "/Hero-1.pcm").c_str(), "rb");
	CHECK(f && fseek(f, 2800008, SEEK_SET) == 0 && fread(buf.data(), 1, 1000, f) == 1000 && memcmp(buf.data(), t1.data() + 2800008, 1000) == 0, "the loop point reads real audio at once");
	if (f)
		fclose(f);
	s.Release();

	// the connection is lost mid-track: it mutes, it does not hang or end the game
	f = fopen((base + "/Hero-2.pcm").c_str(), "rb");
	CHECK(f != nullptr, "the second track opens");
	s.fail_reads = true;
	CHECK(f && fseek(f, 1500000, SEEK_SET) == 0 && fread(buf.data(), 1, buf.size(), f) == buf.size(), "a read after the loss is still a full-length read");
	netfs_cache_wait_idle();
	CHECK(f && fread(buf.data(), 1, buf.size(), f) == buf.size(), "and the next one");
	if (f)
		fclose(f);
	s.fail_reads = false;
	netfs_set_realtime(false);
	netfs_end_game();
	netfs_set_log(nullptr);
}

static void TestMsu1Oversize()
{
	Rig rig("smb://192.168.1.50/games/snes\n");
	FakeServer& s = *rig.srv[0];
	std::string big = Blob(33 * 1024 * 1024, 31); // past the 32 MiB budget: not prefetched
	s.Put("m/Big.sfc", Blob(1000, 30));
	s.Put("m/Big.msu", big);
	rig.Mount();
	const std::string path = std::string(kBase) + "/m/Big.msu";

	netfs_prepare_game(std::string(kBase) + "/m/Big.sfc");
	CHECK(s.bytes_read == 0, "an oversize data file is not fetched before the first frame");
	netfs_set_realtime(true);
	FILE* f = fopen(path.c_str(), "rb");
	CHECK(f != nullptr, "it opens");
	s.gate = true;
	std::vector<char> buf(4096);
	std::atomic<size_t> got{0};
	std::thread t([&] { got = f ? fread(buf.data(), 1, buf.size(), f) : 0; });
	s.WaitGated(1);
	CHECK(netfs_stats().data_stalls == 1, "a miss inside it waits, and is counted");
	s.Release();
	t.join();
	CHECK(got == buf.size() && memcmp(buf.data(), big.data(), buf.size()) == 0, "and the bytes are right");
	if (f)
		fclose(f);
	netfs_set_realtime(false);
	netfs_end_game();
}

int main()
{
	TestParse();
	TestFile();
	TestRedact();
	TestRouting();
	TestFiles();
	TestArchives();
	TestConnectFailures();
	TestWatchdog();
	TestThreads();
	TestLoad();
	TestCacheListings();
	TestCacheReads();
	TestRealtime();
	TestMsu1();
	TestMsu1Oversize();
	printf("netfs: passed %d, failed %d\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

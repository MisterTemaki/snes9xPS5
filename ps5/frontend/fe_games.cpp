// Snes9x PS5 frontend: the game library (fe_games.h).
// SPDX-License-Identifier: MIT

#include "fe_games.h"
#include "fe_7z.h"
#include "fe_titlematch.h"
#include "fe_artmatch.h"
#include "fe_dbart.h"
#include "fe_romheader.h"

#include "OrbisPaths.h"
#include "ProsperoThread.h"
#include "netfs.h"

#include "unzip.h"
#include "zlib.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_map>

#define S9X_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const char sym##_begin[];                                                                       \
	extern "C" const char sym##_end[];

S9X_INCBIN(s9x_gamedb, GAMEDB_TSV)
S9X_INCBIN(s9x_extracrc, EXTRACRC_TSV)
S9X_INCBIN(s9x_dbart, DBART_TSV)

namespace fe
{
namespace
{
std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

// "Super Mario World (USA) (Rev 1)" -> "super mario world"; also drops [tags] and trims.
std::string Key(const std::string& name)
{
	std::string out;
	int depth = 0;
	for (char c : name)
	{
		if (c == '(' || c == '[')
			depth++;
		else if ((c == ')' || c == ']') && depth > 0)
			depth--;
		else if (depth == 0)
			out += char(tolower(uint8_t(c)));
	}
	// collapse spaces, drop punctuation that file names often lose
	std::string k;
	bool space = false;
	for (char c : out)
	{
		if (c == '_' || c == ' ' || c == '.')
			space = !k.empty();
		else if (isalnum(uint8_t(c)) || c == '&' || c == '\'' || c == '-' || c == ',' || c == '!')
		{
			if (space)
				k += ' ';
			space = false;
			k += c;
		}
	}
	return k;
}

int RegionRank(const std::string& name)
{
	// prefer clean dumps: no Beta/Proto/Pirate/Virtual Console/etc.
	int penalty = 0;
	static const char* const bad[] = {"(Beta", "(Proto", "(Pirate", "(Virtual Console", "(Sample", "(Demo", "(Alt",
		"(Unl", "(Aftermarket", "(Arcade", "(Kiosk", "(Program", "(Hack"};
	for (const char* b : bad)
		if (name.find(b) != std::string::npos)
			penalty += 100;
	if (name.find("(Rev") != std::string::npos)
		penalty += 1;
	if (name.find("USA") != std::string::npos)
		return penalty + 0;
	if (name.find("World") != std::string::npos)
		return penalty + 2;
	if (name.find("Europe") != std::string::npos)
		return penalty + 4;
	if (name.find("Japan") != std::string::npos)
		return penalty + 6;
	return penalty + 8;
}

struct Db
{
	std::unordered_map<uint32_t, std::string> by_crc;
	std::unordered_map<std::string, std::string> exact; // lower-case name -> name
	std::unordered_map<std::string, std::string> loose; // Key -> best name
	std::vector<std::string> all_names;
	std::unordered_map<uint32_t, std::string> extra_by_crc; // dumps No-Intro does not list
	dbart::Table covers; // games an online database has a cover for and libretro-thumbnails has not
	Db()
	{
		dbart::Parse(s9x_dbart_begin, s9x_dbart_end, covers);
		const char* p = s9x_gamedb_begin;
		const char* end = s9x_gamedb_end;
		while (p < end)
		{
			const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
			if (!nl)
				nl = end;
			if (*p != '#' && nl - p > 9 && p[8] == '\t')
			{
				const uint32_t crc = uint32_t(strtoul(std::string(p, 8).c_str(), nullptr, 16));
				std::string name(p + 9, size_t(nl - p - 9));
				by_crc.emplace(crc, name);
				all_names.push_back(name);
				exact.emplace(Lower(name), name);
				const std::string k = Key(name);
				auto it = loose.find(k);
				if (it == loose.end() || RegionRank(name) < RegionRank(it->second))
					loose[k] = name;
			}
			p = nl + 1;
		}
		p = s9x_extracrc_begin;
		end = s9x_extracrc_end;
		while (p < end)
		{
			const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
			if (!nl)
				nl = end;
			if (*p != '#' && nl - p > 9 && p[8] == '\t')
				extra_by_crc.emplace(uint32_t(strtoul(std::string(p, 8).c_str(), nullptr, 16)), std::string(p + 9, size_t(nl - p - 9)));
			p = nl + 1;
		}
	}
};

const Db& D()
{
	static const Db db;
	return db;
}

// The biggest ROM file Snes9x takes (CMemory::MAX_ROM_SIZE, 16 MiB, and a 512-byte copier header): the ROM of a .7z is
// the biggest file up to this size, as the core's loader picks it (fe_emu.cpp, Load7z).
constexpr uint64_t kMaxRomFile = 0x1000000 + 512;

bool HasRomExt(const std::string& ext)
{
	static const char* const exts[] = {".sfc", ".smc", ".swc", ".fig", ".bs", ".st", ".zip", ".gz", ".7z", ".bin", ".mgd"};
	for (const char* e : exts)
		if (ext == e)
			return true;
	return false;
}

// ---- CRC cache: path \t size \t mtime \t crc ----
// One per process: the scan and the pass that names network ROMs (NetNaming) share it, each under the lock.
struct CrcCache
{
	std::mutex lock;
	std::unordered_map<std::string, std::string> lines; // path -> "size\tmtime\tcrc" (or "size\tmtime\tfail")
	std::unordered_map<std::string, bool> seen; // the paths this scan looked up
	bool dirty = false;
	bool loaded = false;
	std::string file;
	// A new scan: reads the file the first time, forgets which paths the last scan saw.
	void Begin()
	{
		std::lock_guard<std::mutex> g(lock);
		seen.clear();
		if (loaded)
			return;
		loaded = true;
		file = OrbisDir("covers") + "/crc-cache.txt";
		FILE* f = fopen(file.c_str(), "r");
		if (!f)
			return;
		char line[2048];
		while (fgets(line, sizeof(line), f))
		{
			line[strcspn(line, "\r\n")] = 0;
			char* tab = strchr(line, '\t');
			if (tab)
			{
				*tab = 0;
				lines[line] = tab + 1;
			}
		}
		fclose(f);
	}
	// prune: the scan's end drops the entries of files that are gone; the naming pass (mid-scan, maybe) keeps all.
	void Save(bool prune)
	{
		std::lock_guard<std::mutex> g(lock);
		// entries of files that are gone are dropped (a USB drive that isn't plugged in, or a share that is not
		// connected, keeps its own)
		for (auto it = lines.begin(); prune && it != lines.end();)
		{
			if (!seen.count(it->first) && it->first.rfind("/mnt/", 0) != 0 && !netfs_is_network_path(it->first.c_str()))
			{
				it = lines.erase(it);
				dirty = true;
			}
			else
				++it;
		}
		if (!dirty)
			return;
		const std::string tmp = file + ".part";
		FILE* f = fopen(tmp.c_str(), "w");
		if (!f)
			return;
		for (const auto& kv : lines)
			fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
		bool ok = ferror(f) == 0;
		ok = fclose(f) == 0 && ok;
		if (!ok || rename(tmp.c_str(), file.c_str()) != 0)
		{
			remove(tmp.c_str());
			return;
		}
		dirty = false;
	}
	void Put(const std::string& path, const char* key, const char* val)
	{
		std::lock_guard<std::mutex> g(lock);
		lines[path] = std::string(key) + val;
		dirty = true;
	}
};

CrcCache& SharedCrcCache()
{
	static CrcCache* c = new CrcCache; // never freed: the naming pass may still run at exit
	return *c;
}

// read: false for a ROM on a share, where only a CRC already in the cache is used (reading every ROM over the
// network is what makes a scan slow). known (if given): the cache holds an answer for this file as it is now,
// "could not be read" included.
bool CachedCrc(CrcCache& cache, const std::string& path, uint32_t* crc, bool read, bool* known = nullptr)
{
	if (known)
		*known = false;
	{
		std::lock_guard<std::mutex> g(cache.lock);
		cache.seen[path] = true;
	}
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return false;
	char key[64];
	snprintf(key, sizeof(key), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	{
		std::lock_guard<std::mutex> g(cache.lock);
		auto it = cache.lines.find(path);
		if (it != cache.lines.end() && it->second.compare(0, strlen(key), key) == 0)
		{
			if (known)
				*known = true;
			if (it->second.compare(strlen(key), std::string::npos, "fail") == 0)
				return false; // couldn't be read last time, and hasn't changed since
			*crc = uint32_t(strtoul(it->second.c_str() + strlen(key), nullptr, 16));
			return true;
		}
	}
	if (!read)
		return false;
	char val[96];
	if (!RomCrc32(path, crc))
	{
		// unreadable (or an empty zip): remembered, so the next scans don't read it again until it changes
		cache.Put(path, key, "fail");
		return false;
	}
	snprintf(val, sizeof(val), "%08X", *crc);
	cache.Put(path, key, val);
	return true;
}

// A ROM no name or CRC identified keeps its header's title for the cover lookup. Cached like the CRC (under the path
// plus "|hdr", with the size and mtime), so each such ROM is opened once: "size\tmtime\tTITLE\tCOUNTRY" or "...\tnone".
// read: false for a ROM on a share, where a scan uses only what is cached (NetNaming reads the header with the CRC).
bool HeaderHint(CrcCache& cache, const std::string& path, bool read, std::string& val)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return false;
	const std::string key = path + "|hdr";
	char stamp[64];
	snprintf(stamp, sizeof(stamp), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	{
		std::lock_guard<std::mutex> lk(cache.lock);
		cache.seen[key] = true;
		auto it = cache.lines.find(key);
		if (it != cache.lines.end() && it->second.compare(0, strlen(stamp), stamp) == 0)
		{
			val = it->second.substr(strlen(stamp));
			return true;
		}
	}
	if (!read)
		return false;
	romheader::Info info;
	if (ReadRomHeader(path, &info))
		val = info.title + "\t" + std::to_string(int(info.country));
	else
		val = "none";
	cache.Put(key, stamp, val.c_str());
	return true;
}

void LoadHeaderHint(CrcCache& cache, GameInfo& g)
{
	std::string val;
	if (!HeaderHint(cache, g.path, !g.on_network, val))
		return;
	const size_t tab = val.find('\t');
	if (tab == std::string::npos)
		return;
	g.header_title = val.substr(0, tab);
	g.header_japan = atoi(val.c_str() + tab + 1) == 0;
}

void Walk(const std::string& dir, int depth, bool usb, bool net, std::vector<GameInfo>& out, const ScanProgress& progress)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		const std::string path = dir + "/" + e->d_name;
		struct stat st = {};
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if (depth < 4)
				Walk(path, depth + 1, usb, net, out, progress);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue; // a FIFO or a device would block the CRC read (and the shelf) forever
		const std::string name = e->d_name;
		const size_t dot = name.find_last_of('.');
		if (dot == std::string::npos || dot == 0)
			continue;
		const std::string ext = Lower(name.substr(dot));
		if (!HasRomExt(ext))
			continue;
		GameInfo g;
		g.path = path;
		g.file_base = name.substr(0, dot);
		g.ext = ext;
		g.on_usb = usb;
			g.on_network = net;
		out.push_back(std::move(g));
		if (progress)
			progress(0, out.size(), 0);
	}
	closedir(d);
}

// Names one game, in the order the header describes: its file name, its CRC (crc_given, else the cache's or the file's),
// the file name loosely, then fuzzily; a ROM none of that names keeps its header's title. ScanGames and DescribeGame
// (a game that is being loaded) both come through here, so the shelf and the border name a game alike.
void NameGame(CrcCache& cache, GameInfo& g, const uint32_t* crc_given)
{
	g.nointro = gamedb::Exact(g.file_base);
	if (g.nointro.empty())
	{
		uint32_t crc = 0;
		bool known = false;
		bool have = false;
		if (crc_given)
		{
			crc = *crc_given;
			have = known = true;
		}
		else
			have = CachedCrc(cache, g.path, &crc, !g.on_network, &known);
		if (have)
		{
			g.crc = crc;
			g.nointro = gamedb::ByCrc(crc);
			g.name_by_crc = !g.nointro.empty();
			if (g.nointro.empty())
				g.crc_name = gamedb::ByExtraCrc(crc);
		}
		g.crc_pending = g.on_network && !known; // NetNaming reads it later
	}
	if (g.nointro.empty())
		g.nointro = gamedb::Loose(g.file_base);
	if (g.nointro.empty())
	{
		g.nointro = gamedb::Fuzzy(g.file_base);
		if (!g.nointro.empty())
			OrbisLog("[games] fuzzy: %s -> %s", g.file_base.c_str(), g.nointro.c_str());
	}
	if (g.nointro.empty())
		LoadHeaderHint(cache, g);
	g.title = g.nointro.empty() ? g.file_base : gamedb::Title(g.nointro);
	g.region = gamedb::Region(g.nointro);
}
} // namespace

namespace gamedb
{
size_t Count()
{
	return D().by_crc.size();
}

std::string ByCrc(uint32_t crc)
{
	auto it = D().by_crc.find(crc);
	return it == D().by_crc.end() ? std::string() : it->second;
}

std::string ByExtraCrc(uint32_t crc)
{
	auto it = D().extra_by_crc.find(crc);
	return it == D().extra_by_crc.end() ? std::string() : it->second;
}

std::string DatabaseCoverUrls(const GameInfo& game)
{
	// the host tests point both at a local server
	const char* hasheous = getenv("SNES9X_DBART_HASHEOUS");
	const char* thegamesdb = getenv("SNES9X_DBART_THEGAMESDB");
	const dbart::Row* row = dbart::Find(D().covers, game.crc, game.nointro.empty() ? game.file_base : game.nointro);
	if (!row && game.nointro != game.file_base)
		row = dbart::Find(D().covers, 0, game.file_base);
	if (!row && !game.crc_name.empty())
		row = dbart::Find(D().covers, 0, game.crc_name);
	if (!row)
		return std::string();
	return dbart::Urls(*row, hasheous && *hasheous ? hasheous : "https://hasheous.org/api/v1/images/",
		thegamesdb && *thegamesdb ? thegamesdb : "https://cdn.thegamesdb.net/images/original/boxart/front/");
}

std::string Exact(const std::string& name)
{
	auto it = D().exact.find(Lower(name));
	return it == D().exact.end() ? std::string() : it->second;
}

std::string Loose(const std::string& file_base)
{
	auto it = D().loose.find(Key(file_base));
	return it == D().loose.end() ? std::string() : it->second;
}

std::string Fuzzy(const std::string& file_base)
{
	// close in spelling is not enough when the numbers disagree: "Dragon Quest III" has the letters of "Dragon Quest I & II"
	const std::string best = titles::Best(titles::IndexOf(D().all_names), file_base);
	return artmatch::NumbersConflict(file_base, best) ? std::string() : best;
}

std::string Title(const std::string& nointro)
{
	std::string t = nointro.substr(0, nointro.find(" ("));
	// "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - A Link to the Past"
	static const char* const arts[] = {", The", ", A", ", An"};
	for (const char* a : arts)
	{
		const size_t pos = t.find(a);
		if (pos != std::string::npos)
		{
			const size_t end = pos + strlen(a);
			if (end == t.size() || t.compare(end, 3, " - ") == 0 || t[end] == ':')
			{
				const std::string art = std::string(a + 2);
				t = art + " " + t.substr(0, pos) + t.substr(end);
				break;
			}
		}
	}
	return t;
}

std::string Region(const std::string& nointro)
{
	const size_t a = nointro.find(" (");
	if (a == std::string::npos)
		return "";
	const size_t b = nointro.find(')', a);
	return b == std::string::npos ? "" : nointro.substr(a + 2, b - a - 2);
}
} // namespace gamedb

bool RomCrc32(const std::string& path, uint32_t* crc, const std::atomic<bool>* cancel)
{
	auto stopped = [cancel] { return cancel && cancel->load(); };
	const std::string ext = Lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
	if (ext == ".zip")
	{
		unzFile z = unzOpen(path.c_str());
		if (!z)
			return false;
		bool ok = false;
		uLong best_size = 0;
		for (int r = unzGoToFirstFile(z); r == UNZ_OK && !stopped(); r = unzGoToNextFile(z))
		{
			unz_file_info info;
			char name[512];
			// minizip ends the name with '\0' only when it fits: keep the last byte for it, skip names that don't fit
			name[sizeof(name) - 1] = '\0';
			if (unzGetCurrentFileInfo(z, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK ||
				info.size_filename >= sizeof(name) - 1)
				continue;
			if (info.uncompressed_size < best_size)
				continue; // the ROM is the biggest file in the archive
			best_size = info.uncompressed_size;
			if ((info.uncompressed_size & 0x3ff) != 0x200)
			{
				*crc = uint32_t(info.crc);
				ok = true;
				continue;
			}
			// a copier header: the stored CRC includes it, so read the data
			if (unzOpenCurrentFile(z) != UNZ_OK)
				continue;
			uLong c = crc32(0L, Z_NULL, 0);
			std::vector<uint8_t> buf(1 << 16);
			uLong skip = 512;
			int n;
			while (!stopped() && (n = unzReadCurrentFile(z, buf.data(), unsigned(buf.size()))) > 0)
			{
				uLong off = std::min<uLong>(skip, uLong(n));
				skip -= off;
				c = crc32(c, buf.data() + off, uInt(n - off));
			}
			unzCloseCurrentFile(z);
			*crc = uint32_t(c);
			ok = true;
		}
		unzClose(z);
		return ok && !stopped();
	}
	if (ext == ".7z")
	{
		// the CRC the archive stores for its ROM (nothing unpacked), unless a copier header is in it: then the data's
		std::vector<sevenzip::Entry> entries;
		sevenzip::Error err;
		if (!sevenzip::List(path, &entries, &err))
			return false;
		const int k = sevenzip::PickRom(entries, kMaxRomFile, &err);
		if (k < 0 || stopped())
			return false;
		const sevenzip::Entry& e = entries[size_t(k)];
		if (e.has_crc && (e.size & 0x3ff) != 0x200)
		{
			*crc = e.crc;
			return true;
		}
		std::vector<uint8_t> data;
		if (!sevenzip::Extract(path, e.index, &data, &err) || stopped())
			return false;
		const size_t skip = (data.size() & 0x3ff) == 0x200 ? 512 : 0;
		*crc = uint32_t(crc32(crc32(0L, Z_NULL, 0), data.data() + skip, uInt(data.size() - skip)));
		return true;
	}
	if (ext == ".gz")
		return false;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, (size & 0x3ff) == 0x200 ? 512 : 0, SEEK_SET);
	uLong c = crc32(0L, Z_NULL, 0);
	std::vector<uint8_t> buf(1 << 16);
	size_t n;
	while (!stopped() && (n = fread(buf.data(), 1, buf.size(), f)) > 0)
		c = crc32(c, buf.data(), uInt(n));
	fclose(f);
	if (stopped())
		return false;
	*crc = uint32_t(c);
	return true;
}

// ---- the naming pass for ROMs on shares ----

struct NetNaming::Impl
{
	std::mutex lock;
	std::deque<std::string> todo;
	std::set<std::string> tried; // every path ever queued: a ROM is read once per run
	std::atomic<bool> stop{false};
	std::atomic<int> resolved{0};
	std::atomic<bool> busy{false};
	ps5::BigThread thread;

	void Run()
	{
		CrcCache& cache = SharedCrcCache();
		int named = 0, since_save = 0;
		for (;;)
		{
			std::string path;
			{
				std::unique_lock<std::mutex> g(lock);
				if (stop.load() || todo.empty() || netfs_realtime())
				{
					busy = false;
					break;
				}
				path = todo.front();
				todo.pop_front();
			}
			struct stat st = {};
			if (stat(path.c_str(), &st) != 0)
				continue; // the share went away; the next scan will list it again
			char key[64];
			snprintf(key, sizeof(key), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
			uint32_t crc = 0;
			const bool ok = RomCrc32(path, &crc, &stop);
			if (stop.load())
				break; // cancelled half way: nothing recorded
			char val[16];
			snprintf(val, sizeof(val), "%08X", crc);
			cache.Put(path, key, ok ? val : "fail");
			if (ok && gamedb::ByCrc(crc).empty() && gamedb::ByExtraCrc(crc).empty())
			{
				std::string hint; // nothing names it: its header's title is the last hint for the cover
				HeaderHint(cache, path, true, hint);
			}
			named++;
			resolved++;
			if (++since_save >= 20)
			{
				cache.Save(false);
				since_save = 0;
			}
		}
		cache.Save(false);
		busy = false;
		OrbisLog("[games] network CRC pass: %d ROM(s) read%s", named, stop.load() ? " (stopped)" : "");
	}
};

NetNaming::NetNaming() : m_i(new Impl) {}

NetNaming::~NetNaming()
{
	Stop();
}

void NetNaming::Add(const std::vector<GameInfo>& games)
{
	size_t added = 0;
	{
		std::lock_guard<std::mutex> g(m_i->lock);
		for (const GameInfo& game : games)
			if (game.crc_pending && m_i->tried.insert(game.path).second)
			{
				m_i->todo.push_back(game.path);
				added++;
			}
	}
	if (!added)
		return;
	OrbisLog("[games] naming %zu network ROM(s) by CRC in the background", added);
	if (m_i->busy.load())
		return; // the running pass takes the new ones too
	m_i->thread.join();
	m_i->stop = false;
	m_i->busy = true;
	m_i->thread = ps5::BigThread([this] { m_i->Run(); }, 256 * 1024);
}

void NetNaming::Stop()
{
	m_i->stop = true;
	m_i->thread.join();
	m_i->busy = false;
}

int NetNaming::Resolved() const
{
	return m_i->resolved.load();
}

bool NetNaming::Busy() const
{
	return m_i->busy.load();
}

std::vector<std::string> RomRoots()
{
	std::vector<std::string> roots = OrbisRomRoots();
	for (const std::string& r : netfs_mount_points())
		roots.push_back(r);
	return roots;
}

std::vector<GameInfo> ScanGames(const ScanProgress& progress)
{
	std::vector<GameInfo> games;
	for (const std::string& root : RomRoots())
		Walk(root, 0, root.rfind("/mnt/", 0) == 0, netfs_is_network_path(root.c_str()), games, progress);

	CrcCache& cache = SharedCrcCache();
	cache.Begin();
	size_t identified = 0;
	for (GameInfo& g : games)
	{
		if (progress)
			progress(1, identified++, games.size());
		NameGame(cache, g, nullptr);
	}
	cache.Save(true);

	std::sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const int c = strcasecmp(a.title.c_str(), b.title.c_str());
		return c != 0 ? c < 0 : a.path < b.path;
	});
	OrbisLog("[games] %zu ROM(s); table of %zu names", games.size(), gamedb::Count());
	// each game once per scan is a lot of boot.log for a big library: the first 200
	constexpr size_t kLogged = 200;
	for (size_t i = 0; i < games.size() && i < kLogged; i++)
		OrbisLog("[games]   %s -> \"%s\"%s", games[i].path.c_str(), games[i].nointro.c_str(),
			games[i].name_by_crc ? " (by CRC)" : "");
	if (games.size() > kLogged)
		OrbisLog("[games]   ... and %zu more", games.size() - kLogged);
	return games;
}

GameInfo DescribeGame(const std::string& path, uint32_t rom_crc)
{
	GameInfo g;
	g.path = path;
	const size_t slash = path.find_last_of('/');
	const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	const size_t dot = name.find_last_of('.');
	g.file_base = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
	g.ext = dot == std::string::npos || dot == 0 ? std::string() : Lower(name.substr(dot));
	g.on_usb = path.rfind("/mnt/", 0) == 0;
	g.on_network = netfs_is_network_path(path.c_str());
	NameGame(SharedCrcCache(), g, rom_crc ? &rom_crc : nullptr);
	return g;
}

// How much of a ROM holds every place a header can be: ExHiROM's ($40FFC0), behind a copier header, and its 64 bytes.
constexpr size_t kHeaderPrefix = 0x410000 + 512 + 64;

bool ReadRomHeader(const std::string& path, romheader::Info* info)
{
	const size_t dot = path.find_last_of('.');
	const std::string ext = Lower(dot == std::string::npos ? std::string() : path.substr(dot));
	std::vector<uint8_t> prefix; // a .zip or .gz is unpacked as far as the last header place (a .7z whole)
	uint64_t total = 0;
	if (ext == ".zip")
	{
		unzFile z = unzOpen(path.c_str());
		if (!z)
			return false;
		uLong best_size = 0;
		bool found = false;
		for (int r = unzGoToFirstFile(z); r == UNZ_OK; r = unzGoToNextFile(z))
		{
			unz_file_info zi;
			char name[512];
			name[sizeof(name) - 1] = '\0';
			if (unzGetCurrentFileInfo(z, &zi, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK ||
				zi.size_filename >= sizeof(name) - 1)
				continue;
			if (zi.uncompressed_size < best_size)
				continue; // the ROM is the biggest file in the archive
			if (unzOpenCurrentFile(z) != UNZ_OK)
				continue;
			best_size = zi.uncompressed_size;
			prefix.resize(std::min<size_t>(kHeaderPrefix, size_t(best_size)));
			size_t got = 0;
			int n;
			while (got < prefix.size() && (n = unzReadCurrentFile(z, prefix.data() + got, unsigned(prefix.size() - got))) > 0)
				got += size_t(n);
			prefix.resize(got);
			unzCloseCurrentFile(z);
			total = best_size;
			found = true;
		}
		unzClose(z);
		if (!found)
			return false;
	}
	else if (ext == ".7z")
	{
		sevenzip::Error err;
		if (!sevenzip::ExtractRom(path, kMaxRomFile, &prefix, &err))
			return false;
		total = prefix.size();
		if (prefix.size() > kHeaderPrefix)
			prefix.resize(kHeaderPrefix);
	}
	else if (ext == ".gz")
	{
		gzFile gz = gzopen(path.c_str(), "rb");
		if (!gz)
			return false;
		prefix.resize(kHeaderPrefix);
		size_t got = 0;
		int n;
		while (got < prefix.size() && (n = gzread(gz, prefix.data() + got, unsigned(prefix.size() - got))) > 0)
			got += size_t(n);
		gzclose(gz);
		prefix.resize(got);
		total = got < kHeaderPrefix ? got : 0; // the size is known only when the whole ROM fit
	}
	if (ext == ".zip" || ext == ".gz" || ext == ".7z")
		return romheader::Read(
			[&prefix](uint64_t at, uint8_t* out, size_t n) {
				if (at + n > prefix.size())
					return false;
				memcpy(out, prefix.data() + at, n);
				return true;
			},
			total, info);
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	total = uint64_t(ftell(f));
	const bool ok = romheader::Read(
		[f](uint64_t at, uint8_t* out, size_t n) {
			return fseek(f, long(at), SEEK_SET) == 0 && fread(out, 1, n, f) == n;
		},
		total, info);
	fclose(f);
	return ok;
}
} // namespace fe

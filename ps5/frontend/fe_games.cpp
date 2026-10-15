// Snes9x PS5 frontend: the game library (fe_games.h).
// SPDX-License-Identifier: MIT

#include "fe_games.h"
#include "fe_titlematch.h"

#include "OrbisPaths.h"

#include "unzip.h"
#include "zlib.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
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
	Db()
	{
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
	}
};

const Db& D()
{
	static const Db db;
	return db;
}

bool HasRomExt(const std::string& ext)
{
	static const char* const exts[] = {".sfc", ".smc", ".swc", ".fig", ".bs", ".st", ".zip", ".gz", ".bin", ".mgd"};
	for (const char* e : exts)
		if (ext == e)
			return true;
	return false;
}

// ---- CRC cache: path \t size \t mtime \t crc ----
struct CrcCache
{
	std::mutex lock;
	std::unordered_map<std::string, std::string> lines; // path -> "size\tmtime\tcrc" (or "size\tmtime\tfail")
	std::unordered_map<std::string, bool> seen; // the paths this scan looked up
	bool dirty = false;
	std::string file;
	void Load()
	{
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
	void Save()
	{
		// entries of files that are gone are dropped (a USB drive that isn't plugged in keeps its own)
		for (auto it = lines.begin(); it != lines.end();)
		{
			if (!seen.count(it->first) && it->first.rfind("/mnt/", 0) != 0)
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
};

bool CachedCrc(CrcCache& cache, const std::string& path, uint32_t* crc)
{
	cache.seen[path] = true;
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return false;
	char key[64];
	snprintf(key, sizeof(key), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	auto it = cache.lines.find(path);
	if (it != cache.lines.end() && it->second.compare(0, strlen(key), key) == 0)
	{
		if (it->second.compare(strlen(key), std::string::npos, "fail") == 0)
			return false; // couldn't be read last time, and hasn't changed since
		*crc = uint32_t(strtoul(it->second.c_str() + strlen(key), nullptr, 16));
		return true;
	}
	char val[96];
	if (!RomCrc32(path, crc))
	{
		// unreadable (or an empty zip): remembered, so the next scans don't read it again until it changes
		snprintf(val, sizeof(val), "%sfail", key);
		cache.lines[path] = val;
		cache.dirty = true;
		return false;
	}
	snprintf(val, sizeof(val), "%s%08X", key, *crc);
	cache.lines[path] = val;
	cache.dirty = true;
	return true;
}

void Walk(const std::string& dir, int depth, bool usb, std::vector<GameInfo>& out)
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
				Walk(path, depth + 1, usb, out);
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
		out.push_back(std::move(g));
	}
	closedir(d);
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
	return titles::Best(D().all_names, file_base);
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

bool RomCrc32(const std::string& path, uint32_t* crc)
{
	const std::string ext = Lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
	if (ext == ".zip")
	{
		unzFile z = unzOpen(path.c_str());
		if (!z)
			return false;
		bool ok = false;
		uLong best_size = 0;
		for (int r = unzGoToFirstFile(z); r == UNZ_OK; r = unzGoToNextFile(z))
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
			while ((n = unzReadCurrentFile(z, buf.data(), unsigned(buf.size()))) > 0)
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
		return ok;
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
	while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
		c = crc32(c, buf.data(), uInt(n));
	fclose(f);
	*crc = uint32_t(c);
	return true;
}

std::vector<GameInfo> ScanGames()
{
	std::vector<GameInfo> games;
	for (const std::string& root : OrbisRomRoots())
		Walk(root, 0, root.rfind("/mnt/", 0) == 0, games);

	CrcCache cache;
	cache.Load();
	for (GameInfo& g : games)
	{
		g.nointro = gamedb::Exact(g.file_base);
		if (g.nointro.empty())
		{
			uint32_t crc = 0;
			if (CachedCrc(cache, g.path, &crc))
			{
				g.nointro = gamedb::ByCrc(crc);
				g.name_by_crc = !g.nointro.empty();
			}
		}
		if (g.nointro.empty())
			g.nointro = gamedb::Loose(g.file_base);
		if (g.nointro.empty())
		{
			g.nointro = gamedb::Fuzzy(g.file_base);
			if (!g.nointro.empty())
				OrbisLog("[games] fuzzy: %s -> %s", g.file_base.c_str(), g.nointro.c_str());
		}
		g.title = g.nointro.empty() ? g.file_base : gamedb::Title(g.nointro);
		g.region = gamedb::Region(g.nointro);
	}
	cache.Save();

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
} // namespace fe

// Snes9x PS5 frontend: the borders downloaded from The Bezel Project (fe_borderfetch.h).
// SPDX-License-Identifier: MIT

#include "fe_borderfetch.h"

#include "fe_covers.h"

#include "OrbisPaths.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

#ifndef BORDER_INDEX_TXT
#error BORDER_INDEX_TXT must name the built-in index of the SNES borders The Bezel Project has
#endif
#define S9X_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const char sym##_begin[];                                                                       \
	extern "C" const char sym##_end[];

S9X_INCBIN(s9x_border_index, BORDER_INDEX_TXT)

namespace fe::border
{
namespace
{
constexpr long kMissingRetrySeconds = 30L * 24 * 3600; // as the covers: a 404 isn't asked again for 30 days

const char* const kBezelBase =
	"https://raw.githubusercontent.com/thebezelproject/bezelproject-SNES/master/retroarch/overlay/GameBezels/SNES/";

// "Joe & Mac (USA)" -> "Joe%20%26%20Mac%20%28USA%29": everything but letters, digits and -_.~
std::string Encode(const std::string& s)
{
	static const char* const hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c))
			out += char(c);
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

bool NonEmptyFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// <name>.missing holds the address that gave a 404 (fe_coverfetch.cpp: SaveCoverResult).
bool RecentlyMissing(const std::string& marker, const std::string& url)
{
	struct stat st = {};
	if (stat(marker.c_str(), &st) != 0 || time(nullptr) - st.st_mtime >= kMissingRetrySeconds)
		return false;
	FILE* f = fopen(marker.c_str(), "r");
	if (!f)
		return false;
	char line[8192] = {};
	const bool read = fgets(line, sizeof(line), f) != nullptr;
	fclose(f);
	if (!read)
		return false;
	std::string old(line);
	while (!old.empty() && (old.back() == '\n' || old.back() == '\r'))
		old.pop_back();
	return old == url;
}

std::vector<std::string> ParseIndex(const char* p, const char* end)
{
	std::vector<std::string> names;
	while (p < end)
	{
		const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
		if (!nl)
			break;
		if (*p != '#' && nl > p)
			names.emplace_back(p, size_t(nl - p));
		p = nl + 1;
	}
	return names;
}

const PackMatcher& Matcher()
{
	static const PackMatcher matcher(IndexNames()); // the index never changes: made once
	return matcher;
}

bool BezelNeeded(const std::string& pack, const std::string& name, WantedCover* out)
{
	const std::string file = name + ".png";
	const std::string path = pack + "/games/" + file;
	const std::string url = BezelUrl(name);
	if (NonEmptyFile(path) || RecentlyMissing(path.substr(0, path.size() - 4) + ".missing", url))
		return false;
	out->file = file;
	out->url = url;
	return true;
}

void WriteList(const std::string& path, const std::vector<WantedCover>& list)
{
	std::string text;
	for (const WantedCover& w : list)
		text += w.file + "\t" + w.url + "\n";
	WriteFileAtomicTo(path, std::vector<uint8_t>(text.begin(), text.end()));
}
} // namespace

const std::vector<std::string>& IndexNames()
{
	static const std::vector<std::string> names = ParseIndex(s9x_border_index_begin, s9x_border_index_end);
	return names;
}

std::string BezelNameFor(const GameInfo& game)
{
	return Matcher().Match(game);
}

std::string BezelUrl(const std::string& name)
{
	const char* base = getenv("SNES9X_BORDER_URL");
	return std::string(base && *base ? base : kBezelBase) + Encode(name) + ".png";
}

bool BorderWanted(const std::string& pack, const GameInfo& game, WantedCover* out)
{
	const std::string name = BezelNameFor(game);
	return !name.empty() && BezelNeeded(pack, name, out);
}

std::vector<WantedCover> MissingBorders(const std::string& pack, const std::vector<GameInfo>& games)
{
	std::vector<WantedCover> out;
	std::set<std::string> seen;
	for (const GameInfo& g : games)
	{
		if (NonEmptyFile(pack + "/games/" + g.file_base + ".png"))
			continue; // the player's own border for this ROM
		const std::string name = BezelNameFor(g);
		WantedCover w;
		if (!name.empty() && seen.insert(name).second && BezelNeeded(pack, name, &w))
			out.push_back(w);
	}
	return out;
}

void WriteWantedBorders(const std::string& pack, const std::vector<WantedCover>& wanted)
{
	WriteList(pack + "/wanted.txt", wanted);
}

void WritePriorityBorders(const std::string& pack, const std::vector<WantedCover>& wanted)
{
	WriteList(pack + "/priority.txt", wanted);
}
} // namespace fe::border

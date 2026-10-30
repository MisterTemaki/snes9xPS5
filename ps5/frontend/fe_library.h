// Snes9x PS5 frontend (2.10): the saved game list. The shelf opens at once from the list the last scan found and looks
// at the folders again in the background (fe_shelf.cpp); before 2.10 every opening of the shelf waited for a scan.
//
// The file, /data/snes9x/library.bin, is written after every scan that finished (temporary file, then rename):
//   "S9XLIB\r\n", u32 format version, u32 game count, then per game its strings (u32 length + bytes: path, file name,
//   extension, No-Intro name, title, region, extra-table name, header title), a flags byte (USB, network, named by CRC,
//   CRC pending, header says Japan) and the u32 CRC; last, the CRC32 of everything before it. Little-endian.
// A file that is short, foreign, of another format version, damaged (checksum) or inconsistent is ignored: the shelf
// then scans as before 2.10, with its progress screen. The tab of a game is not stored: tabs::Of derives it from the
// USB and network flags.
//
// The functions in this header touch no file (tests/test_library.cpp checks them); LoadLibrary and SaveLibrary
// (fe_games.cpp) read and write the file.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_games.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace fe
{
namespace library
{
constexpr uint32_t kVersion = 1; // a change of the layout above (or of what a field means) takes the next number
constexpr char kMagic[8] = {'S', '9', 'X', 'L', 'I', 'B', '\r', '\n'};
constexpr uint32_t kMaxGames = 200000;
constexpr uint32_t kMaxString = 4096;

inline uint32_t Crc32(const char* p, size_t n)
{
	static const struct Table
	{
		uint32_t t[256];
		Table()
		{
			for (uint32_t i = 0; i < 256; i++)
			{
				uint32_t c = i;
				for (int k = 0; k < 8; k++)
					c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				t[i] = c;
			}
		}
	} table;
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++)
		crc = table.t[(crc ^ uint8_t(p[i])) & 0xff] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

namespace detail
{
inline void PutU32(std::string& out, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		out += char((v >> (8 * i)) & 0xff);
}
inline void PutStr(std::string& out, const std::string& s)
{
	PutU32(out, uint32_t(s.size()));
	out += s;
}
struct Reader
{
	const std::string& d;
	size_t at;
	size_t end;
	bool U32(uint32_t& v)
	{
		if (end - at < 4)
			return false;
		v = 0;
		for (int i = 0; i < 4; i++)
			v |= uint32_t(uint8_t(d[at + size_t(i)])) << (8 * i);
		at += 4;
		return true;
	}
	bool Str(std::string& s)
	{
		uint32_t n = 0;
		if (!U32(n) || n > kMaxString || end - at < n)
			return false;
		s.assign(d, at, n);
		at += n;
		return true;
	}
};
} // namespace detail

inline std::string Encode(const std::vector<GameInfo>& games)
{
	std::string out(kMagic, sizeof(kMagic));
	detail::PutU32(out, kVersion);
	detail::PutU32(out, uint32_t(games.size()));
	for (const GameInfo& g : games)
	{
		for (const std::string* s : {&g.path, &g.file_base, &g.ext, &g.nointro, &g.title, &g.region, &g.crc_name, &g.header_title})
			detail::PutStr(out, *s);
		out += char((g.on_usb ? 1 : 0) | (g.on_network ? 2 : 0) | (g.name_by_crc ? 4 : 0) | (g.crc_pending ? 8 : 0) |
					(g.header_japan ? 16 : 0));
		detail::PutU32(out, g.crc);
	}
	detail::PutU32(out, Crc32(out.data(), out.size()));
	return out;
}

enum class Bad
{
	None,
	Short, // shorter than a header and its checksum
	Magic, // not a saved list of Snes9x PS5
	Version, // another format version
	Checksum, // damaged
	Format, // the checksum fits, the contents don't (a string too long, bytes left over...)
};

inline const char* Why(Bad b)
{
	switch (b)
	{
		case Bad::None: return "ok";
		case Bad::Short: return "too short";
		case Bad::Magic: return "not a saved game list";
		case Bad::Version: return "another format version";
		case Bad::Checksum: return "damaged (checksum)";
		case Bad::Format: return "malformed";
	}
	return "?";
}

// `out` is filled only when the whole file is good.
inline Bad Decode(const std::string& data, std::vector<GameInfo>& out)
{
	if (data.size() < sizeof(kMagic) + 12)
		return Bad::Short;
	if (memcmp(data.data(), kMagic, sizeof(kMagic)) != 0)
		return Bad::Magic;
	detail::Reader r{data, sizeof(kMagic), data.size() - 4};
	uint32_t version = 0, count = 0, sum = 0;
	r.U32(version);
	if (version != kVersion)
		return Bad::Version;
	detail::Reader tail{data, data.size() - 4, data.size()};
	tail.U32(sum);
	if (sum != Crc32(data.data(), data.size() - 4))
		return Bad::Checksum;
	if (!r.U32(count) || count > kMaxGames)
		return Bad::Format;
	std::vector<GameInfo> games(count);
	for (GameInfo& g : games)
	{
		for (std::string* s : {&g.path, &g.file_base, &g.ext, &g.nointro, &g.title, &g.region, &g.crc_name, &g.header_title})
			if (!r.Str(*s))
				return Bad::Format;
		if (r.at >= r.end || g.path.empty() || g.path[0] != '/')
			return Bad::Format;
		const uint8_t flags = uint8_t(data[r.at++]);
		if (flags & ~31u)
			return Bad::Format;
		g.on_usb = flags & 1;
		g.on_network = flags & 2;
		g.name_by_crc = flags & 4;
		g.crc_pending = flags & 8;
		g.header_japan = flags & 16;
		if (!r.U32(g.crc))
			return Bad::Format;
	}
	if (r.at != r.end)
		return Bad::Format;
	out = std::move(games);
	return Bad::None;
}

// Keeps the games that lie under one of `roots` (the ROM folders there now: a USB drive that was unplugged takes its
// games off the shelf at once, before the check). Returns how many it dropped.
inline size_t KeepUnder(std::vector<GameInfo>& games, const std::vector<std::string>& roots)
{
	const size_t before = games.size();
	std::vector<GameInfo> kept;
	kept.reserve(games.size());
	for (GameInfo& g : games)
	{
		bool under = false;
		for (const std::string& r : roots)
			under = under || (!r.empty() && g.path.size() > r.size() && g.path.compare(0, r.size(), r) == 0 && g.path[r.size()] == '/');
		if (under)
			kept.push_back(std::move(g));
	}
	games = std::move(kept);
	return before - games.size();
}

// After the check: the game the shelf keeps selected. `view` is the paths the open tab showed, `sel` the selected
// place in it; `fresh` the paths the check found. The selected game when it is still there; else the nearest that
// is, the next one first, then the previous one; "" when none is.
inline std::string Successor(const std::vector<std::string>& view, int sel, const std::unordered_set<std::string>& fresh)
{
	const int n = int(view.size());
	if (sel < 0 || sel >= n)
		return std::string();
	for (int d = 0; d < n; d++)
		for (int s : {sel + d, sel - d})
			if (s >= 0 && s < n && fresh.count(view[size_t(s)]))
				return view[size_t(s)];
	return std::string();
}
} // namespace library

// /data/snes9x/library.bin
std::string LibraryPath();
// Writes the list (atomically: the old file stays whole until the new one is complete). False when it could not.
bool SaveLibrary(const std::vector<GameInfo>& games);
// The saved list; false (and a line in the log saying why) when there is none or it is ignored.
bool LoadLibrary(std::vector<GameInfo>& games);
} // namespace fe

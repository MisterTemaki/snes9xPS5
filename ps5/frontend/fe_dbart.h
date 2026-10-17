// Covers from an online database, for the games libretro-thumbnails has none of: hacks, homebrew, unlicensed and
// pirate carts, and games only some databases list.
//
// data/snes-db-art.tsv (tools/make_db_art.py, from Hasheous' public SNES dump) names those games, the CRC32 of
// their dumps, and where a database has the box art: an IGDB image served by hasheous.org, or TheGamesDB's front
// box art. A ROM is found by its CRC32 (the database knows this very dump), or by the words of its name
// (fe_artmatch.h, so tags, versions and numerals do not matter and a different number never matches). Nothing about
// the ROM leaves the console: the addresses come from the table, the helper downloads the image like any cover.
//
// SPDX-License-Identifier: MIT
#pragma once
#include "fe_artmatch.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace fe { namespace dbart {
struct Row
{
    std::string name;
    std::vector<std::string> sources; // "h<image id>" (hasheous.org) or "t<id>" (TheGamesDB), best first
};
struct Table
{
    std::vector<Row> rows;
    std::vector<std::string> names; // rows[i].name, the list the words of a file name are matched against
    std::unordered_map<uint32_t, size_t> by_crc;
    std::unordered_map<std::string, size_t> by_name;
};
// name<TAB>sources<TAB>crc,crc... per line; '#' lines are comments. A malformed line is skipped.
inline void Parse(const char* p, const char* end, Table& t)
{
    while (p < end)
    {
        const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
        if (!nl) nl = end;
        const std::string line(p, size_t(nl - p));
        p = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos || a == 0) continue;
        Row row;
        row.name = line.substr(0, a);
        for (size_t s = a + 1; s < b;)
        {
            size_t e = line.find(' ', s);
            if (e == std::string::npos || e > b) e = b;
            if (e > s + 1 && (line[s] == 'h' || line[s] == 't')) row.sources.push_back(line.substr(s, e - s));
            s = e + 1;
        }
        if (row.sources.empty() || t.by_name.count(row.name)) continue;
        const size_t index = t.rows.size();
        for (size_t s = b + 1; s < line.size();)
        {
            size_t e = line.find(',', s);
            if (e == std::string::npos) e = line.size();
            if (e - s == 8) t.by_crc.emplace(uint32_t(strtoul(line.substr(s, 8).c_str(), nullptr, 16)), index);
            s = e + 1;
        }
        t.by_name.emplace(row.name, index);
        t.names.push_back(row.name);
        t.rows.push_back(std::move(row));
    }
}
// The game a ROM is: by its CRC32 (crc 0 when unknown), else by its name's words. null when no row fits.
inline const Row* Find(const Table& t, uint32_t crc, const std::string& name)
{
    if (crc != 0)
    {
        const auto it = t.by_crc.find(crc);
        if (it != t.by_crc.end()) return &t.rows[it->second];
    }
    if (t.names.empty() || name.empty()) return nullptr;
    const std::string found = artmatch::Resolve(artmatch::IndexOf(t.names), name);
    const auto it = t.by_name.find(found);
    return it == t.by_name.end() ? nullptr : &t.rows[it->second];
}
// Where the row's cover is, tab-separated as covers/wanted.txt wants them. The bases end in '/'.
inline std::string Urls(const Row& row, const std::string& hasheous_base, const std::string& thegamesdb_base)
{
    std::string urls;
    for (const std::string& s : row.sources)
    {
        if (!urls.empty()) urls += '\t';
        urls += s[0] == 'h' ? hasheous_base + s.substr(1) : thegamesdb_base + s.substr(1) + "-1.jpg";
    }
    return urls;
}
}} // namespace fe::dbart

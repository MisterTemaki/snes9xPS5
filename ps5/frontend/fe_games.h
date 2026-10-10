// Snes9x PS5 frontend: the game library -- every ROM under the ROM folders, with its official name.
//
// SNES ROMs carry no serial (PS5SX2 names discs by theirs), so a game is identified the way libretro does:
//   1. its file name, when it is already a No-Intro name ("Super Mario World (USA).sfc");
//   2. otherwise the CRC32 of the ROM (without a 512-byte copier header; for a .zip, the CRC its directory
//      stores), looked up in the No-Intro table built into the ELF (data/snes-nointro.tsv, 4268 games);
//   3. otherwise the file name, loosely: "super mario world.smc" -> the best No-Intro entry with that title
//      (USA first, then World, Europe, Japan).
// The No-Intro name gives the shelf its title ("Super Mario World") and the cover its download name.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
struct GameInfo
{
	std::string path; // full path
	std::string file_base; // file name without extension
	std::string ext; // ".sfc", ".zip"... lower case
	std::string nointro; // official name, "" until known (or unknown)
	std::string title; // what the shelf shows
	std::string region; // "USA", "Europe"... from the name's tags
	bool on_usb = false;
	bool name_by_crc = false; // the name came from the CRC (not the file name)
};

// The table built into the ELF.
namespace gamedb
{
size_t Count();
// "" when unknown.
std::string ByCrc(uint32_t crc);
// Exact No-Intro name ("Super Mario World (USA)") -> itself, or "".
std::string Exact(const std::string& name);
// Loose: title without tags, any case ("super mario world") -> best regional entry, or "".
std::string Loose(const std::string& file_base);
// Conservative fuzzy title rescue when CRC and exact/loose names all miss.
std::string Fuzzy(const std::string& file_base);
// "Super Mario World (USA) (Rev 1)" -> "Super Mario World"; "Legend of Zelda, The - ..." -> "The Legend of Zelda - ..."
std::string Title(const std::string& nointro);
std::string Region(const std::string& nointro);
} // namespace gamedb

// The headerless CRC32 of a ROM file (or the CRC a .zip stores for its ROM); false if it can't be read.
bool RomCrc32(const std::string& path, uint32_t* crc);

// Every ROM under the ROM folders (sub-folders included, 4 levels), sorted by title. Names that need a
// CRC are resolved here too, with a cache (/data/snes9x/covers/crc-cache.txt) so each ROM is read once.
std::vector<GameInfo> ScanGames();
} // namespace fe

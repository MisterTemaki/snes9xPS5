// Snes9x PS5 frontend: the game library -- every ROM under the ROM folders, with its official name.
//
// SNES ROMs carry no serial (PS5SX2 names discs by theirs), so a game is identified the way libretro does:
//   1. its file name, when it is already a No-Intro name ("Super Mario World (USA).sfc");
//   2. otherwise the CRC32 of the ROM (without a 512-byte copier header; for a .zip, the CRC its directory
//      stores), looked up in the No-Intro table built into the ELF (data/snes-nointro.tsv, 4268 games);
//   3. otherwise the file name, loosely: "super mario world.smc" -> the best No-Intro entry with that title
//      (USA first, then World, Europe, Japan).
// The No-Intro name gives the shelf its title ("Super Mario World") and the cover its download name.
// A ROM none of that names (a fan translation, a hack) keeps its file name as the title. For the cover lookup it also
// carries what its CRC is called in the extra table and the title in its own header (fe_artmatch.h).
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_romheader.h"

#include <cstdint>
#include <functional>
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
	std::string crc_name; // the name the ROM's CRC has in the extra table (a translation, a hack), "" if none
	std::string header_title; // the ROM header's title, only for a ROM no name or CRC identified; "" otherwise
	bool header_japan = false; // that header's country byte says Japan
	uint32_t crc = 0; // the ROM's CRC32 (without a copier header) when the scan took it; 0 if not
};

// The table built into the ELF.
namespace gamedb
{
size_t Count();
// "" when unknown.
std::string ByCrc(uint32_t crc);
// The name of a dump No-Intro does not list (data/snes-extra-crc.tsv: fan translations, hacks, other dump sets), or "".
// Not a No-Intro name: only the cover lookup reads it.
std::string ByExtraCrc(uint32_t crc);
// Where an online database has the cover of a game libretro-thumbnails lacks (data/snes-db-art.tsv): addresses,
// tab-separated, found by the ROM's CRC or by the words of its name; "" when no database row fits.
std::string DatabaseCoverUrls(const GameInfo& game);
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

// The internal header of a ROM file (a .zip or .gz is read through); false if there is none to find.
bool ReadRomHeader(const std::string& path, romheader::Info* info);

// The headerless CRC32 of a ROM file (or the CRC a .zip stores for its ROM); false if it can't be read.
bool RomCrc32(const std::string& path, uint32_t* crc);

// Every ROM under the ROM folders (sub-folders included, 4 levels), sorted by title. Names that need a
// CRC are resolved here too, with a cache (/data/snes9x/covers/crc-cache.txt) so each ROM is read once.
// progress (when given) is told how far the scan is: stage 0 while the folders are listed (done = games found
// so far, total = 0), stage 1 while each game is identified (done of total). It is called often: it decides itself
// how often to draw.
using ScanProgress = std::function<void(int stage, size_t done, size_t total)>;
std::vector<GameInfo> ScanGames(const ScanProgress& progress = nullptr);
} // namespace fe

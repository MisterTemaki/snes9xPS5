// Snes9x PS5 frontend: 7-Zip archives (.7z) holding a ROM.
//
// Read with the LZMA SDK's 7z decoder (ps5/third_party/lzma7z, public domain): LZMA, LZMA2, PPMd, the BCJ/BCJ2/ARM...
// branch filters, Delta and stored files. Encrypted archives (a password) and other methods are refused. The archive is
// read with the libc file calls (fopen/fread/fseeko), so a .7z on a network share is read through netfs like a .zip.
//
// The ROM in an archive is the biggest file no bigger than the limit the caller gives (Snes9x's: 16 MiB + a 512-byte
// copier header), as for a .zip. A CD image (.cue .iso .chd ...) is never one: an archive with a cue sheet in it, or
// with nothing but disc files, is refused (a disc would have to be unpacked whole). A 7z unpacks a whole solid block at a
// time: a block over kMaxBlock (64 MiB) is refused, so a ROM packed solid with a big set has to be repacked alone.
// The CRC32 a 7z stores for each file names the game without unpacking anything.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
namespace sevenzip
{
constexpr uint64_t kMaxBlock = 64ull << 20; // the biggest solid block unpacked
constexpr uint64_t kMaxMemory = 256ull << 20; // everything the decoder may allocate besides (PPMd's model, the header)

enum class Error
{
	None,
	Open, // the file can't be opened
	Damaged, // not a 7z, cut short, a CRC that doesn't match
	Encrypted, // a password: not supported
	Unsupported, // a compression method the decoder doesn't have
	NoRom, // no file that can be a ROM (empty, only folders)
	DiscImage, // a CD image (a cue sheet, .iso, .chd...): not supported in a .7z
	TooBig, // the ROM is over the caller's limit
	BlockTooBig, // its solid block is over kMaxBlock
	Memory, // the decoder could not get its memory
};

struct Entry
{
	std::string name; // UTF-8, with its folders ("roms/game.sfc")
	uint64_t size = 0;
	uint32_t crc = 0;
	bool has_crc = false;
	bool is_dir = false;
	uint32_t index = 0; // in the archive
	uint64_t block_size = 0; // what unpacking it takes: its solid block's size (0 for an empty file)
};

// The files of the archive; false (and why) when it can't be read.
bool List(const std::string& path, std::vector<Entry>* entries, Error* err);

// The ROM among them: the biggest file that is not a CD image and is at most max_size bytes. -1 when there is none;
// *err then says NoRom, DiscImage (a cue sheet, or disc files only), or TooBig when a file was left out for its size.
int PickRom(const std::vector<Entry>& entries, uint64_t max_size, Error* err);

// Unpacks the file with that index (an Entry::index); false (and why) on any error, out left empty.
bool Extract(const std::string& path, uint32_t index, std::vector<uint8_t>* out, Error* err);

// List + PickRom + Extract: the archive's ROM. name (if given) gets its name in the archive.
bool ExtractRom(const std::string& path, uint64_t max_size, std::vector<uint8_t>* out, Error* err, std::string* name = nullptr);

// The English sentence for an error (translated with fe::T where it is shown); "" for None.
const char* Message(Error err);

// The last error this thread's Load7z (the core's loader) met, None after a load that worked.
Error LastLoadError();
void SetLastLoadError(Error err);

// True for ".7z" in any case.
bool IsArchivePath(const std::string& path);
} // namespace sevenzip
} // namespace fe

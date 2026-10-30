// Snes9x PS5 (2.11): a game's RetroAchievements hash (fe_ra.h identifies the game by it).
//
// RetroAchievements identifies a SNES game by the MD5 of its ROM file, without a copier header: rcheevos's rc_hash_snes
// leaves out the first 512 bytes when the size is a multiple of 8 KiB plus 512. That rule is rcheevos's own
// (rc_hash_generate_from_buffer, RC_CONSOLE_SUPER_NINTENDO), so it is applied by rcheevos, not by a copy of it here.
// What is hashed is the ROM file's bytes as they are in the file, whatever holds them:
//   - a .zip (and an MSU-1 .msu1 pack holding program.rom): the entry Snes9x plays (loadzip.cpp's choice: the biggest
//     file that fits, or the one named *.1 or program.rom), unpacked;
//   - a .7z: the entry Snes9x plays (fe_7z.h's choice), unpacked;
//   - a .gz: unpacked;
//   - anything else: the file.
// Never Snes9x's copy in memory: the core drops a header by its own rule (a multiple of 2 KiB plus 512), de-interleaves
// some dumps and applies IPS/UPS/BPS patches, all of which change the bytes. Files on a network share are read through
// netfs like any file.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fe::rahash
{
// The biggest ROM file this reads (16 MiB, the core's own limit, plus a copier header).
constexpr size_t kMaxRom = (16u << 20) + 512;

// The ROM's bytes as RetroAchievements hashes them (before the header rule). false with the reason.
bool ReadRom(const std::string& path, std::vector<uint8_t>* rom, std::string* why);

// The RetroAchievements hash of the game at `path` (32 lowercase hex digits). false with the reason.
bool HashFile(const std::string& path, std::string* hash, std::string* why);

// The same for bytes already in memory (rc_hash_snes's header rule applied).
std::string HashBytes(const uint8_t* data, size_t size);
} // namespace fe::rahash

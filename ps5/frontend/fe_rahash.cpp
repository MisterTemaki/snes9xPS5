// Snes9x PS5 (2.11): a game's RetroAchievements hash (fe_rahash.h).
// SPDX-License-Identifier: MIT

#include "fe_rahash.h"

#include "fe_7z.h"

#include "rc_consoles.h"
#include "rc_hash.h"
#include "unzip.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <strings.h>

namespace fe::rahash
{
namespace
{
bool EndsWith(const std::string& s, const char* ext)
{
	const size_t n = strlen(ext);
	return s.size() > n && strcasecmp(s.c_str() + s.size() - n, ext) == 0;
}

// loadzip.cpp's choice: the biggest entry that fits, unless one is named *.1 or program.rom (taken at once). A .msu1 pack
// is a ROM only when it holds program.rom.
bool ReadZip(const std::string& path, std::vector<uint8_t>* rom, std::string* why)
{
	unzFile z = unzOpen(path.c_str());
	if (!z)
	{
		*why = "not a zip archive";
		return false;
	}
	char best[260] = "";
	uLong best_size = 0;
	for (int r = unzGoToFirstFile(z); r == UNZ_OK; r = unzGoToNextFile(z))
	{
		unz_file_info info;
		char name[260];
		if (unzGetCurrentFileInfo(z, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
			continue;
		name[sizeof(name) - 1] = 0;
		if (info.uncompressed_size > kMaxRom)
			continue;
		const size_t len = strlen(name);
		const bool first_part = len > 2 && name[len - 2] == '.' && name[len - 1] == '1';
		const bool program = strncasecmp(name, "program.rom", 11) == 0;
		if (info.uncompressed_size > best_size || first_part || program)
		{
			snprintf(best, sizeof(best), "%s", name);
			best_size = info.uncompressed_size;
		}
		if (first_part || program)
			break;
	}
	if (best_size == 0 || (EndsWith(path, ".msu1") && strcasecmp(best, "program.rom") != 0))
	{
		unzClose(z);
		*why = "no ROM in the zip";
		return false;
	}
	bool ok = unzLocateFile(z, best, 0) == UNZ_OK && unzOpenCurrentFile(z) == UNZ_OK;
	if (ok)
	{
		rom->resize(best_size);
		const int n = unzReadCurrentFile(z, rom->data(), unsigned(best_size));
		ok = unzCloseCurrentFile(z) == UNZ_OK && n == int(best_size);
	}
	unzClose(z);
	if (!ok)
		*why = "the zip could not be read";
	return ok;
}

bool ReadGz(const std::string& path, std::vector<uint8_t>* rom, std::string* why)
{
	gzFile gz = gzopen(path.c_str(), "rb");
	if (!gz)
	{
		*why = "cannot open the file";
		return false;
	}
	rom->clear();
	uint8_t buf[65536];
	int n;
	while ((n = gzread(gz, buf, sizeof(buf))) > 0)
	{
		if (rom->size() + size_t(n) > kMaxRom)
		{
			gzclose(gz);
			*why = "the ROM is too big";
			return false;
		}
		rom->insert(rom->end(), buf, buf + n);
	}
	const bool ok = n == 0;
	gzclose(gz);
	if (!ok)
		*why = "the file could not be read";
	return ok;
}

bool ReadPlain(const std::string& path, std::vector<uint8_t>* rom, std::string* why)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
	{
		*why = "cannot open the file";
		return false;
	}
	rom->clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		if (rom->size() + n > kMaxRom)
		{
			fclose(f);
			*why = "the ROM is too big";
			return false;
		}
		rom->insert(rom->end(), buf, buf + n);
	}
	const bool ok = !ferror(f);
	fclose(f);
	if (!ok)
		*why = "the file could not be read";
	return ok;
}
} // namespace

bool ReadRom(const std::string& path, std::vector<uint8_t>* rom, std::string* why)
{
	std::string dummy;
	if (!why)
		why = &dummy;
	bool ok;
	if (EndsWith(path, ".zip") || EndsWith(path, ".msu1"))
		ok = ReadZip(path, rom, why);
	else if (fe::sevenzip::IsArchivePath(path))
	{
		fe::sevenzip::Error err = fe::sevenzip::Error::None;
		ok = fe::sevenzip::ExtractRom(path, kMaxRom, rom, &err);
		if (!ok)
			*why = fe::sevenzip::Message(err);
	}
	else if (EndsWith(path, ".gz"))
		ok = ReadGz(path, rom, why);
	else
		ok = ReadPlain(path, rom, why);
	if (ok && rom->empty())
	{
		*why = "the ROM is empty";
		ok = false;
	}
	return ok;
}

std::string HashBytes(const uint8_t* data, size_t size)
{
	char hash[33] = "";
	if (!rc_hash_generate_from_buffer(hash, RC_CONSOLE_SUPER_NINTENDO, data, size))
		return "";
	return hash;
}

bool HashFile(const std::string& path, std::string* hash, std::string* why)
{
	std::vector<uint8_t> rom;
	if (!ReadRom(path, &rom, why))
		return false;
	*hash = HashBytes(rom.data(), rom.size());
	if (hash->size() != 32)
	{
		if (why)
			*why = "rcheevos could not hash it";
		return false;
	}
	return true;
}
} // namespace fe::rahash

// Snes9x PS5 host test of the 7z reader (frontend/fe_7z.cpp) on the archives in tests/7z (tests/make_7z_fixtures.py):
// every method the fixtures use, a solid archive with several files and a folder, a UTF-16 name, a copier header,
// an empty archive, a cue sheet, a cut and a damaged archive, a password (header and data), an unsupported method,
// a ROM over the size limit and a solid block over the 64 MiB cap.
//
//   test-7z <fixture dir>
//
// SPDX-License-Identifier: MIT
#include "fe_7z.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace fe::sevenzip;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, label)                                                  \
	do                                                                      \
	{                                                                       \
		if (cond)                                                           \
			g_pass++;                                                       \
		else                                                                \
		{                                                                   \
			g_fail++;                                                       \
			printf("  FAIL: %s (test_7z.cpp:%d)\n", label, __LINE__);       \
		}                                                                   \
	} while (0)

static const uint64_t kMaxRom = 0x1000000 + 512; // Snes9x's limit, as fe_games.cpp and the core give it

// make_7z_fixtures.py's rom(): half xorshift32 noise, half a repeated line
static std::vector<uint8_t> Rom(size_t n, uint32_t seed)
{
	std::vector<uint8_t> out(n);
	const size_t half = n / 2;
	uint32_t x = seed * 2654435761u + 1;
	for (size_t i = 0; i < half; i++)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		out[i] = uint8_t(x);
	}
	char line[64];
	const int len = snprintf(line, sizeof line, "SNES9X PS5 7Z TEST %u ", seed);
	for (size_t i = half; i < n; i++)
		out[i] = uint8_t(line[(i - half) % size_t(len)]);
	return out;
}

static uint32_t Crc32(const std::vector<uint8_t>& d, size_t skip = 0)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = skip; i < d.size(); i++)
	{
		c ^= d[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return ~c;
}

static std::string g_dir;
static std::string P(const char* name)
{
	return g_dir + "/" + name;
}

static std::vector<uint8_t> ReadAll(const std::string& path)
{
	std::vector<uint8_t> d;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		uint8_t b[4096];
		size_t n;
		while ((n = fread(b, 1, sizeof b, f)) > 0)
			d.insert(d.end(), b, b + n);
		fclose(f);
	}
	return d;
}

static std::string WriteTemp(const std::vector<uint8_t>& d)
{
	char tmpl[] = "/tmp/test-7z-XXXXXX.7z";
	const int fd = mkstemps(tmpl, 3);
	if (fd < 0)
		return "";
	const bool ok = write(fd, d.data(), d.size()) == ssize_t(d.size());
	close(fd);
	return ok ? tmpl : "";
}

// one ROM "game.sfc" of 8000 bytes made with this seed, unpacked byte for byte
static void OneMethod(const char* file, uint32_t seed, const char* what)
{
	std::vector<Entry> e;
	Error err = Error::Damaged;
	const bool listed = List(P(file), &e, &err);
	const std::vector<uint8_t> want = Rom(8000, seed);
	char label[160];
	snprintf(label, sizeof label, "%s: listed, one entry game.sfc of 8000 bytes with its CRC", what);
	CHECK(listed && err == Error::None && e.size() == 1 && e[0].name == "game.sfc" && e[0].size == 8000 && e[0].has_crc &&
			  e[0].crc == Crc32(want) && !e[0].is_dir,
		label);
	std::vector<uint8_t> out;
	std::string name;
	const bool ok = ExtractRom(P(file), kMaxRom, &out, &err, &name);
	snprintf(label, sizeof label, "%s: the ROM unpacks byte-identical", what);
	CHECK(ok && err == Error::None && out == want && name == "game.sfc", label);
}

int main(int argc, char** argv)
{
	g_dir = argc > 1 ? argv[1] : "tests/7z";

	OneMethod("lzma.7z", 1, "LZMA");
	OneMethod("lzma2.7z", 2, "LZMA2");
	OneMethod("ppmd.7z", 3, "PPMd");
	OneMethod("bcj.7z", 4, "BCJ (x86) + LZMA2");
	OneMethod("arm.7z", 5, "ARM + LZMA2");
	OneMethod("delta.7z", 6, "Delta + LZMA2");
	OneMethod("copy.7z", 7, "stored (copy)");

	// solid, several files and a folder
	{
		std::vector<Entry> e;
		Error err;
		const bool listed = List(P("solid.7z"), &e, &err);
		CHECK(listed && e.size() == 4, "solid: four entries (a folder, three files)");
		int dirs = 0, rom = -1, patch = -1, readme = -1;
		for (size_t i = 0; i < e.size(); i++)
		{
			dirs += e[i].is_dir;
			if (e[i].name == "Game (USA).sfc")
				rom = int(i);
			if (e[i].name == "Game (USA) [patch].smc")
				patch = int(i);
			if (e[i].name == "docs/readme.txt")
				readme = int(i);
		}
		CHECK(dirs == 1 && rom >= 0 && patch >= 0 && readme >= 0, "solid: the folder is one, the names keep their folders");
		CHECK(rom >= 0 && patch >= 0 && e[size_t(rom)].block_size == e[size_t(patch)].block_size &&
				  e[size_t(rom)].block_size >= 12000 + 6000,
			"solid: the files share one block");
		const int pick = PickRom(e, kMaxRom, &err);
		CHECK(pick == rom && err == Error::None, "solid: the ROM picked is the biggest file");
		std::vector<uint8_t> out;
		bool all = rom >= 0 && patch >= 0 && readme >= 0;
		all = all && Extract(P("solid.7z"), e[size_t(rom)].index, &out, &err) && out == Rom(12000, 8);
		all = all && Extract(P("solid.7z"), e[size_t(patch)].index, &out, &err) && out == Rom(6000, 9);
		std::string text;
		for (int i = 0; i < 20; i++)
			text += "Read me.\n";
		all = all && Extract(P("solid.7z"), e[size_t(readme)].index, &out, &err) && std::string(out.begin(), out.end()) == text;
		CHECK(all, "solid: each file of the block unpacks byte-identical");
	}

	// a UTF-16 name comes out as UTF-8
	{
		std::vector<uint8_t> out;
		std::string name;
		Error err;
		const bool ok = ExtractRom(P("unicode.7z"), kMaxRom, &out, &err, &name);
		CHECK(ok && name == "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0 (Japan).sfc" && out == Rom(1000, 10), "a Japanese file name, in UTF-8");
	}

	// a copier header: the stored CRC is the whole file's; the data has the 512 bytes in front
	{
		std::vector<Entry> e;
		Error err;
		std::vector<uint8_t> want(512, 0);
		const std::vector<uint8_t> body = Rom(8192, 11);
		want.insert(want.end(), body.begin(), body.end());
		const bool listed = List(P("header.7z"), &e, &err);
		CHECK(listed && e.size() == 1 && e[0].size == 8192 + 512 && e[0].crc == Crc32(want), "copier header: the stored CRC covers it");
		std::vector<uint8_t> out;
		CHECK(ExtractRom(P("header.7z"), kMaxRom, &out, &err) && out == want && Crc32(out, 512) == Crc32(body),
			"copier header: unpacked whole (the caller drops the 512 bytes)");
	}

	// refused, each with its reason
	{
		std::vector<Entry> e;
		std::vector<uint8_t> out;
		Error err;
		CHECK(List(P("empty.7z"), &e, &err) && e.empty(), "an empty archive lists nothing");
		CHECK(!ExtractRom(P("empty.7z"), kMaxRom, &out, &err) && err == Error::NoRom && out.empty(), "an empty archive: no ROM");
		CHECK(!ExtractRom(P("disc.7z"), kMaxRom, &out, &err) && err == Error::DiscImage, "a cue sheet and its .bin: a CD image, refused");
		CHECK(!List(P("enc-header.7z"), &e, &err) && err == Error::Encrypted, "a password with the header encrypted: refused as encrypted");
		const bool listed = List(P("enc-data.7z"), &e, &err);
		CHECK(!ExtractRom(P("enc-data.7z"), kMaxRom, &out, &err) && err == Error::Encrypted && out.empty(),
			"a password on the data only: refused as encrypted");
		(void)listed;
		CHECK(!ExtractRom(P("bzip2.7z"), kMaxRom, &out, &err) && err == Error::Unsupported, "BZip2: an unsupported method");
		CHECK(List(P("too-big.7z"), &e, &err) && e.size() == 1 && e[0].size == (17u << 20), "a 17 MiB file is listed");
		CHECK(PickRom(e, kMaxRom, &err) < 0 && err == Error::TooBig, "but it is over the ROM limit");
		CHECK(!ExtractRom(P("too-big.7z"), kMaxRom, &out, &err) && err == Error::TooBig, "and not unpacked");
		CHECK(List(P("big-block.7z"), &e, &err) && e.size() == 2 && e[0].block_size > kMaxBlock, "a block of 65 MiB and more is listed");
		CHECK(!ExtractRom(P("big-block.7z"), kMaxRom, &out, &err) && err == Error::BlockTooBig && out.empty(),
			"its small ROM is refused: the block is over the 64 MiB cap");
		CHECK(!ExtractRom(g_dir + "/no-such.7z", kMaxRom, &out, &err) && err == Error::Open, "a missing file: can't be read");
		std::string none = WriteTemp(std::vector<uint8_t>(1000, 0x5A));
		CHECK(!none.empty() && !List(none, &e, &err) && err == Error::Damaged, "not a 7z: damaged");
		unlink(none.c_str());
	}

	// cut short, and a byte changed in the packed data
	{
		const std::vector<uint8_t> whole = ReadAll(P("lzma2.7z"));
		std::vector<Entry> e;
		std::vector<uint8_t> out;
		Error err;
		bool all = !whole.empty();
		for (size_t cut : {size_t(6), size_t(32), whole.size() / 2, whole.size() - 1})
		{
			std::string t = WriteTemp(std::vector<uint8_t>(whole.begin(), whole.begin() + long(cut)));
			all = all && !t.empty() && !List(t, &e, &err) && err == Error::Damaged && !ExtractRom(t, kMaxRom, &out, &err);
			unlink(t.c_str());
		}
		CHECK(all, "cut short anywhere: damaged, nothing unpacked");
		std::vector<uint8_t> bad = whole;
		bad[32 + 100] ^= 0x55; // the packed data start right after the 32-byte start header
		std::string t = WriteTemp(bad);
		CHECK(!t.empty() && List(t, &e, &err) && !ExtractRom(t, kMaxRom, &out, &err) && err == Error::Damaged && out.empty(),
			"a changed byte in the data: the CRC check refuses it");
		unlink(t.c_str());
	}

	CHECK(std::string(Message(Error::Encrypted)).find("password") != std::string::npos && *Message(Error::None) == 0,
		"each error has its sentence");

	printf("7z: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

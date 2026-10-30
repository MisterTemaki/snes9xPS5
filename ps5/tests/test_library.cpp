// The saved game list (frontend/fe_library.h, 2.10): the file's round trip, the files it refuses, the games of a folder
// that isn't there left out, and the game the shelf keeps selected after the check in the background.
// SPDX-License-Identifier: MIT

#include "fe_library.h"
#include "fe_tabs.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace fe;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c)                                                          \
	do                                                                    \
	{                                                                     \
		g_checks++;                                                       \
		if (!(c))                                                         \
		{                                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
			g_fail++;                                                     \
		}                                                                 \
	} while (0)

static bool Same(const GameInfo& a, const GameInfo& b)
{
	return a.path == b.path && a.file_base == b.file_base && a.ext == b.ext && a.nointro == b.nointro && a.title == b.title &&
		   a.region == b.region && a.on_usb == b.on_usb && a.on_network == b.on_network && a.name_by_crc == b.name_by_crc &&
		   a.crc_pending == b.crc_pending && a.crc_name == b.crc_name && a.header_title == b.header_title &&
		   a.header_japan == b.header_japan && a.crc == b.crc;
}

static std::vector<GameInfo> Sample()
{
	std::vector<GameInfo> v(4);
	v[0].path = "/data/snes9x/roms/Super Mario World (USA).sfc";
	v[0].file_base = "Super Mario World (USA)";
	v[0].ext = ".sfc";
	v[0].nointro = "Super Mario World (USA)";
	v[0].title = "Super Mario World";
	v[0].region = "USA";
	v[1].path = "/mnt/usb0/snes9x/roms/hack/Zelda Ãœber – ゼルダ.smc"; // UTF-8, any byte
	v[1].file_base = "Zelda Ãœber – ゼルダ";
	v[1].ext = ".smc";
	v[1].title = v[1].file_base;
	v[1].on_usb = true;
	v[1].crc = 0xDEADBEEF;
	v[1].crc_name = "Zelda (Hack)";
	v[1].header_title = "ZELDA";
	v[1].header_japan = true;
	v[2].path = "/smb/nas/snes/Chrono Trigger (USA).zip";
	v[2].file_base = "Chrono Trigger (USA)";
	v[2].ext = ".zip";
	v[2].nointro = "Chrono Trigger (USA)";
	v[2].title = "Chrono Trigger";
	v[2].on_network = true;
	v[2].crc_pending = true;
	v[3].path = "/data/snes9x/roms/x.sfc";
	v[3].file_base = "x";
	v[3].ext = ".sfc";
	v[3].nointro = "Donkey Kong Country (USA) (Rev 2)";
	v[3].title = "Donkey Kong Country";
	v[3].name_by_crc = true;
	v[3].crc = 0x12345678;
	return v;
}

int main()
{
	// ---- round trip: every field, every flag, the tab each game stands on
	{
		const std::vector<GameInfo> in = Sample();
		const std::string file = library::Encode(in);
		std::vector<GameInfo> out;
		CHECK(library::Decode(file, out) == library::Bad::None);
		CHECK(out.size() == in.size());
		for (size_t i = 0; i < in.size() && i < out.size(); i++)
		{
			CHECK(Same(in[i], out[i]));
			CHECK(tabs::Of(in[i]) == tabs::Of(out[i]));
		}
		std::vector<GameInfo> none;
		CHECK(library::Decode(library::Encode(none), out) == library::Bad::None && out.empty());
	}
	// ---- what is refused, and `out` left as it was
	{
		const std::string good = library::Encode(Sample());
		std::vector<GameInfo> out(1);
		out[0].path = "/keep";
		CHECK(library::Decode("", out) == library::Bad::Short);
		CHECK(library::Decode(good.substr(0, 10), out) == library::Bad::Short);
		std::string foreign = good;
		foreign[0] = 'X';
		CHECK(library::Decode(foreign, out) == library::Bad::Magic);
		CHECK(library::Decode(std::string(64, '\0'), out) == library::Bad::Magic);
		std::string old = good;
		old[8] = char(library::kVersion + 1); // another format version, its checksum right
		const uint32_t sum = library::Crc32(old.data(), old.size() - 4);
		for (int i = 0; i < 4; i++)
			old[old.size() - 4 + size_t(i)] = char((sum >> (8 * i)) & 0xff);
		CHECK(library::Decode(old, out) == library::Bad::Version);
		std::string flipped = good;
		flipped[40] ^= 0x20; // one bit in a name
		CHECK(library::Decode(flipped, out) == library::Bad::Checksum);
		CHECK(library::Decode(good.substr(0, good.size() - 1), out) == library::Bad::Checksum); // cut short
		CHECK(library::Decode(good + "x", out) == library::Bad::Checksum); // a byte too many
		// a count that doesn't fit the contents, checksum made right: malformed
		std::string lying = good;
		lying[12] = char(lying[12] + 1);
		const uint32_t sum2 = library::Crc32(lying.data(), lying.size() - 4);
		for (int i = 0; i < 4; i++)
			lying[lying.size() - 4 + size_t(i)] = char((sum2 >> (8 * i)) & 0xff);
		CHECK(library::Decode(lying, out) == library::Bad::Format);
		CHECK(out.size() == 1 && out[0].path == "/keep");
		// every cut of the file is refused, never read past its end (ASan)
		int refused = 0;
		for (size_t n = 0; n < good.size(); n++)
			refused += library::Decode(good.substr(0, n), out) != library::Bad::None;
		CHECK(refused == int(good.size()));
		CHECK(library::Crc32("123456789", 9) == 0xCBF43926u); // the standard check value
	}
	// ---- the games of a folder that isn't there are left out (a prefix of a folder's name is not that folder)
	{
		std::vector<GameInfo> v = Sample();
		GameInfo near;
		near.path = "/mnt/usb0/snes9x/roms2/a.sfc";
		v.push_back(near);
		const size_t dropped = library::KeepUnder(v, {"/data/snes9x/roms", "/smb/nas"});
		CHECK(dropped == 2);
		CHECK(v.size() == 3);
		bool usb = false;
		for (const GameInfo& g : v)
			usb = usb || g.on_usb || g.path.rfind("/mnt/", 0) == 0;
		CHECK(!usb);
		std::vector<GameInfo> w = Sample();
		CHECK(library::KeepUnder(w, {"/mnt/usb0/snes9x/roms"}) == 3 && w.size() == 1 && w[0].on_usb);
		std::vector<GameInfo> x = Sample();
		CHECK(library::KeepUnder(x, {}) == 4 && x.empty());
	}
	// ---- the game kept selected after the check: itself, else the next one still there, else the previous one
	{
		const std::vector<std::string> view = {"/a", "/b", "/c", "/d", "/e"};
		CHECK(library::Successor(view, 2, {"/a", "/b", "/c", "/d", "/e", "/new"}) == "/c");
		CHECK(library::Successor(view, 2, {"/a", "/b", "/d", "/e"}) == "/d");
		CHECK(library::Successor(view, 2, {"/a", "/b", "/e"}) == "/b"); // the previous one is nearer than /e
		CHECK(library::Successor(view, 2, {"/a", "/e"}) == "/e"); // as near either way: the next one
		CHECK(library::Successor(view, 4, {"/a"}) == "/a");
		CHECK(library::Successor(view, 0, {"/zzz"}).empty());
		CHECK(library::Successor({}, 0, {"/a"}).empty());
		CHECK(library::Successor(view, 9, {"/a"}).empty());
	}
	// ---- a big list: 5000 games read back, and how long that takes (the shelf's first frame waits for it)
	{
		std::vector<GameInfo> big;
		for (int i = 0; i < 5000; i++)
		{
			GameInfo g = Sample()[size_t(i % 4)];
			g.path += std::to_string(i);
			big.push_back(g);
		}
		const auto t0 = std::chrono::steady_clock::now();
		const std::string file = library::Encode(big);
		const auto t1 = std::chrono::steady_clock::now();
		std::vector<GameInfo> back;
		CHECK(library::Decode(file, back) == library::Bad::None && back.size() == 5000 && Same(back[4321], big[4321]));
		const auto t2 = std::chrono::steady_clock::now();
		printf("5000 games: %zu bytes, written in %.1f ms, read in %.1f ms\n", file.size(),
			std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
	}
	printf("library: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}

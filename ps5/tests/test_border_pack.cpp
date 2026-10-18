// Border pack tests: PNG decode, finding the window, the backdrop and the rim, the lookup order and the Select / Reselect /
// Release calls (frontend/fe_border.cpp, fe_borderimg.cpp). The renderer, the cover matcher and the game naming are
// stand-ins below: the real ones are covered by the host groups in run-tests.sh. Pictures are generated PNGs.
// SPDX-License-Identifier: MIT

#include "fe_border.h"
#include "fe_borderfetch.h"
#include "fe_borderimg.h"
#include "fe_covers.h"
#include "fe_games.h"
#include "fe_settings.h"

#include "OrbisPaths.h"
#include "ProsperoVideo.h"

#include <zlib.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ---- stand-ins for what the border code calls ----
// BigThread arms the crash handler per thread; the host test has none.
namespace crashlog
{
void* ArmThread() { return nullptr; }
void DisarmThread(void*) {}
} // namespace crashlog

// g_shown is set before g_set_calls goes up, so a test that has seen the count may read the border (the download watcher sets
// it from its own thread).
namespace ps5video
{
std::shared_ptr<const Border> g_shown;
std::atomic<int> g_set_calls{0};
void SetBorder(std::shared_ptr<const Border> border)
{
	g_shown = std::move(border);
	g_set_calls++;
}
const char* AspectName(Aspect)
{
	return "aspect";
}
} // namespace ps5video

namespace fe
{
// The real matcher is MatchArtName over the pack's names (fe_covers.cpp); here a name is matched when it is the game's
// No-Intro name, which is the one rule these tests need.
std::string MatchPackName(const std::vector<std::string>& names, const GameInfo& game)
{
	return std::find(names.begin(), names.end(), game.nointro) != names.end() ? game.nointro : std::string();
}

// fe_coverfetch.cpp follows git symlinks with it; nothing here downloads.
std::string UrlEncode(const std::string& s)
{
	return s;
}

// The index of The Bezel Project's names: a game is matched by its No-Intro name, or by the name its CRC has in the extra
// table when that is one of them (the real matcher is covered by the host groups).
struct PackMatcher::Impl
{
	std::vector<std::string> names;
};
PackMatcher::PackMatcher(const std::vector<std::string>& names, bool) : m_i(new Impl)
{
	m_i->names = names;
}
PackMatcher::~PackMatcher() = default;
std::string PackMatcher::Match(const GameInfo& game) const
{
	const auto& n = m_i->names;
	if (std::find(n.begin(), n.end(), game.nointro) != n.end())
		return game.nointro;
	return !game.crc_name.empty() && std::find(n.begin(), n.end(), game.crc_name) != n.end() ? game.crc_name : std::string();
}

GameInfo DescribeGame(const std::string& path, uint32_t rom_crc)
{
	GameInfo g;
	g.path = path;
	const std::string name = path.substr(path.find_last_of('/') + 1);
	g.file_base = name.substr(0, name.find_last_of('.'));
	g.crc = rom_crc;
	// "Mapped Name.sfc" is the ROM of the game the pack calls "Mapped Name (USA)"
	g.nointro = g.file_base.rfind("Mapped", 0) == 0 ? g.file_base + " (USA)" : g.file_base;
	// "Loose Name.sfc" is a translation of Super Mario World: the CRC table knows it by the index's name
	if (g.file_base.rfind("Loose", 0) == 0)
		g.crc_name = "Super Mario World (USA)";
	return g;
}
} // namespace fe

namespace
{
int g_failed = 0;
int g_checked = 0;
#define CHECK(cond, what)                                                                          \
	do                                                                                             \
	{                                                                                              \
		g_checked++;                                                                               \
		if (!(cond))                                                                               \
		{                                                                                          \
			g_failed++;                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what);                                  \
		}                                                                                          \
	} while (0)

using ps5video::Rect;
using fe::border::Image;

// ---- a PNG writer: just enough for the tests (filter 0, one IDAT) ----
struct Px
{
	uint8_t r, g, b, a;
};

void Be32(std::vector<uint8_t>& o, uint32_t v)
{
	for (int s = 24; s >= 0; s -= 8)
		o.push_back(uint8_t(v >> s));
}

void Chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data)
{
	Be32(o, uint32_t(data.size()));
	std::vector<uint8_t> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	o.insert(o.end(), body.begin(), body.end());
	Be32(o, uint32_t(crc32(0, body.data(), uInt(body.size()))));
}

std::vector<uint8_t> Png(int w, int h, int depth, int ctype, const std::vector<uint8_t>& scan, const std::vector<uint8_t>& plte = {},
	const std::vector<uint8_t>& trns = {})
{
	std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	std::vector<uint8_t> ihdr;
	Be32(ihdr, uint32_t(w));
	Be32(ihdr, uint32_t(h));
	ihdr.insert(ihdr.end(), {uint8_t(depth), uint8_t(ctype), 0, 0, 0});
	Chunk(o, "IHDR", ihdr);
	if (!plte.empty())
		Chunk(o, "PLTE", plte);
	if (!trns.empty())
		Chunk(o, "tRNS", trns);
	uLongf n = compressBound(uLong(scan.size()));
	std::vector<uint8_t> z(n);
	compress2(z.data(), &n, scan.data(), uLong(scan.size()), 1);
	z.resize(n);
	Chunk(o, "IDAT", z);
	Chunk(o, "IEND", {});
	return o;
}

template <class F> std::vector<uint8_t> PngRgba(int w, int h, F px, int depth = 8)
{
	std::vector<uint8_t> scan;
	for (int y = 0; y < h; y++)
	{
		scan.push_back(0);
		for (int x = 0; x < w; x++)
		{
			const Px p = px(x, y);
			for (uint8_t c : {p.r, p.g, p.b, p.a})
			{
				scan.push_back(c);
				if (depth == 16)
					scan.push_back(c);
			}
		}
	}
	return Png(w, h, depth, 6, scan);
}

template <class F> std::vector<uint8_t> PngRgb(int w, int h, F px)
{
	std::vector<uint8_t> scan;
	for (int y = 0; y < h; y++)
	{
		scan.push_back(0);
		for (int x = 0; x < w; x++)
		{
			const Px p = px(x, y);
			scan.insert(scan.end(), {p.r, p.g, p.b});
		}
	}
	return Png(w, h, 8, 2, scan);
}

// ---- the art: a frame with a clear window, a soft 2 pixel edge inside it, a soft ring outside it, one opaque corner ----
const Rect kWinA = {261, 21, 1398, 1038};
const Rect kWinB = {200, 40, 1500, 1000};
const Rect kWinC = {300, 60, 1300, 960};
const Rect kWinD = {100, 100, 1000, 700};

Px Art(const Rect& win, int x, int y)
{
	const bool in = x >= win.x && x < win.x + win.w && y >= win.y && y < win.y + win.h;
	if (in)
	{
		if (x == win.x && y == win.y)
			return {200, 100, 50, 255}; // a rounded corner of the glass, opaque
		const int d = std::min(std::min(x - win.x, win.x + win.w - 1 - x), std::min(y - win.y, win.y + win.h - 1 - y));
		if (d < 2)
			return {255, 255, 255, 10}; // the soft edge: counts as clear, still drawn over the picture
		return {0, 0, 0, 0};
	}
	const int dx = std::max(std::max(win.x - x, x - (win.x + win.w - 1)), 0);
	const int dy = std::max(std::max(win.y - y, y - (win.y + win.h - 1)), 0);
	if (std::max(dx, dy) <= 3)
		return {20, 30, 40, 128}; // the frame's own soft edge, outside the window
	return {200, 40, 40, 255};
}

std::vector<uint8_t> ArtPng(const Rect& win)
{
	return PngRgba(1920, 1080, [&](int x, int y) { return Art(win, x, y); });
}

// ---- files ----
std::string g_root;

void WriteBytes(const std::string& path, const std::vector<uint8_t>& b)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (f)
	{
		fwrite(b.data(), 1, b.size(), f);
		fclose(f);
	}
}

void WriteText(const std::string& path, const std::string& text)
{
	WriteBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

void Remove(const std::string& path)
{
	unlink(path.c_str());
}

bool SameRect(const Rect& a, const Rect& b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

fe::GameInfo Game(const char* file_base, const char* nointro = "")
{
	fe::GameInfo g;
	g.file_base = file_base;
	g.nointro = nointro;
	return g;
}

// ---- the tests ----
void TestDecode()
{
	using fe::border::DecodePng;
	Image img;
	std::string why;

	const std::vector<uint8_t> rgba = ArtPng(kWinA);
	CHECK(DecodePng(rgba.data(), rgba.size(), &img, &why), "an RGBA 1920x1080 PNG decodes");
	CHECK(img.w == 1920 && img.h == 1080 && img.px.size() == 1920u * 1080u, "decoded size");
	CHECK(img.px[0] == 0xff2828c8u, "pixel (0,0) is 0xAABBGGRR of the frame colour (200, 40, 40)");
	CHECK(img.px[size_t(540) * 1920 + 960] == 0u, "the centre is clear (alpha 0)");

	const std::vector<uint8_t> rgb = PngRgb(1920, 1080, [](int, int) { return Px{10, 20, 30, 0}; });
	CHECK(DecodePng(rgb.data(), rgb.size(), &img, &why), "an RGB PNG decodes");
	CHECK(img.px[12345] == (0xff000000u | (30u << 16) | (20u << 8) | 10u), "RGB gets alpha 255");

	// palette + tRNS, as The Bezel Project's files are: index 0 clear, 1 the frame, 2 half-clear white
	std::vector<uint8_t> scan;
	for (int y = 0; y < 1080; y++)
	{
		scan.push_back(0);
		for (int x = 0; x < 1920; x++)
			scan.push_back(x >= 261 && x < 1659 && y >= 21 && y < 1059 ? (x == 261 ? 2 : 0) : 1);
	}
	const std::vector<uint8_t> pal =
		Png(1920, 1080, 8, 3, scan, {0, 0, 0, 200, 40, 40, 255, 255, 255}, {0, 255, 128});
	CHECK(DecodePng(pal.data(), pal.size(), &img, &why), "a palette PNG with tRNS decodes");
	CHECK((img.px[size_t(540) * 1920 + 960] >> 24) == 0, "palette index 0 is clear through tRNS");
	CHECK((img.px[5] >> 24) == 255, "palette index 1 is opaque");
	CHECK((img.px[size_t(540) * 1920 + 261] >> 24) == 128, "palette index 2 has alpha 128");

	const std::vector<uint8_t> small = PngRgba(100, 100, [](int, int) { return Px{0, 0, 0, 0}; });
	CHECK(!DecodePng(small.data(), small.size(), &img, &why) && why.find("100 x 100") != std::string::npos,
		"a PNG that is not 1920x1080 is refused, naming its size");

	const std::vector<uint8_t> deep = PngRgba(1920, 1080, [](int, int) { return Px{0, 0, 0, 0}; }, 16);
	CHECK(!DecodePng(deep.data(), deep.size(), &img, &why) && why.find("16 bits") != std::string::npos, "16 bits per channel is refused");

	const std::vector<uint8_t> junk(300, 0x41);
	CHECK(!DecodePng(junk.data(), junk.size(), &img, &why) && why.find("not a PNG") != std::string::npos, "bytes that are no PNG are refused");
	CHECK(!DecodePng(junk.data(), 0, &img, &why) && why.find("empty") != std::string::npos, "an empty file is refused");

	std::vector<uint8_t> cut(rgba.begin(), rgba.begin() + long(rgba.size() / 2));
	CHECK(!DecodePng(cut.data(), cut.size(), &img, &why), "a truncated PNG is refused");
	CHECK(why.find("damaged") != std::string::npos, "and called damaged");

	std::vector<uint8_t> big(fe::border::kMaxFileBytes + 1, 0);
	CHECK(!DecodePng(big.data(), big.size(), &img, &why) && why.find("8 MiB") != std::string::npos, "a file over 8 MiB is refused");
}

void TestWindow()
{
	using fe::border::FindWindow;
	Image img;
	std::string why;
	Rect win = {};

	const std::vector<uint8_t> a = ArtPng(kWinA);
	fe::border::DecodePng(a.data(), a.size(), &img, &why);
	CHECK(FindWindow(img, &win, &why) && SameRect(win, kWinA), "the window is the bounding box of the clear region (261,21 1398x1038)");

	// a clear island far away and a clear diagonal that touches the window only at a corner are not part of it
	Image tricky = img;
	for (int y = 600; y < 610; y++)
		for (int x = 40; x < 50; x++)
			tricky.px[size_t(y) * 1920 + x] = 0;
	for (int k = 0; k < 10; k++)
		tricky.px[size_t(kWinA.y - k) * 1920 + size_t(kWinA.x - 1 - k)] = 0;
	CHECK(FindWindow(tricky, &win, &why) && SameRect(win, kWinA), "a clear island, and a diagonal-only link, do not widen the window");

	Image opaque = img;
	for (int y = 530; y < 550; y++)
		for (int x = 950; x < 970; x++)
			opaque.px[size_t(y) * 1920 + x] = 0xff000000u;
	CHECK(!FindWindow(opaque, &win, &why) && why.find("centre") != std::string::npos, "an opaque centre is refused");

	const std::vector<uint8_t> tiny = ArtPng({900, 500, 300, 200});
	Image t;
	fe::border::DecodePng(tiny.data(), tiny.size(), &t, &why);
	CHECK(!FindWindow(t, &win, &why) && why.find("too small") != std::string::npos, "a window under 480x270 is refused");

	Image wrong;
	wrong.w = 100;
	wrong.h = 100;
	CHECK(!FindWindow(wrong, &win, &why), "an image of the wrong size is refused");

	CHECK(fe::border::WindowFits({0, 0, 1920, 1080}), "a window may be the whole surface");
	CHECK(fe::border::WindowFits({100, 50, 480, 270}), "480x270 is the smallest given window");
	CHECK(!fe::border::WindowFits({100, 50, 479, 270}), "a given window under 480 wide is refused");
	CHECK(!fe::border::WindowFits({100, 50, 480, 269}), "a given window under 270 high is refused");
	CHECK(!fe::border::WindowFits({1700, 0, 400, 400}), "a window past the right edge is refused");
	CHECK(!fe::border::WindowFits({0, 700, 400, 400}), "a window past the bottom edge is refused");
	CHECK(!fe::border::WindowFits({-1, 0, 400, 400}), "a window left of the surface is refused");
}

void TestBuild()
{
	using fe::border::BuildBorder;
	Image img;
	std::string why;
	const std::vector<uint8_t> a = ArtPng(kWinA);
	fe::border::DecodePng(a.data(), a.size(), &img, &why);
	const auto b = BuildBorder(img, kWinA, &why);
	CHECK(b != nullptr, "a border is built");
	if (!b)
		return;
	CHECK(SameRect(b->window, kWinA), "the border keeps its window");
	CHECK(b->backdrop.size() == 1920u * 1080u, "the backdrop is the whole surface");
	CHECK(b->backdrop[0] == 0xff2828c8u, "an opaque frame pixel is itself, alpha 255");
	CHECK(b->backdrop[size_t(540) * 1920 + 960] == 0xff000000u, "a clear window pixel is black");
	// the frame's soft ring at (260, 500): colour (20,30,40) at alpha 128 on black
	CHECK(b->backdrop[size_t(500) * 1920 + 260] == 0xff140f0au, "a half-clear pixel outside the window is composited on black");
	// the soft edge inside the window: white at alpha 10 -> 10 on black, and it is a rim pixel
	const uint32_t edge = uint32_t(500) * 1920 + 262; // two in from the window's left edge: d = 1
	CHECK(b->backdrop[edge - 1] == 0xff0a0a0au, "a soft edge pixel inside the window is on black in the backdrop");

	size_t expected = 0;
	for (int y = kWinA.y; y < kWinA.y + kWinA.h; y++)
		for (int x = kWinA.x; x < kWinA.x + kWinA.w; x++)
			expected += Art(kWinA, x, y).a != 0;
	CHECK(b->rim.size() == expected && expected > 0, "the rim holds every window pixel with alpha 1..255");
	CHECK(std::is_sorted(b->rim.begin(), b->rim.end(), [](const ps5video::RimPixel& p, const ps5video::RimPixel& q) { return p.index < q.index; }),
		"the rim is sorted by index");
	const auto find = [&](uint32_t index) { return std::find_if(b->rim.begin(), b->rim.end(), [&](const ps5video::RimPixel& p) { return p.index == index; }); };
	const auto soft = find(edge - 1);
	CHECK(soft != b->rim.end() && soft->pre == 0x000a0a0au && soft->inv == 245u, "a soft edge pixel: colour times alpha, and 255 - alpha");
	const auto corner = find(uint32_t(kWinA.y) * 1920 + uint32_t(kWinA.x));
	CHECK(corner != b->rim.end() && corner->pre == 0x003264c8u && corner->inv == 0u, "an opaque corner of the glass is in the rim, inv 0");
	CHECK(find(uint32_t(540) * 1920 + 960) == b->rim.end(), "a clear pixel is not in the rim");
	CHECK(find(uint32_t(500) * 1920 + 260) == b->rim.end(), "a pixel outside the window is not in the rim");

	// art that covers most of the window is refused: it would cost frame time
	Image veil = img;
	for (uint32_t& p : veil.px)
		if ((p >> 24) == 0)
			p = 0x20000000u;
	CHECK(BuildBorder(veil, kWinA, &why) == nullptr && why.find("covered") != std::string::npos, "a window covered by art is refused");

	Image bad;
	bad.w = 10;
	bad.h = 10;
	CHECK(BuildBorder(bad, kWinA, &why) == nullptr, "an image of the wrong size builds nothing");
}

// ---- the window follows the aspect ratio ----
// The art for the fit tests: opaque margins that count their own pixels (red = x / 8, green = y / 5), the window of kWinA with
// kWinA's soft edge and one opaque corner, as Art() draws them.
Image GradientArt()
{
	Image img;
	img.w = 1920;
	img.h = 1080;
	img.px.resize(size_t(1920) * 1080);
	for (int y = 0; y < 1080; y++)
		for (int x = 0; x < 1920; x++)
		{
			Px p = Art(kWinA, x, y);
			if (p.a == 255 && !(x == kWinA.x && y == kWinA.y))
				p = {uint8_t(x / 8), uint8_t(y / 5), 90, 255};
			img.px[size_t(y) * 1920 + x] = uint32_t(p.a) << 24 | uint32_t(p.b) << 16 | uint32_t(p.g) << 8 | p.r;
		}
	return img;
}

bool Shaped(const Rect& w, int rw, int rh) // the window has the shape rw:rh, to a pixel of rounding
{
	return std::abs(w.w * rh - w.h * rw) <= std::max(rw, rh) * 2;
}

void TestFit()
{
	using namespace fe::border;
	std::string why;

	int rw = 0, rh = 0;
	CHECK(OpeningRatio(ps5video::Aspect::Ratio4x3, &rw, &rh) && rw == 4 && rh == 3, "4:3 gives a 4:3 opening");
	CHECK(OpeningRatio(ps5video::Aspect::PixelPerfect, &rw, &rh) && rw == 8 && rh == 7, "pixel perfect gives an 8:7 opening");
	CHECK(OpeningRatio(ps5video::Aspect::Stretch, &rw, &rh) && rw == 16 && rh == 9, "stretch gives a 16:9 opening");
	CHECK(!OpeningRatio(ps5video::Aspect::Integer, &rw, &rh), "integer scale has a size, not a shape");
	CHECK(!OpeningRatio(ps5video::Aspect::Count, &rw, &rh), "and no aspect ratio at all keeps the art as drawn");

	// where the window goes
	CHECK(SameRect(FitOpening(kWinA, 4, 3), kWinA), "a window within 3% of 4:3 stays in 4:3 (1398x1038 is 1.347)");
	const Rect w87 = FitOpening(kWinA, 8, 7);
	CHECK(SameRect(w87, Rect{367, 21, 1186, 1038}), "8:7: the height stays, the width follows (1186), the side margins grow in proportion");
	const Rect w169 = FitOpening(kWinA, 16, 9);
	CHECK(SameRect(w169, Rect{104, 59, 1712, 963}), "16:9: the side margins shrink to 40% (261 -> 104), the height gives way (963)");
	CHECK(w169.x * 2 + w169.w == 1920 && std::abs(w169.y * 2 + w169.h - 1080) <= 1, "and the window of a centred frame stays centred");
	const Rect wb = FitOpening(kWinB, 4, 3);
	CHECK(Shaped(wb, 4, 3) && wb.h == kWinB.h && (wb.w & 1) == 0, "a 3:2 window becomes 4:3 at the same height, even width");
	CHECK(SameRect(FitOpening({0, 0, 1920, 1080}, 4, 3), Rect{240, 0, 1440, 1080}), "a window that fills the screen is centred when it narrows");
	CHECK(SameRect(FitOpening({0, 0, 1920, 1080}, 16, 9), Rect{0, 0, 1920, 1080}), "and stays when it already has the shape");
	// Integer scale: the opening is the whole-multiple picture, 4x = 1024x896 for most windows
	CHECK(SameRect(IntegerOpening(kWinA), Rect{448, 92, 1024, 896}), "Integer scale: 1398x1038 becomes 1024x896 (4x), a centred frame stays centred");
	CHECK(SameRect(IntegerOpening(Rect{448, 92, 1024, 896}), Rect{448, 92, 1024, 896}), "a window that is 4x already stays");
	CHECK(SameRect(IntegerOpening(kWinB), Rect{427, 92, 1024, 896}), "an off-centre frame keeps its side margins in proportion (200 : 220 -> 427 : 469)");
	CHECK(SameRect(IntegerOpening(Rect{480, 270, 480, 270}), Rect{555, 285, 256, 224}), "a window that only fits 1x gets 256x224");
	CHECK(SameRect(IntegerOpening(Rect{0, 0, 1920, 1080}), Rect{448, 92, 1024, 896}), "a window that fills the screen is cut to 4x and centred");
	CHECK(SameRect(IntegerOpening(kWinC), Rect{434, 92, 1024, 896}), "the height decides the multiple (kWinC: 960 / 224 = 4)");
	// every window and shape ends up inside the surface, even, in shape (or untouched), and with margins that keep their proportions
	bool sane = true;
	for (const Rect& win : {kWinA, kWinB, kWinC, kWinD, Rect{0, 0, 1920, 1080}, Rect{480, 270, 960, 540}, Rect{100, 20, 1700, 960}})
		for (const std::pair<int, int>& ratio : {std::make_pair(4, 3), std::make_pair(8, 7), std::make_pair(16, 9)})
		{
			const Rect r = FitOpening(win, ratio.first, ratio.second);
			const bool same = SameRect(r, win);
			const bool inside = r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= 1920 && r.y + r.h <= 1080;
			const int mw = 1920 - win.w, mh = 1080 - win.h;
			const bool kept = same || ((mw == 0 || std::abs(r.x * mw - win.x * (1920 - r.w)) <= mw) && (mh == 0 || std::abs(r.y * mh - win.y * (1080 - r.h)) <= mh));
			sane = sane && inside && (same || ((r.w & 1) == 0 && Shaped(r, ratio.first, ratio.second) && kept));
		}
	CHECK(sane, "FitOpening stays inside the surface, in shape, with the margins in proportion, for a range of windows");
	sane = true;
	for (const Rect& win : {kWinA, kWinB, kWinC, kWinD, Rect{0, 0, 1920, 1080}, Rect{480, 270, 960, 540}, Rect{100, 20, 1700, 960}, Rect{448, 92, 1024, 896}})
	{
		const Rect r = IntegerOpening(win);
		const int k = r.w / 256;
		const int mw = 1920 - win.w, mh = 1080 - win.h;
		const bool inside = r.x >= 0 && r.y >= 0 && r.x + r.w <= 1920 && r.y + r.h <= 1080 && r.w <= win.w && r.h <= win.h;
		const bool kept = (mw == 0 || std::abs(r.x * mw - win.x * (1920 - r.w)) <= mw) && (mh == 0 || std::abs(r.y * mh - win.y * (1080 - r.h)) <= mh);
		sane = sane && inside && kept && k >= 1 && r.w == 256 * k && r.h == 224 * k;
	}
	CHECK(sane, "IntegerOpening is a whole multiple of 256x224 inside the drawn window, with the margins in proportion, for a range of windows");

	// the art
	Image src = GradientArt();
	Image img = src;
	Rect win = kWinA;
	CHECK(FitToAspect(&img, &win, ps5video::Aspect::Count, &why) && SameRect(win, kWinA) && img.px == src.px, "no aspect ratio: nothing is touched");
	CHECK(FitToAspect(&img, &win, ps5video::Aspect::Ratio4x3, &why) && SameRect(win, kWinA) && img.px == src.px, "4:3 on a 4:3 window: nothing is touched");

	// 8:7: x changes (the side margins grow), y does not
	win = kWinA;
	CHECK(FitToAspect(&img, &win, ps5video::Aspect::PixelPerfect, &why) && SameRect(win, w87), "8:7: the window is the new opening");
	Rect found = {};
	CHECK(FindWindow(img, &found, &why) && SameRect(found, w87), "and it is the clear hole the window search finds in the stretched art");
	const auto px = [&](int x, int y) { return img.px[size_t(y) * 1920 + x]; };
	const auto src_px = [&](int x, int y) { return src.px[size_t(y) * 1920 + x]; };
	CHECK(px(0, 0) == src_px(0, 0) && px(1919, 1079) == src_px(1919, 1079), "the panels keep their anchor corners: the left one from the top, the right one from the bottom");
	CHECK(px(w87.x - 1, 300) == src_px(kWinA.x - 1, 300), "the last column left of the window is the art's last column left of its window (no blend across the cut)");
	CHECK(px(w87.x + w87.w, 300) == src_px(kWinA.x + kWinA.w, 300), "and the first column right of the window likewise");
	// the left panel (237 px of art, 343 px in the result) is scaled up evenly: x = 200, y = 300 show the art at (138.0, 207.1)
	// (red counts x / 8, green y / 5); a panel squeezed to the height would show y = 300
	CHECK(std::abs(int(px(200, 300) & 0xff) - 17) <= 1 && std::abs(int((px(200, 300) >> 8) & 0xff) - 41) <= 1,
		"the side panel is scaled up evenly and cropped to the height, not stretched sideways");
	bool rim_same = true;
	for (int y = 0; y < 1080; y++)
		for (int i = 0; i < 24; i++)
			rim_same = rim_same && px(w87.x - 24 + i, y) == src_px(kWinA.x - 24 + i, y) &&
					   px(w87.x + w87.w + i, y) == src_px(kWinA.x + kWinA.w + i, y);
	CHECK(rim_same, "the 24 px rim along the window is the art as drawn, top to bottom");
	bool corner_same = true;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			corner_same = corner_same && px(w87.x + x, w87.y + y) == src_px(kWinA.x + x, kWinA.y + y);
	CHECK(corner_same && px(w87.x, w87.y) == 0xff3264c8u, "the 64x64 corner of the window is copied as drawn, the opaque corner pixel with it");
	const auto b87 = BuildBorder(img, win, &why);
	CHECK(b87 && SameRect(b87->window, w87) && b87->rim.size() > 0, "the stretched art builds into a border with the new window");

	// 16:9: both axes change; the window is still found, the corner is still as drawn
	img = src;
	win = kWinA;
	CHECK(FitToAspect(&img, &win, ps5video::Aspect::Stretch, &why) && SameRect(win, w169), "16:9: the window is the new opening");
	CHECK(FindWindow(img, &found, &why) && SameRect(found, w169), "and it is the clear hole the window search finds");
	corner_same = true;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			corner_same = corner_same && px(w169.x + x, w169.y + y) == src_px(kWinA.x + x, kWinA.y + y);
	CHECK(corner_same, "the corner is as drawn");
	CHECK((px(0, 0) & 0x00ffffff) == (src_px(0, 0) & 0x00ffffff), "the outer corner of the frame is the art's");
	bool outer_same = true; // the margins shrink (261 -> 104): the panel is its outer part, as drawn, not squeezed
	for (int y = 0; y < 1080; y++)
		for (int x = 0; x < 80; x++)
			outer_same = outer_same && px(x, y) == src_px(x, y) && px(1919 - x, y) == src_px(1919 - x, y);
	CHECK(outer_same, "a margin that shrinks shows the outer part of its panel as drawn");
	CHECK(BuildBorder(img, win, &why) != nullptr, "the 16:9 art builds into a border");

	// Integer scale: the window is 1024x896; both axes shrink, so the margins stretch (4.4x tall at the top and bottom)
	img = src;
	win = kWinA;
	const Rect wint = {448, 92, 1024, 896};
	CHECK(FitToAspect(&img, &win, ps5video::Aspect::Integer, &why) && SameRect(win, wint), "Integer scale: the window is the whole-multiple picture");
	CHECK(FindWindow(img, &found, &why) && SameRect(found, wint), "and it is the clear hole the window search finds in the stretched art");
	CHECK(px(0, 0) == src_px(0, 0) && px(1919, 1079) == src_px(1919, 1079), "the outer corners of the frame are the art's");
	CHECK(px(wint.x - 1, 300) == src_px(kWinA.x - 1, 300) && px(wint.x + wint.w, 300) == src_px(kWinA.x + kWinA.w, 300), "no blend across the cut at the sides");
	corner_same = true;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			corner_same = corner_same && px(wint.x + x, wint.y + y) == src_px(kWinA.x + x, kWinA.y + y);
	CHECK(corner_same, "the corner of the window is as drawn");
	CHECK(BuildBorder(img, win, &why) != nullptr, "the Integer art builds into a border");
	img = src;
	win = wint;
	CHECK(FitToAspect(&img, &win, ps5video::Aspect::Integer, &why) && SameRect(win, wint) && img.px == src.px, "a window that is the whole multiple already: nothing is touched");

	// a window given by hand, an RGB picture (no hole of its own): punched, then stretched with its window
	Image rgb;
	rgb.w = 1920;
	rgb.h = 1080;
	rgb.px.assign(size_t(1920) * 1080, 0xff804020u);
	const Rect given = {300, 100, 1200, 800};
	PunchWindow(&rgb, given);
	win = given;
	CHECK(FitToAspect(&rgb, &win, ps5video::Aspect::PixelPerfect, &why) && Shaped(win, 8, 7) && FindWindow(rgb, &found, &why) && SameRect(found, win),
		"a window from an .ini is stretched too, and found again in the result");

	// a window that touches the edge has no margin to stretch: the surface still ends up with a hole of the new shape
	Image full;
	full.w = 1920;
	full.h = 1080;
	full.px.assign(size_t(1920) * 1080, 0);
	win = {0, 0, 1920, 1080};
	CHECK(FitToAspect(&full, &win, ps5video::Aspect::Ratio4x3, &why) && SameRect(win, Rect{240, 0, 1440, 1080}), "a full-screen window narrows to 4:3, centred");

	Image wrong;
	wrong.w = 10;
	wrong.h = 10;
	win = kWinA;
	CHECK(!RefitImage(&wrong, kWinA, w87, &why), "an image of the wrong size is refused");
}

// ---- the pack ----
void MakePack(const std::string& root)
{
	mkdir((root + "/borders").c_str(), 0777);
	mkdir((root + "/borders/games").c_str(), 0777);
}

void ClearPack(const std::string& pack)
{
	const std::string games = pack + "/games";
	if (FILE* p = popen(("rm -rf '" + games + "' '" + pack + "/default.png' '" + pack + "/default.ini'").c_str(), "r"))
		pclose(p);
	mkdir(games.c_str(), 0777);
}

void TestChoose()
{
	using fe::border::Choose;
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/games/";
	ClearPack(pack);

	const std::vector<uint8_t> png_a = ArtPng(kWinA), png_b = ArtPng(kWinB), png_c = ArtPng(kWinC), png_d = ArtPng(kWinD);

	// nothing in the pack: the built-in frame
	fe::border::Pick pick = Choose(pack, 1, Game("Super Mario World (USA)", "Super Mario World (USA)"));
	CHECK(pick.border && pick.source == "built-in", "an empty pack falls back to the built-in frame");
	CHECK(pick.border && pick.border->window.w >= 480 && pick.border->window.h >= 270, "the built-in frame has a usable window");
	pick = Choose("/nonexistent/pack", 1, Game("X"));
	CHECK(pick.border && pick.source == "built-in", "a missing pack folder falls back to the built-in frame");

	// default.png
	WriteBytes(pack + "/default.png", png_d);
	pick = Choose(pack, 1, Game("Super Mario World (USA)", "Super Mario World (USA)"));
	CHECK(pick.source == "default.png" && pick.border && SameRect(pick.border->window, kWinD), "no border of its own: default.png");

	// the matcher: the pack calls the game by its No-Intro name, the ROM is called something else
	WriteBytes(games + "Super Mario World (USA).png", png_c);
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "matched" && pick.border && SameRect(pick.border->window, kWinC), "a renamed ROM finds its game's border by the matcher");
	CHECK(pick.file == "Super Mario World (USA).png", "and the pick names the file");

	// the ROM's own file name wins over the matcher
	WriteBytes(games + "smw.png", png_b);
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "own file" && pick.border && SameRect(pick.border->window, kWinB), "games/<ROM name>.png wins over the matcher");

	// Default frame ignores games/
	pick = Choose(pack, 2, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "default.png" && pick.border && SameRect(pick.border->window, kWinD), "Default frame uses default.png, not the game's border");
	Remove(pack + "/default.png");
	pick = Choose(pack, 2, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "built-in", "Default frame without default.png uses the built-in frame");
	WriteBytes(pack + "/default.png", png_d);

	// a file that is there but unusable is skipped, the next choice applies
	WriteBytes(games + "smw.png", std::vector<uint8_t>(500, 7));
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "matched" && pick.border && SameRect(pick.border->window, kWinC), "a corrupt own file falls through to the matcher");
	WriteBytes(games + "Super Mario World (USA).png", ArtPng({900, 500, 300, 200}));
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "default.png", "own and matched both unusable (corrupt, tiny window): default.png");
	WriteBytes(pack + "/default.png", PngRgba(64, 64, [](int, int) { return Px{0, 0, 0, 0}; }));
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "built-in" && pick.border, "a default.png of the wrong size falls through to the built-in frame");
	WriteBytes(pack + "/default.png", png_d);
	WriteBytes(games + "smw.png", png_b);
	WriteBytes(games + "Super Mario World (USA).png", png_c);

	// .ini: off
	WriteText(games + "smw.ini", "off=1\n");
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "off" && !pick.border, "smw.ini with off=1: no border at all");
	Remove(games + "smw.png");
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "off" && !pick.border, "off=1 beside the ROM's name works without a picture of that name");
	Remove(games + "smw.ini");
	WriteText(games + "Super Mario World (USA).ini", "# no border here\noff = 1\n");
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "off" && !pick.border, "off=1 beside the matched picture: no border");
	pick = Choose(pack, 2, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "default.png", "off=1 does not touch the Default frame setting");
	WriteText(games + "Super Mario World (USA).ini", "off=maybe\n");
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "matched", "a bad off value is ignored");
	Remove(games + "Super Mario World (USA).ini");

	// .ini: window
	WriteText(games + "Super Mario World (USA).ini", "window=100, 80, 1200, 900\n");
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.source == "matched" && pick.border && SameRect(pick.border->window, Rect{100, 80, 1200, 900}), "window= replaces the one found");
	WriteText(games + "Super Mario World (USA).ini", "window=100,80,10,10\nunknown=1\n");
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.border && SameRect(pick.border->window, kWinC), "a window= that is too small is ignored, the picture's own window is found");
	WriteText(games + "Super Mario World (USA).ini", "window=1,2,3\n");
	pick = Choose(pack, 1, Game("smw", "Super Mario World (USA)"));
	CHECK(pick.border && SameRect(pick.border->window, kWinC), "a window= with three numbers is ignored");
	WriteText(pack + "/default.ini", "window=50,50,800,600\n");
	pick = Choose(pack, 2, Game("smw"));
	CHECK(pick.border && SameRect(pick.border->window, Rect{50, 50, 800, 600}), "default.ini gives default.png a window");
	Remove(pack + "/default.ini");
	Remove(games + "Super Mario World (USA).ini");

	// the RGB PNG has no alpha: no window to find, but a window= punches one out (and with it any rim)
	WriteBytes(games + "plain.png", PngRgb(1920, 1080, [](int x, int y) { return Px{uint8_t(x), uint8_t(y), 9, 255}; }));
	pick = Choose(pack, 1, Game("plain"));
	CHECK(pick.source == "default.png", "an RGB PNG has no clear centre: refused without a window=, default.png");
	WriteText(games + "plain.ini", "window=300,100,1200,800\n");
	pick = Choose(pack, 1, Game("plain"));
	CHECK(pick.source == "own file" && pick.border && SameRect(pick.border->window, Rect{300, 100, 1200, 800}), "with a window= an RGB PNG is a frame");
	CHECK(pick.border && pick.border->rim.empty(), "whose window has no rim");
	CHECK(pick.border && pick.border->backdrop[size_t(400) * 1920 + 900] == 0xff000000u, "and is black inside the window");
	CHECK(pick.border && pick.border->backdrop[size_t(99) * 1920 + 900] != 0xff000000u, "while the picture outside it is kept");
	WriteText(games + "plain.ini", "window=300,100,479,400\n");
	pick = Choose(pack, 1, Game("plain"));
	CHECK(pick.source == "default.png", "a window= under 480 wide is ignored, so the RGB PNG has no window again");
	// an RGBA picture with a window= loses its soft edge: the rectangle is a hole
	WriteBytes(games + "soft.png", png_a);
	WriteText(games + "soft.ini", "window=261,21,1398,1038\n");
	pick = Choose(pack, 1, Game("soft"));
	CHECK(pick.source == "own file" && pick.border && pick.border->rim.empty(), "window= clears the soft edge and the corner inside it: no rim");
	Remove(games + "soft.png");
	Remove(games + "soft.ini");
	Remove(games + "plain.png");
	Remove(games + "plain.ini");

	// upper-case extension, odd characters in the name
	WriteBytes(games + "Dr. Mario & Co! (USA).PNG", png_a);
	pick = Choose(pack, 1, Game("Dr. Mario & Co! (USA)"));
	CHECK(pick.source == "own file" && pick.border && SameRect(pick.border->window, kWinA), "a .PNG file with dots and symbols in its name is found");

	// time one choice: a typical file, decoded, searched, built (host figure; the console is slower)
	const auto t0 = std::chrono::steady_clock::now();
	pick = Choose(pack, 1, Game("Dr. Mario & Co! (USA)"));
	const auto t1 = std::chrono::steady_clock::now();
	printf("info: one choice (decode, window, backdrop, rim) took %lld ms on the host\n",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
	pick = Choose(pack, 1, Game("Dr. Mario & Co! (USA)"), ps5video::Aspect::Stretch);
	const auto t2 = std::chrono::steady_clock::now();
	printf("info: the same stretched to a 16:9 window took %lld ms\n",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
}

void TestSelect()
{
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/games/";
	ClearPack(pack);
	WriteBytes(games + "Mapped Game.png", ArtPng(kWinB));
	WriteBytes(games + "Mapped Game (USA).png", ArtPng(kWinC));

	fe::Config().border = 0;
	ps5video::g_set_calls = 0;
	fe::border::Select("/data/snes9x/roms/Mapped Game.sfc", 0x1234);
	CHECK(ps5video::g_set_calls == 0 && !ps5video::g_shown, "Border Off: the renderer is not touched");

	fe::Config().border = 1;
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == 1 && ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinB), "Off -> Auto while a game runs: its own border is set");

	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == 1, "Reselect with the setting unchanged does nothing (every settings change comes through it)");

	fe::Config().border = 2;
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == 2 && ps5video::g_shown && ps5video::g_shown->window.w >= 480, "Auto -> Default frame: the built-in frame");

	fe::Config().border = 0;
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == 3 && !ps5video::g_shown, "-> Off: the border is cleared");
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == 3, "and clearing again does not touch the renderer");

	fe::Config().border = 1;
	fe::border::Select("/data/snes9x/roms/Mapped Game.sfc", 0x1234);
	CHECK(ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinB), "Select sets the border of the loaded game");
	fe::border::Release();
	CHECK(!ps5video::g_shown, "Release clears it");
	const int calls = ps5video::g_set_calls;
	fe::Config().border = 2;
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == calls, "Reselect with no game does nothing");
	fe::border::Release();
	CHECK(ps5video::g_set_calls == calls, "and Release with nothing shown does not touch the renderer");

	// a corrupt pack never fails Select
	WriteBytes(games + "Mapped Game.png", std::vector<uint8_t>(10, 1));
	WriteBytes(games + "Mapped Game (USA).png", std::vector<uint8_t>(10, 1));
	fe::Config().border = 1;
	fe::border::Select("/data/snes9x/roms/Mapped Game.sfc", 0);
	CHECK(ps5video::g_shown != nullptr, "a pack of corrupt files still ends in the built-in frame");
	fe::border::Release();
}

// ---- the names The Bezel Project has, the downloads, and the border that arrives while a game runs ----
std::string ReadText(const std::string& path)
{
	std::string out;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			out.append(buf, n);
		fclose(f);
	}
	return out;
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

// The helper's way of saving: the whole file appears at once.
void Land(const std::string& path, const std::vector<uint8_t>& b)
{
	WriteBytes(path + ".part", b);
	rename((path + ".part").c_str(), path.c_str());
}

bool WaitForCalls(int calls, int ms = 4000)
{
	for (int t = 0; t < ms; t += 20)
	{
		if (ps5video::g_set_calls >= calls)
			return true;
		usleep(20 * 1000);
	}
	return ps5video::g_set_calls >= calls;
}

void TestFetch()
{
	using namespace fe::border;
	using fe::WantedCover;
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/games/";
	ClearPack(pack);
	unlink((pack + "/wanted.txt").c_str());
	unsetenv("SNES9X_BORDER_URL");

	const std::vector<std::string>& names = IndexNames();
	CHECK(names.size() > 4000 && names.size() < 4500, "the index has the 1445 SNES and 2685 Super Famicom borders The Bezel Project had in 2026-10");
	CHECK(std::find(names.begin(), names.end(), "Super Mario World (USA)") != names.end(), "Super Mario World (USA) is in it");
	CHECK(std::find(names.begin(), names.end(), "Joe & Mac (USA)") != names.end(), "so is a name with an ampersand, as the repository spells it");
	CHECK(std::find(names.begin(), names.end(), "Star Ocean (Japan)") != names.end() &&
			std::find(names.begin(), names.end(), "Seiken Densetsu 3 (Japan)") != names.end(),
		"the Japanese games are in it (the Super Famicom repository's names)");
	CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size(), "no name twice");
	bool clean = true;
	for (const std::string& n : names)
		clean = clean && !n.empty() && n.find('/') == std::string::npos && n.find('\t') == std::string::npos &&
			n.size() < 200 && (n.size() < 4 || n.compare(n.size() - 4, 4, ".png") != 0);
	CHECK(clean, "every name is a plain file name without the extension");

	CHECK(BezelUrl("Joe & Mac (USA)") ==
			"https://raw.githubusercontent.com/thebezelproject/bezelproject-SNES/master/retroarch/overlay/GameBezels/SNES/"
			"Joe%20%26%20Mac%20%28USA%29.png",
		"the address is the repository's raw file, every odd character escaped");
	CHECK(BezelUrl("Super Mario World 2 - Yoshi's Island (USA)").find("Yoshi%27s%20Island") != std::string::npos, "an apostrophe is escaped");
	CHECK(BezelUrl("Star Ocean (Japan)") ==
			"https://raw.githubusercontent.com/thebezelproject/bezelproject-SFC/master/retroarch/overlay/GameBezels/SFC/"
			"Star%20Ocean%20%28Japan%29.png",
		"a Japanese game's border is in The Bezel Project's Super Famicom repository");
	CHECK(BezelUrl("Actraiser 2 (Europe) (En,Fr,De)").find("/bezelproject-SFC/") != std::string::npos &&
			BezelUrl("Super Mario World (USA)").find("/bezelproject-SNES/") != std::string::npos,
		"each name is downloaded from the repository whose list it is in");
	setenv("SNES9X_BORDER_URL", "http://127.0.0.1:1/b/", 1);
	CHECK(BezelUrl("A (USA)") == "http://127.0.0.1:1/b/A%20%28USA%29.png", "SNES9X_BORDER_URL replaces the base (the host tests)");
	unsetenv("SNES9X_BORDER_URL");

	WantedCover w;
	CHECK(BorderWanted(pack, Game("Super Mario World (USA)", "Super Mario World (USA)"), &w) && w.file == "Super Mario World (USA).png" &&
			w.url == BezelUrl("Super Mario World (USA)"),
		"a game The Bezel Project has a border for, and the pack lacks, is wanted");
	CHECK(!BorderWanted(pack, Game("Not In Any Pack (USA)", "Not In Any Pack (USA)"), &w), "a game it has none for is not");
	CHECK(BorderWanted(pack, Game("Super Mario World", "Super Mario World"), &w) == false, "(the stand-in matcher knows only exact names)");
	CHECK(BorderWanted(pack, Game("Star Ocean (Japan)", "Star Ocean (Japan)"), &w) && w.file == "Star Ocean (Japan).png" &&
			w.url.find("bezelproject-SFC") != std::string::npos,
		"a Japanese game is wanted from the Super Famicom repository");

	// the loose translation: matched through the name its CRC has
	fe::GameInfo loose = Game("Loose Translation", "Loose Translation");
	loose.crc_name = "Super Mario World (USA)";
	CHECK(BezelNameFor(loose) == "Super Mario World (USA)", "a translation is given its game's border by the CRC name");

	WriteBytes(games + "Super Mario World (USA).png", ArtPng(kWinA));
	CHECK(!BorderWanted(pack, Game("Super Mario World (USA)", "Super Mario World (USA)"), &w), "a border already in the pack is not wanted again");
	unlink((games + "Super Mario World (USA).png").c_str());

	// a 404 is not asked again for 30 days, and only for the same address
	const std::string marker = games + "Super Mario World (USA).missing";
	if (FILE* f = fopen(marker.c_str(), "w"))
	{
		fprintf(f, "%s\n", BezelUrl("Super Mario World (USA)").c_str());
		fclose(f);
	}
	CHECK(!BorderWanted(pack, Game("Super Mario World (USA)", "Super Mario World (USA)"), &w), "a recent 404 for the address: not wanted");
	setenv("SNES9X_BORDER_URL", "http://127.0.0.1:1/b/", 1);
	CHECK(BorderWanted(pack, Game("Super Mario World (USA)", "Super Mario World (USA)"), &w), "a 404 for another address does not count");
	unsetenv("SNES9X_BORDER_URL");
	struct timeval old[2] = {{time(nullptr) - 31L * 24 * 3600, 0}, {time(nullptr) - 31L * 24 * 3600, 0}};
	utimes(marker.c_str(), old);
	CHECK(BorderWanted(pack, Game("Super Mario World (USA)", "Super Mario World (USA)"), &w), "a 404 older than 30 days is tried again");
	unlink(marker.c_str());

	// the library: one picture per bezel, none for a ROM with its own border, none for a game without one
	WriteBytes(games + "Chrono Trigger (USA).png", ArtPng(kWinA)); // the player's own border for this ROM name
	std::vector<fe::GameInfo> library = {Game("Super Mario World (USA)", "Super Mario World (USA)"), Game("Super Mario World (USA)", "Super Mario World (USA)"), Game("Chrono Trigger (USA)", "Chrono Trigger (USA)"),
		Game("F-Zero (USA)", "F-Zero (USA)"), Game("Not In Any Pack (USA)", "Not In Any Pack (USA)"), loose};
	const std::vector<WantedCover> missing = MissingBorders(pack, library);
	CHECK(missing.size() == 2 && missing[0].file == "Super Mario World (USA).png" && missing[1].file == "F-Zero (USA).png",
		"the library list has Super Mario World (once, though two ROMs and a translation share it) and F-Zero");

	WriteWantedBorders(pack, missing);
	WritePriorityBorders(pack, {missing[1]});
	const std::vector<WantedCover> read = fe::ParseWantedList(ReadText(pack + "/wanted.txt"));
	CHECK(read.size() == 2 && read[0].file == missing[0].file && read[0].url == missing[0].url, "borders/wanted.txt is the helper's wanted-list format");
	const std::vector<WantedCover> prio = fe::ParseWantedList(ReadText(pack + "/priority.txt"));
	CHECK(prio.size() == 1 && prio[0].file == "F-Zero (USA).png", "borders/priority.txt holds the one given");
	WriteWantedBorders(pack, {});
	CHECK(fe::ParseWantedList(ReadText(pack + "/wanted.txt")).empty(), "and an empty list empties the file");
}

void TestAutoDownload()
{
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/games/";
	const std::string rom = "/data/snes9x/roms/Super Mario World (USA).sfc";
	ClearPack(pack);
	unlink((pack + "/priority.txt").c_str());
	fe::Config().border = 1;
	fe::Config().border_download = true;

	// Auto, no border for the game yet: the built-in frame at once, the helper asked, the border put up when it lands
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, 0);
	CHECK(ps5video::g_set_calls == 1 && ps5video::g_shown && ps5video::g_shown->window.w >= 480, "no border yet: the game starts with the built-in frame");
	const std::vector<fe::WantedCover> asked = fe::ParseWantedList(ReadText(pack + "/priority.txt"));
	CHECK(asked.size() == 1 && asked[0].file == "Super Mario World (USA).png", "and the helper is asked for it, first (borders/priority.txt)");
	usleep(700 * 1000);
	CHECK(ps5video::g_set_calls == 1, "nothing changes while the picture has not arrived");
	Land(games + "Super Mario World (USA).png", ArtPng(kWinD));
	CHECK(WaitForCalls(2), "the picture lands: the game's border is put up");
	CHECK(ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinD), "it is the downloaded picture");
	fe::border::Release();
	CHECK(!ps5video::g_shown, "Release clears it");

	// the next time the game starts, the file is in the pack: found at once, nothing asked
	unlink((pack + "/priority.txt").c_str());
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, 0);
	CHECK(ps5video::g_set_calls == 1 && SameRect(ps5video::g_shown->window, kWinD), "the next start finds the downloaded border in the pack");
	CHECK(!Exists(pack + "/priority.txt"), "and asks for nothing");
	fe::border::Release();

	// the game's name is not the bezel's: the file is found by the index name, not by the pack's own matcher
	ps5video::g_set_calls = 0;
	fe::border::Select("/data/snes9x/roms/Loose Translation.sfc", 0);
	CHECK(ps5video::g_set_calls == 1 && SameRect(ps5video::g_shown->window, kWinD), "a translation finds the downloaded border by The Bezel Project's name for its game");
	fe::border::Release();

	// the game closes before the picture arrives: it is not put up afterwards, and Release does not wait for the helper
	unlink((games + "Super Mario World (USA).png").c_str());
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, 0);
	const auto t0 = std::chrono::steady_clock::now();
	fe::border::Release();
	const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
	CHECK(ms < 400, "Release stops the watcher at once");
	const int calls = ps5video::g_set_calls;
	Land(games + "Super Mario World (USA).png", ArtPng(kWinB));
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls, "a picture that arrives after the game closed is not put up");

	// another game starts before it arrives
	unlink((games + "Super Mario World (USA).png").c_str());
	fe::border::Select(rom, 0);
	fe::border::Select("/data/snes9x/roms/Unknown Game.sfc", 0);
	const int calls2 = ps5video::g_set_calls;
	Land(games + "Super Mario World (USA).png", ArtPng(kWinB));
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls2, "nor when another game has started meanwhile");
	fe::border::Release();

	// the setting changes while it waits
	unlink((games + "Super Mario World (USA).png").c_str());
	fe::border::Select(rom, 0);
	fe::Config().border = 2;
	fe::border::Reselect();
	const int calls3 = ps5video::g_set_calls;
	Land(games + "Super Mario World (USA).png", ArtPng(kWinB));
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls3, "nor after Auto was switched to Default frame");
	fe::border::Release();

	// a 404: the watcher gives up, the frame stays
	fe::Config().border = 1;
	unlink((games + "Super Mario World (USA).png").c_str());
	fe::border::Select(rom, 0);
	const int calls4 = ps5video::g_set_calls;
	if (FILE* f = fopen((games + "Super Mario World (USA).missing").c_str(), "w"))
	{
		fprintf(f, "%s\n", fe::border::BezelUrl("Super Mario World (USA)").c_str());
		fclose(f);
	}
	usleep(1200 * 1000);
	CHECK(ps5video::g_set_calls == calls4, "a 404 leaves the built-in frame");
	fe::border::Release();
	unlink((games + "Super Mario World (USA).missing").c_str());

	// downloads off: nothing asked, nothing watched
	fe::Config().border_download = false;
	unlink((pack + "/priority.txt").c_str());
	fe::border::Select(rom, 0);
	CHECK(!Exists(pack + "/priority.txt"), "border_download=0: the helper is not asked");
	fe::border::Release();
	fe::Config().border_download = true;

	// Default frame never asks, a game with its own file never asks
	fe::Config().border = 2;
	fe::border::Select(rom, 0);
	CHECK(!Exists(pack + "/priority.txt"), "Default frame: the helper is not asked");
	fe::border::Release();
	fe::Config().border = 1;
	WriteBytes(games + "Super Mario World (USA).ini", std::vector<uint8_t>{'o', 'f', 'f', '=', '1', '\n'});
	fe::border::Select(rom, 0);
	CHECK(!Exists(pack + "/priority.txt"), "off=1 in the game's .ini: the helper is not asked");
	fe::border::Release();
}
// ---- the aspect ratio setting reaches the border ----
void TestAspect()
{
	using ps5video::Aspect;
	const std::string pack = g_root + "/borders";
	const std::string games = pack + "/games/";
	const std::string rom = "/data/snes9x/roms/Super Mario World (USA).sfc";
	const fe::GameInfo game = Game("Super Mario World (USA)", "Super Mario World (USA)");
	ClearPack(pack);
	unlink((pack + "/priority.txt").c_str());
	fe::Config().border_download = false;
	fe::Config().border = 1;

	// the choice: a window of the shape of each aspect ratio, from the same picture (kWinB is 3:2)
	WriteBytes(games + "Super Mario World (USA).png", ArtPng(kWinB));
	fe::border::Pick pick = fe::border::Choose(pack, 1, game, Aspect::Ratio4x3);
	CHECK(pick.border && pick.source == "own file" && Shaped(pick.border->window, 4, 3), "4:3: the game's own border gets a 4:3 window");
	pick = fe::border::Choose(pack, 1, game, Aspect::PixelPerfect);
	CHECK(pick.border && Shaped(pick.border->window, 8, 7), "8:7: an 8:7 window");
	pick = fe::border::Choose(pack, 1, game, Aspect::Stretch);
	CHECK(pick.border && Shaped(pick.border->window, 16, 9), "Stretch: a 16:9 window");
	pick = fe::border::Choose(pack, 1, game, Aspect::Integer);
	CHECK(pick.border && SameRect(pick.border->window, fe::border::IntegerOpening(kWinB)) && pick.border->window.w == 1024 && pick.border->window.h == 896,
		"Integer scale: the window is the 1024x896 picture, no black around it");
	pick = fe::border::Choose(pack, 1, game);
	CHECK(pick.border && SameRect(pick.border->window, kWinB), "and the window as drawn is what no aspect ratio asked for gives");

	// the built-in frame, which is 1398x1038 (4:3) as drawn
	ClearPack(pack);
	const Rect frame = fe::border::Choose(pack, 2, game, Aspect::Count).border->window;
	CHECK(SameRect(fe::border::Choose(pack, 2, game, Aspect::Ratio4x3).border->window, frame), "the built-in frame is already 4:3: unchanged");
	const Rect frame87 = fe::border::Choose(pack, 2, game, Aspect::PixelPerfect).border->window;
	const Rect frame169 = fe::border::Choose(pack, 2, game, Aspect::Stretch).border->window;
	CHECK(SameRect(frame87, fe::border::FitOpening(frame, 8, 7)) && Shaped(frame87, 8, 7), "the built-in frame: an 8:7 window");
	CHECK(SameRect(frame169, fe::border::FitOpening(frame, 16, 9)) && Shaped(frame169, 16, 9) && frame169.w > frame.w, "and a wider 16:9 one");

	// Reselect: the aspect ratio changes while a game runs
	WriteBytes(games + "Super Mario World (USA).png", ArtPng(kWinB));
	fe::Config().aspect = int(Aspect::Count);
	fe::border::Select(rom, 0);
	CHECK(ps5video::g_shown && SameRect(ps5video::g_shown->window, kWinB), "no aspect ratio: the picture as drawn");
	int calls = ps5video::g_set_calls;
	fe::Config().aspect = int(Aspect::PixelPerfect);
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == calls + 1 && Shaped(ps5video::g_shown->window, 8, 7), "Aspect ratio -> 8:7 while a game runs: the border is fitted again");
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == calls + 1, "and nothing more while the aspect ratio stays");
	fe::Config().aspect = int(Aspect::Stretch);
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == calls + 2 && Shaped(ps5video::g_shown->window, 16, 9), "-> Stretch: a 16:9 window");
	fe::Config().border = 0;
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == calls + 3 && !ps5video::g_shown, "Border Off clears it");
	fe::Config().aspect = int(Aspect::Integer);
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == calls + 3, "and with Border Off the aspect ratio has nothing to fit");
	fe::Config().border = 1;
	fe::border::Reselect();
	CHECK(ps5video::g_set_calls == calls + 4 && SameRect(ps5video::g_shown->window, fe::border::IntegerOpening(kWinB)), "Border back to Auto: the picture, fitted to the aspect ratio set meanwhile (Integer scale)");
	fe::border::Release();

	// a border that is downloaded while the game runs is fitted to the aspect ratio of the moment
	unlink((games + "Super Mario World (USA).png").c_str());
	unlink((pack + "/priority.txt").c_str());
	fe::Config().border_download = true;
	fe::Config().aspect = int(Aspect::Stretch);
	ps5video::g_set_calls = 0;
	fe::border::Select(rom, 0);
	CHECK(ps5video::g_set_calls == 1 && Shaped(ps5video::g_shown->window, 16, 9), "the frame it starts with is fitted");
	Land(games + "Super Mario World (USA).png", ArtPng(kWinD));
	CHECK(WaitForCalls(2) && Shaped(ps5video::g_shown->window, 16, 9) && ps5video::g_shown->window.h == kWinD.h, "and so is the downloaded border (kWinD is 10:7)");
	fe::border::Release();
	fe::Config().border_download = true;
	fe::Config().aspect = int(Aspect::Count);
}

} // namespace

int main()
{
	char dir[] = "/tmp/border-pack-test-XXXXXX";
	if (!mkdtemp(dir))
	{
		perror("mkdtemp");
		return 2;
	}
	g_root = dir;
	setenv("SNES9X_PS5_ROOT", dir, 1);
	MakePack(g_root);
	OrbisLogSetEnabled(true);
	fe::Config().aspect = int(ps5video::Aspect::Count); // the art as drawn: the tests below compare windows (TestAspect fits them)

	TestDecode();
	TestWindow();
	TestBuild();
	TestFit();
	TestChoose();
	TestSelect();
	TestFetch();
	TestAutoDownload();
	TestAspect();

	if (FILE* p = popen(("rm -rf '" + g_root + "'").c_str(), "r"))
		pclose(p);
	printf("%d checks, %d failed\n", g_checked, g_failed);
	return g_failed ? 1 : 0;
}

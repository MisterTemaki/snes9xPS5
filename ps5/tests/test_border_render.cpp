// The border in the renderer (ProsperoVideo.cpp): where the picture goes in a window, when the backdrop is restored, what a
// frame reports to Present, and the rim blend. Runs the real renderer against the host's stub video output.
// SPDX-License-Identifier: MIT

#include "ProsperoVideo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace ps5video;

static int g_fail = 0;
#define CHECK(c)                                                          \
	do                                                                    \
	{                                                                     \
		if (!(c))                                                         \
		{                                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
			g_fail++;                                                     \
		}                                                                 \
	} while (0)

static bool Same(const Rect& a, const Rect& b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
#define CHECK_RECT(got, ex_x, ex_y, ex_w, ex_h)                                                        \
	do                                                                                                 \
	{                                                                                                  \
		const Rect _g = (got);                                                                         \
		if (!Same(_g, Rect{ex_x, ex_y, ex_w, ex_h}))                                                   \
		{                                                                                              \
			printf("FAIL %s:%d  %s = %d,%d %dx%d, expected %d,%d %dx%d\n", __FILE__, __LINE__, #got,   \
				_g.x, _g.y, _g.w, _g.h, ex_x, ex_y, ex_w, ex_h);                                       \
			g_fail++;                                                                                  \
		}                                                                                              \
	} while (0)

static const Rect kScreen = {0, 0, kWidth, kHeight};

// ComputeRect as it was before borders existed (the whole surface), kept here as the reference.
static Rect OldComputeRect(int src_w, int src_h, Aspect aspect)
{
	const int base_w = src_w > 256 ? src_w / 2 : src_w;
	const int lines = src_h > 256 ? src_h / 2 : src_h;
	int dw = kWidth, dh = kHeight;
	switch (aspect)
	{
		case Aspect::Ratio4x3:
			dh = kHeight;
			dw = kHeight * 4 / 3;
			break;
		case Aspect::PixelPerfect:
			dh = kHeight;
			dw = (kHeight * base_w + lines / 2) / lines;
			break;
		case Aspect::Integer:
		{
			const int k = std::max(1, std::min(kWidth / base_w, kHeight / lines));
			dw = base_w * k;
			dh = lines * k;
			break;
		}
		default:
			break;
	}
	dw = std::min(dw, int(kWidth)) & ~1;
	dh = std::min(dh, int(kHeight));
	return Rect{(kWidth - dw) / 2, (kHeight - dh) / 2, dw, dh};
}

static void TestComputeRect()
{
	// no border: the same numbers as before, for every picture size the core makes
	const int sizes[][2] = {{256, 224}, {256, 239}, {512, 224}, {512, 448}, {512, 478}, {256, 192}, {512, 239}};
	for (const auto& sz : sizes)
		for (int a = 0; a < int(Aspect::Count); a++)
		{
			const Rect now = ComputeRect(sz[0], sz[1], Aspect(a), kScreen), old = OldComputeRect(sz[0], sz[1], Aspect(a));
			if (!Same(now, old))
				printf("FAIL %dx%d aspect %d: %d,%d %dx%d, was %d,%d %dx%d\n", sz[0], sz[1], a, now.x, now.y, now.w, now.h,
					old.x, old.y, old.w, old.h);
			CHECK(Same(now, old));
		}
	CHECK_RECT(ComputeRect(256, 224, Aspect::Ratio4x3, kScreen), 240, 0, 1440, 1080);
	CHECK_RECT(ComputeRect(256, 224, Aspect::PixelPerfect, kScreen), 343, 0, 1234, 1080);
	CHECK_RECT(ComputeRect(256, 224, Aspect::Integer, kScreen), 448, 92, 1024, 896);
	CHECK_RECT(ComputeRect(256, 224, Aspect::Stretch, kScreen), 0, 0, 1920, 1080);

	// the window of the Bezel Project's SNES art (1398x1038 at 261,21): 1.347 is within 3% of 4:3, so 4:3 fills it
	const Rect win = {261, 21, 1398, 1038};
	CHECK_RECT(ComputeRect(256, 224, Aspect::Ratio4x3, win), 261, 21, 1398, 1038);
	CHECK_RECT(ComputeRect(256, 224, Aspect::Stretch, win), 261, 21, 1398, 1038);
	CHECK_RECT(ComputeRect(256, 190, Aspect::PixelPerfect, win), 261, 21, 1398, 1038); // 256:190 = 1.347: fills too
	CHECK_RECT(ComputeRect(256, 224, Aspect::PixelPerfect, win), 367, 21, 1186, 1038); // 8:7 is narrower: centred
	CHECK_RECT(ComputeRect(256, 224, Aspect::Integer, win), 448, 92, 1024, 896); // 4x, centred in the window
	CHECK_RECT(ComputeRect(512, 448, Aspect::Integer, win), 448, 92, 1024, 896);

	// the window Integer scale cuts the art to (IntegerOpening in fe_borderimg.cpp) is exactly 4x: the picture fills it, and a
	// 239-line picture, which has no whole multiple there (3x would leave black around it), fills it like 8:7; on the bare
	// screen it is still the whole multiple
	const Rect win4x = {448, 92, 1024, 896};
	CHECK_RECT(ComputeRect(256, 224, Aspect::Integer, win4x), 448, 92, 1024, 896);
	CHECK_RECT(ComputeRect(512, 448, Aspect::Integer, win4x), 448, 92, 1024, 896);
	CHECK_RECT(ComputeRect(256, 239, Aspect::Integer, win4x), 480, 92, 960, 896);
	CHECK_RECT(ComputeRect(512, 478, Aspect::Integer, win4x), 480, 92, 960, 896);
	CHECK_RECT(ComputeRect(256, 239, Aspect::Integer, kScreen), 448, 62, 1024, 956);
	CHECK_RECT(ComputeRect(256, 224, Aspect::Integer, Rect{100, 100, 768, 672}), 100, 100, 768, 672); // 3x window

	// the 3% edge of the fill rule: 1480x1080 is 2.8% off 4:3 and fills, 1490x1080 is 3.5% off and is fitted by height
	CHECK_RECT(ComputeRect(256, 224, Aspect::Ratio4x3, Rect{0, 0, 1480, 1080}), 0, 0, 1480, 1080);
	CHECK_RECT(ComputeRect(256, 224, Aspect::Ratio4x3, Rect{0, 0, 1490, 1080}), 25, 0, 1440, 1080);
	// a window narrower than 4:3: fitted by width
	CHECK_RECT(ComputeRect(256, 224, Aspect::Ratio4x3, Rect{100, 50, 1000, 1000}), 100, 175, 1000, 750);
	// a window smaller than the picture: Integer has no whole multiple that fits and uses the 4:3 rule; an odd width
	// loses a pixel
	CHECK_RECT(ComputeRect(256, 239, Aspect::Integer, Rect{100, 100, 320, 180}), 140, 100, 240, 180);
	CHECK_RECT(ComputeRect(256, 224, Aspect::Stretch, Rect{0, 0, 1001, 1080}), 0, 0, 1000, 1080);

	// the smallest window a border may have (480x270, the pack's limit): every aspect and picture size stays inside it
	const Rect windows[] = {{0, 0, 480, 270}, {1440, 810, 480, 270}, {700, 300, 480, 270}, {261, 21, 1398, 1038}};
	for (const Rect& w : windows)
		for (const auto& sz : sizes)
			for (int a = 0; a < int(Aspect::Count); a++)
			{
				const Rect r = ComputeRect(sz[0], sz[1], Aspect(a), w);
				const bool inside = r.w > 0 && r.h > 0 && r.w % 2 == 0 && r.x >= w.x && r.y >= w.y &&
									r.x + r.w <= w.x + w.w && r.y + r.h <= w.y + w.h;
				if (!inside)
					printf("FAIL %dx%d aspect %d in %d,%d %dx%d: %d,%d %dx%d\n", sz[0], sz[1], a, w.x, w.y, w.w, w.h, r.x, r.y,
						r.w, r.h);
				CHECK(inside);
			}
	CHECK_RECT(ComputeRect(256, 239, Aspect::Integer, Rect{0, 0, 480, 270}), 112, 15, 256, 239); // 1x fits
}

// ---- the renderer against a border ------------------------------------------------------------------------------

static const uint32_t kBorderColor = Rgb(10, 20, 200);
static const Rect kWindow = {261, 21, 1398, 1038};

static uint32_t Px(int x, int y)
{
	return Surface()[size_t(y) * kWidth + x];
}

static uint32_t Expand565(uint16_t c) // what DrawSnes makes of an RGB565 pixel
{
	const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
	return Rgb(uint8_t((r5 << 3) | (r5 >> 2)), uint8_t((g6 << 2) | (g6 >> 4)), uint8_t((b5 << 3) | (b5 >> 2)));
}

// The border colour outside the window, black inside it; 'rim' pixels (index order) sit in the window and are on black in
// the backdrop, as fe_border builds it.
static std::shared_ptr<const Border> MakeBorder(uint32_t color, const Rect& window, const std::vector<RimPixel>& rim)
{
	auto b = std::make_shared<Border>();
	b->backdrop.assign(size_t(kWidth) * kHeight, color);
	for (int y = window.y; y < window.y + window.h; y++)
		std::fill(b->backdrop.begin() + size_t(y) * kWidth + window.x,
			b->backdrop.begin() + size_t(y) * kWidth + window.x + window.w, 0xff000000u);
	b->window = window;
	b->rim = rim;
	for (const RimPixel& p : rim)
	{
		// the pixel on black: colour * alpha / 255, which is `pre`
		b->backdrop[p.index] = 0xff000000u | p.pre;
	}
	return b;
}

static RimPixel Rim(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	const uint32_t pr = (uint32_t(r) * a + 127) / 255, pg = (uint32_t(g) * a + 127) / 255, pb = (uint32_t(b) * a + 127) / 255;
	return RimPixel{uint32_t(y) * kWidth + x, (pb << 16) | (pg << 8) | pr, uint32_t(255 - a)};
}

// the blend the renderer has to make, in floating point
static uint32_t Exact(uint32_t pic, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	const double k = a / 255.0;
	auto ch = [&](uint32_t p, int shift, uint8_t c) { return uint32_t(std::lround(((p >> shift) & 0xff) * (1 - k) + c * k)); };
	return 0xff000000u | (ch(pic, 16, b) << 16) | (ch(pic, 8, g) << 8) | ch(pic, 0, r);
}

// the blend as the renderer does it, in integers
static uint32_t Formula(uint32_t pic, const RimPixel& p)
{
	auto ch = [&](int shift) { return ((((pic >> shift) & 0xff) * p.inv + 127) / 255 + ((p.pre >> shift) & 0xff)); };
	return 0xff000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

static bool Near(uint32_t a, uint32_t b)
{
	for (int shift = 0; shift < 24; shift += 8)
		if (std::abs(int((a >> shift) & 0xff) - int((b >> shift) & 0xff)) > 1)
			return false;
	return (a >> 24) == (b >> 24);
}

static Rect Draw(const std::vector<uint16_t>& src, int w, int h, Aspect aspect, int shader = 0)
{
	return DrawSnes(src.data(), w * 2, w, h, aspect, false, shader);
}

static void TestBorderFrames()
{
	const uint16_t kPic = 0x7bef;
	const uint32_t pic = Expand565(kPic);
	const std::vector<uint16_t> f224(256 * 224, kPic), f224wide(512 * 224, kPic), f239(256 * 239, kPic);
	auto border = MakeBorder(kBorderColor, kWindow, {});

	SetBorder(border);
	InvalidateSnes();
	// first frame: the backdrop, then the picture in the window (Integer: 4x, centred, with window around it)
	Rect r = Draw(f224, 256, 224, Aspect::Integer);
	CHECK_RECT(r, 0, 0, kWidth, kHeight); // everything changed
	const Rect picture = {448, 92, 1024, 896};
	CHECK(Same(LastSnesRect(), picture));
	CHECK(Px(100, 100) == kBorderColor); // outside the window
	CHECK(Px(300, 300) == 0xff000000u); // in the window, beside the picture
	CHECK(Px(448, 92) == pic && Px(1471, 987) == pic && Px(960, 540) == pic);
	CHECK(Px(447, 92) == 0xff000000u && Px(448, 91) == 0xff000000u && Px(1472, 987) == 0xff000000u);

	// a normal frame changes nothing but the picture: that is what Present is given
	r = Draw(f224, 256, 224, Aspect::Integer);
	CHECK_RECT(r, picture.x, picture.y, picture.w, picture.h);

	// a new source size that keeps the rectangle (512 wide hi-res draws the same area) leaves the surface alone
	Surface()[100 * kWidth + 100] = 0xffabcdefu;
	r = Draw(f224wide, 512, 224, Aspect::Integer);
	CHECK_RECT(r, picture.x, picture.y, picture.w, picture.h);
	CHECK(Px(100, 100) == 0xffabcdefu); // the backdrop was not copied again
	CHECK(Px(960, 540) == pic);

	// a new rectangle copies the backdrop again and reports the whole surface
	r = Draw(f239, 256, 239, Aspect::Integer);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Px(100, 100) == kBorderColor);
	CHECK(Same(LastSnesRect(), ComputeRect(256, 239, Aspect::Integer, kWindow)));
	r = Draw(f239, 256, 239, Aspect::Integer);
	CHECK(Same(r, LastSnesRect()));

	// the app clears the surface around menus and calls InvalidateSnes(): the border comes back with the picture
	FillRect(0, 0, kWidth, kHeight, Rgb(0, 0, 0));
	InvalidateSnes();
	r = Draw(f239, 256, 239, Aspect::Integer);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Px(100, 100) == kBorderColor && Px(960, 540) == pic);

	// another aspect: new rectangle, whole surface; 4:3 fills this window
	r = Draw(f224, 256, 224, Aspect::Ratio4x3);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Same(LastSnesRect(), kWindow));
	CHECK(Px(261, 21) == pic && Px(1658, 1058) == pic && Px(260, 21) == kBorderColor && Px(1659, 540) == kBorderColor);

	// another border: repainted even though the rectangle is the same
	auto other = MakeBorder(Rgb(200, 10, 10), kWindow, {});
	SetBorder(other);
	r = Draw(f224, 256, 224, Aspect::Ratio4x3);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK(Px(100, 100) == Rgb(200, 10, 10));

	// no border: today's black and today's rectangle
	SetBorder(nullptr);
	r = Draw(f224, 256, 224, Aspect::Ratio4x3);
	CHECK_RECT(r, 0, 0, kWidth, kHeight);
	CHECK_RECT(LastSnesRect(), 240, 0, 1440, 1080);
	CHECK(Px(100, 100) == 0xff000000u && Px(300, 300) == pic && Px(239, 540) == 0xff000000u && Px(240, 0) == pic);
	r = Draw(f224, 256, 224, Aspect::Ratio4x3);
	CHECK_RECT(r, 240, 0, 1440, 1080);

	// a border that does not fit the surface is ignored: no border, no crash
	auto bad = std::make_shared<Border>();
	bad->backdrop.assign(1000, kBorderColor);
	bad->window = kWindow;
	SetBorder(bad);
	r = Draw(f224, 256, 224, Aspect::Ratio4x3);
	CHECK_RECT(LastSnesRect(), 240, 0, 1440, 1080);
	CHECK(Px(100, 100) == 0xff000000u);
	auto bad_window = std::make_shared<Border>(); // a window that runs off the surface
	bad_window->backdrop.assign(size_t(kWidth) * kHeight, kBorderColor);
	bad_window->window = Rect{1000, 500, 1000, 700};
	SetBorder(bad_window);
	r = Draw(f224, 256, 224, Aspect::Ratio4x3);
	CHECK_RECT(LastSnesRect(), 240, 0, 1440, 1080);
	SetBorder(nullptr);
}

static void TestRim()
{
	const uint16_t kPic = 0x7bef;
	const uint32_t pic = Expand565(kPic);
	const std::vector<uint16_t> f224(256 * 224, kPic);
	const Rect picture = {448, 92, 1024, 896}; // Integer, 4x

	// in index order: the picture's top left (opaque, a rounded corner), a soft pixel inside, one beside the picture in
	// the window, one at the bottom right of the picture, one on its right edge column
	std::vector<RimPixel> rim = {
		Rim(448, 92, 9, 8, 7, 255),
		Rim(500, 100, 200, 50, 100, 128),
		Rim(1471, 500, 255, 255, 255, 1),
		Rim(1471, 987, 0, 0, 0, 200),
		Rim(300, 300, 200, 50, 100, 128),
	};
	std::sort(rim.begin(), rim.end(), [](const RimPixel& a, const RimPixel& b) { return a.index < b.index; });
	auto border = MakeBorder(kBorderColor, kWindow, rim);
	SetBorder(border);
	InvalidateSnes();
	Draw(f224, 256, 224, Aspect::Integer);
	CHECK(Same(LastSnesRect(), picture));

	CHECK(Px(448, 92) == Rgb(9, 8, 7)); // alpha 255: the border's own colour
	CHECK(Px(500, 100) == Formula(pic, Rim(500, 100, 200, 50, 100, 128))); // the integer formula
	CHECK(Near(Px(500, 100), Exact(pic, 200, 50, 100, 128)));
	CHECK(Near(Px(1471, 500), Exact(pic, 255, 255, 255, 1)));
	CHECK(Near(Px(1471, 987), Exact(pic, 0, 0, 0, 200)));
	CHECK(Px(300, 300) == border->backdrop[300 * kWidth + 300]); // outside the picture: only the backdrop
	CHECK(Px(300, 300) != 0xff000000u);
	CHECK(Px(449, 92) == pic && Px(448, 93) == pic && Px(1470, 987) == pic); // untouched neighbours

	// a second frame draws the picture again and blends the rim over the new pixels, not over the first blend
	Draw(f224, 256, 224, Aspect::Integer);
	CHECK(Px(448, 92) == Rgb(9, 8, 7));
	CHECK(Near(Px(500, 100), Exact(pic, 200, 50, 100, 128)));
	const uint16_t kOther = 0x07e0;
	const std::vector<uint16_t> green(256 * 224, kOther);
	Draw(green, 256, 224, Aspect::Integer);
	CHECK(Near(Px(500, 100), Exact(Expand565(kOther), 200, 50, 100, 128)));

	// the rim over a CRT shader's picture: the shader draws first, the rim is blended after it
	std::vector<RimPixel> one = {Rim(700, 300, 255, 0, 0, 100)};
	SetBorder(MakeBorder(kBorderColor, kWindow, {}));
	Draw(f224, 256, 224, Aspect::Integer, 1);
	const uint32_t shaded = Px(700, 300);
	SetBorder(MakeBorder(kBorderColor, kWindow, one));
	Draw(f224, 256, 224, Aspect::Integer, 1);
	CHECK(Near(Px(700, 300), Exact(shaded, 255, 0, 0, 100)));
	CHECK(Px(700, 300) != shaded);
	CHECK(Px(100, 100) == kBorderColor);
	SetBorder(nullptr);
}

int main()
{
	TestComputeRect();
	if (!Init())
	{
		printf("FAIL the host video output did not start\n");
		return 1;
	}
	TestBorderFrames();
	TestRim();
	Shutdown();
	if (g_fail)
		return 1;
	printf("border render: all checks passed\n");
	return 0;
}

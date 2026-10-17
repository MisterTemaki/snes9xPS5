// Snes9x PS5 frontend: a border picture (fe_borderimg.h).
// SPDX-License-Identifier: MIT

#include "fe_borderimg.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

// its own copy of stb_image, as fe_covers.cpp has: static, so the two never meet
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// a border is 1920x1080: a bigger image (a broken or hostile file) is refused before stb_image allocates for it
#define STBI_MAX_DIMENSIONS 8192
#include "third_party/stb_image.h"

namespace fe::border
{
namespace
{
using ps5video::kHeight;
using ps5video::kWidth;

void Fail(std::string* why, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Fail(std::string* why, const char* fmt, ...)
{
	char text[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (why)
		*why = text;
}

// c * a / 255, rounded
inline uint32_t Mul255(uint32_t c, uint32_t a)
{
	return (c * a + 127) / 255;
}

bool SameRect(const ps5video::Rect& a, const ps5video::Rect& b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Where one output row or column reads from: the pixels i0 and i1 of the source, i1 with weight f / 256.
struct Tap
{
	int i0, i1, f;
};

// One axis of RefitImage. 'src' and 'dst' are the cut points of the five pieces (margin, corner, middle, corner, margin);
// piece k of the output shows piece k of the source, 1:1 or stretched, and never reads outside its piece.
void AxisTaps(const int (&src)[6], const int (&dst)[6], int size, std::vector<Tap>* taps)
{
	taps->resize(size_t(size));
	for (int k = 0; k < 5; k++)
	{
		const int64_t len_s = src[k + 1] - src[k], len_d = dst[k + 1] - dst[k];
		const int last = src[k + 1] - 1;
		for (int d = dst[k]; d < dst[k + 1]; d++)
		{
			Tap& t = (*taps)[size_t(d)];
			if (len_s <= 0) // nothing to show here (a window that touches the edge): the nearest pixel
			{
				const int i = std::min(std::max(src[k], 0), size - 1);
				t = {i, i, 0};
				continue;
			}
			// the output pixel's centre in the source, minus the half pixel taps are counted from, scaled by 2 * len_d
			const int64_t pos = (2 * int64_t(d - dst[k]) + 1) * len_s - len_d;
			const int64_t den = 2 * len_d;
			const int i0 = pos <= 0 ? src[k] : src[k] + int(pos / den);
			if (pos <= 0 || i0 >= last)
				t = {std::min(i0, last), std::min(i0, last), 0};
			else
				t = {i0, i0 + 1, int((pos % den) * 256 / den)};
		}
	}
}

// Four pixels (0xAABBGGRR, straight alpha) mixed with weights that add up to 65536; the colours count by their alpha.
uint32_t Bilerp(const uint32_t (&p)[4], int fx, int fy)
{
	const uint32_t w[4] = {uint32_t((256 - fx) * (256 - fy)), uint32_t(fx * (256 - fy)), uint32_t((256 - fx) * fy), uint32_t(fx * fy)};
	if ((p[0] & p[1] & p[2] & p[3]) >> 24 == 0xff) // all opaque: a plain average
	{
		uint32_t out = 0xff000000u;
		for (int sh = 0; sh < 24; sh += 8)
		{
			uint32_t sum = 32768;
			for (int i = 0; i < 4; i++)
				sum += w[i] * ((p[i] >> sh) & 0xff);
			out |= (sum >> 16) << sh;
		}
		return out;
	}
	uint64_t a_sum = 0, c_sum[3] = {0, 0, 0};
	for (int i = 0; i < 4; i++)
	{
		const uint64_t wa = uint64_t(w[i]) * (p[i] >> 24);
		a_sum += wa;
		for (int c = 0; c < 3; c++)
			c_sum[c] += wa * ((p[i] >> (8 * c)) & 0xff);
	}
	if (a_sum == 0)
		return 0;
	uint32_t out = uint32_t((a_sum + 32768) >> 16) << 24;
	for (int c = 0; c < 3; c++)
		out |= uint32_t((c_sum[c] + a_sum / 2) / a_sum) << (8 * c);
	return out;
}
} // namespace

bool OpeningRatio(ps5video::Aspect aspect, int* rw, int* rh)
{
	switch (aspect)
	{
		case ps5video::Aspect::Ratio4x3:
			*rw = 4;
			*rh = 3;
			return true;
		case ps5video::Aspect::PixelPerfect:
			*rw = 8;
			*rh = 7;
			return true;
		case ps5video::Aspect::Stretch:
			*rw = 16;
			*rh = 9;
			return true;
		default:
			return false;
	}
}

ps5video::Rect FitOpening(const ps5video::Rect& win, int rw, int rh)
{
	const int64_t diff = std::llabs(int64_t(win.w) * rh - int64_t(win.h) * rw); // as the renderer's FitRatio
	if (diff * 100 <= 3 * int64_t(win.h) * rw)
		return win;
	int th = win.h;
	int tw = int((int64_t(th) * rw + rh / 2) / rh);
	const int max_w = kWidth - (kWidth - win.w) * kMinMarginPct / 100;
	if (tw > max_w)
	{
		tw = max_w;
		th = int((int64_t(tw) * rh + rw / 2) / rw);
	}
	tw &= ~1; // the renderer keeps the picture's width even
	if (tw < 2 || th < 1)
		return win;
	const int mw = kWidth - win.w, mh = kHeight - win.h; // the margins together keep the proportions they had
	const int nx = mw > 0 ? int((int64_t(win.x) * (kWidth - tw) + mw / 2) / mw) : (kWidth - tw) / 2;
	const int ny = mh > 0 ? int((int64_t(win.y) * (kHeight - th) + mh / 2) / mh) : (kHeight - th) / 2;
	return ps5video::Rect{nx, ny, tw, th};
}

bool RefitImage(Image* img, const ps5video::Rect& from, const ps5video::Rect& to, std::string* why)
{
	if (img->w != kWidth || img->h != kHeight || img->px.size() != size_t(kWidth) * kHeight)
	{
		Fail(why, "the picture is not 1920 x 1080");
		return false;
	}
	const int c = std::min({kOpeningCorner, from.w / 4, from.h / 4, to.w / 4, to.h / 4});
	const int sx[6] = {0, from.x, from.x + c, from.x + from.w - c, from.x + from.w, kWidth};
	const int dx[6] = {0, to.x, to.x + c, to.x + to.w - c, to.x + to.w, kWidth};
	const int sy[6] = {0, from.y, from.y + c, from.y + from.h - c, from.y + from.h, kHeight};
	const int dy[6] = {0, to.y, to.y + c, to.y + to.h - c, to.y + to.h, kHeight};
	std::vector<Tap> tx, ty;
	std::vector<uint32_t> out;
	try
	{
		AxisTaps(sx, dx, kWidth, &tx);
		AxisTaps(sy, dy, kHeight, &ty);
		out.resize(img->px.size());
	}
	catch (const std::bad_alloc&)
	{
		Fail(why, "out of memory");
		return false;
	}
	for (int y = 0; y < kHeight; y++)
	{
		const Tap& v = ty[size_t(y)];
		const uint32_t* r0 = &img->px[size_t(v.i0) * kWidth];
		const uint32_t* r1 = &img->px[size_t(v.i1) * kWidth];
		uint32_t* o = &out[size_t(y) * kWidth];
		for (int x = 0; x < kWidth; x++)
		{
			const Tap& h = tx[size_t(x)];
			if (h.f == 0 && v.f == 0)
			{
				o[x] = r0[h.i0];
				continue;
			}
			const uint32_t p[4] = {r0[h.i0], r0[h.i1], r1[h.i0], r1[h.i1]};
			o[x] = Bilerp(p, h.f, v.f);
		}
	}
	img->px.swap(out);
	return true;
}

bool FitToAspect(Image* img, ps5video::Rect* window, ps5video::Aspect aspect, std::string* why)
{
	int rw = 0, rh = 0;
	if (!OpeningRatio(aspect, &rw, &rh))
		return true;
	const ps5video::Rect to = FitOpening(*window, rw, rh);
	if (SameRect(to, *window))
		return true;
	if (!RefitImage(img, *window, to, why))
		return false;
	*window = to;
	return true;
}

bool DecodePng(const uint8_t* data, size_t size, Image* out, std::string* why)
{
	if (size == 0)
	{
		Fail(why, "the file is empty");
		return false;
	}
	if (size > kMaxFileBytes)
	{
		Fail(why, "the file is over 8 MiB");
		return false;
	}
	int w = 0, h = 0, comp = 0;
	if (!stbi_info_from_memory(data, int(size), &w, &h, &comp))
	{
		if (why)
			*why = std::string("not a PNG (") + stbi_failure_reason() + ")";
		return false;
	}
	if (w != kWidth || h != kHeight)
	{
		Fail(why, "the picture is %d x %d, not 1920 x 1080", w, h);
		return false;
	}
	if (stbi_is_16_bit_from_memory(data, int(size)))
	{
		Fail(why, "16 bits per channel: use 8");
		return false;
	}
	unsigned char* rgba = stbi_load_from_memory(data, int(size), &w, &h, &comp, 4);
	if (!rgba)
	{
		if (why)
			*why = std::string("damaged PNG (") + stbi_failure_reason() + ")";
		return false;
	}
	try
	{
		out->px.resize(size_t(kWidth) * kHeight);
	}
	catch (const std::bad_alloc&)
	{
		stbi_image_free(rgba);
		Fail(why, "out of memory");
		return false;
	}
	// R, G, B, A bytes are 0xAABBGGRR in a little-endian word, as everywhere in the surface
	memcpy(out->px.data(), rgba, out->px.size() * sizeof(uint32_t));
	stbi_image_free(rgba);
	out->w = kWidth;
	out->h = kHeight;
	return true;
}

bool FindWindow(const Image& img, ps5video::Rect* window, std::string* why)
{
	const int W = img.w, H = img.h;
	if (W != kWidth || H != kHeight || img.px.size() != size_t(W) * H)
	{
		Fail(why, "the picture is not 1920 x 1080");
		return false;
	}
	auto clear = [&](int x, int y) { return (img.px[size_t(y) * W + x] >> 24) < uint32_t(kClearAlpha); };
	const int cx = W / 2, cy = H / 2;
	if (!clear(cx, cy))
	{
		Fail(why, "no clear window at the centre");
		return false;
	}
	std::vector<uint8_t> seen;
	std::vector<std::pair<int, int>> stack;
	try
	{
		seen.assign(size_t(W) * H, 0);
		stack.emplace_back(cx, cy);
	}
	catch (const std::bad_alloc&)
	{
		Fail(why, "out of memory");
		return false;
	}
	int x0 = cx, x1 = cx, y0 = cy, y1 = cy;
	try
	{
		// scanline flood fill, 4-connected: a stack of seeds, one per run of clear pixels found above or below a span
		while (!stack.empty())
		{
			const auto [sx, sy] = stack.back();
			stack.pop_back();
			if (seen[size_t(sy) * W + sx])
				continue;
			int l = sx, r = sx;
			while (l > 0 && clear(l - 1, sy))
				l--;
			while (r < W - 1 && clear(r + 1, sy))
				r++;
			memset(&seen[size_t(sy) * W + l], 1, size_t(r - l + 1));
			x0 = l < x0 ? l : x0;
			x1 = r > x1 ? r : x1;
			y0 = sy < y0 ? sy : y0;
			y1 = sy > y1 ? sy : y1;
			for (int ny = sy - 1; ny <= sy + 1; ny += 2)
			{
				if (ny < 0 || ny >= H)
					continue;
				bool run = false;
				for (int x = l; x <= r; x++)
				{
					const bool open = clear(x, ny) && !seen[size_t(ny) * W + x];
					if (open && !run)
						stack.emplace_back(x, ny);
					run = open;
				}
			}
		}
	}
	catch (const std::bad_alloc&)
	{
		Fail(why, "out of memory");
		return false;
	}
	const ps5video::Rect found = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
	if (found.w < kMinWindowW || found.h < kMinWindowH)
	{
		Fail(why, "the clear window is %d x %d: too small", found.w, found.h);
		return false;
	}
	*window = found;
	return true;
}

bool WindowFits(const ps5video::Rect& w)
{
	return w.x >= 0 && w.y >= 0 && w.w >= kMinWindowW && w.h >= kMinWindowH && w.x + w.w <= kWidth && w.y + w.h <= kHeight;
}

void PunchWindow(Image* img, const ps5video::Rect& w)
{
	for (int y = w.y; y < w.y + w.h; y++)
		memset(&img->px[size_t(y) * img->w + w.x], 0, size_t(w.w) * sizeof(uint32_t));
}

std::shared_ptr<const ps5video::Border> BuildBorder(const Image& img, const ps5video::Rect& win, std::string* why)
{
	if (img.w != kWidth || img.h != kHeight || img.px.size() != size_t(kWidth) * kHeight)
	{
		Fail(why, "the picture is not 1920 x 1080");
		return nullptr;
	}
	try
	{
		auto border = std::make_shared<ps5video::Border>();
		border->window = win;
		border->backdrop.resize(img.px.size());
		for (int y = 0; y < kHeight; y++)
		{
			const bool row_in = y >= win.y && y < win.y + win.h;
			for (int x = 0; x < kWidth; x++)
			{
				const size_t i = size_t(y) * kWidth + x;
				const uint32_t p = img.px[i];
				const uint32_t a = p >> 24;
				uint32_t pre = p & 0x00ffffffu; // the colours times alpha, alpha 0
				if (a != 255)
					pre = Mul255(p & 0xff, a) | (Mul255((p >> 8) & 0xff, a) << 8) | (Mul255((p >> 16) & 0xff, a) << 16);
				border->backdrop[i] = 0xff000000u | pre;
				if (row_in && a != 0 && x >= win.x && x < win.x + win.w)
				{
					if (border->rim.size() >= kMaxRim)
					{
						Fail(why, "too much of the window is covered by art (over %d pixels)", int(kMaxRim));
						return nullptr;
					}
					border->rim.push_back({uint32_t(i), pre, 255u - a});
				}
			}
		}
		return border;
	}
	catch (const std::bad_alloc&)
	{
		Fail(why, "out of memory");
		return nullptr;
	}
}
} // namespace fe::border

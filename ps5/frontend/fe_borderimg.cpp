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

// A window of tw x th for the art whose window is 'win': the margins together keep the proportions they had (a window that
// touches the edge on one axis is centred on it).
ps5video::Rect PlaceOpening(const ps5video::Rect& win, int tw, int th)
{
	const int mw = kWidth - win.w, mh = kHeight - win.h;
	const int nx = mw > 0 ? int((int64_t(win.x) * (kWidth - tw) + mw / 2) / mw) : (kWidth - tw) / 2;
	const int ny = mh > 0 ? int((int64_t(win.y) * (kHeight - th) + mh / 2) / mh) : (kHeight - th) / 2;
	return ps5video::Rect{nx, ny, tw, th};
}

// Where one output row or column reads from: the pixels i0 and i1 of the source, i1 with weight f / 256.
struct Tap
{
	int i0, i1, f;
};

// Output columns (or rows) d0..d1 show the source s0..s1, 1:1 or stretched.
struct Piece
{
	int d0, d1, s0, s1;
};

// One axis of RefitImage: the pieces are in output order; each shows its part of the source, 1:1 or stretched, and never
// reads outside it.
void AxisTaps(const std::vector<Piece>& pieces, int size, std::vector<Tap>* taps)
{
	taps->assign(size_t(size), Tap{0, 0, 0});
	for (const Piece& p : pieces)
	{
		const int64_t len_s = p.s1 - p.s0, len_d = p.d1 - p.d0;
		const int last = p.s1 - 1;
		for (int d = p.d0; d < p.d1; d++)
		{
			Tap& t = (*taps)[size_t(d)];
			if (len_s <= 0) // nothing to show here (a window that touches the edge): the nearest pixel
			{
				const int i = std::min(std::max(p.s0, 0), size - 1);
				t = {i, i, 0};
				continue;
			}
			// the output pixel's centre in the source, minus the half pixel taps are counted from, scaled by 2 * len_d
			const int64_t pos = (2 * int64_t(d - p.d0) + 1) * len_s - len_d;
			const int64_t den = 2 * len_d;
			const int i0 = pos <= 0 ? p.s0 : p.s0 + int(pos / den);
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
	return PlaceOpening(win, tw, th);
}

ps5video::Rect IntegerOpening(const ps5video::Rect& win)
{
	const int k = std::min(win.w / ps5video::kPictureW, win.h / ps5video::kPictureH);
	if (k < 1)
		return win;
	return PlaceOpening(win, k * ps5video::kPictureW, k * ps5video::kPictureH);
}

namespace
{
// Fills the output rectangle the pieces cover (xp and yp are each in output order, without gaps) from 'in', bilinear.
void Resample(const std::vector<uint32_t>& in, std::vector<uint32_t>* out, const std::vector<Piece>& xp, const std::vector<Piece>& yp)
{
	std::vector<Tap> tx, ty;
	AxisTaps(xp, kWidth, &tx);
	AxisTaps(yp, kHeight, &ty);
	for (int y = yp.front().d0; y < yp.back().d1; y++)
	{
		const Tap& v = ty[size_t(y)];
		const uint32_t* r0 = &in[size_t(v.i0) * kWidth];
		const uint32_t* r1 = &in[size_t(v.i1) * kWidth];
		uint32_t* o = &(*out)[size_t(y) * kWidth];
		for (int x = xp.front().d0; x < xp.back().d1; x++)
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
}
} // namespace

bool RefitImage(Image* img, const ps5video::Rect& from, const ps5video::Rect& to, std::string* why)
{
	if (img->w != kWidth || img->h != kHeight || img->px.size() != size_t(kWidth) * kHeight)
	{
		Fail(why, "the picture is not 1920 x 1080");
		return false;
	}
	const int c = std::min({kOpeningCorner, from.w / 4, from.h / 4, to.w / 4, to.h / 4});
	// Each side margin is a panel of art, then a rim along the window that stays as drawn. The panel is never squeezed or
	// stretched out of shape: a margin that grows scales its panel up evenly and crops it to the height (from the top on the
	// left, from the bottom on the right: that is where covers keep their logos), one that shrinks shows the outer part of it
	// as drawn. A margin no wider than the rim has no panel and is stretched as a whole.
	const int from_r = kWidth - (from.x + from.w), to_r = kWidth - (to.x + to.w);
	const bool panel_l = from.x > kOpeningRim && to.x > kOpeningRim;
	const bool panel_r = from_r > kOpeningRim && to_r > kOpeningRim;
	const int ps_l = from.x - kOpeningRim, pd_l = to.x - kOpeningRim; // panel widths in the art and in the result
	const int ps_r = from_r - kOpeningRim, pd_r = to_r - kOpeningRim;
	std::vector<Piece> xp, yp;
	if (panel_l)
		xp.push_back({pd_l, to.x, ps_l, from.x});
	else
		xp.push_back({0, to.x, 0, from.x});
	xp.push_back({to.x, to.x + c, from.x, from.x + c});
	xp.push_back({to.x + c, to.x + to.w - c, from.x + c, from.x + from.w - c});
	xp.push_back({to.x + to.w - c, to.x + to.w, from.x + from.w - c, from.x + from.w});
	if (panel_r)
		xp.push_back({to.x + to.w, kWidth - pd_r, from.x + from.w, kWidth - ps_r});
	else
		xp.push_back({to.x + to.w, kWidth, from.x + from.w, kWidth});
	if (panel_l)
		xp.front().d0 = pd_l;
	yp.push_back({0, to.y, 0, from.y});
	yp.push_back({to.y, to.y + c, from.y, from.y + c});
	yp.push_back({to.y + c, to.y + to.h - c, from.y + c, from.y + from.h - c});
	yp.push_back({to.y + to.h - c, to.y + to.h, from.y + from.h - c, from.y + from.h});
	yp.push_back({to.y + to.h, kHeight, from.y + from.h, kHeight});
	std::vector<uint32_t> out;
	try
	{
		out.resize(img->px.size());
		Resample(img->px, &out, xp, yp);
		auto rows_for = [](int panel_src, int panel_dst) { // the source rows a grown panel shows, to keep its shape
			return std::min(kHeight, std::max(1, int((int64_t(kHeight) * panel_src + panel_dst / 2) / panel_dst)));
		};
		if (panel_l && pd_l > 0)
		{
			const bool grow = pd_l >= ps_l;
			const int rows = grow ? rows_for(ps_l, pd_l) : kHeight;
			Resample(img->px, &out, {{0, pd_l, 0, grow ? ps_l : pd_l}}, {{0, kHeight, 0, rows}});
		}
		if (panel_r && pd_r > 0)
		{
			const bool grow = pd_r >= ps_r;
			const int rows = grow ? rows_for(ps_r, pd_r) : kHeight;
			Resample(img->px, &out, {{kWidth - pd_r, kWidth, grow ? kWidth - ps_r : kWidth - pd_r, kWidth}}, {{0, kHeight, kHeight - rows, kHeight}});
		}
	}
	catch (const std::bad_alloc&)
	{
		Fail(why, "out of memory");
		return false;
	}
	img->px.swap(out);
	return true;
}

bool FitToAspect(Image* img, ps5video::Rect* window, ps5video::Aspect aspect, std::string* why)
{
	int rw = 0, rh = 0;
	ps5video::Rect to;
	if (OpeningRatio(aspect, &rw, &rh))
		to = FitOpening(*window, rw, rh);
	else if (aspect == ps5video::Aspect::Integer)
		to = IntegerOpening(*window);
	else
		return true;
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

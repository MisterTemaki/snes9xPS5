// Snes9x PS5 frontend: a border picture (fe_borderimg.h).
// SPDX-License-Identifier: MIT

#include "fe_borderimg.h"

#include <cstdarg>
#include <cstdio>
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
} // namespace

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

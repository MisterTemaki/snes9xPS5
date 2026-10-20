// Snes9x PS5: the picture, through libSceVideoOut.
//
// The scan-out setup and the tile swizzle are the ones of the ps5-payload-dev SDK's SDL2 port
// (src/video/ps5/SDL_ps5video.c and SDL_ps5tilemap.c, (C) 2026 John Tornblom, zlib licence), which a
// payload ELF can use without any GPU driver:
//   - sceVideoOutOpen(255, 0, 0, NULL) on the system user, a 64 MiB direct-memory block split in two
//     scan-out buffers, sceVideoOutSetBufferAttribute2(A8B8G8R8, 1920x1080), RegisterBuffers2;
//   - a flip event queue to know when a buffer is on screen;
//   - the buffers are tiled (64 KiB tiles of 512x128 pixels, with an XOR swizzle inside), so every frame
//     the changed rectangle of a linear surface is swizzled in with AVX2 streaming stores.
// Each scan-out buffer remembers what changed since it was last written ("pending"), so with two buffers
// only the dirty rectangle has to be copied, never the whole screen.
//
// SPDX-License-Identifier: MIT

#include "ProsperoVideo.h"

#include "OrbisPaths.h"
#include "ProsperoCrt.h"
#include "ProsperoSce.h"

#include <immintrin.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

namespace ps5video
{
namespace
{
constexpr uint32_t kTileW = 512;
constexpr uint32_t kTileH = 128;
constexpr uint32_t kTileSize = kTileW * kTileH;
constexpr uint32_t kBand = 8;
constexpr size_t k2M = 0x200000;

struct R
{
	int x = 0, y = 0, w = 0, h = 0;
	bool Empty() const { return w <= 0 || h <= 0; }
};

void Union(R& acc, const R& r)
{
	if (r.Empty())
		return;
	if (acc.Empty())
	{
		acc = r;
		return;
	}
	const int x0 = std::min(acc.x, r.x), y0 = std::min(acc.y, r.y);
	const int x1 = std::max(acc.x + acc.w, r.x + r.w), y1 = std::max(acc.y + acc.h, r.y + r.h);
	acc = {x0, y0, x1 - x0, y1 - y0};
}

bool Intersect(R& r, const R& clip)
{
	const int x0 = std::max(r.x, clip.x), y0 = std::max(r.y, clip.y);
	const int x1 = std::min(r.x + r.w, clip.x + clip.w), y1 = std::min(r.y + r.h, clip.y + clip.h);
	r = {x0, y0, x1 - x0, y1 - y0};
	return !r.Empty();
}

uint32_t TileOffset(uint32_t x, uint32_t y)
{
	return (((x)&1u) << 0 | ((x >> 1) & 1u) << 1 | ((y >> 0) & 1u) << 2 | ((y >> 1) & 1u) << 3 |
			((y >> 2) & 1u) << 4 | ((x >> 2) & 1u) << 5 | (((x >> 3) ^ (y >> 3)) & 1u) << 6 |
			(((x >> 4) ^ (y >> 4)) & 1u) << 7 | (((x >> 6) ^ (y >> 5)) & 1u) << 8 |
			(((x >> 5) ^ (y >> 6)) & 1u) << 9 | ((y >> 3) & 1u) << 10 | ((x >> 4) & 1u) << 11 |
			((y >> 6) & 1u) << 12 | ((x >> 6) & 1u) << 13 | ((x >> 7) & 1u) << 14 | ((x >> 8) & 1u) << 15);
}

uint32_t TilePixel(uint32_t x, uint32_t y, uint32_t width)
{
	return (x / kTileW) * kTileSize + (y / kTileH) * (kTileH * width) +
		   (TileOffset(x % kTileW, 0) ^ TileOffset(0, y % kTileH));
}

size_t TiledBufferSize(uint32_t width, uint32_t height)
{
	const uint32_t last_band = ((height - 1) / kTileH) * (kTileH * width);
	const uint32_t last_tile = ((width - 1) / kTileW) * kTileSize;
	return size_t(last_band + last_tile + kTileSize) * sizeof(uint32_t);
}

struct State
{
	bool ok = false;
	int handle = -1;
	SceKernelEqueue flip_queue = nullptr;
	intptr_t phys = 0; // direct memory (0 when the buffers are flexible memory)
	void* vmem = nullptr;
	size_t vmem_len = 0;
	int out_w = kWidth, out_h = kHeight; // scan-out size: 1920x1080, or 1280x720 when memory is short
	std::vector<uint32_t> scaled; // the surface scaled to out_w x out_h (720p only)
	std::vector<uint32_t> xmap, ymap; // out pixel -> surface pixel
	SceVideoOutBuffers vbuf[2] = {};
	uint32_t* surface = nullptr;
	std::vector<uint32_t> colx; // tiled offset of every 4th column
	uint32_t yoff[kTileH] = {};
	R pending[2];
	bool full[2] = {true, true};
	uint32_t frame = 0;
	int flips_in_flight = 0;
};
State g;

__attribute__((target("avx2"))) void TileArea(const uint32_t* src, uint32_t pitch, uint32_t* dst, uint32_t x0,
	uint32_t x1, uint32_t y0, uint32_t y1)
{
	const uint32_t* colx = g.colx.data();
	const uint32_t width = g.out_w;
	const uint32_t quads = x1 / 4;

	for (uint32_t y = y0; y < y1; y += kBand)
	{
		const uint32_t base = (y / kTileH) * (kTileH * width);
		const uint32_t yo = g.yoff[y % kTileH];
		const uint32_t* s = src + size_t(y) * pitch;
		uint32_t* d = dst + base;
		const uint32_t rows = std::min(kBand, y1 - y);

		for (uint32_t q = x0 / 4; q < quads; q++)
		{
			const uint32_t* p = s + (q << 2);
			uint32_t* o = d + (colx[q] ^ yo);
			uint32_t k = 0;
			for (; k + 1 < rows; k += 2)
			{
				const __m256i v = _mm256_inserti128_si256(
					_mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))),
					_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + pitch)), 1);
				_mm256_stream_si256(reinterpret_cast<__m256i*>(o), v);
				p += 2 * pitch;
				o += 8;
			}
			if (rows & 1)
				_mm_stream_si128(reinterpret_cast<__m128i*>(o), _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
		}
		for (uint32_t k = 0; k < rows; k++)
		{
			const uint32_t* p = s + size_t(k) * pitch;
			for (uint32_t x = quads * 4; x < x1; x++)
				dst[TilePixel(x, y + k, width)] = p[x];
		}
	}
	_mm_sfence();
}

void TileBlit(int buf_idx, R damage)
{
	const R screen = {0, 0, g.out_w, g.out_h};
	if (g.out_w != kWidth && !damage.Empty())
	{
		// the damage is in surface (1920x1080) pixels: the scan-out pixels showing them, rounded outwards
		const int x0 = damage.x * g.out_w / kWidth, y0 = damage.y * g.out_h / kHeight;
		const int x1 = ((damage.x + damage.w) * g.out_w + kWidth - 1) / kWidth;
		const int y1 = ((damage.y + damage.h) * g.out_h + kHeight - 1) / kHeight;
		damage = R{x0, y0, x1 - x0, y1 - y0};
		Intersect(damage, screen);
	}
	R area;
	if (g.full[buf_idx])
		area = screen;
	else
	{
		area = g.pending[buf_idx];
		Union(area, damage);
	}
	g.full[buf_idx] = false;
	g.pending[buf_idx] = R();
	Union(g.pending[buf_idx ^ 1], damage); // the other buffer still shows the old picture there

	if (area.Empty())
		return;

	area.w += area.x & 3;
	area.x &= ~3;
	area.w = std::min((area.w + 3) & ~3, g.out_w - area.x);
	area.h += area.y & (kBand - 1);
	area.y &= ~(kBand - 1);
	area.h = std::min(int((area.h + kBand - 1) & ~(kBand - 1)), g.out_h - area.y);

	if (g.out_w != kWidth)
	{
		// 720p scan-out: nearest-neighbour downscale of the (aligned) area first
		for (int y = area.y; y < area.y + area.h; y++)
		{
			const uint32_t* src = g.surface + size_t(g.ymap[y]) * kWidth;
			uint32_t* dst = g.scaled.data() + size_t(y) * g.out_w;
			for (int x = area.x; x < area.x + area.w; x++)
				dst[x] = src[g.xmap[x]];
		}
	}
	const uint32_t* src = g.out_w != kWidth ? g.scaled.data() : g.surface;
	TileArea(src, g.out_w, static_cast<uint32_t*>(g.vbuf[buf_idx].data), area.x, area.x + area.w, area.y,
		area.y + area.h);
}

void WaitOneFlip()
{
	alignas(16) uint8_t ev[128]; // one struct kevent (64 bytes on FreeBSD 12+)
	int out = 0;
	const int r = sceKernelWaitEqueue(g.flip_queue, ev, 1, &out, nullptr);
	if (r != 0)
		OrbisLog("[video] sceKernelWaitEqueue -> %x", r);
	if (g.flips_in_flight > 0)
		g.flips_in_flight--;
}

// ---- scan-out memory --------------------------------------------------------------------------------
// The SDK's SDL port asks for 64 MiB of main direct memory. A payload process can get much less than that
// (the first console logs: sceKernelAllocateMainDirectMemory -> 0x80020023, EAGAIN), so ask only for what
// two buffers need, then try other ways to get it, then 720p.
void FreeScanout()
{
	if (g.phys)
		sceKernelReleaseDirectMemory(g.phys, g.vmem_len); // also unmaps
	else if (g.vmem)
		munmap(g.vmem, g.vmem_len);
	g.phys = 0;
	g.vmem = nullptr;
	g.vmem_len = 0;
}

bool TryRegister(int w, int h)
{
	memset(g.vmem, 0, g.vmem_len);
	g.vbuf[0].data = g.vmem;
	g.vbuf[1].data = static_cast<uint8_t*>(g.vmem) + g.vmem_len / 2;
	SceVideoOutBufferAttribute2 attr;
	memset(&attr, 0, sizeof(attr));
	sceVideoOutSetBufferAttribute2(&attr, SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB, 0, w, h, 0, 0, 0);
	const int r = sceVideoOutRegisterBuffers2(g.handle, 0, 0, g.vbuf, 2, &attr, 0, nullptr);
	OrbisLog("[video]   sceVideoOutRegisterBuffers2(%dx%d) -> %x", w, h, r);
	return r == 0;
}

bool TryDirect(const char* how, int r, size_t len, size_t align, int w, int h)
{
	OrbisLog("[video]   %s(%zu MiB) -> %x phys=%lx", how, len >> 20, r, long(g.phys));
	if (r != 0)
	{
		g.phys = 0;
		return false;
	}
	g.vmem_len = len;
	g.vmem = nullptr;
	r = sceKernelMapDirectMemory(&g.vmem, len, 0x33, 0, g.phys, align);
	OrbisLog("[video]   sceKernelMapDirectMemory -> %x at %p", r, g.vmem);
	if (r != 0)
	{
		sceKernelReleaseDirectMemory(g.phys, len);
		g.phys = 0;
		g.vmem = nullptr;
		return false;
	}
	if (TryRegister(w, h))
		return true;
	FreeScanout();
	return false;
}

bool AllocScanout(int w, int h)
{
	const size_t each = (TiledBufferSize(w, h) + k2M - 1) & ~(k2M - 1);
	const size_t len = each * 2;
	const int64_t dsize = int64_t(sceKernelGetDirectMemorySize());
	int64_t avail_start = 0;
	size_t avail = 0;
	const int ar = sceKernelAvailableDirectMemorySize(0, dsize, 0x10000, &avail_start, &avail);
	size_t flex = 0;
	sceKernelAvailableFlexibleMemorySize(&flex);
	OrbisLog("[video] %dx%d needs %zu MiB; direct memory %lld MiB, largest free %zu MiB (%x), flexible free %zu MiB",
		w, h, len >> 20, (long long)(dsize >> 20), avail >> 20, ar, flex >> 20);

	static const int types[] = {3, 0};
	for (int type : types)
	{
		int r = sceKernelAllocateMainDirectMemory(len, k2M, type, &g.phys);
		if (TryDirect(type == 3 ? "AllocateMainDirectMemory type 3" : "AllocateMainDirectMemory type 0", r, len,
				k2M, w, h))
			return true;
		r = sceKernelAllocateDirectMemory(0, dsize, len, k2M, type, &g.phys);
		if (TryDirect(type == 3 ? "AllocateDirectMemory type 3" : "AllocateDirectMemory type 0", r, len, k2M, w,
				h))
			return true;
	}
	// last resort: flexible memory mapped for the GPU (not known to be accepted by the display yet)
	void* addr = nullptr;
	const int r = sceKernelMapNamedFlexibleMemory(&addr, len, 0x33, 0, "snes9x scanout");
	OrbisLog("[video]   MapNamedFlexibleMemory(%zu MiB) -> %x at %p", len >> 20, r, addr);
	if (r == 0 && addr)
	{
		g.phys = 0;
		g.vmem = addr;
		g.vmem_len = len;
		if (TryRegister(w, h))
			return true;
		FreeScanout();
	}
	return false;
}

void SetOutputSize(int w, int h)
{
	g.out_w = w;
	g.out_h = h;
	g.colx.assign(w / 4 + 1, 0);
	for (uint32_t x = 0; x < uint32_t(w); x += 4)
		g.colx[x / 4] = (x / kTileW) * kTileSize + TileOffset(x % kTileW, 0);
	if (w != kWidth)
	{
		g.scaled.assign(size_t(w) * h, Rgb(0, 0, 0));
		g.xmap.resize(w);
		g.ymap.resize(h);
		for (int x = 0; x < w; x++)
			g.xmap[x] = uint32_t((int64_t(x) * kWidth + kWidth / 2) / w);
		for (int y = 0; y < h; y++)
			g.ymap[y] = uint32_t((int64_t(y) * kHeight + kHeight / 2) / h);
	}
	else
	{
		g.scaled.clear();
		g.xmap.clear();
		g.ymap.clear();
	}
	g.full[0] = g.full[1] = true;
}
} // namespace

bool Init()
{
	if (g.ok)
		return true;

	g.surface = static_cast<uint32_t*>(aligned_alloc(64, size_t(kWidth) * kHeight * 4));
	if (!g.surface)
	{
		OrbisLog("[video] out of memory for the surface");
		return false;
	}
	std::fill(g.surface, g.surface + size_t(kWidth) * kHeight, Rgb(0, 0, 0));
	for (uint32_t y = 0; y < kTileH; y++)
		g.yoff[y] = TileOffset(0, y);

	// 0x80290009: the screen is still held by another process (the 1.5 console log: a second copy started
	// while the first still ran). Give it a few seconds to let go.
	for (int attempt = 0;; attempt++)
	{
		g.handle = sceVideoOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, 0, 0, nullptr);
		OrbisLog("[video] sceVideoOutOpen -> %d (%x)", g.handle, unsigned(g.handle));
		if (g.handle >= 0 || unsigned(g.handle) != 0x80290009u || attempt >= 10)
			break;
		usleep(500 * 1000);
	}
	if (g.handle < 0)
		return false;

	int r = sceKernelCreateEqueue(&g.flip_queue, "snes9x flip");
	if (r != 0)
	{
		OrbisLog("[video] sceKernelCreateEqueue -> %x", r);
		return false;
	}
	r = sceVideoOutAddFlipEvent(g.flip_queue, g.handle, nullptr);
	if (r != 0)
	{
		OrbisLog("[video] sceVideoOutAddFlipEvent -> %x", r);
		return false;
	}
	sceVideoOutSetFlipRate(g.handle, 0); // 60 Hz

	// When started from the dashboard icon, the launcher app still holds the game memory for a moment
	// after it hands us over: keep trying for a few seconds before giving up.
	bool ok = false;
	for (int attempt = 0; attempt < 12 && !ok; attempt++)
	{
		if (attempt > 0)
		{
			OrbisLog("[video] no scan-out memory yet, retrying (%d)", attempt);
			usleep(500000);
		}
		static const int sizes[][2] = {{kWidth, kHeight}, {1280, 720}};
		for (const auto& sz : sizes)
		{
			if (AllocScanout(sz[0], sz[1]))
			{
				SetOutputSize(sz[0], sz[1]);
				ok = true;
				break;
			}
		}
	}
	if (!ok)
	{
		OrbisLog("[video] could not get memory for the scan-out buffers");
		return false;
	}
	OrbisLog("[video] scan-out %dx%d, %zu MiB of %s memory", g.out_w, g.out_h, g.vmem_len >> 20,
		g.phys ? "direct" : "flexible");

	g.ok = true;
	Present(0, 0, 0, 0, true); // black screen over the home screen
	return true;
}

void Shutdown()
{
	if (g.handle >= 0)
	{
		while (g.flips_in_flight > 0)
			WaitOneFlip();
		if (g.flip_queue)
			sceVideoOutDeleteFlipEvent(g.flip_queue, g.handle);
		sceVideoOutClose(g.handle);
		g.handle = -1;
	}
	if (g.flip_queue)
	{
		sceKernelDeleteEqueue(g.flip_queue);
		g.flip_queue = nullptr;
	}
	FreeScanout();
	free(g.surface);
	g.surface = nullptr;
	g.ok = false;
}

uint32_t* Surface()
{
	return g.surface;
}

void Present(int x, int y, int w, int h, bool wait_vsync)
{
	if (!g.ok)
		return;
	// The buffer about to be written may still be on screen until its last flip completes.
	while (g.flips_in_flight > 0)
		WaitOneFlip();

	R damage = (w <= 0 || h <= 0) ? R{0, 0, kWidth, kHeight} : R{x, y, w, h};
	Intersect(damage, R{0, 0, kWidth, kHeight});

	const int idx = g.frame & 1;
	TileBlit(idx, damage);

	const int r = sceVideoOutSubmitFlip(g.handle, idx, SCE_VIDEO_OUT_FLIP_MODE_VSYNC, g.frame);
	if (r != 0)
	{
		OrbisLog("[video] sceVideoOutSubmitFlip -> %x", r);
		return;
	}
	g.flips_in_flight++;
	g.frame++;
	if (wait_vsync)
		WaitOneFlip();
}

void FillRect(int x, int y, int w, int h, uint32_t color)
{
	R r{x, y, w, h};
	if (!g.surface || !Intersect(r, R{0, 0, kWidth, kHeight}))
		return;
	for (int j = r.y; j < r.y + r.h; j++)
		std::fill(g.surface + size_t(j) * kWidth + r.x, g.surface + size_t(j) * kWidth + r.x + r.w, color);
}

void DarkenRect(int x, int y, int w, int h)
{
	R r{x, y, w, h};
	if (!g.surface || !Intersect(r, R{0, 0, kWidth, kHeight}))
		return;
	for (int j = r.y; j < r.y + r.h; j++)
	{
		uint32_t* p = g.surface + size_t(j) * kWidth + r.x;
		for (int i = 0; i < r.w; i++)
			p[i] = ((p[i] >> 1) & 0x007f7f7fu) | 0xff000000u;
	}
}

// ---------------------------------------------------------------------------------------------------------
// The SNES picture
namespace
{
struct SnesGeom
{
	int src_w = 0, src_h = 0;
	Aspect aspect = Aspect::Count;
	bool scanlines = false;
	int shader = 0;
	uint64_t border_gen = 0; // s_border_gen when the surface was last prepared
	Rect dst = {0, 0, 0, 0};
	std::vector<uint16_t> xlut; // dst column -> src column
	std::vector<uint16_t> ylut; // dst row -> src row
	std::vector<uint8_t> dark; // dst row is a scanline row
};
SnesGeom s_geom;
uint32_t* s_rgb565 = nullptr; // RGB565 -> A8B8G8R8 (the surface's format)
uint32_t* s_argb565 = nullptr; // RGB565 -> A8R8G8B8 (what the CRT shaders read)
std::vector<uint32_t> s_argb; // the frame in A8R8G8B8, for the shaders
double s_shader_ms = 0;
int s_shader_frames = 0;
uint64_t s_frame_no = 0;
std::shared_ptr<const Border> s_border; // published by SetBorder with atomic_store, read once per DrawSnes with atomic_load
std::atomic<uint64_t> s_border_gen{0}; // counts SetBorder calls: a new border repaints the surface

void BuildColorLut()
{
	s_rgb565 = static_cast<uint32_t*>(malloc(65536 * sizeof(uint32_t)));
	for (uint32_t c = 0; c < 65536; c++)
	{
		const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
		s_rgb565[c] = Rgb(uint8_t((r5 << 3) | (r5 >> 2)), uint8_t((g6 << 2) | (g6 >> 4)), uint8_t((b5 << 3) | (b5 >> 2)));
	}
	s_argb565 = static_cast<uint32_t*>(malloc(65536 * sizeof(uint32_t)));
	for (uint32_t c = 0; c < 65536; c++)
	{
		const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
		s_argb565[c] = 0xff000000u | (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8) | ((b5 << 3) | (b5 >> 2));
	}
}

bool SameRect(const Rect& a, const Rect& b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// The rim of a border over the picture: out = pic * (255 - a) / 255 + colour * a / 255 for every rim pixel that lies in
// the picture rectangle (the others are in the backdrop already). The rim is sorted by index, so only the rows of the
// rectangle are visited.
void BlendRim(const Border& b, const Rect& d)
{
	const uint32_t lo = uint32_t(d.y) * kWidth, hi = uint32_t(d.y + d.h) * kWidth;
	const auto cmp = [](const RimPixel& p, uint32_t index) { return p.index < index; };
	const RimPixel* it = std::lower_bound(b.rim.data(), b.rim.data() + b.rim.size(), lo, cmp);
	const RimPixel* end = std::lower_bound(it, b.rim.data() + b.rim.size(), hi, cmp);
	uint32_t* surf = g.surface;
	for (; it != end; ++it)
	{
		const uint32_t x = it->index % kWidth;
		if (x < uint32_t(d.x) || x >= uint32_t(d.x + d.w))
			continue;
		const uint32_t pic = surf[it->index], pre = it->pre, inv = it->inv;
		const uint32_t r = ((pic & 0xff) * inv + 127) / 255 + (pre & 0xff);
		const uint32_t gr = (((pic >> 8) & 0xff) * inv + 127) / 255 + ((pre >> 8) & 0xff);
		const uint32_t bl = (((pic >> 16) & 0xff) * inv + 127) / 255 + ((pre >> 16) & 0xff);
		surf[it->index] = 0xff000000u | (bl << 16) | (gr << 8) | r;
	}
}

// A picture of ratio rw:rh in 'bounds': as large as fits, or all of 'bounds' when it is within 3% of that ratio.
void FitRatio(const Rect& bounds, int64_t rw, int64_t rh, int& dw, int& dh)
{
	const int64_t diff = std::llabs(int64_t(bounds.w) * rh - int64_t(bounds.h) * rw); // |bw/bh - rw/rh| * bh * rh
	if (diff * 100 <= 3 * int64_t(bounds.h) * rw)
	{
		dw = bounds.w;
		dh = bounds.h;
		return;
	}
	dh = bounds.h;
	dw = int((int64_t(bounds.h) * rw + rh / 2) / rh);
	if (dw > bounds.w)
	{
		dw = bounds.w;
		dh = int((int64_t(bounds.w) * rh + rw / 2) / rw);
	}
}

bool UsableBorder(const Border& b)
{
	const Rect& w = b.window;
	return b.backdrop.size() == size_t(kWidth) * kHeight && w.w > 0 && w.h > 0 && w.x >= 0 && w.y >= 0 &&
		   w.x + w.w <= kWidth && w.y + w.h <= kHeight;
}
} // namespace

Rect ComputeRect(int src_w, int src_h, Aspect aspect, const Rect& bounds)
{
	// 512-wide hi-res and 448/478-line interlaced pictures cover the same screen area as 256 x 224/239.
	const int base_w = src_w > 256 ? src_w / 2 : src_w;
	const int lines = src_h > 256 ? src_h / 2 : src_h;
	int dw = bounds.w, dh = bounds.h;
	switch (aspect)
	{
		case Aspect::Ratio4x3:
			FitRatio(bounds, 4, 3, dw, dh);
			break;
		case Aspect::PixelPerfect:
			FitRatio(bounds, base_w, lines, dw, dh);
			break;
		case Aspect::Integer:
		{
			const int k = std::min(bounds.w / base_w, bounds.h / lines);
			// a window cut to a whole multiple of the 256 x 224 picture (fe_borderimg.cpp, IntegerOpening) and a picture of
			// another height: it fills the window, not a smaller multiple with black around it
			if (lines != kPictureH && bounds.w % kPictureW == 0 && bounds.h % kPictureH == 0 &&
				bounds.w / kPictureW == bounds.h / kPictureH)
				FitRatio(bounds, base_w, lines, dw, dh);
			else if (k < 1) // a window smaller than the picture: no whole multiple fits
				FitRatio(bounds, 4, 3, dw, dh);
			else
			{
				dw = base_w * k;
				dh = lines * k;
			}
			break;
		}
		default:
			break;
	}
	dw = std::min(dw, bounds.w) & ~1; // always inside 'bounds'
	dh = std::min(dh, bounds.h);
	return Rect{bounds.x + (bounds.w - dw) / 2, bounds.y + (bounds.h - dh) / 2, dw, dh};
}

const char* AspectName(Aspect a)
{
	switch (a)
	{
		case Aspect::Ratio4x3: return "4:3";
		case Aspect::PixelPerfect: return "8:7 (pixel perfect)";
		case Aspect::Integer: return "Integer scale";
		case Aspect::Stretch: return "Stretch 16:9";
		default: return "?";
	}
}

void SetBorder(std::shared_ptr<const Border> border)
{
	std::atomic_store(&s_border, std::move(border));
	s_border_gen.fetch_add(1, std::memory_order_release); // after the pointer: DrawSnes reads them in the other order
}

Rect LastSnesRect()
{
	return s_geom.dst;
}

void InvalidateSnes()
{
	s_geom.src_w = 0;
}

double TakeShaderMs()
{
	const double ms = s_shader_frames ? s_shader_ms / s_shader_frames : 0.0;
	s_shader_ms = 0;
	s_shader_frames = 0;
	return ms;
}

Rect DrawSnes(const uint16_t* src, int pitch_bytes, int w, int h, Aspect aspect, bool scanlines, int shader)
{
	if (shader < 0 || shader >= int(ps5crt::Shader::Count))
		shader = 0;
	if (!g.surface || w <= 0 || h <= 0)
		return Rect{0, 0, 0, 0};
	if (!s_rgb565)
		BuildColorLut();

	// read once: SetBorder may run meanwhile (the count first, so a border newer than the count only repaints again)
	const uint64_t border_gen = s_border_gen.load(std::memory_order_acquire);
	std::shared_ptr<const Border> border = std::atomic_load(&s_border);
	if (border && !UsableBorder(*border))
	{
		static const Border* refused = nullptr;
		if (refused != border.get())
		{
			refused = border.get();
			OrbisLog("[border] refused: backdrop %zu pixels, window %d,%d %dx%d", border->backdrop.size(),
				border->window.x, border->window.y, border->window.w, border->window.h);
		}
		border.reset();
	}

	bool cleared = false;
	if (w != s_geom.src_w || h != s_geom.src_h || aspect != s_geom.aspect || scanlines != s_geom.scanlines ||
		shader != s_geom.shader || border_gen != s_geom.border_gen)
	{
		const bool surface_known = s_geom.src_w != 0; // InvalidateSnes() or the first frame: unknown contents
		const bool border_changed = border_gen != s_geom.border_gen;
		const Rect old_dst = s_geom.dst;
		s_geom.shader = shader;
		s_geom.src_w = w;
		s_geom.src_h = h;
		s_geom.aspect = aspect;
		s_geom.scanlines = scanlines;
		s_geom.border_gen = border_gen;
		s_geom.dst = ComputeRect(w, h, aspect, border ? border->window : Rect{0, 0, kWidth, kHeight});
		const Rect& d = s_geom.dst;
		s_geom.xlut.resize(d.w);
		for (int x = 0; x < d.w; x++)
			s_geom.xlut[x] = uint16_t((int64_t(x) * w) / d.w);
		s_geom.ylut.resize(d.h);
		s_geom.dark.assign(d.h, 0);
		const int lines = h > 256 ? h / 2 : h; // scanlines follow the 224/239-line grid, even interlaced
		for (int y = 0; y < d.h; y++)
		{
			s_geom.ylut[y] = uint16_t((int64_t(y) * h) / d.h);
			if (scanlines && y + 1 < d.h)
			{
				const int line_here = int((int64_t(y) * lines) / d.h);
				const int line_next = int((int64_t(y + 1) * lines) / d.h);
				s_geom.dark[y] = (line_here != line_next) && (d.h / lines >= 3);
			}
		}
		if (!border)
		{
			FillRect(0, 0, kWidth, kHeight, Rgb(0, 0, 0));
			cleared = true;
		}
		else if (!surface_known || border_changed || !SameRect(old_dst, d))
		{
			// only the picture rectangle changes from frame to frame: the border goes in once, here
			memcpy(g.surface, border->backdrop.data(), size_t(kWidth) * kHeight * sizeof(uint32_t));
			cleared = true;
		}
		const Rect& r = s_geom.dst;
		// logged when it changes (the pause menu redraws the frame, with InvalidateSnes, every time it draws)
		static int logged[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
		const Rect win = border ? border->window : Rect{0, 0, 0, 0};
		const int now[9] = {w, h, int(aspect), scanlines ? 1 : 0, shader, win.x, win.y, win.w, win.h};
		if (memcmp(logged, now, sizeof(now)) != 0)
		{
			memcpy(logged, now, sizeof(now));
			char in_border[64] = "";
			if (border)
				snprintf(in_border, sizeof(in_border), ", border window %d,%d %dx%d", win.x, win.y, win.w, win.h);
			if (shader > 0)
				OrbisLog("[video] picture %dx%d, %s, shader %s -> %d,%d %dx%d%s", w, h, AspectName(aspect),
					ps5crt::Name(ps5crt::Shader(shader)), r.x, r.y, r.w, r.h, in_border);
			else
				OrbisLog("[video] picture %dx%d, %s%s -> %d,%d %dx%d%s", w, h, AspectName(aspect),
					scanlines ? ", scanlines" : "", r.x, r.y, r.w, r.h, in_border);
		}
	}

	const Rect& d = s_geom.dst;
	const uint8_t* src8 = reinterpret_cast<const uint8_t*>(src);
	s_frame_no++;
	if (shader > 0)
	{
		// the CRT shader reads the whole frame (hi-res 512-wide and interlaced pictures as they are)
		timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		s_argb.resize(size_t(w) * h);
		for (int y = 0; y < h; y++)
		{
			const uint16_t* in = reinterpret_cast<const uint16_t*>(src8 + size_t(y) * pitch_bytes);
			uint32_t* out = s_argb.data() + size_t(y) * w;
			for (int x = 0; x < w; x++)
				out[x] = s_argb565[in[x]];
		}
		ps5crt::Render(ps5crt::Shader(shader), s_argb.data(), w, h, g.surface, kWidth, d.x, d.y, d.w, d.h, s_frame_no);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		s_shader_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
		s_shader_frames++;
		if (border)
			BlendRim(*border, d);
		return cleared ? Rect{0, 0, kWidth, kHeight} : d;
	}
	const uint16_t* xl = s_geom.xlut.data();
	uint32_t* bright = nullptr; // last row written at full brightness
	int bright_src = -1;
	for (int y = 0; y < d.h; y++)
	{
		uint32_t* out = g.surface + size_t(d.y + y) * kWidth + d.x;
		const int sy = s_geom.ylut[y];
		if (sy != bright_src)
		{
			const uint16_t* in = reinterpret_cast<const uint16_t*>(src8 + size_t(sy) * pitch_bytes);
			for (int x = 0; x < d.w; x++)
				out[x] = s_rgb565[in[xl[x]]];
			bright = out;
			bright_src = sy;
			if (s_geom.dark[y])
			{
				// This row is both the first and the last of its source line (small scale): keep it bright.
			}
			continue;
		}
		if (s_geom.dark[y])
		{
			for (int x = 0; x < d.w; x++)
				out[x] = ((bright[x] >> 1) & 0x007f7f7fu) | 0xff000000u;
		}
		else
			memcpy(out, bright, size_t(d.w) * 4);
	}
	if (border)
		BlendRim(*border, d);
	return cleared ? Rect{0, 0, kWidth, kHeight} : d;
}
} // namespace ps5video

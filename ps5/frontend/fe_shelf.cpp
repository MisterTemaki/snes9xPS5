// Snes9x PS5 frontend: the 3D game shelf (fe_shelf.h).
// SPDX-License-Identifier: MIT

#include "fe_shelf.h"

#include "fe_covers.h"
#include "fe_cheatdownload.h"
#include "fe_games.h"
#include "fe_menu.h"
#include "fe_prefetch.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoCrash.h"
#include "ProsperoThread.h"
#include "ProsperoVideo.h"

#include "snes9x.h"

#include <immintrin.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifndef SNES9X_PS5_VERSION
#define SNES9X_PS5_VERSION "dev"
#endif

namespace fe
{
namespace
{
using ps5video::Rgb;
constexpr int W = ps5video::kWidth;
constexpr int H = ps5video::kHeight;
// The author, on the shelf under the wordmark (with the GitHub mark), as PS5SX2 shows its author's handles.
constexpr const char* kAuthorGitHub = "github.com/MisterTemaki";

// ---- camera and layout (world units: the middle cover is kCoverH tall at distance kCamDist) ----------
constexpr float kFocal = 1000.0f;
constexpr float kCamDist = 1000.0f;
constexpr float kCoverH = 540.0f;
constexpr float kCamY = kCoverH * 0.42f; // eye height above the floor
constexpr float kHorizon = 455.0f; // screen row of the eye
constexpr float kSideX = 600.0f; // first side cover
constexpr float kSideStep = 165.0f; // the next ones
constexpr float kSideZ = 330.0f; // how far back the side covers stand
constexpr float kSideAngle = 1.10f; // radians (63 degrees)
constexpr float kVisible = 7.5f; // covers each side
constexpr float kReflect = 0.42f; // reflection length, of the cover's height
constexpr int kKeepRadius = 14; // textures kept around the selection

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

inline uint32_t Lerp32(uint32_t a, uint32_t b, uint32_t w) // w 0..256
{
	const uint32_t iw = 256 - w;
	const uint32_t rb = (((a & 0xff00ffu) * iw + (b & 0xff00ffu) * w) >> 8) & 0xff00ffu;
	const uint32_t g = (((a & 0x00ff00u) * iw + (b & 0x00ff00u) * w) >> 8) & 0x00ff00u;
	return 0xff000000u | rb | g;
}

inline uint32_t Scale32(uint32_t a, uint32_t s) // s 0..256
{
	const uint32_t rb = (((a & 0xff00ffu) * s) >> 8) & 0xff00ffu;
	const uint32_t g = (((a & 0x00ff00u) * s) >> 8) & 0x00ff00u;
	return 0xff000000u | rb | g;
}

inline uint32_t Add32(uint32_t a, uint32_t b) // per channel, saturating
{
	return uint32_t(_mm_cvtsi128_si32(_mm_adds_epu8(_mm_cvtsi32_si128(int(a)), _mm_cvtsi32_si128(int(b))))) | 0xff000000u;
}

// ---- a small pool: the screen is split in vertical bands, one per thread --------------------------------
class Pool
{
public:
	Pool()
	{
		unsigned n = std::thread::hardware_concurrency();
		n = std::clamp(n == 0 ? 4u : n, 2u, 6u);
		for (unsigned i = 1; i < n; i++)
			m_threads.emplace_back([this, i] { Worker(int(i)); }, 2 * 1024 * 1024);
		m_count = int(n);
	}
	~Pool()
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_quit = true;
			m_gen++;
		}
		m_cv.notify_all();
		for (auto& t : m_threads)
			t.join();
	}
	// job(x0, x1) on every band, the calling thread included; returns when all are done.
	void Run(const std::function<void(int, int)>& job)
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_job = &job;
			m_pending = m_count - 1;
			m_gen++;
		}
		m_cv.notify_all();
		Band(0, job);
		std::unique_lock<std::mutex> lock(m_lock);
		m_done.wait(lock, [this] { return m_pending == 0; });
		m_job = nullptr;
	}

private:
	void Band(int i, const std::function<void(int, int)>& job)
	{
		const int step = ((W / m_count) + 31) & ~31;
		const int x0 = i * step, x1 = std::min(W, x0 + step);
		if (x0 < x1)
			job(x0, x1);
	}
	void Worker(int i)
	{
		uint64_t seen = 0;
		for (;;)
		{
			const std::function<void(int, int)>* job;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_cv.wait(lock, [&] { return m_gen != seen; });
				seen = m_gen;
				if (m_quit)
					return;
				job = m_job;
			}
			if (job)
				Band(i, *job);
			{
				std::lock_guard<std::mutex> lock(m_lock);
				if (--m_pending == 0)
					m_done.notify_one();
			}
		}
	}
	std::vector<ps5::BigThread> m_threads;
	int m_count = 1;
	std::mutex m_lock;
	std::condition_variable m_cv, m_done;
	uint64_t m_gen = 0;
	int m_pending = 0;
	bool m_quit = false;
	const std::function<void(int, int)>* m_job = nullptr;
};

// ---- the backdrop: the selected cover, blurred, darkened, with a floor and a vignette -----------------
struct Backdrop
{
	std::vector<uint32_t> px; // W*H
	const CoverTex* from = nullptr;
};

void BuildBackdrop(Pool& pool, Backdrop& bd, const CoverTex* tex)
{
	bd.px.resize(size_t(W) * H);
	bd.from = tex;
	// 1. a 64x36 thumbnail: the cover's smallest level, scaled to fill (cropped), or a plain colour
	constexpr int TW = 64, TH = 36;
	float small[TH][TW][3];
	for (int y = 0; y < TH; y++)
		for (int x = 0; x < TW; x++)
		{
			uint32_t p = Rgb(28, 22, 60);
			if (tex)
			{
				const int l = CoverTex::kLevels - 1;
				const int tw = tex->w[l], th = tex->h[l];
				const float s = std::max(float(TW) / tw, float(TH) / th);
				const int sx = std::clamp(int((x - TW / 2.0f) / s + tw / 2.0f), 0, tw - 1);
				const int sy = std::clamp(int((y - TH / 2.0f) / s + th / 2.0f), 0, th - 1);
				p = tex->px[l][size_t(sx) * th + sy];
			}
			small[y][x][0] = float(p & 0xff);
			small[y][x][1] = float((p >> 8) & 0xff);
			small[y][x][2] = float((p >> 16) & 0xff);
		}
	// 2. blur: three box passes each way
	for (int pass = 0; pass < 3; pass++)
	{
		float tmp[TH][TW][3];
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++)
				for (int c = 0; c < 3; c++)
				{
					float s = 0;
					int n = 0;
					for (int k = -3; k <= 3; k++)
					{
						const int xx = std::clamp(x + k, 0, TW - 1);
						s += small[y][xx][c];
						n++;
					}
					tmp[y][x][c] = s / n;
				}
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++)
				for (int c = 0; c < 3; c++)
				{
					float s = 0;
					int n = 0;
					for (int k = -3; k <= 3; k++)
					{
						const int yy = std::clamp(y + k, 0, TH - 1);
						s += tmp[yy][x][c];
						n++;
					}
					small[y][x][c] = s / n;
				}
	}
	// 3. darken towards a deep blue-violet, so text stays readable whatever the art
	const float base[3] = {10, 9, 22};
	for (int y = 0; y < TH; y++)
		for (int x = 0; x < TW; x++)
			for (int c = 0; c < 3; c++)
				small[y][x][c] = small[y][x][c] * 0.36f + base[c] * 0.64f + 6;
	// 4. scale up (bilinear) with a vignette and a darker floor below the covers
	const float floor_y = kHorizon + kFocal * kCamY / kCamDist;
	pool.Run([&](int x0, int x1) {
		for (int y = 0; y < H; y++)
		{
			const float fy = (y + 0.5f) * TH / H - 0.5f;
			const int y0 = std::clamp(int(std::floor(fy)), 0, TH - 1), y1 = std::min(y0 + 1, TH - 1);
			const float wy = std::clamp(fy - y0, 0.0f, 1.0f);
			const float vy = (y - H * 0.45f) / (H * 0.75f);
			float floor_k = 1.0f;
			if (y > floor_y)
				floor_k = 0.78f - 0.25f * std::min(1.0f, (y - floor_y) / (H - floor_y));
			uint32_t* row = &bd.px[size_t(y) * W];
			for (int x = x0; x < x1; x++)
			{
				const float fx = (x + 0.5f) * TW / W - 0.5f;
				const int xa = std::clamp(int(std::floor(fx)), 0, TW - 1), xb = std::min(xa + 1, TW - 1);
				const float wx = std::clamp(fx - xa, 0.0f, 1.0f);
				const float vx = (x - W * 0.5f) / (W * 0.62f);
				const float vig = std::max(0.25f, 1.0f - 0.55f * (vx * vx + vy * vy));
				int ch[3];
				for (int c = 0; c < 3; c++)
				{
					const float top = small[y0][xa][c] * (1 - wx) + small[y0][xb][c] * wx;
					const float bot = small[y1][xa][c] * (1 - wx) + small[y1][xb][c] * wx;
					ch[c] = int((top * (1 - wy) + bot * wy) * vig * floor_k);
				}
				row[x] = Rgb(uint8_t(std::min(255, ch[0])), uint8_t(std::min(255, ch[1])), uint8_t(std::min(255, ch[2])));
			}
		}
	});
	// a faint horizon line on the floor
	const int fy = int(floor_y);
	for (int x = 0; x < W; x++)
	{
		const float k = 1.0f - std::abs(x - W * 0.5f) / (W * 0.5f);
		uint32_t& p = bd.px[size_t(fy) * W + x];
		p = Add32(p, Rgb(uint8_t(30 * k), uint8_t(26 * k), uint8_t(52 * k)));
	}
}

// ---- one cover in 3D ------------------------------------------------------------------------------------
struct Placed
{
	const CoverTex* tex = nullptr;
	float d = 0; // offset from the selection (fractional while gliding)
	float cx = 0, dist = kCamDist; // centre: x, distance from the eye
	float a = 0, b = 0; // x(t) = cx + a*t, dist(t) = dist + b*t, t in [-0.5, 0.5]
	float wh = kCoverH; // world height
	uint32_t bright = 256; // 0..256
	uint32_t alpha = 256; // fade at the ends of the shelf
	int sx0 = 0, sx1 = 0; // screen columns
};

bool Place(Placed& p, const CoverTex* tex, float d)
{
	p.tex = tex;
	p.d = d;
	const float ad = std::abs(d), sgn = d < 0 ? -1.0f : 1.0f;
	const float k = std::min(ad, 1.0f);
	const float e = k * k * (3 - 2 * k); // smoothstep
	const float x = ad <= 1 ? sgn * kSideX * e : sgn * (kSideX + (ad - 1) * kSideStep);
	const float z = kSideZ * e;
	const float ang = kSideAngle * e;
	const float aspect = float(tex->w[0]) / float(tex->h[0]);
	const float ww = kCoverH * aspect;
	p.cx = x;
	p.dist = kCamDist + z;
	p.a = ww * std::cos(ang);
	// the end towards the middle stands further back: t = +0.5 for a left cover, -0.5 for a right one
	p.b = (d < 0 ? 1.0f : -1.0f) * ww * std::sin(ang);
	p.wh = kCoverH;
	float br = 1.0f - 0.40f * e - 0.07f * std::max(0.0f, ad - 1);
	p.bright = uint32_t(std::clamp(br, 0.25f, 1.0f) * 256);
	p.alpha = uint32_t(std::clamp((kVisible - ad) / 1.0f, 0.0f, 1.0f) * 256);
	// screen extent
	float xs[2];
	for (int i = 0; i < 2; i++)
	{
		const float t = i == 0 ? -0.5f : 0.5f;
		const float dd = p.dist + p.b * t;
		if (dd < 50)
			return false;
		xs[i] = W * 0.5f + kFocal * (p.cx + p.a * t) / dd;
	}
	p.sx0 = int(std::floor(std::min(xs[0], xs[1])));
	p.sx1 = int(std::ceil(std::max(xs[0], xs[1])));
	return p.sx1 > 0 && p.sx0 < W && p.alpha > 0;
}

// Draws columns [x0, x1) of one placed cover: the cover itself and its reflection on the floor.
void DrawCover(const Placed& p, uint32_t* surf, int x0, int x1)
{
	const CoverTex& t = *p.tex;
	const int cx0 = std::max(x0, p.sx0), cx1 = std::min(x1, p.sx1);
	for (int x = cx0; x < cx1; x++)
	{
		const float X = (x + 0.5f - W * 0.5f) / kFocal;
		const float den = p.a - X * p.b;
		if (std::abs(den) < 1e-4f)
			continue;
		const float tt = (X * p.dist - p.cx) / den; // -0.5 .. 0.5
		if (tt < -0.5f || tt > 0.5f)
			continue;
		const float dd = p.dist + p.b * tt;
		const float top = kHorizon - kFocal * (p.wh - kCamY) / dd;
		const float bot = kHorizon + kFocal * kCamY / dd;
		const float span = bot - top;
		// mip level from the vertical scale
		int l = 0;
		const float scale = span / float(t.h[0]);
		if (scale < 0.38f)
			l = 2;
		else if (scale < 0.75f)
			l = 1;
		const int tw = t.w[l], th = t.h[l];
		const uint32_t* px = t.px[l].data();
		const float u = (tt + 0.5f) * tw - 0.5f;
		const int u0 = std::clamp(int(std::floor(u)), 0, tw - 1), u1 = std::min(u0 + 1, tw - 1);
		const uint32_t fu = uint32_t(std::clamp(u - std::floor(u), 0.0f, 1.0f) * 256);
		const uint32_t* c0 = px + size_t(u0) * th;
		const uint32_t* c1 = px + size_t(u1) * th;
		const float dv = th / span;
		// edge columns: soften the vertical edges a little
		uint32_t edge = 256;
		if (x == p.sx0 || x == p.sx1 - 1)
			edge = 140;
		const uint32_t bright = p.bright;
		const uint32_t alpha = (p.alpha * edge) >> 8;

		// fixed point: v in 16.16
		const int32_t vmax = (th - 1) << 16;
		auto sample = [&](int32_t v) -> uint32_t {
			v = std::clamp(v, 0, vmax - 1);
			const int y0 = v >> 16;
			const uint32_t fv = uint32_t(v >> 8) & 0xff;
			const uint32_t a0 = Lerp32(c0[y0], c0[y0 + 1], fv);
			const uint32_t a1 = Lerp32(c1[y0], c1[y0 + 1], fv);
			return Lerp32(a0, a1, fu);
		};
		const int32_t dv16 = int32_t(dv * 65536.0f);

		// the cover: full rows in the middle, partial coverage on the first and last row
		const int ytop = std::max(0, int(std::floor(top)));
		const int ybot = std::min(H - 1, int(std::ceil(bot)) - 1);
		int32_t v = int32_t(((ytop + 0.5f - top) * dv - 0.5f) * 65536.0f);
		uint32_t* d = surf + size_t(ytop) * W + x;
		for (int y = ytop; y <= ybot; y++, v += dv16, d += W)
		{
			uint32_t a = alpha;
			if (y == ytop || y == ybot)
			{
				float cov = 1.0f;
				if (y < top)
					cov -= top - y;
				if (y + 1 > bot)
					cov -= (y + 1) - bot;
				if (cov <= 0)
					continue;
				a = (alpha * uint32_t(cov * 256)) >> 8;
			}
			const uint32_t c = Scale32(sample(v), bright);
			*d = a >= 256 ? c : Lerp32(*d, c, a);
		}
		// the reflection, fading out, mirrored from the cover's bottom edge up
		const float rlen = span * kReflect;
		const int r0 = std::max(0, int(std::ceil(bot)));
		const int r1 = std::min(H - 1, int(bot + rlen));
		const float inv_rlen = 1.0f / rlen;
		int32_t rv = int32_t((float(th - 1) - (r0 + 0.5f - bot) * dv) * 65536.0f);
		uint32_t* rd = surf + size_t(r0) * W + x;
		for (int y = r0; y <= r1; y++, rv -= dv16, rd += W)
		{
			const float k = (y + 0.5f - bot) * inv_rlen;
			if (k >= 1)
				break;
			const uint32_t a = uint32_t(0.30f * (1 - k) * (1 - k) * alpha);
			if (a == 0)
				continue;
			*rd = Lerp32(*rd, Scale32(sample(std::max(0, rv)), bright), a);
		}
	}
}

// The glow behind the middle cover, in the cover's average colour.
void DrawGlow(const Placed& p, uint32_t* surf, int x0, int x1, float strength)
{
	if (strength <= 0)
		return;
	const float top = kHorizon - kFocal * (p.wh - kCamY) / p.dist;
	const float bot = kHorizon + kFocal * kCamY / p.dist;
	const float l = float(p.sx0), r = float(p.sx1);
	const float R = 70.0f;
	const uint32_t col = p.tex->average;
	const int gx0 = std::max(x0, int(l - R)), gx1 = std::min(x1, int(r + R));
	const int gy0 = std::max(0, int(top - R)), gy1 = std::min(H, int(bot + R * 0.6f));
	for (int y = gy0; y < gy1; y++)
	{
		const float dy = std::max({top - y, 0.0f, y - bot});
		const bool inside_rows = dy == 0;
		uint32_t* row = surf + size_t(y) * W;
		for (int x = gx0; x < gx1; x++)
		{
			if (inside_rows && x >= int(l) && x < int(r))
			{
				x = int(r) - 1; // the cover covers this part
				continue;
			}
			const float dx = std::max({l - x, 0.0f, x - r});
			const float dist2 = dx * dx + dy * dy;
			if (dist2 >= R * R)
				continue;
			const float k = 1 - std::sqrt(dist2) / R;
			const uint32_t a = uint32_t(k * k * strength * 0.85f * 256);
			row[x] = Add32(row[x], Scale32(col, a));
		}
	}
}

// ---- input: left/right with key repeat that speeds up -------------------------------------------------
struct Repeat
{
	double next = 0, since = 0;
	bool held = false;
	bool Fire(bool down, double now)
	{
		if (!down)
		{
			held = false;
			return false;
		}
		if (!held)
		{
			held = true;
			since = now;
			next = now + 0.32;
			return true;
		}
		if (now >= next)
		{
			next = now + (now - since > 1.4 ? 0.045 : 0.095);
			return true;
		}
		return false;
	}
};

void CenterText(int y, const std::string& s, int scale, uint32_t color)
{
	const std::string fit = FitText(s, scale, W - 200);
	DrawText((W - TextWidth(fit.c_str(), scale)) / 2, y, fit.c_str(), scale, color);
}

bool Confirm(const char* question)
{
	std::vector<uint32_t> saved(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
	ps5input::Poll();
	uint32_t prev = ps5input::Pad(0).buttons;
	ps5video::DarkenRect(0, 0, W, H);
	const int bw = 900, bh = 240, bx = (W - bw) / 2, by = (H - bh) / 2;
	ps5video::FillRect(bx - 3, by - 3, bw + 6, bh + 6, Rgb(150, 125, 255));
	ps5video::FillRect(bx, by, bw, bh, Rgb(26, 24, 48));
	DrawText(bx + 50, by + 50, question, 5, Rgb(240, 240, 250));
	DrawText(bx + 50, by + 160, (std::string(icon::Cross) + " Yes       " + icon::Circle + " No").c_str(), 3, Rgb(170, 160, 210));
	ps5video::Present(0, 0, 0, 0, true);
	for (;;)
	{
		ps5input::Poll();
		const uint32_t cur = ps5input::Pad(0).buttons;
		const uint32_t down = cur & ~prev;
		prev = cur;
		if (down & SCE_PAD_BUTTON_CROSS)
			return true;
		if (down & (SCE_PAD_BUTTON_CIRCLE | SCE_PAD_BUTTON_OPTIONS))
		{
			memcpy(ps5video::Surface(), saved.data(), saved.size() * 4);
			return false;
		}
		ps5video::Present(0, 0, 1, 1, true);
	}
}

// ---- state kept between visits to the shelf (the game list, its covers) -----------------------------------
struct ShelfState
{
	std::vector<GameInfo> games;
	CoverService covers;
	std::vector<CoverPtr> slots;
	bool started = false;
	bool downloads = true;
};
ShelfState& S()
{
	static ShelfState* s = new ShelfState(); // never destroyed: the cover thread may outlive main's statics
	return *s;
}

std::shared_ptr<CoverTex> LoadingCard()
{
	static std::shared_ptr<CoverTex> card;
	if (card)
		return card;
	const int w = 716, h = kCoverTexH;
	auto tex = std::make_shared<CoverTex>();
	int lw = w, lh = h;
	for (int l = 0; l < CoverTex::kLevels; l++)
	{
		tex->w[l] = lw;
		tex->h[l] = lh;
		tex->px[l].resize(size_t(lw) * lh);
		for (int x = 0; x < lw; x++)
			for (int y = 0; y < lh; y++)
			{
				const bool border = x < 3 || y < 3 || x >= lw - 3 || y >= lh - 3;
				const float t = float(y) / lh;
				tex->px[l][size_t(x) * lh + y] = border ? Rgb(80, 70, 130) : Rgb(uint8_t(36 - 14 * t), uint8_t(32 - 12 * t), uint8_t(64 - 26 * t));
			}
		lw /= 2;
		lh /= 2;
	}
	tex->average = Rgb(60, 50, 110);
	card = tex;
	return card;
}

void Loading(const char* text)
{
	ps5video::FillRect(0, 0, W, H, Rgb(12, 10, 26));
	DrawText(80, 50, "Snes9x PS5", 6, Rgb(150, 125, 255));
	CenterText(H / 2 - 20, text, 4, Rgb(220, 220, 235));
	ps5video::Present(0, 0, 0, 0, true);
}
} // namespace

// Retries missing box art using the existing helper; never removes a user's own cover.
int RepairMissingCovers()
{
	ShelfState& st = S();
	int queued = 0;
	for (size_t i = 0; i < st.games.size(); ++i)
	{
		const GameInfo& game = st.games[i];
		const std::string name = CoverNameFor(game);
		const std::string cache = OrbisDir("covers") + "/" + ThumbnailName(name) + ".png";
		if (i < st.slots.size() && st.slots[i] && st.slots[i]->real) continue;
		if (i < st.slots.size() && !st.slots[i] && OrbisIsFile(cache)) continue;
		st.covers.Refetch(int(i));
		++queued;
	}
	WriteWantedList(MissingCovers(st.games));
	if (!ShelfDownloads())
		CoversRestartIfNeeded(st.games, Config().covers_download, true, Loading);
	OrbisLog("[covers] user requested repair of %d missing covers", queued);
	return queued;
}

void ShelfShutdown()
{
	S().covers.Stop();
}

std::string Shelf()
{
	Settings& cfg = Config();
	ShelfState& st = S();
	S9X_STAGE(Shelf, "pool");
	Pool pool;

	S9X_STAGE(Shelf, "scan games");
	Loading("Looking for games...");
	std::vector<GameInfo> games = ScanGames();
	bool same = games.size() == st.games.size();
	for (size_t i = 0; same && i < games.size(); i++)
		same = games[i].path == st.games[i].path;
	if (!ShelfDownloads() && (!same || !st.started))
	{
		// the console: covers come from the prefetch at start (fe_prefetch.h); new ones restart the app
		S9X_STAGE(Shelf, "wanted covers");
		CoversRestartIfNeeded(games, cfg.covers_download, false, Loading);
	}
	if (!same || !st.started || st.downloads != cfg.covers_download)
	{
		st.games = games;
		st.slots.assign(games.size(), nullptr);
		st.downloads = cfg.covers_download;
		S9X_STAGE(Shelf, "cover service start");
		st.covers.Start(st.games, cfg.covers_download && ShelfDownloads(), cfg.covers_download && BackgroundCovers());
		st.started = true;
	}
	const int n = int(st.games.size());

	int sel = 0;
	for (int i = 0; i < n; i++)
		if (st.games[size_t(i)].path == cfg.last_rom)
			sel = i;
	float pos = float(sel);
	st.covers.SetFocus(sel);

	Backdrop bd_cur, bd_old;
	float fade = 1.0f; // 0..1: old -> cur
	S9X_STAGE(Shelf, "build backdrop");
	BuildBackdrop(pool, bd_cur, nullptr);
	S9X_STAGE(Shelf, "first frame");
	const CoverTex* bd_wanted = nullptr;
	double sel_since = Now();

	Repeat rep_l, rep_r, rep_l1, rep_r1;
	ps5input::Poll();
	uint32_t prev = ps5input::Pad(0).buttons;
	double last = Now();
	bool dirty = true;
	int last_status = -1;
	int bulk_covers = 0;
	double bulk_covers_until = 0;

	for (;;)
	{
		S9X_STAGE(Shelf, "loop");
		const double now = Now();
		const float dt = float(std::min(0.05, now - last));
		last = now;

		// -- input
		ps5input::Poll();
		const uint32_t cur = ps5input::Pad(0).buttons;
		const uint32_t down = cur & ~prev;
		prev = cur;
		const int old_sel = sel;
		if (n > 0)
		{
			if (rep_l.Fire(cur & SCE_PAD_BUTTON_LEFT, now))
				sel = std::max(0, sel - 1);
			if (rep_r.Fire(cur & SCE_PAD_BUTTON_RIGHT, now))
				sel = std::min(n - 1, sel + 1);
			if (rep_l1.Fire(cur & SCE_PAD_BUTTON_L1, now))
				sel = std::max(0, sel - 10);
			if (rep_r1.Fire(cur & SCE_PAD_BUTTON_R1, now))
				sel = std::min(n - 1, sel + 10);
		}
		if (sel != old_sel)
		{
			st.covers.SetFocus(sel);
			sel_since = now;
			dirty = true;
		}
		if ((down & SCE_PAD_BUTTON_CROSS) && n > 0)
		{
			cfg.last_rom = st.games[size_t(sel)].path;
			cfg.Save();
			return st.games[size_t(sel)].path;
		}
		if ((down & SCE_PAD_BUTTON_R2) && n > 0)
		{
			const GameInfo& entry = st.games[size_t(sel)];
			CheatDownloadsMenu({entry.file_base, entry.nointro});
			ps5input::Poll();
			prev = ps5input::Pad(0).buttons;
			dirty = true;
		}
		if (down & SCE_PAD_BUTTON_R3)
		{
			bulk_covers = RepairMissingCovers();
			bulk_covers_until = now + 5.0;
			dirty = true;
		}
		if (down & SCE_PAD_BUTTON_TRIANGLE)
		{
			SettingsMenu();
			if (st.downloads != cfg.covers_download)
			{
				st.downloads = cfg.covers_download;
				st.covers.Start(st.games, cfg.covers_download && ShelfDownloads(), cfg.covers_download && BackgroundCovers());
				st.slots.assign(st.games.size(), nullptr);
			}
			ps5input::Poll();
			prev = ps5input::Pad(0).buttons;
			dirty = true;
		}
		if ((down & SCE_PAD_BUTTON_SQUARE) && n > 0)
		{
			st.covers.Refetch(sel);
			st.slots[size_t(sel)] = nullptr;
			dirty = true;
			if (!ShelfDownloads()) // the console: the helper fetches it (or the prefetch at the next start)
			{
				cfg.last_rom = st.games[size_t(sel)].path;
				cfg.Save();
				CoversRestartIfNeeded(st.games, cfg.covers_download, true, Loading);
			}
		}
		if (down & SCE_PAD_BUTTON_OPTIONS)
		{
			if (Confirm("Quit Snes9x PS5?"))
				return "";
			ps5input::Poll();
			prev = ps5input::Pad(0).buttons;
			dirty = true;
		}

		// -- covers that arrived
		const size_t before = std::count_if(st.slots.begin(), st.slots.end(), [](const CoverPtr& c) { return c != nullptr; });
		st.covers.Collect(st.slots, kKeepRadius);
		const size_t after = std::count_if(st.slots.begin(), st.slots.end(), [](const CoverPtr& c) { return c != nullptr; });
		if (after != before)
			dirty = true;

		// -- glide
		const float target = float(sel);
		if (std::abs(target - pos) > 0.0005f)
		{
			pos += (target - pos) * (1.0f - std::exp(-dt * 11.0f));
			if (std::abs(target - pos) < 0.0005f)
				pos = target;
			dirty = true;
		}

		// -- backdrop follows the selection once it rests for a moment
		const CoverTex* sel_tex = (n > 0 && st.slots[size_t(sel)]) ? st.slots[size_t(sel)].get() : nullptr;
		if (sel_tex && sel_tex != bd_cur.from && sel_tex != bd_wanted && now - sel_since > 0.12)
		{
			bd_wanted = sel_tex;
			std::swap(bd_old, bd_cur);
			BuildBackdrop(pool, bd_cur, sel_tex);
			fade = bd_old.px.empty() ? 1.0f : 0.0f;
			dirty = true;
		}
		if (fade < 1.0f)
		{
			fade = std::min(1.0f, fade + dt * 4.0f);
			dirty = true;
		}

		const int status = (st.covers.ToDownload() + st.covers.BackgroundLeft()) * 1000 + st.covers.Downloaded() +
						   (st.covers.BackgroundOffline() ? 1 << 30 : 0);
		if (status != last_status)
		{
			last_status = status;
			dirty = true;
		}

		if (!dirty)
		{
			// nothing moved: flip a 1-pixel change so the loop still runs at the display's pace
			ps5video::Present(0, 0, 1, 1, true);
			continue;
		}
		dirty = false;

		// -- place the covers, far ones first
		std::vector<Placed> placed;
		if (n > 0)
		{
			const int lo = std::max(0, int(std::floor(pos - kVisible)));
			const int hi = std::min(n - 1, int(std::ceil(pos + kVisible)));
			for (int i = lo; i <= hi; i++)
			{
				const CoverTex* tex = st.slots[size_t(i)] ? st.slots[size_t(i)].get() : LoadingCard().get();
				Placed p;
				if (Place(p, tex, float(i) - pos))
					placed.push_back(p);
			}
			std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return std::abs(a.d) > std::abs(b.d); });
		}
		uint32_t* surf = ps5video::Surface();
		const uint32_t fade_w = uint32_t(fade * 256);
		const double t_render = Now();
		pool.Run([&](int x0, int x1) {
			// backdrop
			for (int y = 0; y < H; y++)
			{
				uint32_t* out = surf + size_t(y) * W;
				const uint32_t* a = bd_cur.px.data() + size_t(y) * W;
				if (fade_w >= 256 || bd_old.px.empty())
					memcpy(out + x0, a + x0, size_t(x1 - x0) * 4);
				else
				{
					const uint32_t* o = bd_old.px.data() + size_t(y) * W;
					for (int x = x0; x < x1; x++)
						out[x] = Lerp32(o[x], a[x], fade_w);
				}
			}
			// covers; the glow goes right before the middle one
			for (const Placed& p : placed)
			{
				if (std::abs(p.d) < 0.5f)
					DrawGlow(p, surf, x0, x1, 1.0f - std::abs(p.d) * 2.0f);
				DrawCover(p, surf, x0, x1);
			}
		});

		{
			static double sum = 0;
			static int frames = 0;
			sum += Now() - t_render;
			if (++frames == 60)
			{
				OrbisLog("[shelf] 3D pass: %.2f ms a frame (average of 60)", sum / frames * 1000.0);
				sum = 0;
				frames = 0;
			}
		}
		// -- text
		DrawText(80, 46, "Snes9x PS5", 6, Rgb(170, 150, 255));
		const char* ver = "Snes9x " VERSION " - PS5 " SNES9X_PS5_VERSION;
		DrawText(84, 118, ver, 2, Rgb(150, 145, 180));
		// the author's line, under the wordmark, as PS5SX2 shows its author's handles (fe_app.cpp)
		DrawText(84, 150, (std::string(icon::GitHub) + " " + kAuthorGitHub).c_str(), 3, Rgb(160, 152, 200));
		if (n > 0)
		{
			char count[64];
			snprintf(count, sizeof(count), "%d / %d", sel + 1, n);
			DrawText(W - 80 - TextWidth(count, 4), 50, count, 4, Rgb(220, 220, 235));
			const GameInfo& g = st.games[size_t(sel)];
			CenterText(800, g.title, 6, Rgb(245, 245, 252));
			std::string sub;
			if (!g.region.empty())
				sub = g.region + "   ";
			std::string ext = g.ext.size() > 1 ? g.ext.substr(1) : g.ext;
			for (char& c : ext)
				c = char(toupper(uint8_t(c)));
			sub += ext;
			if (g.on_usb)
				sub += "   USB";
			if (sel_tex && !sel_tex->real)
				sub += "   no cover";
			CenterText(880, sub, 3, Rgb(175, 170, 205));
		}
		else
		{
			CenterText(300, "No games found.", 5, Rgb(240, 240, 250));
			CenterText(420, "Copy your SNES ROMs (.sfc .smc .zip ...) to", 3, Rgb(200, 200, 220));
			CenterText(470, "/data/snes9x/roms   or   snes9x/roms  on a USB drive", 4, Rgb(200, 190, 255));
			CenterText(560, "and open Snes9x PS5 again.", 3, Rgb(200, 200, 220));
		}
		// download status
		if (st.covers.ToDownload() > 0 && !st.covers.Offline() && cfg.covers_download)
		{
			char s[96];
			snprintf(s, sizeof(s), "Downloading covers... %d left", st.covers.ToDownload());
			DrawText(W - 80 - TextWidth(s, 3), 112, s, 3, Rgb(170, 160, 210));
		}
		else if (st.covers.BackgroundLeft() > 0 && !st.covers.BackgroundOffline() && cfg.covers_download)
		{
			char s[96];
			snprintf(s, sizeof(s), "Downloading covers in the background... %d left", st.covers.BackgroundLeft());
			DrawText(W - 80 - TextWidth(s, 3), 112, s, 3, Rgb(170, 160, 210));
		}
		else if ((st.covers.Offline() || (st.covers.BackgroundLeft() > 0 && st.covers.BackgroundOffline())) &&
				 cfg.covers_download)
		{
			const char* s = "Offline: covers next time";
			DrawText(W - 80 - TextWidth(s, 3), 112, s, 3, Rgb(170, 160, 210));
		}
		if (now < bulk_covers_until)
		{
			char notice[128];
			snprintf(notice, sizeof(notice), "Retrying %d missing covers (box art, then title screens)", bulk_covers);
			CenterText(950, notice, 3, Rgb(225, 220, 245));
		}
		// hints
		ps5video::DarkenRect(0, H - 78, W, 78);
		const std::string sp = "      ";
		const std::string hint = n > 0
			? std::string(icon::Cross) + " Play" + sp + icon::DpadLeftRight + " Browse" + sp + icon::L1 + " " + icon::R1 +
				" Skip 10" + sp + icon::Triangle + " Settings" + sp + icon::Square + " Get cover" + sp +
				icon::R2 + " Cheats" + sp + icon::R3 + " Fix Covers" + sp + icon::Options + " Quit"
			: std::string(icon::Triangle) + " Settings" + sp + icon::Options + " Quit";
		DrawText((W - TextWidth(hint.c_str(), 2)) / 2, H - 54, hint.c_str(), 2, Rgb(205, 200, 228));

		ps5video::Present(0, 0, 0, 0, true);
	}
}
} // namespace fe

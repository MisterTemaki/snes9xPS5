// Snes9x PS5 frontend: covers (fe_covers.h).
// SPDX-License-Identifier: MIT

#include "fe_covers.h"
#include "fe_titlematch.h"
#include "fe_artaliases.h"

#include "fe_coverworker.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"

#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// a cover is a few hundred pixels: a bigger image (a broken or hostile file) is refused before stb_image
// allocates for it (its default limit, 2^24 pixels a side, would let one PNG ask for gigabytes)
#define STBI_MAX_DIMENSIONS 8192
#include "third_party/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include "third_party/stb_image_resize2.h"

#ifndef COVER_MANIFEST_TXT
#error COVER_MANIFEST_TXT must name the built-in Libretro box art index
#endif
#define S9X_COVER_INCBIN(sym,path) \
  __asm__(".section .rodata\n.global " #sym "_begin\n.balign 16\n" #sym "_begin:\n" \
          ".incbin \"" path "\"\n.global " #sym "_end\n" #sym "_end:\n.previous\n"); \
  extern "C" const char sym##_begin[]; \
  extern "C" const char sym##_end[];
S9X_COVER_INCBIN(s9x_cover_index, COVER_MANIFEST_TXT)
#ifndef TITLE_MANIFEST_TXT
#error TITLE_MANIFEST_TXT must refer to Libretro title-screen index
#endif
#ifndef SNAP_MANIFEST_TXT
#error SNAP_MANIFEST_TXT must refer to Libretro screenshot index
#endif
S9X_COVER_INCBIN(s9x_title_index, TITLE_MANIFEST_TXT)
S9X_COVER_INCBIN(s9x_snap_index, SNAP_MANIFEST_TXT)

namespace fe
{
namespace
{
std::vector<std::string> ParseArtIndex(const char* p, const char* end)
{
    std::vector<std::string> names;
    while (p < end)
    {
        const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
        if (!nl) break;
        if (*p != '#' && nl > p) names.emplace_back(p, size_t(nl - p));
        p = nl + 1;
    }
    return names;
}
const std::vector<std::string>& BoxArtNames()
{
    static const auto names = ParseArtIndex(s9x_cover_index_begin, s9x_cover_index_end);
    return names;
}
const std::vector<std::string>& TitleArtNames()
{
    static const auto names = ParseArtIndex(s9x_title_index_begin, s9x_title_index_end);
    return names;
}
const std::vector<std::string>& SnapArtNames()
{
    static const auto names = ParseArtIndex(s9x_snap_index_begin, s9x_snap_index_end);
    return names;
}
std::string MatchArtName(const std::vector<std::string>& names, const GameInfo& game)
{
    if (artaliases::DistinctBootleg(game.file_base))
    {
        // Bootleg title wins over a misleading underlying-ROM CRC. Use only
        // true, exact bootleg artwork when present, never another game's.
        const std::string title=artaliases::TrimFrontendSuffix(game.file_base);
        return std::find(names.begin(),names.end(),title)!=names.end() ? title : "";
    }
    // A translated/hacked ROM's filename can be unlike the original Japanese
    // box-art entry. Try only verified aliases for such known games.
    const std::string alias = artaliases::Canonical(game.file_base);
    if (!alias.empty() && std::find(names.begin(), names.end(), alias) != names.end())
        return alias;
    const std::string requested = artaliases::DistinctBootleg(game.file_base) || game.nointro.empty()
        ? game.file_base : game.nointro;
    auto exact = std::find(names.begin(), names.end(), requested);
    if (exact != names.end()) return *exact; // preserve revisions and git symlinks
    std::string result = titles::Best(names, requested);
    if (result.empty())
    {
        const std::string cleaned = artaliases::TrimFrontendSuffix(game.file_base);
        result = titles::Best(names, cleaned);
    }
    return result;
}
std::string MatchFallbackArt(const std::vector<std::string>& names, const GameInfo& game)
{
    const std::string requested = artaliases::DistinctBootleg(game.file_base) || game.nointro.empty()
        ? game.file_base : game.nointro;
    static std::mutex match_mutex;
    static std::unordered_map<std::string, std::string> cache;
    // The lookup cache is shared safely by the cover thread and the menu.
    const std::string key = (&names == &TitleArtNames() ? "T:" : "S:") + requested + "|" + game.file_base;
    {
        std::lock_guard<std::mutex> lock(match_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    const std::string result = MatchArtName(names, game);
    {
        std::lock_guard<std::mutex> lock(match_mutex);
        cache.emplace(key, result);
    }
    if (!result.empty() && result != requested)
        OrbisLog("[covers] alternative art title: %s -> %s", requested.c_str(), result.c_str());
    return result;
}

constexpr int kLoadRadius = 12; // textures kept around the selection
constexpr long kMissingRetrySeconds = 30L * 24 * 3600;

// the status line's counter: the worker counts down, Refetch (another thread) up
void DecrementToZero(std::atomic<int>& a)
{
	int v = a.load();
	while (v > 0 && !a.compare_exchange_weak(v, v - 1))
	{
	}
}

const char* const kDefaultUrl =
	"https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System/master/"
	"Named_Boxarts/${name}.png";

std::string CoverUrlTemplate()
{
	const char* env = getenv("SNES9X_COVER_URL"); // the host tests point this at a local server
	return env && *env ? env : kDefaultUrl;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		out.insert(out.end(), buf, buf + n);
		if (out.size() > (32u << 20))
			break;
	}
	fclose(f);
	return !out.empty();
}

bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& data)
{
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

bool NonEmptyFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

std::string FindWithExts(const std::string& base)
{
	static const char* const exts[] = {".png", ".jpg", ".jpeg", ".PNG", ".JPG"};
	for (const char* e : exts)
		if (NonEmptyFile(base + e))
			return base + e;
	return "";
}

// Square on the shelf: "<cover>.refetch" asks for the cover again. The cover itself stays where it is until a new
// one is saved over it (atomically), so a refetch that fails (offline, 404, no restart) never loses it.
std::string RefetchMarker(const std::string& cover)
{
	return cover.substr(0, cover.size() - 4) + ".refetch";
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

bool RecentlyMissing(const std::string& marker, const std::string& url)
{
    struct stat st = {};
    if (stat(marker.c_str(), &st) != 0 || time(nullptr) - st.st_mtime >= kMissingRetrySeconds)
        return false;
    // A 404 from an older artwork index should not suppress a newly found
    // box/title/screenshot URL for the next 30 days.
    FILE* f = fopen(marker.c_str(), "r");
    if (!f) return false;
    char line[8192] = {};
    const bool read = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    if (!read) return false;
    std::string old(line);
    while (!old.empty() && (old.back() == '\n' || old.back() == '\r'))
        old.pop_back();
    return old == url;
}

uint32_t Pack(int r, int g, int b)
{
	return 0xff000000u | (uint32_t(std::clamp(b, 0, 255)) << 16) | (uint32_t(std::clamp(g, 0, 255)) << 8) |
		   uint32_t(std::clamp(r, 0, 255));
}

// Row-major RGBA (h rows of w) -> a CoverTex with 3 levels, column-major.
std::shared_ptr<CoverTex> MakeTex(const uint32_t* rgba, int w, int h)
{
	auto tex = std::make_shared<CoverTex>();
	std::vector<uint32_t> level(rgba, rgba + size_t(w) * h);
	int lw = w, lh = h;
	uint64_t sr = 0, sg = 0, sb = 0;
	for (int l = 0; l < CoverTex::kLevels; l++)
	{
		tex->w[l] = lw;
		tex->h[l] = lh;
		tex->px[l].resize(size_t(lw) * lh);
		for (int y = 0; y < lh; y++)
			for (int x = 0; x < lw; x++)
				tex->px[l][size_t(x) * lh + y] = level[size_t(y) * lw + x];
		if (l + 1 == CoverTex::kLevels)
		{
			for (uint32_t p : level)
			{
				sr += p & 0xff;
				sg += (p >> 8) & 0xff;
				sb += (p >> 16) & 0xff;
			}
			const uint64_t n = std::max<uint64_t>(1, level.size());
			tex->average = Pack(int(sr / n), int(sg / n), int(sb / n));
			break;
		}
		// 2x2 box down
		const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
		std::vector<uint32_t> next(size_t(nw) * nh);
		for (int y = 0; y < nh; y++)
			for (int x = 0; x < nw; x++)
			{
				const uint32_t* r0 = &level[size_t(std::min(2 * y, lh - 1)) * lw];
				const uint32_t* r1 = &level[size_t(std::min(2 * y + 1, lh - 1)) * lw];
				const int x0 = std::min(2 * x, lw - 1), x1 = std::min(2 * x + 1, lw - 1);
				const uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
				uint32_t out = 0xff000000u;
				for (int sh = 0; sh < 24; sh += 8)
				{
					const uint32_t v = (((a >> sh) & 0xff) + ((b >> sh) & 0xff) + ((c >> sh) & 0xff) + ((d >> sh) & 0xff) + 2) / 4;
					out |= v << sh;
				}
				next[size_t(y) * nw + x] = out;
			}
		level.swap(next);
		lw = nw;
		lh = nh;
	}
	return tex;
}

std::shared_ptr<CoverTex> Decode(const std::vector<uint8_t>& file)
{
	int w = 0, h = 0, comp = 0;
	stbi_uc* img = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
	if (!img || w < 8 || h < 8)
	{
		if (img)
			stbi_image_free(img);
		return nullptr;
	}
	// level 0: kCoverTexH tall, the art's own aspect (kept between 0.5:1 and 1.6:1)
	const float aspect = std::clamp(float(w) / float(h), 0.5f, 1.6f);
	const int th = kCoverTexH;
	const int tw = int(std::lround(th * aspect)) & ~3;
	std::vector<uint32_t> out(size_t(tw) * th);
	stbir_resize_uint8_srgb(img, w, h, 0, reinterpret_cast<unsigned char*>(out.data()), tw, th, 0, STBIR_RGBA);
	stbi_image_free(img);
	for (uint32_t& p : out)
		p |= 0xff000000u; // covers are opaque
	return MakeTex(out.data(), tw, th);
}

std::shared_ptr<CoverTex> Placeholder(const GameInfo& g)
{
	const int w = 720, h = kCoverTexH;
	std::vector<uint32_t> px(size_t(w) * h);
	for (int y = 0; y < h; y++)
	{
		const float t = float(y) / h;
		const uint32_t c = Pack(int(52 - 30 * t), int(42 - 24 * t), int(96 - 52 * t));
		std::fill(px.begin() + size_t(y) * w, px.begin() + size_t(y + 1) * w, c);
	}
	// frame
	const uint32_t edge = Pack(150, 125, 255);
	for (int i = 0; i < 6; i++)
		for (int x = 0; x < w; x++)
		{
			px[size_t(i) * w + x] = edge;
			px[size_t(h - 1 - i) * w + x] = edge;
		}
	for (int y = 0; y < h; y++)
		for (int i = 0; i < 6; i++)
		{
			px[size_t(y) * w + i] = edge;
			px[size_t(y) * w + w - 1 - i] = edge;
		}
	// four coloured buttons, top right
	const uint32_t dots[4] = {Pack(225, 60, 70), Pack(240, 200, 60), Pack(70, 180, 90), Pack(70, 110, 230)};
	const int cx[4] = {w - 130, w - 95, w - 60, w - 95}, cy[4] = {80, 50, 80, 110};
	for (int k = 0; k < 4; k++)
		for (int y = -14; y <= 14; y++)
			for (int x = -14; x <= 14; x++)
				if (x * x + y * y <= 196)
					px[size_t(cy[k] + y) * w + cx[k] + x] = dots[k];

	Canvas canvas{px.data(), w, h, w};
	// the title, word-wrapped, up to 4 lines
	std::vector<std::string> lines;
	std::string cur;
	size_t pos = 0;
	const std::string& t = g.title;
	while (pos <= t.size())
	{
		size_t sp = t.find(' ', pos);
		if (sp == std::string::npos)
			sp = t.size();
		const std::string word = t.substr(pos, sp - pos);
		const std::string tryline = cur.empty() ? word : cur + " " + word;
		if (!cur.empty() && TextWidth(tryline.c_str(), 5) > w - 80)
		{
			lines.push_back(cur);
			cur = word;
		}
		else
			cur = tryline;
		pos = sp + 1;
	}
	if (!cur.empty())
		lines.push_back(cur);
	if (lines.size() > 4)
	{
		lines.resize(4);
		lines[3] = FitText(lines[3] + "...", 5, w - 80);
	}
	int y = 190 - int(lines.size()) * 28;
	for (const std::string& l : lines)
	{
		const std::string fit = FitText(l, 5, w - 80);
		DrawTextOn(canvas, (w - TextWidth(fit.c_str(), 5)) / 2, y, fit.c_str(), 5, Pack(240, 240, 250));
		y += 58;
	}
	const char* sub = "SUPER FAMICOM / SNES";
	DrawTextOn(canvas, (w - TextWidth(sub, 3)) / 2, h - 80, sub, 3, Pack(170, 160, 210));
	auto tex = MakeTex(px.data(), w, h);
	tex->source = "placeholder";
	return tex;
}
} // namespace

std::string CoverNameFor(const GameInfo& game)
{
    const std::string requested = artaliases::DistinctBootleg(game.file_base) || game.nointro.empty()
        ? game.file_base : game.nointro;
    const std::string cache_key = requested + "|" + game.file_base;
    static std::mutex mutex;
    static std::unordered_map<std::string,std::string> cache;
    {
        std::lock_guard<std::mutex> guard(mutex);
        const auto it = cache.find(cache_key);
        if (it!=cache.end()) return it->second;
    }
    const std::string best_boxart = MatchArtName(BoxArtNames(), game);
    std::string best = best_boxart;
    if (best.empty()) best = requested; // fallback: title art/screenshot or 404 marker
    if (best!=requested) OrbisLog("[covers] fuzzy art: %s -> %s", requested.c_str(), best.c_str());
    {
        std::lock_guard<std::mutex> guard(mutex);
        cache.emplace(cache_key,best);
    }
    return best;
}

std::string ThumbnailName(const std::string& nointro)
{
	std::string s = nointro;
	for (char& c : s)
		if (strchr("&*/:`<>?\\|\"", c))
			c = '_';
	return s;
}

namespace
{
std::atomic<bool> g_shelf_downloads{true};
}

void SetShelfDownloads(bool on)
{
	g_shelf_downloads = on;
}

bool ShelfDownloads()
{
	return g_shelf_downloads;
}

std::string CoverUrlFor(const std::string& nointro)
{
    std::string url = CoverUrlTemplate();
    const size_t p = url.find("${name}");
    if (p != std::string::npos)
        url.replace(p, 7, UrlEncode(ThumbnailName(nointro)));
    if (getenv("SNES9X_COVER_URL")) return url; // host's local fixture
    const std::string encoded = UrlEncode(ThumbnailName(nointro));
    const std::string root =
        "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System/master/";
    return url + "\t" + root + "Named_Titles/" + encoded + ".png\t" +
        root + "Named_Snaps/" + encoded + ".png";
}

std::string CoverUrlForGame(const GameInfo& game)
{
    const std::string box = CoverNameFor(game);
    const std::string default_url = CoverUrlFor(box);
    if (getenv("SNES9X_COVER_URL")) return default_url;
    const std::string root =
        "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System/master/";
    const std::string title = MatchFallbackArt(TitleArtNames(), game);
    const std::string snap = MatchFallbackArt(SnapArtNames(), game);
    if (title.empty() && snap.empty()) return default_url;
    // Use real Libretro filenames independently for all 3 artwork categories.
    std::string url = CoverUrlTemplate();
    const size_t p = url.find("${name}");
    if (p != std::string::npos)
        url.replace(p, 7, UrlEncode(ThumbnailName(box)));
    const std::string title_name = title.empty() ? box : title;
    const std::string snap_name = snap.empty() ? box : snap;
    return url + "\t" + root + "Named_Titles/" + UrlEncode(ThumbnailName(title_name)) +
        ".png\t" + root + "Named_Snaps/" + UrlEncode(ThumbnailName(snap_name)) + ".png";
}

std::string WantedListPath()
{
	return OrbisDir("covers") + "/wanted.txt";
}

std::vector<WantedCover> MissingCovers(const std::vector<GameInfo>& games)
{
	std::vector<WantedCover> out;
	const std::string covers = OrbisDir("covers");
	for (const GameInfo& g : games)
	{
		const std::string name = CoverNameFor(g);
		const std::string file = ThumbnailName(name) + ".png";
		const std::string cache = covers + "/" + file;
		if (!Exists(RefetchMarker(cache)) &&
			(NonEmptyFile(cache) || RecentlyMissing(cache.substr(0, cache.size() - 4) + ".missing", CoverUrlForGame(g))))
			continue;
		const std::string own = FindWithExts(covers + "/" + g.file_base);
		if (!own.empty() && own != cache)
			continue; // your own cover (a ROM named as its official name shares the downloaded cover's file)
		const std::string dir = g.path.substr(0, g.path.find_last_of('/'));
		if (!FindWithExts(dir + "/" + g.file_base).empty())
			continue; // a cover beside the ROM
		bool dup = false;
		for (const WantedCover& w : out)
			dup = dup || w.file == file;
		if (!dup)
			out.push_back({file, CoverUrlForGame(g)});
	}
	return out;
}

void WriteWantedList(const std::vector<WantedCover>& wanted)
{
	std::string text;
	for (const WantedCover& w : wanted)
		text += w.file + "\t" + w.url + "\n";
	WriteFileAtomic(WantedListPath(), std::vector<uint8_t>(text.begin(), text.end()));
	OrbisLog("[covers] wanted list: %zu cover(s) for the next prefetch", wanted.size());
}

std::string CoverService::CachePath(int i) const
{
	const GameInfo& g = m_games[size_t(i)];
	const std::string name = CoverNameFor(g);
	return OrbisDir("covers") + "/" + ThumbnailName(name) + ".png";
}

bool CoverService::NeedsDownload(int i) const
{
	const GameInfo& g = m_games[size_t(i)];
	const std::string cache = CachePath(i);
	if (cache.empty())
		return false;
	const std::string own = FindWithExts(OrbisDir("covers") + "/" + g.file_base);
	if (!own.empty() && own != cache)
		return false; // your own cover, named after the ROM (not the downloaded one, when the names are the same)
	const std::string dir = g.path.substr(0, g.path.find_last_of('/'));
	if (!FindWithExts(dir + "/" + g.file_base).empty())
		return false; // a picture beside the ROM is used before a download
	if (Exists(RefetchMarker(cache)))
		return true; // asked for again (Square)
	return !NonEmptyFile(cache) && !RecentlyMissing(cache.substr(0, cache.size() - 4) + ".missing", CoverUrlForGame(g));
}

void CoverService::Start(const std::vector<GameInfo>& games, bool allow_download, bool background)
{
	Stop();
	m_games = games;
	m_allow_download = allow_download;
	m_background = background && !allow_download;
	m_placeholder.assign(games.size(), 0);
	m_refetching.clear();
	m_prio_focus = -1;
	m_last_prio.clear();
	m_bg_left = 0;
	m_bg_offline = false;
	m_quit = false;
	m_state.assign(games.size(), 0);
	m_fetched.assign(games.size(), 0);
	m_ready.clear();
	m_downloaded = 0;
	int need = 0;
	if (allow_download)
		for (size_t i = 0; i < games.size(); i++)
			need += NeedsDownload(int(i)) ? 1 : 0;
	m_to_download = need;
	OrbisLog("[covers] %zu game(s), %d cover(s) to download%s", games.size(), need,
		allow_download ? "" : m_background ? " (the helper downloads them in the background)" : " (downloads off)");
	m_thread = ps5::BigThread([this] { Run(); }, 8 * 1024 * 1024);
}

void CoverService::Stop()
{
	if (!m_thread.joinable())
		return;
	m_quit = true;
	m_http.Abort();
	m_wake.notify_all();
	m_thread.join();
	m_http.Term();
}

void CoverService::Collect(std::vector<CoverPtr>& slots, int keep_radius)
{
	if (slots.size() != m_games.size())
		slots.assign(m_games.size(), nullptr);
	std::lock_guard<std::mutex> lock(m_lock);
	while (!m_ready.empty())
	{
		auto& r = m_ready.front();
		if (r.first >= 0 && size_t(r.first) < slots.size())
			slots[size_t(r.first)] = r.second;
		m_ready.pop_front();
	}
	const int focus = m_focus;
	for (size_t i = 0; i < slots.size(); i++)
		if (slots[i] && std::abs(int(i) - focus) > keep_radius)
		{
			slots[i] = nullptr;
			if (m_state[i] == 2)
				m_state[i] = 0;
		}
}

void CoverService::Refetch(int i)
{
	if (i < 0 || size_t(i) >= m_games.size())
		return;
	const std::string cache = CachePath(i);
	if (cache.empty())
		return;
	// the cover stays: the marker asks for it again (here, or through the prefetch at the next start)
	if (FILE* f = fopen(RefetchMarker(cache).c_str(), "w"))
		fclose(f);
	unlink((cache.substr(0, cache.size() - 4) + ".missing").c_str());
	if (NeedsDownload(i))
		m_to_download++;
	struct stat st = {};
	const long long mtime = stat(cache.c_str(), &st) == 0 ? (long long)st.st_mtime : -1;
	std::lock_guard<std::mutex> lock(m_lock);
	m_fetched[size_t(i)] = 0;
	if (m_state[size_t(i)] == 2)
		m_state[size_t(i)] = 0;
	if (m_background)
		m_refetching[i] = mtime; // the helper's new file replaces the card when it lands (PollBackground)
	m_wake.notify_one();
}

bool CoverService::Download(int i)
{
	const GameInfo& g = m_games[size_t(i)];
	const std::string cache = CachePath(i);
	if (cache.empty())
		return false;
	const std::string marker = cache.substr(0, cache.size() - 4) + ".missing";
	if (m_offline || RecentlyMissing(marker, CoverUrlForGame(g)))
		return false;
	const std::string url = CoverUrlForGame(g);
	std::vector<uint8_t> data;
	S9X_STAGE(Cover, "http get cover");
	const int status = FetchCoverUrl(m_http, url, ThumbnailName(CoverNameFor(g)), data);
	if (status == 200)
	{
		if (WriteFileAtomic(cache, data))
		{
			unlink(RefetchMarker(cache).c_str()); // the new cover is in place
			m_downloaded++;
			return true;
		}
		OrbisLog("[covers] can't write %s", cache.c_str());
		return false;
	}
	if (status == 404)
	{
		unlink(RefetchMarker(cache).c_str()); // the server has none: the cover there (if any) stays
		FILE* f = fopen(marker.c_str(), "w");
		if (f)
		{
			fprintf(f, "%s\n", url.c_str());
			fclose(f);
		}
	}
	if (status == -1 && m_http.Offline())
		m_offline = true;
	return false;
}

CoverPtr CoverService::Load(int i, bool* downloaded)
{
	const GameInfo& g = m_games[size_t(i)];
	*downloaded = false;
	struct Cand
	{
		std::string path;
		const char* source;
	};
	std::vector<Cand> cands;
	const std::string manual = FindWithExts(OrbisDir("covers") + "/" + g.file_base);
	if (!manual.empty())
		cands.push_back({manual, "manual"});
	const std::string dir = g.path.substr(0, g.path.find_last_of('/'));
	const std::string beside = FindWithExts(dir + "/" + g.file_base);
	if (!beside.empty())
		cands.push_back({beside, "beside"});
	const std::string cache = CachePath(i);
	if (!cache.empty() && NonEmptyFile(cache))
		cands.push_back({cache, "cache"});
	bool try_download = false;
	if ((cands.empty() || (NonEmptyFile(cache) && NeedsDownload(i))) && m_allow_download)
	{
		std::lock_guard<std::mutex> lock(m_lock); // Refetch (the shelf's thread) writes m_fetched too
		try_download = !m_fetched[size_t(i)];
		m_fetched[size_t(i)] = 1;
	}
	if (try_download)
	{
		const bool counted = NeedsDownload(i);
		if (Download(i))
		{
			cands.insert(cands.begin(), {cache, "download"});
			*downloaded = true;
		}
		if (counted)
			DecrementToZero(m_to_download);
	}
	for (const Cand& c : cands)
	{
		std::vector<uint8_t> file;
		if (!ReadFile(c.path, file))
			continue;
		S9X_STAGE(Cover, "decode image");
		auto tex = Decode(file);
		if (tex)
		{
			tex->real = true;
			tex->source = c.source;
			return tex;
		}
		OrbisLog("[covers] can't decode %s", c.path.c_str());
	}
	return Placeholder(g);
}

void CoverService::Run()
{
	auto last_poll = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	std::unique_lock<std::mutex> lock(m_lock);
	while (!m_quit)
	{
		const int focus = m_focus;
		int best = -1;
		for (int i = std::max(0, focus - kLoadRadius); i <= std::min(int(m_games.size()) - 1, focus + kLoadRadius); i++)
			if (m_state[size_t(i)] == 0 && (best < 0 || std::abs(i - focus) < std::abs(best - focus)))
				best = i;
		if (best >= 0)
		{
			m_state[size_t(best)] = 1;
			lock.unlock();
			static char st[48]; // one cover worker: a lasting buffer the crash handler can read
			snprintf(st, sizeof(st), "load cover %d/%zu", best, m_games.size());
			S9X_STAGE(Cover, st);
			bool dl = false;
			CoverPtr tex = Load(best, &dl);
			S9X_STAGE(Cover, "idle");
			lock.lock();
			if (m_state[size_t(best)] == 1)
			{
				m_ready.emplace_back(best, tex);
				m_state[size_t(best)] = 2;
				m_placeholder[size_t(best)] = tex && !tex->real ? 1 : 0;
			}
			continue;
		}
		// Everything near the selection is loaded: fetch the rest of the library to disk, nearest first.
		int fetch = -1;
		if (m_allow_download && !m_offline)
			for (size_t d = 0; d < m_games.size() && fetch < 0; d++)
				for (int sgn = -1; sgn <= 1 && fetch < 0; sgn += 2)
				{
					const long i = long(focus) + sgn * long(d);
					if (i < 0 || size_t(i) >= m_games.size() || m_fetched[size_t(i)])
						continue;
					m_fetched[size_t(i)] = 1;
					fetch = int(i);
				}
		if (fetch >= 0)
		{
			// the disk is looked at with the lock released (the shelf's thread takes it every frame)
			lock.unlock();
			if (NeedsDownload(fetch))
			{
				Download(fetch);
				DecrementToZero(m_to_download);
			}
			lock.lock();
			continue;
		}
		if (m_background)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - last_poll >= std::chrono::milliseconds(1000))
			{
				last_poll = now;
				lock.unlock();
				PollBackground(focus);
				lock.lock();
				continue;
			}
		}
		m_wake.wait_for(lock, std::chrono::milliseconds(250));
	}
}

void CoverService::PollBackground(int focus)
{
	const int lo = std::max(0, focus - kLoadRadius), hi = std::min(int(m_games.size()) - 1, focus + kLoadRadius);
	// 1. covers the helper saved since: their placeholder cards are loaded again
	std::vector<int> cards;
	std::map<int, long long> refetching;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		for (int i = lo; i <= hi; i++)
			if (m_state[size_t(i)] == 2 && m_placeholder[size_t(i)])
				cards.push_back(i);
		refetching = m_refetching;
	}
	std::vector<int> landed;
	for (int i : cards)
	{
		const std::string cache = CachePath(i);
		if (!cache.empty() && NonEmptyFile(cache))
			landed.push_back(i);
	}
	for (const auto& r : refetching)
	{
		// Square: a new file (another mtime) once the helper has it
		struct stat st = {};
		const std::string cache = CachePath(r.first);
		if (!cache.empty() && stat(cache.c_str(), &st) == 0 && st.st_size > 0 && (long long)st.st_mtime != r.second &&
			!Exists(RefetchMarker(cache)))
			landed.push_back(r.first);
	}
	if (!landed.empty())
	{
		std::lock_guard<std::mutex> lock(m_lock);
		for (int i : landed)
		{
			m_refetching.erase(i);
			if (m_state[size_t(i)] == 2)
			{
				m_state[size_t(i)] = 0;
				m_placeholder[size_t(i)] = 0;
			}
		}
	}
	// 2. the covers around the selection go first: covers/priority.txt, nearest first
	if (focus != m_prio_focus)
	{
		m_prio_focus = focus;
		std::string text;
		for (int d = 0; d <= kLoadRadius; d++)
			for (int sgn = -1; sgn <= 1; sgn += 2)
			{
				const int i = focus + sgn * d;
				if ((d == 0 && sgn > 0) || i < lo || i > hi || !NeedsDownload(i))
					continue;
				const GameInfo& game = m_games[size_t(i)];
                text += ThumbnailName(CoverNameFor(game)) + ".png\t" + CoverUrlForGame(game) + "\n";
			}
		if (text != m_last_prio)
		{
			m_last_prio = text;
			WriteFileAtomic(PriorityListPath(), std::vector<uint8_t>(text.begin(), text.end()));
		}
	}
	// 3. how far the helper got
	const CoverProgress p = ReadCoverProgress();
	m_bg_left = p.valid ? p.left : 0;
	m_bg_offline = p.valid && p.state == "offline";
}
} // namespace fe

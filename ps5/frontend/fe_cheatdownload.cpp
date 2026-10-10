// Snes9x PS5: background cheat downloads. No runtime GitHub API/token required.
// SPDX-License-Identifier: MIT
#include "fe_cheatdownload.h"
#include "fe_coverfetch.h"
#include "fe_coverworker.h"
#include "fe_http.h"
#include "fe_titlematch.h"
#include "OrbisPaths.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef CHEAT_MANIFEST_TXT
#error CHEAT_MANIFEST_TXT must refer to the bundled SNES cheat index
#endif
#define SNES9X_CHEATS_INCBIN(sym, file) \
    __asm__(".section .rodata\n.global " #sym "_begin\n.balign 16\n" #sym "_begin:\n" \
            ".incbin \"" file "\"\n.global " #sym "_end\n" #sym "_end:\n.previous\n"); \
    extern "C" const char sym##_begin[]; \
    extern "C" const char sym##_end[];
SNES9X_CHEATS_INCBIN(snes9x_cheat_index, CHEAT_MANIFEST_TXT)

namespace fe {
namespace {
const char* kLibretroFolder =
    "https://raw.githubusercontent.com/libretro/libretro-database/master/"
    "cht/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System/";
constexpr size_t kMaxCheatFile = 512u << 10;
constexpr size_t kMaxRequestBytes = 1024u << 10;
const char* const kRequest = "download-request.txt";
const char* const kStatus = "download-status.txt";
const char* const kHeartbeat = "download-worker.ready";
std::string Path(const char* name); // helper worker stores its heartbeat in the cheat folder
void TouchHeartbeat()
{
    static time_t last_touch = 0; // called only by the single worker thread
    const time_t now = time(nullptr);
    if (now - last_touch >= 2)
    {
        const char ready[] = "ready\n";
        WriteFileAtomicTo(Path(kHeartbeat), std::vector<uint8_t>(ready,ready+sizeof(ready)-1));
        last_touch = now;
    }
}

std::string Root() { return OrbisDir("cheats"); }
std::string Path(const char* name) { return Root() + "/" + name; }
std::string Basename(std::string s)
{
    const size_t dot = s.rfind('.');
    if (dot != std::string::npos && s.substr(dot) == ".cht") s.resize(dot);
    return s;
}
bool SafeStem(const std::string& s)
{
    return !s.empty() && s.size() <= 220 && s[0] != '.' &&
        s.find("..") == std::string::npos &&
        s.find_first_of("/\\\r\n\t") == std::string::npos;
}
bool EndsWith(const std::string& text, const char* ending)
{
    const size_t n = strlen(ending);
    return text.size() >= n && text.compare(text.size() - n, n, ending) == 0;
}
std::string Key(const std::string& s)
{
    std::string result;
    int depth = 0;
    for (char c : Basename(s))
    {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (depth == 0 && std::isalnum(static_cast<unsigned char>(c)))
            result += char(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}
std::string Region(const std::string& s)
{
    static const char* tags[] = {"USA", "Europe", "Japan", "World"};
    for (const char* tag : tags)
        if (s.find(tag) != std::string::npos) return tag;
    return "";
}
const std::vector<std::string>& SourceFiles()
{
    static const std::vector<std::string> files = [] {
        std::vector<std::string> result;
        const char* p = snes9x_cheat_index_begin;
        const char* end = snes9x_cheat_index_end;
        while (p < end)
        {
            const char* e = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
            if (!e) break;
            std::string name(p, size_t(e - p));
            if (!name.empty() && name[0] != '#' && EndsWith(name, ".cht") && SafeStem(Basename(name)))
                result.push_back(std::move(name));
            p = e + 1;
        }
        return result;
    }();
    return files;
}
bool NonEmpty(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}
bool ReadSmall(const std::string& path, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st = {};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_size > 0 && size_t(st.st_size) <= kMaxCheatFile;
    if (ok)
    {
        bytes.resize(size_t(st.st_size));
        size_t n = 0;
        while (n < bytes.size())
        {
            const ssize_t got = read(fd, bytes.data() + n, bytes.size() - n);
            if (got <= 0) { ok = false; break; }
            n += size_t(got);
        }
    }
    close(fd);
    if (!ok) bytes.clear();
    return ok;
}
bool PlausibleCheats(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty() || bytes.size() > kMaxCheatFile) return false;
    const std::string t(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return t.find("cheat0_code") != std::string::npos ||
           (t.find("cheat\n") != std::string::npos && t.find("  code:") != std::string::npos);
}
bool CopyCheats(const std::string& from, const std::string& to)
{
    std::vector<uint8_t> bytes;
    return ReadSmall(from, bytes) && PlausibleCheats(bytes) && WriteFileAtomicTo(to, bytes);
}
void Status(int done, int total, int found, int failed, const char* state)
{
    char text[128];
    snprintf(text, sizeof(text), "%d %d %d %d %s\n", done, total, found, failed, state);
    const std::vector<uint8_t> bytes(text, text + strlen(text));
    WriteFileAtomicTo(Path(kStatus), bytes);
}
bool Queue(const std::string& request)
{
    OrbisMkdirs(Root());
    if (request.empty() || request.size() > kMaxRequestBytes) return false;
    if (!CheatDownloadWorkerAlive())
    {
        OrbisLog("[cheat-download] queue rejected: no live helper heartbeat (install/update helper ELF)");
        return false;
    }
    const CheatDownloadStatus progress = ReadCheatDownloadStatus();
    if (progress.valid && progress.state == "downloading" && progress.done < progress.total)
    {
        OrbisLog("[cheat-download] queue busy (%d/%d)", progress.done, progress.total);
        return false;
    }
    const std::string path = Path(kRequest);
    if (NonEmpty(path))
    {
        struct stat st = {};
        // The queue is normally consumed in under a second; old requests may be
        // left behind when the installed helper was not upgraded.
        if (stat(path.c_str(), &st)==0 && time(nullptr)-st.st_mtime>30)
            unlink(path.c_str());
        else return false;
    }
    if (!WriteFileAtomicTo(path, std::vector<uint8_t>(request.begin(),request.end())))
        return false;
    Status(0, 0, 0, 0, "queued");
    OrbisLog("[cheat-download] queued %zu bytes for helper",request.size());
    return true;
}
bool DownloadSource(HttpClient& http, const std::string& source, const std::string& target)
{
    if (NonEmpty(target)) return true;
    TouchHeartbeat();
    std::vector<uint8_t> bytes;
    const int code = http.Get(std::string(kLibretroFolder) + UrlEncode(source), bytes, kMaxCheatFile);
    TouchHeartbeat();
    if (code != 200 || !PlausibleCheats(bytes))
    {
        OrbisLog("[cheat-download] %s -> %d (%zu bytes)", source.c_str(), code, bytes.size());
        return false;
    }
    const bool ok = WriteFileAtomicTo(target, bytes);
    OrbisLog("[cheat-download] %s -> %s (%s)", source.c_str(), target.c_str(), ok ? "saved" : "write error");
    return ok;
}
bool DownloadGame(HttpClient& http, const CheatRequestGame& game)
{
    if (!SafeStem(game.basename)) return false;
    const std::string target = CheatDownloadPath(game.basename);
    if (NonEmpty(target)) return true; // never erase manually enabled cheat groups
    const std::string source = BestCheatSourceFile(game);
    if (source.empty()) return false;
    const std::string cache = Root() + "/database/" + source;
    if (NonEmpty(cache) && CopyCheats(cache, target)) return true;
    // Write straight to the ROM-stem path for per-game and installed-library downloads.
    return DownloadSource(http, source, target);
}
bool Consume(std::vector<std::string>& lines)
{
    const std::string path = Path(kRequest);
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st = {};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0 && size_t(st.st_size) <= kMaxRequestBytes;
    std::string text;
    if (ok)
    {
        text.resize(size_t(st.st_size));
        size_t n = 0;
        while (n < text.size())
        {
            const ssize_t r = read(fd, &text[n], text.size() - n);
            if (r <= 0) { ok = false; break; }
            n += size_t(r);
        }
    }
    close(fd);
    unlink(path.c_str());
    if (!ok) return false;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) break; // refuse partial final line
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return !lines.empty();
}
void* DownloadThread(void*)
{
    OrbisMkdirs(Root());
    OrbisMkdirs(Root() + "/database");
    std::unique_ptr<HttpClient> http = MakeCoverWorkerHttp(); // helper: TlsHttp using own TLS
    OrbisLog("[cheat-download] helper online: %zu Libretro SNES source files", SourceFiles().size());
    // A previous helper may have quit halfway through a job. Never leave the
    // persisted "downloading" state blocking all future requests on restart.
    Status(0, 0, 0, 0, "idle");
    for (;;)
    {
        TouchHeartbeat();
        std::vector<std::string> lines;
        if (!Consume(lines)) { usleep(500 * 1000); continue; }
        const bool all = lines[0] == "all";
        if (!all && lines[0] != "library") { Status(0, 0, 0, 1, "invalid"); continue; }
        const int total = all ? int(SourceFiles().size()) : int(lines.size()) - 1;
        int done = 0, found = 0, failed = 0;
        Status(done, total, found, failed, "downloading");
        for (int i = 0; i < total; ++i)
        {
            bool ok = false;
            if (all)
            {
                const std::string& source = SourceFiles()[size_t(i)];
                ok = DownloadSource(*http, source, Root() + "/database/" + source);
            }
            else
            {
                const std::string& line = lines[size_t(i + 1)];
                const size_t tab = line.find('\t');
                if (tab != std::string::npos)
                    ok = DownloadGame(*http, {line.substr(0, tab), line.substr(tab + 1)});
            }
            if (ok) ++found; else ++failed;
            ++done;
            if (done % 4 == 0 || done == total) Status(done, total, found, failed, "downloading");
            if (http->Offline()) { Status(done, total, found, failed, "offline"); http->Term(); break; }
        }
        if (done == total) Status(done, total, found, failed, "completed");
    }
    return nullptr;
}
} // namespace

size_t CheatDatabaseCount() { return SourceFiles().size(); }
bool CheatDownloadWorkerAlive()
{
    struct stat st = {};
    return stat(Path(kHeartbeat).c_str(), &st)==0 && S_ISREG(st.st_mode) &&
        st.st_size>0 && time(nullptr) >= st.st_mtime &&
        time(nullptr)-st.st_mtime <= 40;
}
std::string CheatDownloadPath(const std::string& basename)
{
    return SafeStem(basename) ? Root() + "/" + basename + ".cht" : "";
}
std::string BestCheatSourceFile(const CheatRequestGame& game)
{
    const std::string wanted = Key(game.nointro.empty() ? game.basename : game.nointro);
    if (wanted.empty()) return "";
    const std::string region = Region(game.nointro);
    int highest = -100000;
    std::string result;
    for (const std::string& name : SourceFiles())
    {
        if (Key(name) != wanted) continue;
        int score = 0;
        if (Basename(name) == game.nointro) score += 1000;
        const std::string from_region = Region(name);
        if (!region.empty() && from_region == region) score += 100;
        else if (!region.empty() && !from_region.empty()) score -= 100;
        if (name.find("(Game Genie)") != std::string::npos) score -= 10;
        if (name.find("(Action Replay)") != std::string::npos) score -= 10;
        if (name.find("(Rev") != std::string::npos) score -= 1;
        if (score > highest) { highest = score; result = name; }
    }
    if (result.empty())
        result = titles::Best(SourceFiles(), game.nointro.empty() ? game.basename : game.nointro);
    return result;
}
bool InstallCachedCheat(const CheatRequestGame& game)
{
    if (!SafeStem(game.basename)) return false;
    const std::string target = CheatDownloadPath(game.basename);
    if (NonEmpty(target)) return true;
    const std::string source = BestCheatSourceFile(game);
    return !source.empty() && CopyCheats(Root() + "/database/" + source, target);
}
bool RequestGameCheats(const CheatRequestGame& game)
{
    if (!SafeStem(game.basename) || BestCheatSourceFile(game).empty()) return false;
    return Queue("library\n" + game.basename + "\t" + game.nointro + "\n");
}
bool RequestLibraryCheats(const std::vector<CheatRequestGame>& games)
{
    std::string request = "library\n";
    for (const CheatRequestGame& g : games)
    {
        if (!SafeStem(g.basename)) continue;
        request += g.basename + "\t" + g.nointro + "\n";
        if (request.size() > kMaxRequestBytes) return false;
    }
    return request.size() > 8 && Queue(request);
}
bool RequestAllSnesCheats() { return Queue("all\n"); }
CheatDownloadStatus ReadCheatDownloadStatus()
{
    CheatDownloadStatus state;
    state.helper_alive = CheatDownloadWorkerAlive();
    FILE* f = fopen(Path(kStatus).c_str(), "r");
    if (!f) return state;
    char name[32] = {};
    state.valid = fscanf(f, "%d %d %d %d %31s", &state.done, &state.total, &state.found,
                         &state.failed, name) == 5;
    fclose(f);
    if (state.valid) state.state = name;
    return state;
}
void StartCheatDownloadWorker()
{
    static bool started = false;
    if (started) return;
    started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    const int rc = pthread_create(&t, &attr, DownloadThread, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        started = false;
        OrbisLog("[cheat-download] cannot start helper thread (%d)", rc);
    }
}
} // namespace fe

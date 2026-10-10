// Snes9x PS5 frontend: the Snes9x core and its port callbacks.
//
// Frame pacing:
//   - 60 Hz (NTSC) games: Present waits for the flip, so the console's vsync paces the emulation, and
//     Snes9x's dynamic rate control stretches the sound by up to 0.5% to keep the audio ring half full
//     (the SNES runs at 60.10 Hz, the TV at 60.00 Hz).
//   - 50 Hz (PAL) games: the audio clock paces instead (wait while the ring is more than half full), and
//     the flips don't wait (each frame still waits, before writing, for the buffer it last flipped).
//   - In any case, if the ring gets 3/4 full (vsync not blocking for some reason), wait on audio.
//   - Fast forward (hold R2): no waiting, sound off, 1 frame in 4 drawn.
//
// SPDX-License-Identifier: MIT

#include "fe_emu.h"

#include "fe_games.h"
#include "fe_cheatlookup.h"
#include "fe_cheatdownload.h"
#include "fe_settings.h"
#include "fe_shortcuts.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "snes9x.h"
#include "apu/apu.h"
#include "cheats.h"
#include "conffile.h"
#include "controls.h"
#include "display.h"
#include "fscompat.h"
#include "gfx.h"
#include "memmap.h"
#include "ppu.h"
#include "snapshot.h"

#include <dirent.h>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>

#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

namespace
{
// SNES button ids for S9xMapButton/S9xReportButton: (player << 4) | button
enum SnesBtn
{
	B_A,
	B_B,
	B_X,
	B_Y,
	B_L,
	B_R,
	B_START,
	B_SELECT,
	B_UP,
	B_DOWN,
	B_LEFT,
	B_RIGHT,
	B_COUNT
};
const char* const kBtnNames[B_COUNT] = {"A", "B", "X", "Y", "L", "R", "Start", "Select", "Up", "Down", "Left", "Right"};

// PS5 -> SNES, by position: Cross (bottom) = B, Circle (right) = A, Square (left) = Y, Triangle (top) = X.
const uint32_t kPadFor[B_COUNT] = {
	SCE_PAD_BUTTON_CIRCLE, SCE_PAD_BUTTON_CROSS, SCE_PAD_BUTTON_TRIANGLE, SCE_PAD_BUTTON_SQUARE,
	SCE_PAD_BUTTON_L1, SCE_PAD_BUTTON_R1, SCE_PAD_BUTTON_OPTIONS, SCE_PAD_BUTTON_TOUCH_PAD,
	SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_DOWN, SCE_PAD_BUTTON_LEFT, SCE_PAD_BUTTON_RIGHT};

uint32_t MakeId(int player, int btn)
{
	return uint32_t(((player + 1) << 4) | btn);
}

struct State
{
	bool core_ok = false;
	bool loaded = false;
	std::string rom_path;
	bool multitap = false;
	bool fast_forward = false;
	bool quit = false;
	uint32_t frame = 0;
	uint32_t prev_p1 = 0;
	// After the game starts or the pause menu closes, player 1's pad reaches the game only once the buttons that
	// did it (Cross, Circle, L3 + R3) are let go: Resume with Cross must not press B in the game.
	bool wait_release = true;
	std::string cheat_status = "No cheat file checked";
	std::vector<int16_t> mix;
	// last frame, for the pause menu
	std::vector<uint16_t> last;
	int last_w = 0, last_h = 0;
};
State g;

bool IsPal()
{
	return Memory.ROMFramesPerSecond == 50;
}

void SetControllers(bool multitap)
{
	S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
	if (multitap)
		S9xSetController(1, CTL_MP5, 1, 2, 3, -1);
	else
		S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
	S9xVerifyControllers();
	g.multitap = multitap;
	OrbisLog("[emu] port 2: %s", multitap ? "multitap (3-4 players)" : "joypad");
}

void MapButtons()
{
	S9xUnmapAllControls();
	char cmd[64];
	for (int p = 0; p < ps5input::kMaxPads; p++)
		for (int b = 0; b < B_COUNT; b++)
		{
			snprintf(cmd, sizeof(cmd), "Joypad%d %s", p + 1, kBtnNames[b]);
			S9xMapButton(MakeId(p, b), S9xGetCommandT(cmd), false);
		}
}

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void WaitAudioBelow(int frames)
{
	// at most ~100 ms, so a stalled audio thread can't freeze the game
	for (int i = 0; i < 100 && ps5audio::Queued() > frames; i++)
		usleep(1000);
}

// part (written by the caller) holds the whole new file: check its size, flush it to the disk, then rename it over
// path. On any failure part is removed and path keeps its old content. expect: the exact size, or 0 for "not empty".
bool CommitPart(const std::string& part, const std::string& path, size_t expect)
{
	const int fd = open(part.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	struct stat st = {};
	bool ok = fstat(fd, &st) == 0 && (expect ? size_t(st.st_size) == expect : st.st_size > 0);
	ok = fsync(fd) == 0 && ok;
	ok = close(fd) == 0 && ok;
	if (!ok || rename(part.c_str(), path.c_str()) != 0)
	{
		unlink(part.c_str());
		return false;
	}
	return true;
}

// The size Memory.SaveSRAM writes for the cartridge's battery RAM (memmap.cpp's own formula); 0 when none.
size_t SramFileSize()
{
	if ((Settings.SuperFX && Memory.ROMType < 0x15) || (Settings.SA1 && Memory.ROMType == 0x34))
		return 0;
	size_t size = Memory.SRAMSize ? size_t(1 << (Memory.SRAMSize + 3)) * 128 : 0;
	if (Memory.LoROM)
		size = size < 0x70000 ? size : 0x70000;
	else if (Memory.HiROM)
		size = size < 0x40000 ? size : 0x40000;
	return size;
}


struct RetroCheat
{
	std::string name, code;
	bool enabled = false;
};

std::string TrimField(std::string s)
{
	const size_t first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	const size_t last = s.find_last_not_of(" \t\r\n");
	s = s.substr(first, last - first + 1);
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		s = s.substr(1, s.size() - 2);
	return s;
}

// RetroArch .cht and Snes9x .cht have the same extension, not the same syntax.
// Detect RetroArch assignments before the native Snes9x parser, without network dependencies.
bool LoadRetroArchCheats(const std::string& file)
{
	std::ifstream in(file);
	if (!in) return false;
	std::map<int, RetroCheat> cheats;
	std::string line;
	bool recognized = false;
	while (std::getline(in, line))
	{
		if (line.size() > 32768) continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = TrimField(line.substr(0, eq));
		const std::string val = TrimField(line.substr(eq + 1));
		if (key.size() < 8 || key.compare(0, 5, "cheat") != 0) continue;
		char* end = nullptr;
		const long idx = strtol(key.c_str() + 5, &end, 10);
		if (end == key.c_str() + 5 || idx < 0 || idx >= 4096 || !end || *end != '_')
			continue;
		const std::string field = end + 1;
		RetroCheat& c = cheats[int(idx)];
		if (field == "code") { c.code = val; recognized = true; }
		else if (field == "desc") c.name = val;
		else if (field == "enable") c.enabled = (val == "true" || val == "1");
	}
	if (!recognized) return false;
	int added = 0;
	for (const auto& item : cheats)
	{
		if (item.second.code.empty() || added >= 1024) continue;
		const std::string& code = item.second.code;
		std::string name = item.second.name.empty() ? ("Cheat " + std::to_string(item.first + 1)) : item.second.name;
		// Snes9x stores names on a single BML line.
		for (char& ch : name) if (ch == '\r' || ch == '\n') ch = ' ';
		const int group = S9xAddCheatGroup(name, code);
		if (group < 0) continue; // unsupported/invalid codes are not selectable
		if (item.second.enabled) S9xEnableCheatGroup(uint32(group));
		++added;
	}
	OrbisLog("[cheats] imported %d RetroArch cheat groups from %s", added, file.c_str());
	return added > 0;
}

// Use the ROM *file* name, not the internal name exposed by a zipped ROM.
// S9xGetFilename derives its name from Memory.ROMFilename which can differ.
std::string CheatPathForRom()
{
    return OrbisDir("cheats") + "/" + S9xBasenameNoExt(g.rom_path) + ".cht";
}
std::string OfficialCheatTitle()
{
    if (g.rom_path.empty()) return "";
    std::string official = fe::gamedb::ByCrc(Memory.ROMCRC32);
    const std::string base = S9xBasenameNoExt(g.rom_path);
    if (official.empty()) official = fe::gamedb::Exact(base);
    if (official.empty()) official = fe::gamedb::Loose(base);
    if (official.empty()) official = fe::gamedb::Fuzzy(base);
    return official;
}
// Users often copy EarthBound (USA).cht with EarthBound.sfc, or copy
// the unpacked RetroArch library with filenames nested one level below.
// Only match .cht files with the same ROM title; don't load other games.
std::string FindManualCheatFile()
{
    if (g.rom_path.empty()) return "";
    const std::string direct = CheatPathForRom();
    if (OrbisIsFile(direct)) return direct;
    std::vector<std::string> choices;
    const std::string root = OrbisDir("cheats");
    const char* const subdirs[] = {"", "database", "Nintendo - Super Nintendo Entertainment System"};
    std::vector<std::string> dirs;
    for (const char* sub : subdirs)
        dirs.push_back(*sub ? root + "/" + sub : root);
    const size_t slash = g.rom_path.find_last_of("/");
    if (slash!=std::string::npos) dirs.push_back(g.rom_path.substr(0,slash));
    for (const std::string& dir : dirs)
    {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d))
        {
            const std::string filename=e->d_name;
            if (!fe::cheatlookup::EndsWithCht(filename)) continue;
            const std::string full = dir + "/" + filename;
            struct stat st = {};
            if (lstat(full.c_str(), &st)==0 && S_ISREG(st.st_mode) && st.st_size>0)
                choices.push_back(full);
        }
        closedir(d);
    }
    return fe::cheatlookup::Best(choices, S9xBasenameNoExt(g.rom_path), OfficialCheatTitle());
}
bool ImportCheatsFromFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        g.cheat_status = "Unable to open cheat file";
        return false;
    }
    std::string head(4096, '\0');
    in.read(&head[0], head.size());
    head.resize(size_t(in.gcount()));
    // A RetroArch file can have codes anywhere in its first 4KB.
    // Never feed that file to the Snes9x *binary* fallback parser: it
    // previously said success for invalid text while importing zero codes.
    const bool retro = head.find("cheats =")!=std::string::npos ||
                       head.find("cheat0_")!=std::string::npos ||
                       head.find("cheat1_")!=std::string::npos;
    const bool native = head.find("cheat\n")!=std::string::npos ||
                        head.rfind("cheat\r\n",0)==0;
    bool binary = false;
    if (!retro && !native && head.size()>=28)
    {
        binary = head.find('\0')!=std::string::npos;
        if (binary)
        {
            struct stat st = {};
            binary = stat(path.c_str(),&st)==0 && st.st_size%28==0;
        }
    }
    const int before=int(Cheat.group.size());
    bool parsed = false;
    if (retro) parsed = LoadRetroArchCheats(path);
    else if (native || binary) parsed = S9xLoadCheatFile(path);
    const int added=int(Cheat.group.size())-before;
    if (!parsed || added<=0)
    {
        g.cheat_status="Found " + S9xBasename(path) + " but no supported codes";
        OrbisLog("[cheats] %s", g.cheat_status.c_str());
        return false;
    }
    g.cheat_status = "Loaded " + std::to_string(added) + " cheats from " + S9xBasename(path);
    OrbisLog("[cheats] %s", g.cheat_status.c_str());
    return true;
}
bool LoadManualCheatsForRom()
{
    const std::string file = FindManualCheatFile();
    if (file.empty())
    {
        g.cheat_status = "No matching .cht file in /data/snes9x/cheats";
        return false;
    }
    return ImportCheatsFromFile(file);
}

std::string StatePath(int slot)
{
	char ext[8];
	snprintf(ext, sizeof(ext), ".%03d", slot);
	return S9xGetFilename(ext, SNAPSHOT_DIR);
}
} // namespace

// =========================================================================================================
// Port callbacks the Snes9x core calls
// =========================================================================================================

bool8 S9xInitUpdate()
{
	return TRUE;
}

bool8 S9xDeinitUpdate(int width, int height)
{
	const fe::Settings& cfg = fe::Config();
	const ps5video::Rect r = ps5video::DrawSnes(GFX.Screen, GFX.Pitch, width, height,
		ps5video::Aspect(cfg.aspect), cfg.scanlines, cfg.shader);
	// keep a copy for the pause menu
	g.last_w = width;
	g.last_h = height;
	g.last.resize(size_t(width) * height);
	for (int y = 0; y < height; y++)
		memcpy(&g.last[size_t(y) * width], reinterpret_cast<const uint8_t*>(GFX.Screen) + size_t(y) * GFX.Pitch,
			size_t(width) * 2);

	const bool wait_vsync = !g.fast_forward && !IsPal();
	ps5video::Present(r.x, r.y, r.w, r.h, wait_vsync);
	return TRUE;
}

bool8 S9xContinueUpdate(int width, int height)
{
	return S9xDeinitUpdate(width, height);
}

void S9xSyncSpeed()
{
	if (Settings.Mute || g.fast_forward)
	{
		S9xClearSamples();
		return;
	}
	const int samples = S9xGetSampleCount(); // int16 values, stereo
	if (samples <= 0)
		return;
	if (int(g.mix.size()) < samples)
		g.mix.resize(samples);
	S9xMixSamples(reinterpret_cast<uint8*>(g.mix.data()), samples);
	ps5audio::Push(g.mix.data(), samples / 2);
	S9xUpdateDynamicRate(ps5audio::Free(), ps5audio::kCapacity);
}

void S9xInitInputDevices()
{
}

bool8 S9xOpenSoundDevice()
{
	return TRUE;
}

void S9xToggleSoundChannel(int)
{
}

void S9xMessage(int type, int number, const char* message)
{
	(void)number;
	if (!message || !*message)
		return;
	OrbisLog("[s9x%s] %s", type == S9X_ERROR ? " error" : (type == S9X_WARNING ? " warning" : ""), message);
}

const char* S9xStringInput(const char*)
{
	return nullptr;
}

std::string S9xGetDirectory(enum s9x_getdirtype dirtype)
{
	switch (dirtype)
	{
		case ROMFILENAME_DIR:
		case ROM_DIR:
		{
			const size_t slash = g.rom_path.find_last_of('/');
			if (slash != std::string::npos)
				return g.rom_path.substr(0, slash);
			return OrbisDir("roms");
		}
		case SRAM_DIR:
		case SAT_DIR: return OrbisDir("saves");
		case SNAPSHOT_DIR: return OrbisDir("states");
		case SCREENSHOT_DIR:
		case SPC_DIR: return OrbisDir("screenshots");
		case CHEAT_DIR: return OrbisDir("cheats");
		case PATCH_DIR: return OrbisDir("patches");
		case BIOS_DIR: return OrbisDir("bios");
		case LOG_DIR: return OrbisDir("logs");
		default: return OrbisRoot();
	}
}

std::string S9xGetFilenameInc(std::string ext, enum s9x_getdirtype dirtype)
{
	const std::string dir = S9xGetDirectory(dirtype);
	const std::string base = S9xBasenameNoExt(Memory.ROMFilename);
	char name[64];
	for (int i = 0; i < 1000; i++)
	{
		snprintf(name, sizeof(name), ".%03d", i);
		const std::string path = dir + "/" + base + name + ext;
		if (!OrbisIsFile(path))
			return path;
	}
	return dir + "/" + base + ".999" + ext;
}

bool8 S9xOpenSnapshotFile(const char* filepath, bool8 read_only, STREAM* file)
{
	*file = OPEN_STREAM(filepath, read_only ? "rb" : "wb");
	return *file != nullptr;
}

void S9xCloseSnapshotFile(STREAM file)
{
	CLOSE_STREAM(file);
}

void S9xAutoSaveSRAM()
{
	// Snes9x writes the file in place: through path.part, checked and renamed, so a power cut or a full disk while
	// it is written leaves the save as it was
	const std::string path = S9xGetFilename(".srm", SRAM_DIR);
	const size_t size = SramFileSize();
	if (size == 0)
		return;
	const std::string part = path + ".part";
	const bool ok = Memory.SaveSRAM(part.c_str()) && CommitPart(part, path, size);
	if (!ok)
		unlink(part.c_str());
	OrbisLog("[emu] battery save -> %s (%s)", path.c_str(), ok ? "ok" : "failed, the save on disk is unchanged");
}

void S9xExit()
{
	g.quit = true;
}

void S9xParsePortConfig(ConfigFile&, int)
{
}

void S9xExtraUsage()
{
}

void S9xParseArg(char**, int&, int)
{
}

void S9xHandlePortCommand(s9xcommand_t, int16, int16)
{
}

bool S9xPollButton(uint32, bool*)
{
	return false;
}

bool S9xPollAxis(uint32, int16*)
{
	return false;
}

bool S9xPollPointer(uint32, int16*, int16*)
{
	return false;
}

// =========================================================================================================
// emu::
// =========================================================================================================
namespace emu
{
bool InitCore(int argc, char** argv)
{
	memset(&Settings, 0, sizeof(Settings));
	// Defaults, then /data/snes9x/snes9x.conf if the user wrote one (Snes9x's own format and keys).
	S9xLoadConfigFiles(argv, argc);

	// What the port needs whatever the config file says.
	Settings.SoundPlaybackRate = ps5audio::kRate;
	Settings.SoundInputRate = 32040;
	Settings.SixteenBitSound = TRUE;
	Settings.Stereo = TRUE;
	Settings.DynamicRateControl = TRUE;
	if (Settings.DynamicRateLimit <= 0)
		Settings.DynamicRateLimit = 5;
	Settings.SoundSync = FALSE;
	Settings.FrameTimeNTSC = 16639;
	Settings.FrameTimePAL = 20000;
	Settings.OneClockCycle = 6;
	Settings.OneSlowClockCycle = 8;
	Settings.TwoClockCycles = 12;
	Settings.AutoDisplayMessages = TRUE;
	if (Settings.InitialInfoStringTimeout == 0)
		Settings.InitialInfoStringTimeout = 120;
	Settings.AutoSaveDelay = 3; // seconds after the game writes its battery RAM
	Settings.DontSaveOopsSnapshot = TRUE;
	Settings.StopEmulation = TRUE;
	CPU.Flags = 0;

	if (!Memory.Init() || !S9xInitAPU())
	{
		OrbisLog("[emu] Memory.Init / S9xInitAPU failed");
		Memory.Deinit();
		S9xDeinitAPU();
		return false;
	}
	S9xInitSound(32);
	S9xSetSoundMute(FALSE);
	S9xSetSamplesAvailableCallback(nullptr, nullptr);
	S9xGraphicsInit();
	S9xInitInputDevices();
	MapButtons();
	SetControllers(false);
	ApplySettings();
	g.core_ok = true;
	OrbisLog("[emu] core ready (Snes9x %s)", VERSION);
	return true;
}

void DeinitCore()
{
	if (!g.core_ok)
		return;
	CloseGame();
	S9xGraphicsDeinit();
	S9xDeinitAPU();
	Memory.Deinit();
	g.core_ok = false;
}

bool LoadGame(const std::string& path)
{
	if (!g.core_ok)
		return false;
	CloseGame();
	g.rom_path = path;
	OrbisLog("[emu] loading %s", path.c_str());
	if (!Memory.LoadROM(path.c_str()))
	{
		OrbisLog("[emu] LoadROM failed");
		g.rom_path.clear();
		return false;
	}
	const std::string srm = S9xGetFilename(".srm", SRAM_DIR);
	Memory.LoadSRAM(srm.c_str());

	S9xDeleteCheats();
	S9xCheatsEnable();
	const std::string base = S9xBasenameNoExt(path);
	const std::string official = OfficialCheatTitle();
	// The user's own EarthBound (USA).cht must take priority over any
	// automatically cached alternative. Never replace an existing choice.
	if (FindManualCheatFile().empty())
		fe::InstallCachedCheat({base, official});
	const bool found = !FindManualCheatFile().empty();
	if (!LoadManualCheatsForRom() && !found && fe::Config().cheats_auto_download)
	{
		const fe::CheatRequestGame request{base, official};
		if (!fe::BestCheatSourceFile(request).empty() && fe::RequestGameCheats(request))
			OrbisLog("[cheats] queued automatic download for %s", base.c_str());
	}

	g.loaded = true;
	g.frame = 0;
	g.fast_forward = false;
	g.quit = false;
	g.prev_p1 = 0;
	g.wait_release = true;
	ps5video::InvalidateSnes();
	ApplySettings();
	OrbisLog("[emu] \"%s\" %s, %d fps, %s", Memory.ROMName, Memory.ROMFilename.c_str(), Memory.ROMFramesPerSecond,
		Settings.PAL ? "PAL" : "NTSC");
	return true;
}

void CloseGame()
{
	if (!g.loaded)
		return;
	if (Memory.SRAMSize > 0)
		S9xAutoSaveSRAM();
	if (!Cheat.group.empty())
		SaveCheats();
	g.loaded = false;
	g.rom_path.clear();
}

bool GameLoaded()
{
	return g.loaded;
}

int CheatCount()
{
	return g.loaded ? int(Cheat.group.size()) : 0;
}

std::string RomBase()
{
	return g.loaded ? S9xBasenameNoExt(g.rom_path) : "";
}
std::string RomNoIntro()
{
	return g.loaded ? OfficialCheatTitle() : "";
}
std::string CheatStatus()
{
	return g.cheat_status;
}
bool ReloadDownloadedCheats()
{
	if (!g.loaded || !Cheat.group.empty()) return false; // never replace user's current cheat selections
	// CheatMenu is redrawn at 60 Hz; scanning a 2,773-file library
	// every frame would stall input and waste CPU.
	static double next_attempt = 0;
	const double now = Now();
	if (now < next_attempt) return false;
	next_attempt = now + 1.0;
	return LoadManualCheatsForRom();
}

std::string CheatName(int index)
{
	if (!g.loaded || index < 0 || index >= CheatCount()) return "";
	return Cheat.group[size_t(index)].name;
}

bool CheatEnabled(int index)
{
	return g.loaded && index >= 0 && index < CheatCount() && Cheat.group[size_t(index)].enabled;
}

bool SaveCheats()
{
	if (!g.loaded) return false;
	if (Cheat.group.empty()) return true;
	const std::string path = CheatPathForRom();
	const std::string tmp = path + ".part";
	if (!S9xSaveCheatFile(tmp) || !CommitPart(tmp, path, 0))
	{
		unlink(tmp.c_str());
		OrbisLog("[cheats] save failed: %s", path.c_str());
		return false;
	}
	OrbisLog("[cheats] saved %zu groups to %s", Cheat.group.size(), path.c_str());
	return true;
}

bool ToggleCheat(int index)
{
	if (!g.loaded || index < 0 || index >= CheatCount()) return false;
	if (Cheat.group[size_t(index)].enabled)
		S9xDisableCheatGroup(uint32(index));
	else
		S9xEnableCheatGroup(uint32(index));
	return SaveCheats();
}

void SetAllCheats(bool enabled)
{
	if (!g.loaded) return;
	for (int i = 0; i < CheatCount(); ++i)
	{
		if (enabled && !Cheat.group[size_t(i)].enabled) S9xEnableCheatGroup(uint32(i));
		else if (!enabled && Cheat.group[size_t(i)].enabled) S9xDisableCheatGroup(uint32(i));
	}
	SaveCheats();
}

std::string GameName()
{
	// the official title, as on the shelf: by the ROM's CRC (Snes9x computes it on load), else by file name
	const std::string base = S9xBasenameNoExt(g.rom_path);
	std::string name = g.loaded ? fe::gamedb::ByCrc(Memory.ROMCRC32) : std::string();
	if (name.empty())
		name = fe::gamedb::Exact(base);
	if (name.empty())
		name = fe::gamedb::Loose(base);
	return name.empty() ? base : fe::gamedb::Title(name);
}

void ApplySettings()
{
	const fe::Settings& cfg = fe::Config();
	Settings.Transparency = cfg.transparency;
	Settings.DisplayFrameRate = cfg.show_fps;
	Settings.SuperFXClockMultiplier = cfg.superfx_clock;
	Settings.Mute = !cfg.audio;
	S9xSetSoundMute(Settings.Mute);
	ps5video::InvalidateSnes();
}

void Osd(const char* fmt, ...)
{
	static char text[256]; // S9xSetInfoString keeps the pointer
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	S9xSetInfoString(text);
	OrbisLog("[osd] %s", text);
}

bool SaveState(int slot)
{
	if (!g.loaded)
		return false;
	// the state in memory, then written as S9xFreezeGame writes it (gzip) to path.part, checked and renamed: a full
	// disk or a power cut leaves the slot's old state as it was (S9xFreezeGame overwrites in place and can't tell)
	const std::string path = StatePath(slot), part = path + ".part";
	const uint32 size = S9xFreezeSize();
	std::vector<uint8> buf(size);
	bool ok = size > 0 && S9xFreezeGameMem(buf.data(), size);
	if (ok)
	{
		gzFile gz = gzopen(part.c_str(), "wb");
		ok = gz != nullptr && gzwrite(gz, buf.data(), unsigned(size)) == int(size);
		ok = gz != nullptr && gzclose(gz) == Z_OK && ok;
		ok = ok && CommitPart(part, path, 0);
		if (!ok)
			unlink(part.c_str());
	}
	if (ok)
		S9xResetSaveTimer(TRUE);
	OrbisLog("[emu] save state %d -> %s: %s", slot, path.c_str(), ok ? "ok" : "failed, the slot is unchanged");
	return ok;
}

bool LoadState(int slot)
{
	if (!g.loaded)
		return false;
	const std::string path = StatePath(slot);
	if (!OrbisIsFile(path))
		return false;
	const bool ok = S9xUnfreezeGame(path.c_str());
	OrbisLog("[emu] load state %d <- %s: %s", slot, path.c_str(), ok ? "ok" : "failed");
	return ok;
}

bool StateExists(int slot)
{
	return g.loaded && OrbisIsFile(StatePath(slot));
}

void Reset()
{
	if (g.loaded)
		S9xSoftReset();
}

void AfterMenu()
{
	g.wait_release = true;
}

void RedrawLastFrame()
{
	if (g.last.empty())
		return;
	const fe::Settings& cfg = fe::Config();
	ps5video::InvalidateSnes();
	ps5video::DrawSnes(g.last.data(), g.last_w * 2, g.last_w, g.last_h, ps5video::Aspect(cfg.aspect), cfg.scanlines,
		cfg.shader);
}

FrameResult RunFrame()
{
	if (!g.loaded)
		return FrameResult::Quit;

	ps5input::Poll();
	const ps5input::PadState& p1 = ps5input::Pad(0);
	const uint32_t raw = p1.buttons;
	const uint32_t physical = p1.raw_buttons;
	if (g.wait_release)
	{
		g.wait_release = physical != 0;
		g.prev_p1 = raw; // never pass a menu button through as a new game button
	}
	const uint32_t pressed = raw & ~g.prev_p1;
	const uint32_t pressed_physical = pressed & physical;
	g.prev_p1 = raw;
	const fe::Settings& bindings = fe::Config();
	if (!g.wait_release && fe::shortcuts::JustPressed(physical, pressed_physical, bindings.shortcut_pause))
		return FrameResult::OpenMenu;
	if (!g.wait_release && fe::shortcuts::JustPressed(physical, pressed_physical, bindings.shortcut_list))
		return FrameResult::BackToList;
	if (!g.wait_release && fe::shortcuts::JustPressed(physical, pressed_physical, bindings.shortcut_cheats))
		return FrameResult::OpenCheats;

	// L2 + D-pad: quick save / load / slot (the D-pad doesn't reach the game while L2 is held)
	fe::Settings& cfg = fe::Config();
	const bool l2 = (p1.raw_buttons & SCE_PAD_BUTTON_L2) != 0;
	if (l2)
	{
		if (pressed & SCE_PAD_BUTTON_UP)
			Osd(SaveState(cfg.state_slot) ? "State saved to slot %d" : "Could not save slot %d", cfg.state_slot);
		else if (pressed & SCE_PAD_BUTTON_DOWN)
			Osd(LoadState(cfg.state_slot) ? "State loaded from slot %d" : "Slot %d is empty", cfg.state_slot);
		else if (pressed & (SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT))
		{
			cfg.state_slot = (cfg.state_slot + ((pressed & SCE_PAD_BUTTON_RIGHT) ? 1 : 9)) % 10;
			Osd("State slot: %d%s", cfg.state_slot, StateExists(cfg.state_slot) ? " (used)" : " (empty)");
		}
	}

	// R2 held: fast forward
	g.fast_forward = (p1.raw_buttons & SCE_PAD_BUTTON_R2) != 0;

	// Multitap only when a third controller shows up.
	const bool want_tap = ps5input::Pad(2).connected || ps5input::Pad(3).connected;
	if (want_tap != g.multitap)
		SetControllers(want_tap);

	for (int p = 0; p < ps5input::kMaxPads; p++)
	{
		const ps5input::PadState& ps = ps5input::Pad(p);
		uint32_t b = ps.connected ? ps.buttons : 0;
		if (p == 0 && g.wait_release)
			b = 0;
		if (p == 0 && l2)
			b &= ~uint32_t(SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT);
		if (p == 0)
		{
			// Keep active shortcut chord buttons out of SNES button reports.
			if (fe::shortcuts::Held(physical, bindings.shortcut_pause))
				b &= ~fe::shortcuts::kChoices[bindings.shortcut_pause].buttons;
			if (fe::shortcuts::Held(physical, bindings.shortcut_list))
				b &= ~fe::shortcuts::kChoices[bindings.shortcut_list].buttons;
			if (fe::shortcuts::Held(physical, bindings.shortcut_cheats))
				b &= ~fe::shortcuts::kChoices[bindings.shortcut_cheats].buttons;
		}
		for (int i = 0; i < B_COUNT; i++)
			S9xReportButton(MakeId(p, i), (b & kPadFor[i]) != 0);
	}

	IPPU.RenderThisFrame = !g.fast_forward || (g.frame % 4) == 0;
	S9xMainLoop();
	g.frame++;
	if (g.frame == 60 || g.frame == 240 || g.frame % 3600 == 0)
		OrbisLog("[emu] frame %u: audio queued %d, underruns %llu, shader %.1f ms", g.frame, ps5audio::Queued(),
			(unsigned long long)ps5audio::Underruns(), ps5video::TakeShaderMs());

	if (!g.fast_forward && !Settings.Mute && ps5audio::Available())
	{
		if (IsPal())
			WaitAudioBelow(ps5audio::kCapacity / 2);
		else if (ps5audio::Queued() > ps5audio::kCapacity * 3 / 4)
			WaitAudioBelow(ps5audio::kCapacity / 2);
	}
	else if (!g.fast_forward && IsPal())
	{
		// muted PAL game, or no sound output: pace on the clock instead
		static double next = 0;
		const double now = Now();
		if (next < now - 0.1)
			next = now;
		next += 0.02;
		if (next > now)
			usleep(useconds_t((next - now) * 1e6));
	}

	if (g.quit)
		return FrameResult::Quit;
	return FrameResult::Continue;
}
} // namespace emu

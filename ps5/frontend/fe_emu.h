// Snes9x PS5 frontend: the Snes9x core behind a small API, and the S9x* port callbacks.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace emu
{
bool InitCore(int argc, char** argv);
void DeinitCore();

bool LoadGame(const std::string& path);
void CloseGame(); // writes the battery save
bool GameLoaded();

// Native Snes9x cheat groups, enabled/disabled live and persisted per game.
int CheatCount();
std::string RomBase();
std::string RomNoIntro();
std::string CheatStatus(); // Which file was found, or why codes could not be loaded.
// Reads the cheat file again when it changed on disk and puts its cheats in the running game; true when the list
// changed. now: at once (the cheat menu just opened); otherwise at most once a second, and a file that is still being
// copied waits for the next look. Called by the cheat menu; RunFrame does the same for a game being played.
bool RefreshCheats(bool now);
std::string CheatName(int index);
bool CheatEnabled(int index);
bool ToggleCheat(int index);
void SetAllCheats(bool enabled);
bool SaveCheats();
std::string GameName(); // the file name without its extension

// Settings from fe::Config() -> Snes9x (aspect, scanlines, FPS counter, sound, ...).
void ApplySettings();

enum class FrameResult
{
	Continue,
	OpenMenu, // configurable pause-menu chord
	OpenCheats, // configurable quick cheat-manager chord
	BackToList, // configurable quick library chord
	Quit, // the core asked to exit (S9xExit)
};
// Polls the pads, runs one SNES frame, shows it and paces to the console's audio/video clock.
FrameResult RunFrame();

bool SaveState(int slot);
bool LoadState(int slot); // refused (false) while RetroAchievements' hardcore mode applies
bool StateExists(int slot);
void Reset(); // also starts the achievements over (fe_ra.h)
// 2.11: RetroAchievements' hardcore mode applies (fe_ra.h): no state may be loaded
bool LoadStateRefused();
// 2.11: hardcore mode started or stopped applying: the cheats off or on again, Transparency forced on or not
void HardcoreChanged();
// 2.11 review: why hardcore can't start in the game running ("" when it can): snes9x.conf forced its region to PAL
std::string HardcoreRefusal();
void Osd(const char* fmt, ...) __attribute__((format(printf, 1, 2))); // message drawn in the picture

// Redraws the last frame into the surface (the pause menu draws over it).
void RedrawLastFrame();
// Menus closed: held navigation/shortcut buttons must not reach the SNES until all are released.
void AfterMenu();
} // namespace emu

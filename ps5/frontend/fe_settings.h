// Snes9x PS5 frontend: settings kept in /data/snes9x/snes9x-ps5.ini (key=value lines).
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace fe
{
struct Settings
{
	int shader = 1; // ps5crt::Shader: CRT Easymode style unless another is picked (0 = Off)
	int aspect = 0; // ps5video::Aspect
	bool scanlines = false; // with the shader Off
	bool show_fps = false;
	bool audio = true;
	int state_slot = 0; // 0..9
	int shortcut_pause = 0; // L3+R3 by default
	int shortcut_list = 3; // Touchpad+Options
	int shortcut_cheats = 1; // L2+R3
	bool transparency = true; // Snes9x "Transparency"
	int superfx_clock = 100; // % (Snes9x SuperFXClockMultiplier)
	bool covers_download = true; // fetch box art from libretro-thumbnails
	bool cheats_auto_download = true; // queue matching cheats on first ROM launch if missing
	bool debug_logs = true;      // boot.log and the others in /data/snes9x/logs (OrbisLogSetEnabled)
	std::string last_dir; // the browser reopens here
	std::string last_rom; // and puts the cursor on this file

	void Load();
	void Save() const;
};

Settings& Config();
} // namespace fe

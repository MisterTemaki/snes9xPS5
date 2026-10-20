// Snes9x PS5 frontend: the border around the picture (art in /data/snes9x/borders, see ps5video::Border).
//
// The pack, in /data/snes9x/borders:
//   default.png         optional: for a game with no border of its own
//   default.ini         optional: its settings
//   games/<name>.png    a border for one game; The Bezel Project's SNES bezels (GameBezels/SNES/*.png) work as they are
//   games/<name>.ini    optional, same name; keys: window=x,y,w,h (at least 480x270; use this window instead of finding
//                       it, and clear the picture inside it), off=1 (this game has no border even in Auto)
// A PNG is 1920x1080, 8 bits per channel, RGBA or palette with tRNS (or RGB with a window=, as it has no clear area of its
// own), under 8 MiB; its window is the clear area around the centre.
//
// Setting Auto picks, first hit wins (a file that is there but unusable is logged and skipped):
//   1. games/<ROM file name>.ini says off=1                       -> no border
//   2. games/<ROM file name>.png
//   3. the cover matcher over the names in games/ (renamed and translated ROMs find their game's border)
//   4. default.png
//   5. the frame built into the ELF
// Default frame uses 4 and 5 only; Off sets none.
//
// The window follows the Aspect ratio setting: the art is refitted so the window has the picture's shape (4:3, 8:7 for
// Pixel perfect, 16:9 for Stretch), which is how a bezel drawn for a 4:3 game fits any of them, with the side art scaled evenly
// and cropped, not squeezed. Integer scale cuts it to the
// whole-multiple picture (1024x896 at 4x). See FitToAspect in fe_borderimg.h.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "ProsperoVideo.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fe
{
struct GameInfo;
}

namespace fe::border
{
// Picks and loads the border for the game that was just loaded (its ROM path and the CRC the core computed), or clears it
// when the setting is Off. Calls ps5video::SetBorder. Never throws, never fails the game: a problem is one OrbisLog line.
void Select(const std::string& rom_path, uint32_t rom_crc);
// The Border or Aspect ratio setting changed while a game is loaded: re-pick (and re-fit) for the same game. No-op with no
// game, and while the settings are the ones already applied (every other settings change comes through here too).
void Reselect();
// The game was closed.
void Release();

// The choice itself, without the renderer, for the tests.
struct Pick
{
	std::shared_ptr<const ps5video::Border> border; // null: none
	std::string source; // "own file", "matched", "default.png", "built-in", "off" (an .ini says so), "" (nothing usable)
	std::string file; // the picture's file name inside the pack
};
// pack: the borders folder; setting: 1 Auto or 2 Default frame (0 is the caller's: no border); game: named as ScanGames does;
// aspect: the Aspect ratio setting the window is fitted to (the default, Aspect::Count, keeps the art as drawn).
Pick Choose(const std::string& pack, int setting, const GameInfo& game, ps5video::Aspect aspect = ps5video::Aspect::Count);
} // namespace fe::border

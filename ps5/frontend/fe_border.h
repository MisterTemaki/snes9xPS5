// Snes9x PS5 frontend: the border around the picture (art in /data/snes9x/borders, see ps5video::Border).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace fe::border
{
// Picks and loads the border for the game that was just loaded (its ROM path and the CRC the core computed), or clears it
// when the setting is Off. Calls ps5video::SetBorder. Never throws, never fails the game: a problem is one OrbisLog line.
void Select(const std::string& rom_path, uint32_t rom_crc);
// The setting changed while a game is loaded: re-pick for the same game. No-op with no game.
void Reselect();
// The game was closed.
void Release();
} // namespace fe::border

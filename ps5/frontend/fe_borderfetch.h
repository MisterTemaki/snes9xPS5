// Snes9x PS5 frontend: the borders downloaded from The Bezel Project (ADR 006), as SwanStationPS5 downloads its PS1 bezels.
//
// The app ships the names of the SNES borders The Bezel Project has (data/snes-border-index.txt), never the pictures. A
// game's border is the index name the cover matcher picks for it ("Super Mario World (USA)"). The helper payload downloads
// the picture, as it does the covers (fe_coverworker.h; the sandboxed app cannot): the app writes borders/priority.txt
// (the game being played) or borders/wanted.txt (Settings, Download all borders), and the file lands in borders/games/ under
// The Bezel Project's own name, so the pack finds it like any hand-copied border (fe_border.h). A 404 leaves <name>.missing.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_coverfetch.h"
#include "fe_games.h"

#include <string>
#include <vector>

namespace fe::border
{
// The names (no extension) of the SNES borders The Bezel Project has, in file order.
const std::vector<std::string>& IndexNames();

// The Bezel Project's name for this game's border (the cover matcher over IndexNames), "" when it has none for it.
std::string BezelNameFor(const GameInfo& game);

// The picture's download address; SNES9X_BORDER_URL (a folder address ending in "/") replaces the base for the host tests.
std::string BezelUrl(const std::string& name);

// The download this game's border needs: false when The Bezel Project has none for it, the picture is already in `pack` (the
// borders folder), or a 404 was seen in the last 30 days.
bool BorderWanted(const std::string& pack, const GameInfo& game, WantedCover* out);

// The downloads the library still needs, one per bezel (several ROMs of a game share it); a game with its own games/<ROM
// name>.png is left out.
std::vector<WantedCover> MissingBorders(const std::string& pack, const std::vector<GameInfo>& games);

// borders/wanted.txt (the library, the helper works through it last) and borders/priority.txt (the game being played, first).
void WriteWantedBorders(const std::string& pack, const std::vector<WantedCover>& wanted);
void WritePriorityBorders(const std::string& pack, const std::vector<WantedCover>& wanted);
} // namespace fe::border

// The shelf's search (frontend/fe_search.h, 2.10): which games match what was typed, and how the shelf's places map to
// the library once a search cuts a tab (the view the selection, the skip keys and the covers count in).
// SPDX-License-Identifier: MIT

#include "fe_search.h"
#include "fe_tabs.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace fe;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c)                                                          \
	do                                                                    \
	{                                                                     \
		g_checks++;                                                       \
		if (!(c))                                                         \
		{                                                                 \
			printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
			g_fail++;                                                     \
		}                                                                 \
	} while (0)

static GameInfo Game(const std::string& path, const std::string& title, bool net = false)
{
	GameInfo g;
	g.path = path;
	g.title = title;
	const size_t slash = path.find_last_of('/'), dot = path.find_last_of('.');
	g.file_base = path.substr(slash + 1, dot == std::string::npos || dot < slash ? std::string::npos : dot - slash - 1);
	g.ext = dot == std::string::npos ? "" : path.substr(dot);
	g.on_network = net;
	return g;
}

static bool Match(const GameInfo& g, const char* typed)
{
	return search::Matches(g, search::Query(typed));
}

int main()
{
	// ---- folding: capitals, accents, two-letter forms, apostrophes; other characters and broken UTF-8 as they are
	CHECK(search::Fold("Super MARIO World") == "super mario world");
	CHECK(search::Fold("Pok\xC3\xA9mon") == "pokemon");                    // é
	CHECK(search::Fold("\xC3\x89\xC3\x80\xC3\x87\xC3\x91\xC3\x9C") == "eacnu"); // ÉÀÇÑÜ
	CHECK(search::Fold("Stra\xC3\x9F" "e") == "strasse");                  // ß
	CHECK(search::Fold("\xC3\x86on \xC5\x92uvre") == "aeon oeuvre");        // Æ Œ
	CHECK(search::Fold("\xC5\x81\xC3\xB3""d\xC5\xBA") == "lodz");           // Łódź
	CHECK(search::Fold("Yoshi\xE2\x80\x99s") == "yoshi's");                // ’
	CHECK(search::Fold("3 \xC3\x97 4") == "3 \xC3\x97 4");                 // × stays
	CHECK(search::Fold("\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA") == "\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA"); // マリオ stays
	CHECK(search::Fold("A\xC9" "B") == "a\xC9" "b");                       // a lone byte: kept, not read as É
	CHECK(search::Fold("\xC3") == "\xC3");                                 // cut short: kept
	CHECK(search::Fold("") == "");

	// ---- the query: folded, spaces at the ends off; nothing typed (or only spaces) is no search
	CHECK(search::Query("  Mario  ") == "mario");
	CHECK(search::Query("   ").empty());
	CHECK(search::Query("").empty());
	CHECK(search::Query("super  mario") == "super  mario"); // inside, as typed

	// ---- matching: the shelf's name or the file name, any case, accents folded on both sides
	const GameInfo smw = Game("/data/snes9x/roms/Super Mario World (USA).sfc", "Super Mario World");
	const GameInfo poke = Game("/data/snes9x/roms/Pocket Monsters Stadium.sfc", "Pok\xC3\xA9mon Stadium");
	const GameInfo zelda = Game("/data/snes9x/roms/zelda3.smc", "The Legend of Zelda - A Link to the Past");
	const GameInfo hack = Game("/smb/10.0.0.5/games/RPG/Kirby Dream Land 3 (Hack).zip", "kirby dream land 3 (hack)", true);
	CHECK(Match(smw, "mario"));
	CHECK(Match(smw, "MARIO WORLD"));
	CHECK(Match(smw, "rio wo")); // a part of a word
	CHECK(Match(smw, "(usa)"));  // only in the file name
	CHECK(Match(smw, ".sfc"));   // the file name with its extension
	CHECK(!Match(smw, "zelda"));
	CHECK(Match(poke, "pokemon"));
	CHECK(Match(poke, "POK\xC3\x89MON")); // typed with an accent too
	CHECK(Match(poke, "pocket"));          // the file name
	CHECK(Match(zelda, "zelda3"));          // the file name, the title differs
	CHECK(Match(zelda, "link to the past"));
	CHECK(Match(hack, "dream land"));
	CHECK(!Match(hack, "rpg"));     // the folder isn't searched
	CHECK(!Match(hack, "10.0.0.5")); // nor the share
	CHECK(Match(smw, ""));          // nothing typed: everything
	CHECK(Match(smw, "   "));
	CHECK(!Match(smw, "mario kart"));

	// ---- the view: a tab (fe_tabs.h) cut by the search, in shelf order, its places mapped to the library
	std::vector<GameInfo> games = {
		Game("/data/snes9x/roms/F-Zero (USA).sfc", "F-Zero"),
		Game("/data/snes9x/roms/Super Mario Kart (USA).sfc", "Super Mario Kart"),
		Game("/mnt/usb0/snes9x/roms/Super Mario All-Stars (USA).sfc", "Super Mario All-Stars"),
		Game("/data/snes9x/roms/Super Mario World (USA).sfc", "Super Mario World"),
		Game("/smb/10.0.0.5/games/Mario Paint (USA).sfc", "Mario Paint", true),
		Game("/data/snes9x/roms/Super Metroid (USA).sfc", "Super Metroid"),
	};
	games[2].on_usb = true;
	tabs::OrderByTab(games); // Internal: F-Zero, Kart, World, Metroid; USB: All-Stars; Network: Paint
	const std::vector<int> internal = tabs::View(games, tabs::Internal);
	CHECK(internal == std::vector<int>({0, 1, 2, 3}));
	const std::vector<int> mario = search::Filter(games, internal, "mario");
	CHECK(mario == std::vector<int>({1, 2}));
	CHECK(games[size_t(mario[0])].title == "Super Mario Kart" && games[size_t(mario[1])].title == "Super Mario World");
	CHECK(search::Filter(games, internal, "") == internal);  // no search: the tab as it is
	CHECK(search::Filter(games, internal, "  ") == internal);
	CHECK(search::Filter(games, internal, "zzz").empty());   // no match: an empty view (the shelf says so)
	// the search stays when the tab changes: the same text cuts the other tabs
	CHECK(search::Filter(games, tabs::View(games, tabs::Usb), "mario") == std::vector<int>({4}));
	CHECK(search::Filter(games, tabs::View(games, tabs::Network), "MARIO") == std::vector<int>({5}));
	CHECK(search::Filter(games, tabs::View(games, tabs::Network), "metroid").empty());
	// indices that aren't the library's are left out, not read
	CHECK(search::Filter(games, {-1, 3, 99}, "e") == std::vector<int>({3}));
	// a place in the view and back
	CHECK(search::PositionOf(mario, 2) == 1 && search::PositionOf(mario, 0) == -1);

	// ---- the skip keys and the cursor count in the view: L2 / R2 stop at its ends (fe_shelf.cpp's rule)
	{
		const int n = int(mario.size());
		int sel = 0;
		sel = std::min(n - 1, sel + 10);
		CHECK(sel == 1 && mario[size_t(sel)] == 2);
		sel = std::max(0, sel - 10);
		CHECK(sel == 0 && mario[size_t(sel)] == 1);
	}

	// ---- the covers' order (the cover service loads and keeps the selection's neighbours in the view)
	{
		search::ViewOrder lib; // not set: the library's order, as up to 2.9
		lib.Clear(10);
		CHECK(lib.Around(5, 2) == std::vector<int>({5, 4, 6, 3, 7}));
		CHECK(lib.Around(0, 2) == std::vector<int>({0, 1, 2}));
		CHECK(lib.Around(9, 1) == std::vector<int>({9, 8}));
		CHECK(lib.Near(3, 5, 2) && !lib.Near(2, 5, 2));
		CHECK(lib.Around(10, 2).empty() && lib.Around(-1, 2).empty());

		search::ViewOrder cut; // a search left games 1, 4, 7 and 9 of ten
		cut.Set({1, 4, 7, 9}, 10);
		CHECK(cut.Around(4, 1) == std::vector<int>({4, 1, 7})); // the neighbours on the shelf, not 3 and 5
		CHECK(cut.Around(9, 12) == std::vector<int>({9, 7, 4, 1}));
		CHECK(cut.Near(1, 7, 2) && !cut.Near(1, 9, 2));
		CHECK(!cut.Near(5, 4, 9));      // a game the view doesn't show is never kept
		CHECK(cut.Around(5, 3).empty()); // a focus outside the view loads nothing
		cut.Set({}, 10);                 // a search with no match
		CHECK(cut.Around(1, 3).empty() && !cut.Near(1, 1, 0));
		cut.Set({3, 3, 12, -2, 2}, 10);  // duplicates and outsiders dropped
		CHECK(cut.Around(3, 1) == std::vector<int>({3, 2}));
	}

	// ---- thousands of games: the filter is quick (it runs at each key on the keyboard's count, and on Done)
	{
		std::vector<GameInfo> big;
		for (int i = 0; i < 10000; i++)
		{
			char path[128], title[96];
			snprintf(path, sizeof(path), "/data/snes9x/roms/sub%d/Game %05d (USA).sfc", i % 37, i);
			snprintf(title, sizeof(title), i % 100 == 7 ? "Pok\xC3\xA9mon Edition %d" : "Game Number %d", i);
			big.push_back(Game(path, title));
		}
		const std::vector<int> all = tabs::View(big, tabs::Internal);
		const auto t0 = std::chrono::steady_clock::now();
		const std::vector<int> hits = search::Filter(big, all, "POKEMON");
		const std::vector<int> one = search::Filter(big, all, "game 01234 (");
		const std::vector<int> none = search::Filter(big, all, "no such game");
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		CHECK(hits.size() == 100);
		CHECK(!hits.empty() && hits.front() == 7 && hits.back() == 9907);
		CHECK(one == std::vector<int>({1234}));
		CHECK(none.empty());
		printf("10000 games, three searches: %.1f ms\n", ms);
		CHECK(ms < 1500.0); // sanitizers included; a few ms each without
	}

	printf("search: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}

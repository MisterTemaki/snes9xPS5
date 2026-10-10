// Standalone regression tests for the shared filename matcher (no PS5 SDK required).
#include "../frontend/fe_titlematch.h"
#include "../frontend/fe_cheatlookup.h"
#include "../frontend/fe_artaliases.h"
#include <cassert>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <algorithm>
int main()
{
    using fe::titles::Best;
    const std::vector<std::string> art = {
        "Kidou Butouden G Gundam (Japan)",
        "Kidou Senshi V Gundam (Japan)",
        "Super Mario World (USA)",
        "Super Mario World 2 - Yoshi's Island (USA)",
        "Secret of Mana (USA)",
        "Secret of Mana 2 (Japan)",
        "Mega Man X (USA)"
    };
    assert(Best(art,"Kidou Butoden G-Gundam") == "Kidou Butouden G Gundam (Japan)");
    assert(Best(art,"Super Mario World (USA)") == "Super Mario World (USA)");
    assert(Best(art,"Mega-Man X.sfc") == "Mega Man X (USA)");
    assert(Best(art,"Completely Different Game (USA)").empty());
    assert(Best(art,"Secret of Mana 2 (Japan)") == "Secret of Mana 2 (Japan)");
    assert(Best(art,"Secret of Mana (USA)") == "Secret of Mana (USA)");
    const std::vector<std::string> cheats = {
        "Kidou Butouden G Gundam (Japan).cht",
        "Super Mario World (USA).cht",
        "Secret of Mana (USA) (Game Genie).cht"
    };
    assert(Best(cheats,"Kidou Butoden G-Gundam") == cheats[0]);
    assert(Best(cheats,"Super Mario World (USA)") == cheats[1]);
    assert(Best(cheats,"This Name Has No Match").empty());
    // EarthBound can be named EarthBound.sfc, while the manually copied
    // Libretro file is usually EarthBound (USA).cht or a named subset.
    const std::vector<std::string> local = {
        "/data/snes9x/cheats/EarthBound (USA) (Action Replay).cht",
        "/data/snes9x/cheats/EarthBound (USA).cht",
        "/data/snes9x/cheats/EarthBound (USA) (Game Genie).cht",
        "/data/snes9x/cheats/EarthBound Zero (Japan).cht",
        "/data/snes9x/cheats/Chrono Trigger (USA).cht"
    };
    assert(fe::cheatlookup::Best(local,"EarthBound","EarthBound (USA)") == local[1]);
    assert(fe::cheatlookup::Best(local,"EarthBound","") == local[1]);
    assert(fe::cheatlookup::Best(local,"EarthBound (USA)","EarthBound (USA)") == local[1]);
    assert(fe::cheatlookup::Best(local,"Chrono Trigger","") == local[4]);
    assert(fe::cheatlookup::Best(local,"EarthBound Zero","") == local[3]);
    assert(fe::cheatlookup::Best(local,"Different Game","").empty());
    assert(fe::cheatlookup::Best({local[0],local[2]},"EarthBound","EarthBound (USA)") == local[0] ||
           fe::cheatlookup::Best({local[0],local[2]},"EarthBound","EarthBound (USA)") == local[2]);
    // A real game may have a title/screenshot but no matching Named_Boxarts.
    auto index = [](const char* path) {
        std::ifstream file(path);
        assert(file.good());
        std::vector<std::string> entries;
        std::string name;
        while (std::getline(file,name)) if (!name.empty() && name[0]!='#') entries.push_back(name);
        return entries;
    };
    const auto boxarts=index("ps5/frontend/data/snes-cover-index.txt");
    const auto titles=index("ps5/frontend/data/snes-title-index.txt");
    const auto snaps=index("ps5/frontend/data/snes-snap-index.txt");
    const std::string missing_box="BASS Masters Classic - Pro Edition (USA)";
    assert(std::find(boxarts.begin(),boxarts.end(),missing_box)==boxarts.end());
    assert(std::find(titles.begin(),titles.end(),missing_box)!=titles.end());
    assert(std::find(snaps.begin(),snaps.end(),missing_box)!=snaps.end());
    assert(Best(titles,missing_box)==missing_box);
    assert(Best(snaps,missing_box)==missing_box);
    // Screenshots supplied from the PS5: translated/renamed ROM filenames
    // must resolve to actual entries in all three Libretro indexes.
    const std::vector<std::pair<std::string,std::string>> translated = {
        {"Final Fantasy 6 (ENG) # SNES", "Final Fantasy VI (Japan)"},
        {"Dragon-Ball-Z - Super Gokuden 2 (ENG) # SNES", "Dragon Ball Z - Super Gokuu Den - Kakusei Hen (Japan)"},
        {"Dragon-Ball Z - Super Butouden 3 (ENG) # SNES", "Dragon Ball Z - Super Butouden 3 (Japan)"},
        {"Dragon-Ball Z - Super Butouden (ENG) # SNES", "Dragon Ball Z - Super Butouden (Japan)"},
        {"Dragon-Ball Z - Hyper Dimension (ENG) # SNES", "Dragon Ball Z - Hyper Dimension (Japan)"},
        {"Dragon Quest 1 and 2 (ENG) # SNES", "Dragon Quest I _ II (Japan)"},
        {"Dai 3 Ji - Super Robot Taisen (ENG) # SNES", "Dai-3-ji Super Robot Taisen (Japan)"},
        {"Bahamut Lagoon (ENG) # SNES", "Bahamut Lagoon (Japan)"}
    };
    for (const auto& item : translated)
    {
        const std::string mapped=fe::artaliases::Canonical(item.first);
        assert(mapped==item.second);
        assert(std::find(boxarts.begin(),boxarts.end(),mapped)!=boxarts.end());
        assert(std::find(titles.begin(),titles.end(),mapped)!=titles.end());
        assert(std::find(snaps.begin(),snaps.end(),mapped)!=snaps.end());
    }
    assert(fe::artaliases::Canonical("Dragon Ball Z - Super Butouden (France)").empty());
    assert(fe::artaliases::Canonical("Bahamut Lagoon (USA)").empty());
    assert(fe::artaliases::Canonical("Pokemon Gold & Silver").empty());
    assert(fe::artaliases::Canonical("Aladdin 2000").empty());
    assert(fe::artaliases::Canonical("Final Fantasy 6 (ENG) # SNES")=="Final Fantasy VI (Japan)");
    assert(fe::artaliases::TrimFrontendSuffix("Chrono Trigger (ENG) # SNES")=="Chrono Trigger (ENG)");
    const auto chrono = Best(boxarts,fe::artaliases::TrimFrontendSuffix("Chrono Trigger (ENG) # SNES"));
    assert(!chrono.empty() && std::find(boxarts.begin(),boxarts.end(),chrono)!=boxarts.end());
    assert(Best(boxarts,"Aladdin 2000").empty()); // never use unrelated official Aladdin image
    assert(Best(boxarts,"Pokemon Gold & Silver").empty());
    std::cout << "Title matcher regressions passed\n";
    return 0;
}

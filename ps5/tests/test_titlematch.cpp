// Standalone regression tests for the shared filename matcher (no PS5 SDK required).
#include "../frontend/fe_titlematch.h"
#include "../frontend/fe_cheatlookup.h"
#include "../frontend/fe_artaliases.h"
#include "../frontend/fe_artmatch.h"
#include "../frontend/fe_dbart.h"
#include "../frontend/fe_romheader.h"
#include <cassert>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
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
    // the index (first four characters) gives the plain scan's answers: every art name, its variants, and the
    // names of the other lists, looked up both ways
    {
        const fe::titles::Index& ix = fe::titles::IndexOf(boxarts);
        size_t checked = 0;
        auto same = [&](const std::string& q) {
            assert(Best(ix, q) == Best(boxarts, q));
            ++checked;
        };
        for (const auto* list : {&boxarts, &titles, &snaps})
            for (const std::string& n : *list)
            {
                same(n);
                same(n.substr(0, n.find(" (")));
                std::string typo = n;
                if (typo.size() > 6) typo[typo.size() / 2] = 'x';
                same(typo);
            }
        for (const auto& item : translated)
            same(item.first);
        std::cout << "Index lookups equal to the plain scan: " << checked << "\n";
    }

    // Renamed and fan-translated ROMs (fe_artmatch.h): the art is filed under the Japanese No-Intro name.
    {
        namespace am = fe::artmatch;
        const fe::artmatch::Index& ix = am::IndexOf(boxarts);
        const std::string srw4 = "Dai-4-ji Super Robot Taisen (Japan)";
        assert(std::find(boxarts.begin(), boxarts.end(), srw4) != boxarts.end());
        // what a name says besides its title
        assert(am::Parse("Super Robot Wars 4 (English).sfc").title == "Super Robot Wars 4");
        assert(am::Parse("Super Robot Wars 4 (English).sfc").translated);
        assert(am::Parse("Final Fantasy 6 [T+Eng v1.1]").title == "Final Fantasy 6");
        assert(am::Parse("Chrono Trigger (ENG) # SNES").title == "Chrono Trigger");
        assert(am::Parse("Dragon_Quest_V_English_Patched_v2.0").title == "Dragon Quest V");
        assert(am::Parse("Mother 2 (Japan, USA) (En,Ja)").region == "Japan");
        assert(am::Parse("Super Mario World (U) [!]").region == "USA");
        assert(!am::Parse("Super Mario World (USA)").translated);
        // spelling, numerals and word order do not tell two names of one game apart
        assert(am::TitleKey("Final Fantasy VI (Japan)") == am::TitleKey("final_fantasy_6 (English)"));
        assert(am::TitleKey("Kidou Butouden G Gundam (Japan)") == am::TitleKey("Kidou Butoden G-Gundam"));
        assert(am::TitleKey("Legend of Zelda, The - A Link to the Past (USA)") == am::TitleKey("The Legend of Zelda - A Link to the Past"));
        assert(am::TitleKey("Super Mario World (USA)") != am::TitleKey("Super Mario World 2 - Yoshi's Island (USA)"));
        // Jason's example, and the ways a translator or a library tool may write it
        for (const char* name : {"Super Robot Wars 4 (English)", "Super Robot Wars 4 (English).sfc", "Super Robot Wars 4 [T+Eng v1.1]",
                                 "Super Robot Wars 4", "Super Robot Wars IV (Translated)", "super_robot_wars_4_english_patched",
                                 "Super Robot Wars 4 (ENG) # SNES", "Super Robot Wars 4 - English v1.02", "Super Robot Taisen 4 (En)",
                                 "SUPER ROBOT WARS 4 (Fan Translation)"})
            assert(am::Resolve(ix, name) == srw4);
        // the names a hash database gives translated dumps ("T-Eng", year, group), and a Hepburn/Wars retitle
        assert(am::Parse("Super Robot Wars 4 T-Eng (2026)(Aeon Genesis)").title == "Super Robot Wars 4");
        assert(am::Parse("Super Robot Wars 4 T-Eng (2026)(Aeon Genesis)").translated);
        assert(am::Parse("Dai-4-Ji Super Robot Taisen (J) (V1.0) [T Eng025_Fraka]").title == "Dai-4-Ji Super Robot Taisen");
        assert(am::Resolve(ix, "Super Robot Wars 4 T-Eng (2026)(Aeon Genesis)") == srw4);
        assert(am::Resolve(ix, "Dai-4-Ji Super Robot Taisen (J) (V1.0) [T Eng025_Fraka]") == srw4);
        assert(am::Resolve(ix, "Super Robot Wars EX T-Eng (2018)(Aeon Genesis)") == "Super Robot Taisen EX (Japan)");
        assert(am::TitleKey(am::Resolve(ix, "Super Robot Wars 3 T-Eng v2.00m2 (2019)(Aeon Genesis)")) == am::TitleKey("Dai-3-ji Super Robot Taisen"));
        // the main title is kept, the subtitle is the translator's own; a different number is a different game
        assert(am::Resolve(ix, "Flying Hero - Bugyuru's Adventure (English)") == "Flying Hero - Bugyuru no Daibouken (Japan)");
        assert(am::Resolve(ix, "Chrono Trigger - Retranslation") == "Chrono Trigger (USA)");
        assert(am::Resolve(ix, "Ranma 1-2 - Hard Battle II - Super Move Hustle").empty());
        // a game that explains the whole name beats the main title alone: Gun Hazard is not Front Mission
        assert(am::Resolve(ix, "Front Mission - Gun Hazard (English)") == "Front Mission Series - Gun Hazard (Japan)");
        assert(am::Resolve(ix, "Front Mission - Gun Hazard T-Eng (2004)(Aeon Genesis)") == "Front Mission Series - Gun Hazard (Japan)");
        assert(am::Resolve(ix, "Front Mission T-Eng v1.02 (2023)(mteam, F.H.)") == "Front Mission (Japan)");
        assert(am::Resolve(ix, "Front Mission - Something Else") == "Front Mission (Japan)"); // no such game: the main title's art, as for a hack
        // close in spelling is not enough when the numbers disagree
        assert(am::NumbersConflict("Dragon Quest III (English)", "Dragon Quest I & II (Japan)"));
        assert(am::NumbersConflict("Final Fantasy III", "Final Fantasy II (USA)"));
        assert(!am::NumbersConflict("Fire Emblem 5 - Thracia 776", "Fire Emblem - Thracia 776 (Japan)"));
        assert(!am::NumbersConflict("Super Mario World", "Super Mario World (USA)"));
        // a different number, no number, a game that is not in the list, a single common word: nothing
        assert(am::Resolve(ix, "Super Robot Wars 5 (English)").empty());
        assert(am::Resolve(ix, "Super Robot Wars (English)").empty());
        assert(am::Resolve(ix, "Completely Different Game (English)").empty());
        assert(am::Resolve(ix, "Super (English)").empty());
        // subtitles left out, roman numerals, Hepburn spelling: still one game
        assert(am::Resolve(ix, "Dragon Quest V (English)") == "Dragon Quest V - Tenkuu no Hanayome (Japan)");
        assert(am::Resolve(ix, "dragon quest 5 english") == "Dragon Quest V - Tenkuu no Hanayome (Japan)");
        assert(am::TitleKey(am::Resolve(ix, "Final Fantasy 5 (English)")) == am::TitleKey("Final Fantasy V"));
        assert(am::Resolve(ix, "Kidou Butoden G-Gundam (English)") == "Kidou Butouden G Gundam (Japan)");
        // a number that differs is a different game, whatever else matches
        assert(am::TitleKey(am::Resolve(ix, "Dragon Quest III (English)")) != am::TitleKey("Dragon Quest I & II"));
        // region: the tag wins; without one USA comes first, a clean release before a Rev
        assert(am::Resolve(ix, "Chrono Trigger (Japan) (English)") == "Chrono Trigger (Japan)");
        assert(am::Resolve(ix, "Chrono Trigger (English)") == "Chrono Trigger (USA)");
        assert(am::Resolve(ix, "Dai 4 Ji Super Robot Taisen (Rev 1)") == srw4);
        // every name of each list, written the ways above, finds its own game (and never another)
        size_t variants = 0;
        for (const auto* list : {&boxarts, &titles, &snaps})
        {
            const fe::artmatch::Index& lx = am::IndexOf(*list);
            for (const std::string& name : *list)
            {
                const std::string base = am::Parse(name).title;
                std::string underscored = base;
                std::replace(underscored.begin(), underscored.end(), ' ', '_');
                for (const std::string& variant : {base + " (English)", am::Lower(base) + " [T+Eng v1.1]", underscored + "_English",
                                                   name + ".sfc"})
                {
                    ++variants;
                    const std::string found = am::Resolve(lx, variant);
                    if (am::TitleKey(found) != am::TitleKey(name))
                        std::cout << "Resolve(\"" << variant << "\") = \"" << found << "\"\n";
                    assert(am::TitleKey(found) == am::TitleKey(name));
                }
            }
        }
        // a name with one rare word spoiled finds its game or nothing, never another game
        size_t spoiled = 0;
        for (const std::string& name : boxarts)
        {
            std::vector<std::string> words = am::Tokens(am::Parse(name).title);
            if (words.size() < 3) continue;
            size_t rare = 0;
            for (size_t i = 0; i < words.size(); ++i)
                if (!am::IsDigits(words[i]) && (am::IsDigits(words[rare]) || am::Weight(ix, words[i]) > am::Weight(ix, words[rare]))) rare = i;
            std::string query;
            for (size_t i = 0; i < words.size(); ++i) query += (i == rare ? std::string("qzxwv") : words[i]) + " ";
            const std::string found = am::Resolve(ix, query);
            ++spoiled;
            assert(found.empty() || am::TitleKey(found) == am::TitleKey(name));
        }
        std::cout << "Word matcher: " << variants << " renamed variants and " << spoiled << " spoiled names\n";
    }
    // The extra CRC table (dumps No-Intro does not list): made by tools/make_extra_crc.py from a hash database.
    {
        namespace am = fe::artmatch;
        const auto nointro = index("ps5/frontend/data/snes-nointro.tsv");
        const auto extra = index("ps5/frontend/data/snes-extra-crc.tsv");
        std::vector<std::string> known;
        for (const std::string& line : nointro) known.push_back(line.substr(0, line.find('\t')));
        std::sort(known.begin(), known.end());
        std::map<std::string, std::string> by_crc;
        for (const std::string& line : extra)
        {
            const size_t tab = line.find('\t');
            assert(tab == 8);
            assert(!std::binary_search(known.begin(), known.end(), line.substr(0, 8))); // only what No-Intro lacks
            assert(by_crc.emplace(line.substr(0, 8), line.substr(9)).second);           // one name per CRC
        }
        const fe::artmatch::Index& ix = am::IndexOf(boxarts);
        const std::string srw4 = "Dai-4-ji Super Robot Taisen (Japan)";
        assert(by_crc.at("72BBACCA") == "Super Robot Wars 4 T-Eng (2026)(Aeon Genesis)");
        assert(am::Resolve(ix, by_crc.at("72BBACCA")) == srw4);
        assert(am::Resolve(ix, by_crc.at("6B36672F")) == srw4); // an older translation of the same game, named the Japanese way
        // how many English translations in it find art (the rest are games the art list does not have, or names too different)
        size_t translations = 0, found = 0;
        for (const auto& entry : by_crc)
        {
            const std::string name = am::Lower(entry.second);
            bool english = false;
            for (const char* mark : {"t-eng", "t+eng", "t eng", "[t-en", "[t+en", "(english", "translat"})
                english = english || name.find(mark) != std::string::npos;
            if (!english) continue;
            ++translations;
            if (!am::Resolve(ix, name).empty()) ++found;
        }
        std::cout << "Extra CRC table: " << by_crc.size() << " dumps, " << found << " of " << translations << " English translations find art\n";
        assert(translations > 300 && found * 100 >= translations * 75);
    }
    // The database covers (tools/make_db_art.py): games libretro-thumbnails has no art for.
    {
        namespace am = fe::artmatch;
        namespace da = fe::dbart;
        std::ifstream f("ps5/frontend/data/snes-db-art.tsv", std::ios::binary);
        assert(f);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        da::Table table;
        da::Parse(text.data(), text.data() + text.size(), table);
        assert(table.rows.size() > 300 && table.names.size() == table.rows.size());
        // by CRC32: a pirate cart that is a dump of Aladdin 2000, and Pokemon Stadium
        const da::Row* aladdin = da::Find(table, 0x752A25D3, "mystery pirate");
        assert(aladdin && aladdin->name == "Aladdin 2000");
        assert(da::Urls(*aladdin, "https://h/", "https://t/") == "https://h/AF768536EEED4254A29FE3F540CE879677D5FE1D");
        const da::Row* stadium = da::Find(table, 0xA8142249, "");
        assert(stadium && stadium->name == "Pokemon Stadium");
        // by the words of the name: the article after the comma, a different number or subtitle never matches
        const da::Row* kof = da::Find(table, 0, "King of Fighters 2000");
        assert(kof && da::Urls(*kof, "https://h/", "https://t/") == "https://t/27252-1.jpg");
        assert(da::Find(table, 0, "Street Fighter EX Plus Alpha (Japan)"));
        assert(!da::Find(table, 0, "Street Fighter III - Super Version")); // no database lists a SNES Street Fighter III
        assert(!da::Find(table, 0, "Completely Different Game"));
        assert(!da::Find(table, 0xDEADBEEF, ""));
        // the table lists only what libretro-thumbnails lacks, so no CRC of it is a game with art
        const auto nointro = index("ps5/frontend/data/snes-nointro.tsv");
        std::map<std::string, std::string> nointro_name;
        for (const std::string& line : nointro) nointro_name[line.substr(0, 8)] = line.substr(9);
        const std::set<std::string> covered(boxarts.begin(), boxarts.end());
        size_t overlap = 0;
        for (const auto& entry : table.by_crc)
        {
            char crc[9];
            std::snprintf(crc, sizeof crc, "%08X", entry.first);
            const auto n = nointro_name.find(crc);
            if (n != nointro_name.end() && covered.count(n->second)) ++overlap;
        }
        assert(overlap == 0);
        std::cout << "Database covers: " << table.rows.size() << " games, " << table.by_crc.size() << " CRCs\n";
    }
    // The ROM header: LoROM, HiROM, ExHiROM, a copier header, a wrong checksum, and no header at all.
    {
        namespace rh = fe::romheader;
        namespace am = fe::artmatch;
        auto image = [](size_t size, size_t at, const char* title, uint8_t map, uint8_t country, bool checksum_ok, bool copier) {
            std::vector<uint8_t> rom((copier ? 512 : 0) + size, 0);
            const size_t h = at + (copier ? 512 : 0);
            std::string padded = title;
            padded.resize(21, ' ');
            for (size_t i = 0; i < 21; ++i) rom[h + i] = uint8_t(padded[i]);
            rom[h + 0x15] = map;
            rom[h + 0x17] = 0x0A;
            rom[h + 0x19] = country;
            const uint16_t sum = 0x1234, comp = checksum_ok ? uint16_t(~sum) : uint16_t(0x0001);
            rom[h + 0x1C] = uint8_t(comp); rom[h + 0x1D] = uint8_t(comp >> 8);
            rom[h + 0x1E] = uint8_t(sum); rom[h + 0x1F] = uint8_t(sum >> 8);
            return rom;
        };
        auto read = [](const std::vector<uint8_t>& rom, rh::Info* info) {
            return rh::Read([&rom](uint64_t at, uint8_t* out, size_t n) {
                if (at + n > rom.size()) return false;
                std::memcpy(out, rom.data() + at, n);
                return true;
            }, rom.size(), info);
        };
        rh::Info info;
        assert(read(image(0x100000, 0x7FC0, "ZELDANODENSETSU", 0x20, 0, true, false), &info));
        assert(info.title == "ZELDANODENSETSU" && info.japan() && info.map == 0x20);
        assert(read(image(0x200000, 0xFFC0, "DRAGONQUEST5", 0x21, 0, true, false), &info) && info.title == "DRAGONQUEST5");
        assert(read(image(0x600000, 0x40FFC0, "EXHIROM TEST ROM", 0x25, 1, true, false), &info) && info.title == "EXHIROM TEST ROM" && !info.japan());
        assert(read(image(0x100000, 0x7FC0, "COPIER HEADER GAME", 0x20, 0, true, true), &info) && info.title == "COPIER HEADER GAME");
        assert(read(image(0x200000, 0xFFC0, "UNFIXED CHECKSUM", 0x31, 0, false, false), &info) && info.title == "UNFIXED CHECKSUM");
        assert(read(image(0x200000, 0xFFC0, "UNFIXED AND COPIER", 0x21, 0, false, true), &info) && info.title == "UNFIXED AND COPIER");
        std::vector<uint8_t> junk(0x200000);
        uint32_t seed = 12345; // a seeded generator: the same junk on every run
        for (uint8_t& b : junk) { seed = seed * 1664525u + 1013904223u; b = uint8_t(seed >> 24); }
        assert(!read(junk, &info));
        assert(!read(std::vector<uint8_t>(0x100000, 0), &info));
        assert(!read(std::vector<uint8_t>(100, 0x20), &info));
        // half-width katakana become romaji (Nihon-shiki spelling, as fe_artmatch.h's Phon)
        const uint8_t goemon[] = {0xB6, 0xDE, 0xDD, 0xCA, 0xDE, 0xDA, 0xBA, 0xDE, 0xB4, 0xD3, 0xDD, 0x20, 0xB7, 0xD7, 0xB7, 0xD7};
        assert(rh::Title(goemon, sizeof(goemon)) == "ganbaregoemon kirakira");
        const uint8_t shuppon[] = {0xBC, 0xAD, 0xAF, 0xCE, 0xDF, 0xDD, 0xB0};
        assert(rh::Title(shuppon, sizeof(shuppon)) == "syuppon");
        // the header names the game when the file name does not
        auto from_header = [&](const char* title, const char* map_name) {
            (void)map_name;
            return am::ResolveHeader(am::IndexOf(boxarts), title, true);
        };
        assert(from_header("ZELDANODENSETSU", "") == "Zelda no Densetsu - Kamigami no Triforce (Japan)");
        assert(from_header("FINAL FANTASY 6", "") == "Final Fantasy VI (Japan)");
        assert(from_header("DRAGONQUEST5", "") == "Dragon Quest V - Tenkuu no Hanayome (Japan)");
        assert(from_header("SUPER ROBOT TAISEN4", "") == "Dai-4-ji Super Robot Taisen (Japan)");
        assert(am::TitleKey(from_header("ganbaregoemon kirakira", "")) == am::TitleKey("Ganbare Goemon Kirakira Douchuu - Boku ga Dancer ni Natta Wake"));
        assert(from_header("SUPER BOMBERMAN", "") == "Super Bomberman (Japan)");
        assert(am::ResolveHeader(am::IndexOf(boxarts), "SUPER BOMBERMAN", false) == "Super Bomberman (USA)");
        // not told apart (a cut-short title that starts several games), too short, or generic: nothing
        assert(from_header("DRAGON BALL Z SUPER BUTO", "").empty());
        assert(from_header("TEST", "").empty());
        assert(from_header("SUPER NES", "").empty());
        assert(from_header("", "").empty());
    }
    std::cout << "Title matcher regressions passed\n";
    return 0;
}

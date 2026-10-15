// Recognize translated SNES ROM filenames when Libretro keeps original Japanese names.
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_titlematch.h"
#include <cctype>
#include <string>

namespace fe { namespace artaliases {
inline std::string TrimFrontendSuffix(std::string title)
{
    // e.g. "Bahamut Lagoon (ENG) # SNES" -> "Bahamut Lagoon (ENG)".
    // "ENG" is a patch/language tag already ignored by titles::Key.
    const size_t marker=title.rfind('#');
    if (marker!=std::string::npos)
    {
        std::string suffix=title.substr(marker+1);
        std::string normalized;
        for (unsigned char c:suffix)
            if (std::isalnum(c)) normalized+=char(std::tolower(c));
        if (normalized=="snes" || normalized=="superfamicom" || normalized=="sfc")
            title.erase(marker);
    }
    while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back())))
        title.pop_back();
    return title;
}
// A few unlicensed games deliberately lack a Libretro cover entry. They
// must not inherit the art of a different official game via a misleading CRC.
inline bool DistinctBootleg(const std::string& filename)
{
    const std::string key=titles::Key(TrimFrontendSuffix(filename));
    return key=="pokemongoldsilver" || key=="aladdin2000";
}
// Deliberately exact aliases for known translation names; never infer an unrelated
// game from a similar title or append a generic "(Japan)" suffix.
inline std::string Canonical(const std::string& filename)
{
    // Respect explicit cartridge regions. Translated filenames in this table
    // have no USA/Japan/France region tag, only a language/collection suffix.
    if (!titles::Region(filename).empty() ||
        filename.find("(France)")!=std::string::npos ||
        filename.find("(Germany)")!=std::string::npos ||
        filename.find("(Spain)")!=std::string::npos ||
        filename.find("(Korea)")!=std::string::npos) return "";
    const std::string key=titles::Key(TrimFrontendSuffix(filename));
    if (key=="finalfantasy6" || key=="finalfantasyvi")
        return "Final Fantasy VI (Japan)";
    if (key=="dragonballzsupergokuden2" ||
        key=="dragonballzsupergokuuden2")
        return "Dragon Ball Z - Super Gokuu Den - Kakusei Hen (Japan)";
    if (key=="dragonballzsuperbutouden3" ||
        key=="dragonballzsuperbutoden3")
        return "Dragon Ball Z - Super Butouden 3 (Japan)";
    if (key=="dragonballzsuperbutouden" ||
        key=="dragonballzsuperbutoden")
        return "Dragon Ball Z - Super Butouden (Japan)";
    if (key=="dragonballzhyperdimension")
        return "Dragon Ball Z - Hyper Dimension (Japan)";
    if (key=="dragonquest1and2" || key=="dragonquestiandii" ||
        key=="dragonquest1ii")
        return "Dragon Quest I _ II (Japan)";
    if (key=="dai3jisuperrobottaisen" || key=="dai3jisuperrobotwars")
        return "Dai-3-ji Super Robot Taisen (Japan)";
    if (key=="bahamutlagoon")
        return "Bahamut Lagoon (Japan)";
    // No Libretro SNES art for Pokemon Gold & Silver (unlicensed platformer)
    // or Aladdin 2000 (unlicensed Genesis-to-SNES port). Do not fabricate a match.
    return "";
}
}} // namespace fe::artaliases

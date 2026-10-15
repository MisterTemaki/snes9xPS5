// Safe lookup of user-supplied Snes9x / Libretro .cht files by ROM title.
// SPDX-License-Identifier: MIT
#pragma once
#include "fe_titlematch.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <string>
#include <vector>

namespace fe { namespace cheatlookup {
inline std::string Lower(std::string x)
{
    for (char& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
    return x;
}
inline bool EndsWithCht(const std::string& s)
{
    return s.size()>4 && Lower(s.substr(s.size()-4))==".cht";
}
inline std::string Stem(const std::string& s)
{
    const size_t slash=s.find_last_of("/\\");
    const std::string base=slash==std::string::npos ? s : s.substr(slash+1);
    return EndsWithCht(base) ? base.substr(0,base.size()-4) : base;
}
// Prefer exact/official filename, prefer a complete cheat collection over
// limited "Game Genie" or "Action Replay" subsets, and only accept a small,
// unambiguous Levenshtein distance when the title differs.
inline std::string Best(const std::vector<std::string>& filenames, const std::string& rom,
                        const std::string& official)
{
    const std::string expected=titles::Key(!official.empty()?official:rom);
    const std::string alt=titles::Key(rom);
    if (expected.size()<4 && alt.size()<4) return "";
    const std::string wanted_region=titles::Region(official.empty()?rom:official);
    std::string best, best_key;
    int best_score=INT_MIN;
    bool ambiguous=false;
    for (const std::string& file:filenames)
    {
        if (!EndsWithCht(file)) continue;
        const std::string candidate=Stem(file);
        const std::string key=titles::Key(candidate);
        if (key.size()<4) continue;
        const std::string& compare=expected.empty()?alt:expected;
        const int limit=compare.size()<8?0:compare.size()<15?1:compare.size()<32?2:3;
        int distance=titles::Distance(compare,key,limit);
        if (alt!=compare && !alt.empty())
            distance=std::min(distance,titles::Distance(alt,key,limit));
        if (distance>limit) continue;
        const std::string lower=Lower(candidate);
        const std::string region=titles::Region(candidate);
        int score=1000-distance*100;
        if (Lower(candidate)==Lower(rom)) score+=500;
        if (!official.empty() && Lower(candidate)==Lower(official)) score+=450;
        if (!wanted_region.empty() && region==wanted_region) score+=60;
        else if (!wanted_region.empty() && !region.empty()) score-=60;
        else if (wanted_region.empty() && region=="USA") score+=15;
        if (lower.find("(game genie)")!=std::string::npos) score-=10;
        if (lower.find("(action replay)")!=std::string::npos) score-=10;
        if (lower.find("(rev ")!=std::string::npos) score-=2;
        if (score>best_score)
        {
            best_score=score; best=file; best_key=key; ambiguous=false;
        }
        else if (score==best_score && key!=best_key)
            ambiguous=true;
    }
    return ambiguous ? "" : best;
}
}} // namespace fe::cheatlookup

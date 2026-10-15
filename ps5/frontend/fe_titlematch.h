// Conservative No-Intro/Libretro filename matching; never alter ROM bytes or extensions.
// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cctype>
#include <climits>
#include <string>
#include <vector>

namespace fe { namespace titles {
// Matching regex semantics: strip optional (...) and [...] tags; ignore spaces,
// hyphens, punctuation, and case. Allow only a handful of spelling edits.
// E.g. "Kidou Butoden G-Gundam" ~ "Kidou Butouden G Gundam (Japan)".
inline std::string Key(const std::string& name)
{
    std::string out;
    int depth = 0;
    std::string stem = name;
    for (const char* ext : {".cht", ".png", ".sfc", ".smc", ".zip"})
    {
        const size_t n = std::char_traits<char>::length(ext);
        if (stem.size()>n && stem.compare(stem.size()-n,n,ext)==0)
        { stem.resize(stem.size()-n); break; }
    }
    for (unsigned char c : stem)
    {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (depth == 0 && std::isalnum(c)) out += char(std::tolower(c));
    }
    return out;
}
inline std::string Region(const std::string& name)
{
    const char* tags[] = {"USA", "Europe", "Japan", "World"};
    for (const char* tag : tags)
        if (name.find(std::string("(") + tag) != std::string::npos ||
            name.find(std::string(", ") + tag) != std::string::npos) return tag;
    return "";
}
// Bounded Levenshtein distance with early exit. O(k*length), k<=3.
inline int Distance(const std::string& a, const std::string& b, int limit)
{
    if (a == b) return 0;
    if (a.empty() || b.empty() || a.size() > b.size() + size_t(limit) ||
        b.size() > a.size() + size_t(limit)) return limit + 1;
    std::vector<int> prev(b.size()+1), curr(b.size()+1);
    for (size_t j=0;j<=b.size();++j) prev[j]=int(j);
    for (size_t i=1;i<=a.size();++i)
    {
        curr[0]=int(i);
        int min_row=curr[0];
        for (size_t j=1;j<=b.size();++j)
        {
            curr[j]=std::min({prev[j]+1, curr[j-1]+1,
                              prev[j-1] + (a[i-1]==b[j-1] ? 0 : 1)});
            min_row=std::min(min_row,curr[j]);
        }
        if (min_row>limit) return limit+1;
        prev.swap(curr);
    }
    return prev[b.size()];
}
// Explicit region preference; tag variants and revisions are otherwise allowed.
// If another distinct title is equally close, refuse the fuzzy match.
inline std::string Best(const std::vector<std::string>& names, const std::string& requested)
{
    const std::string query=Key(requested);
    if (query.size()<4) return "";
    const std::string wanted_region=Region(requested);
    const int limit=query.size()<8 ? 0 : query.size()<15 ? 1 : query.size()<32 ? 2 : 3;
    int best_distance=INT_MAX, best_region=INT_MAX;
    std::string best, best_key;
    bool ambiguous=false;
    for (const std::string& candidate:names)
    {
        const std::string key=Key(candidate);
        if (key.empty() || (key.size()>=4 && query.size()>=4 &&
            key.compare(0,4,query,0,4)!=0)) continue;
        const int d=Distance(query,key,limit);
        if (d>limit) continue;
        const std::string region=Region(candidate);
        const int region_penalty=(!wanted_region.empty() && region!=wanted_region) ? 1 : 0;
        if (d<best_distance || (d==best_distance && region_penalty<best_region))
        {
            best_distance=d; best_region=region_penalty;
            best=candidate; best_key=key; ambiguous=false;
        }
        else if (d==best_distance && region_penalty==best_region && key!=best_key)
            ambiguous=true;
    }
    return ambiguous ? "" : best;
}
}} // namespace fe::titles

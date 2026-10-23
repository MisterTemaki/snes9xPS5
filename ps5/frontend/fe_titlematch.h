// Conservative No-Intro/Libretro filename matching; never alter ROM bytes or extensions.
// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cctype>
#include <climits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
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
    for (const char* ext : {".cht", ".png", ".sfc", ".smc", ".zip", ".7z"})
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
// A list's keys worked out once, grouped by their first four characters: Best then only measures the
// candidates that can match (the same first four, or keys shorter than four), not every name of the list.
// The same answers as before, in the same order; a library of 1500 unknown ROMs went from seconds to nothing.
struct Index
{
    const std::vector<std::string>* names = nullptr;
    std::vector<std::string> keys;
    std::unordered_map<std::string, std::vector<int>> by_prefix; // first 4 characters -> positions, in order
    std::vector<int> short_keys;                                  // keys of 1 to 3 characters
};
inline void Build(Index& ix, const std::vector<std::string>& names)
{
    ix.names = &names;
    ix.keys.clear();
    ix.by_prefix.clear();
    ix.short_keys.clear();
    ix.keys.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i)
    {
        ix.keys.push_back(Key(names[i]));
        const std::string& k = ix.keys.back();
        if (k.size() >= 4) ix.by_prefix[k.substr(0, 4)].push_back(int(i));
        else if (!k.empty()) ix.short_keys.push_back(int(i));
    }
}
// The index of a list that lives as long as the program (the built-in tables), made on first use.
inline const Index& IndexOf(const std::vector<std::string>& names)
{
    static std::mutex lock;
    static std::map<const std::vector<std::string>*, std::unique_ptr<Index>> made;
    std::lock_guard<std::mutex> hold(lock);
    std::unique_ptr<Index>& ix = made[&names];
    if (!ix || ix->keys.size() != names.size())
    {
        ix.reset(new Index);
        Build(*ix, names);
    }
    return *ix;
}
// Explicit region preference; tag variants and revisions are otherwise allowed.
// If another distinct title is equally close, refuse the fuzzy match.
inline std::string Best(const Index& ix, const std::string& requested)
{
    const std::vector<std::string>& names = *ix.names;
    const std::string query=Key(requested);
    if (query.size()<4) return "";
    const std::string wanted_region=Region(requested);
    const int limit=query.size()<8 ? 0 : query.size()<15 ? 1 : query.size()<32 ? 2 : 3;
    int best_distance=INT_MAX, best_region=INT_MAX;
    std::string best, best_key;
    bool ambiguous=false;
    // the candidates in the list's order: this prefix's keys and the short keys, merged
    std::vector<int> order;
    auto it=ix.by_prefix.find(query.substr(0,4));
    if (it!=ix.by_prefix.end()) order=it->second;
    order.insert(order.end(), ix.short_keys.begin(), ix.short_keys.end());
    std::sort(order.begin(), order.end());
    for (int i:order)
    {
        const std::string& candidate=names[size_t(i)];
        const std::string& key=ix.keys[size_t(i)];
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
// The same, on a list of names as it is (the tests, small lists): the plain scan.
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

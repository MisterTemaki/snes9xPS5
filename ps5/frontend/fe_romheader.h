// The internal header of a SNES ROM image: its title, map mode and country.
//
// A fan translation or a hack keeps the cartridge's own header, so the title in it names the original game when the
// file name does not (fe_artmatch.h ResolveHeader). The header sits at $7FC0 (LoROM), $FFC0 (HiROM) or $40FFC0
// (ExHiROM), 512 bytes later behind a copier header. Nothing here trusts one place: each is scored by how header-like
// it looks, and the best one must look enough like a header. A bad checksum (translators often leave it) only costs
// points.
//
// SPDX-License-Identifier: MIT
#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace fe { namespace romheader {
struct Info
{
    std::string title;      // printable ASCII; half-width katakana turned into romaji
    uint8_t map = 0;        // map mode byte
    uint8_t country = 0xFF; // 0 is Japan
    bool japan() const { return country == 0; }
};
// Fills out with n bytes at offset; false if the ROM has fewer.
using Reader = std::function<bool(uint64_t offset, uint8_t* out, size_t n)>;

inline bool IsVowel(char c) { return c != 0 && std::strchr("aiueo", c) != nullptr; }
// Dakuten and handakuten (the two marks of half-width kana) turn the last syllable's consonant: ka -> ga, ha -> ba/pa.
inline void Voice(std::string& out, bool semi)
{
    if (out.empty()) return;
    size_t at = out.size() - 1;
    if (IsVowel(out[at]) && at > 0 && !IsVowel(out[at - 1]) && out[at - 1] != ' ') --at;
    const char was = out[at];
    switch (was)
    {
    case 'k': out[at] = 'g'; break;
    case 's': out[at] = 'z'; break;
    case 't': out[at] = 'd'; break;
    case 'h': out[at] = semi ? 'p' : 'b'; break;
    case 'u': if (at == out.size() - 1) out.insert(at, "v"); break;
    default: break;
    }
    if (at > 0 && out[at] != was && out[at - 1] == was) out[at - 1] = out[at]; // a doubled consonant (small tsu) too
}
// The 21 title bytes as text. ASCII stays; half-width katakana (JIS X 0201) becomes Nihon-shiki romaji (si, ti, tu,
// hu), the spelling fe_artmatch.h's Phon uses.
inline std::string Title(const uint8_t* t, size_t n)
{
    static const char* const kana[] = {"o", "a", "i", "u", "e", "o", "ya", "yu", "yo", "", "", "a", "i", "u", "e", "o", "ka",
        "ki", "ku", "ke", "ko", "sa", "si", "su", "se", "so", "ta", "ti", "tu", "te", "to", "na", "ni", "nu", "ne", "no",
        "ha", "hi", "hu", "he", "ho", "ma", "mi", "mu", "me", "mo", "ya", "yu", "yo", "ra", "ri", "ru", "re", "ro", "wa", "n"};
    static_assert(sizeof(kana) / sizeof(kana[0]) == 0xDD - 0xA6 + 1, "one entry per byte from $A6 to $DD");
    std::string out;
    bool small_tsu = false;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = t[i];
        if (c >= 0x20 && c <= 0x7E) { out += char(c); continue; }
        if (c == 0xDE || c == 0xDF) { Voice(out, c == 0xDF); continue; }
        if (c == 0xAF) { small_tsu = true; continue; }
        if (c == 0xB0) { if (!out.empty() && IsVowel(out.back())) out += out.back(); continue; } // long vowel mark
        if (c >= 0xA6 && c <= 0xDD)
        {
            const std::string syl = kana[c - 0xA6];
            const bool small_ya = c >= 0xAC && c <= 0xAE;
            if (small_ya && out.size() >= 2 && out.back() == 'i' && !IsVowel(out[out.size() - 2]))
                out.pop_back(); // ki + small ya -> kya
            else if (small_tsu && !syl.empty() && !IsVowel(syl[0]))
                out += syl[0]; // small tsu doubles the next consonant
            small_tsu = false;
            out += syl;
            continue;
        }
        if (!out.empty() && out.back() != ' ') out += ' '; // kana punctuation and anything else
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t b = 0;
    while (b < out.size() && out[b] == ' ') ++b;
    return out.substr(b);
}
// How much the 64 bytes at a candidate header look like a header; 0 when the title is not text. kind: 0 LoROM, 1 HiROM,
// 2 ExHiROM.
inline int Score(const uint8_t* h, int kind)
{
    int text = 0, letters = 0;
    for (int i = 0; i < 21; ++i)
    {
        const unsigned char c = h[i];
        if ((c >= 0x20 && c <= 0x7E) || (c >= 0xA1 && c <= 0xDF)) ++text;
        if (std::isalnum(c) || (c >= 0xA6 && c <= 0xDD)) ++letters;
    }
    if (text < 19 || letters < 3) return 0;
    int s = 2;
    if (uint16_t(h[0x1C] | h[0x1D] << 8) == uint16_t(~(h[0x1E] | h[0x1F] << 8))) s += 4; // complement and checksum
    const uint8_t map = h[0x15];
    if ((map & 0xE0) == 0x20) s += 2;
    const int low = map & 0x0F;
    if (kind == 0 ? (low == 0 || low == 2 || low == 3) : kind == 1 ? (low == 1 || low == 10) : low == 5) ++s;
    if (h[0x17] >= 0x07 && h[0x17] <= 0x0D) ++s; // ROM size 128 KiB to 8 MiB
    if (h[0x19] <= 0x14) ++s;                    // country
    return s;
}
// total: the image's size in bytes, or 0 when not known.
inline bool Read(const Reader& rd, uint64_t total, Info* out)
{
    static const uint64_t offsets[] = {0x7FC0, 0xFFC0, 0x40FFC0};
    int best = 0;
    uint8_t found[64] = {};
    for (int copier = 0; copier < 2; ++copier)
        for (int kind = 0; kind < 3; ++kind)
        {
            const uint64_t at = offsets[kind] + (copier ? 512 : 0);
            uint8_t h[64];
            if ((total && at + 64 > total) || !rd(at, h, sizeof(h))) continue;
            int s = Score(h, kind);
            if (s > 0 && total && ((total & 0x3ff) == 0x200) == (copier == 1)) ++s; // the size agrees with the copier header
            if (s > best) { best = s; std::memcpy(found, h, sizeof(found)); }
        }
    if (best < 6) return false;
    out->title = Title(found, 21);
    out->map = found[0x15];
    out->country = found[0x19];
    return !out->title.empty();
}
}} // namespace fe::romheader

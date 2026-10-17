// Snes9x PS5 frontend: the interface's languages.
//
// English is the default and the source: every text on the screen is written in English in the code and passed
// through T(), which returns it in the language picked in Settings (SYSTEM, Language), or as it is when there is
// no translation. A format string keeps its % specifiers in every language (tests/check_i18n.py checks them).
//
// SPDX-License-Identifier: MIT
#pragma once

namespace fe
{
enum class Language : int
{
	English = 0,
	Spanish,
	PortugueseBrazil,
	French,
	German,
	Count
};

// The language's own name ("Español"...), for the Settings row.
const char* LanguageName(int lang);
// The text in the current language (Config().language).
const char* T(const char* english);
} // namespace fe

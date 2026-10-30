// Snes9x PS5 frontend: an on-screen keyboard driven by the pad. The app has no other text input: the system's own
// keyboard dialog (libSceImeDialog) is not among the imports a native title links (ps5/proto/native/tooling/native/
// runtime/imports.txt) and cannot be tried without a console, so the keyboard is drawn here like the menus.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace fe
{
// The player edits `text` (shown under `title`). Returns true on Done (OPTIONS, or the Done key) with the new
// text in `text`; false on Cancel (Circle) with `text` as it was. `secret` draws every character as * (and the
// text never reaches a log). At most `max_len` bytes. `note` (2.10, the shelf's search): a line shown under the field,
// asked again each time the text changes ("12 games match").
bool EditText(const std::string& title, std::string& text, bool secret = false, size_t max_len = 128,
	const std::function<std::string(const std::string&)>& note = nullptr);
} // namespace fe

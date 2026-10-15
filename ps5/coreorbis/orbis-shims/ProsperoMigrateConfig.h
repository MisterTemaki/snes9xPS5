// Snes9x PS5's title IDs (ProsperoMigrate.h): the current one and the ones it used before.
//
// SPDX-License-Identifier: MIT
#pragma once

#ifndef SNES9X_TITLE_ID
#define SNES9X_TITLE_ID "PPSA99922"
#endif

#define MIG_APP_NAME "Snes9x PS5"
#define MIG_NEW_TITLE SNES9X_TITLE_ID
// up to 2.5 it was PPSA99009, which the homebrew catalog (homebrew.page) lists for another app
#define MIG_OLD_TITLES "PPSA99009"
// the label of our content ID (UP9000-<title>_00-<label>), the same under every title ID
#define MIG_CONTENT_LABEL "SNES9XPS50000001"
// the host tests' override: <prefix>PS5_HOMEBREW
#define MIG_ENV_PREFIX "SNES9X_"

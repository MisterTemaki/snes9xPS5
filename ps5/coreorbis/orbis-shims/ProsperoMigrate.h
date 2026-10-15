// The app's earlier title IDs (ProsperoMigrate.cpp).
//
// A title ID is the app's identity on the console: /data/homebrew/<ID>, /user/appmeta/<ID> and the home-screen
// registration. Our first IDs (PPSA99009..PPSA99012) turned out to be taken in the homebrew catalog (homebrew.page)
// by other apps, so the app moved to its own ID (MIG_NEW_TITLE, ProsperoMigrateConfig.h) and installs there.
//
// Nothing under an old ID is renamed, removed or unregistered: /data/homebrew/<old ID> may just as well be another
// app's (the one the catalog lists under that ID), and its icon belongs to whoever installed it. The installer only
// looks: when the folder there is our own earlier version (its param.json carries our content ID), the notification
// says so, and how to delete that icon by hand. The data folder (/data/<app>/: games, saves, states, covers,
// settings) doesn't depend on the title ID, so the new version finds everything where it was.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace migrate
{
// Our own earlier installs under the old IDs ("PPSA99011"; "" when there is none). Only reads.
std::string FindOldInstalls();

// UP9000-<title>_00-<our label>: our content ID under that title ID.
std::string ContentIdFor(const char* title_id);
} // namespace migrate

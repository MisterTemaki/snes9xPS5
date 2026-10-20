// The app's earlier title IDs (ProsperoMigrate.cpp).
//
// A title ID is the app's identity on the console: /data/homebrew/<ID>, /user/appmeta/<ID> and the home-screen
// registration. Our first IDs (PPSA99009..PPSA99012) turned out to be taken in the homebrew catalog (homebrew.page)
// by other apps, so the app moved to its own ID (MIG_NEW_TITLE, ProsperoMigrateConfig.h) and installs there.
//
// Nothing under an old ID is renamed, removed or unregistered: /data/homebrew/<old ID> may just as well be another
// app's (the one the catalog lists under that ID), and its icon belongs to whoever installed it. The installer only
// looks: when a folder there is our own other copy (its param.json carries our content ID, under any folder name),
// the notification says so, and how to delete that icon by hand. The data folder (/data/<app>/: games, saves, states,
// covers, settings) doesn't depend on the title ID, so the new version finds everything where it was.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace migrate
{
struct OtherInstall
{
	std::string folder;   // its name under /data/homebrew
	std::string title_id; // the titleId its param.json carries
};
// Our own copies in /data/homebrew besides the current one (PPSA99922): the folders, whatever their names, whose
// param.json carries our content ID. Each is a second source for the home screen, so a second icon. Only reads.
std::vector<OtherInstall> FindOtherInstalls();
} // namespace migrate

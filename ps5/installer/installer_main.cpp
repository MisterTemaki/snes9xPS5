// Snes9x PS5: the payloads (Snes9xPS5.elf and Snes9xPS5-helper.elf).
//
// Snes9xPS5.elf, sent with the PS5 Payload Manager / the ELF loader (port 9021), is what PS5SX2's installer and
// helper are together:
//   1. it installs or updates the dashboard app in /data/homebrew/<TITLE_ID> (eboot.bin = Snes9x itself,
//      sce_module/libc.prx, sce_sys/param.json, icon0.png, pic0.dds, pic1.dds), built into it (install_data.cpp);
//   2. then it stays running as the helper that lets the app out of its sandbox (ProsperoJailbreak.h), until
//      the console is turned off. A second copy sent while one runs only installs.
// Snes9xPS5-helper.elf (SNES9X_HELPER_ONLY) is step 2 alone: the app carries it and sends it to the ELF
// loader itself when no helper answers (after a reboot, for instance).
//
// Earlier versions used the title ID PPSA99009, which another app uses: the app is PPSA99922 now. Nothing under the old
// ID is touched (it may be another app's); when our earlier version is still there, the notification says so
// (ProsperoMigrate.h).
//
// Nothing here writes outside /data/snes9x, the app's folder and /user/appmeta/<TITLE_ID>. 2.10: the app's folder is the
// one the console runs it from (ShadowMount+ may mount it from a USB drive, an extended drive or another folder:
// ProsperoAppPlace.h), written through its mount /system_ex/app/<TITLE_ID>; /data/homebrew/<TITLE_ID> when nothing
// runs it yet.
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"
#include "ProsperoJailbreak.h"
#include "ProsperoMigrate.h"
#include "ProsperoNotify.h"
#ifndef SNES9X_HELPER_ONLY
#include "ProsperoAppPlace.h"
#include "ProsperoInstall.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#ifndef SNES9X_PS5_VERSION
#define SNES9X_PS5_VERSION "dev"
#endif

namespace
{
std::string g_ready_message;

#ifndef SNES9X_HELPER_ONLY
// where the payload installs when nothing else runs the app
std::string DefaultAppDir()
{
	const char* env = getenv("SNES9X_PS5_HOMEBREW");
	return std::string(env && *env ? env : "/data/homebrew") + "/" SNES9X_TITLE_ID;
}
#endif

void OnReady()
{
	if (!g_ready_message.empty())
		ProsperoNotify("%s", g_ready_message.c_str());
	ProsperoNotifyFlush();
}
} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const bool have_data = OrbisPathsInit();
#ifdef SNES9X_HELPER_ONLY
	OrbisLogOpen("helper");
	OrbisLog("[helper] Snes9x PS5 helper %s (built %s %s), pid %d%s", SNES9X_PS5_VERSION, __DATE__, __TIME__,
		int(getpid()), have_data ? "" : " (no /data: no log file)");
#else
	OrbisLogOpen("installer");
	OrbisLog("[installer] Snes9x PS5 %s (built %s %s), pid %d", SNES9X_PS5_VERSION, __DATE__, __TIME__, int(getpid()));
	if (!have_data)
		OrbisLog("[installer] can't create %s", OrbisRoot().c_str());

	const char* open_hint = "Open it from the Snes9x PS5 icon on the home screen.";
	char msg[1024];
	const InstallResult installed = InstallApp();
	switch (installed)
	{
		case InstallResult::Installed:
			snprintf(msg, sizeof(msg), "Snes9x PS5 %s installed. %s", SNES9X_PS5_VERSION, open_hint);
			break;
		case InstallResult::Updated:
			if ((!AppInstallPlace().mount.empty() || AppInstallPlace().via_link) &&
				AppInstallPlace().source != DefaultAppDir())
				snprintf(msg, sizeof(msg), "Snes9x PS5 updated to %s (in %s, the copy ShadowMount+ runs). %s",
					SNES9X_PS5_VERSION, AppInstallPlace().source.c_str(), open_hint);
			else
				snprintf(msg, sizeof(msg), "Snes9x PS5 updated to %s. %s", SNES9X_PS5_VERSION, open_hint);
			break;
		case InstallResult::UpToDate:
			snprintf(msg, sizeof(msg), "Snes9x PS5 %s is ready. %s", SNES9X_PS5_VERSION, open_hint);
			break;
		case InstallResult::NewerInstalled:
			snprintf(msg, sizeof(msg),
				"Snes9x PS5: a newer version is installed (updated from the app); this payload (%s) left it as it is. "
				"Put the newer payload in your autoload. %s",
				SNES9X_PS5_VERSION, open_hint);
			break;
		case InstallResult::CantWrite:
			// 2.10: ShadowMount+ runs the app from an image or a read-only folder: not written, and no second copy made
			snprintf(msg, sizeof(msg), "%s (This payload is %s; the helper runs.)", AppInstallPlace().reason.c_str(),
				SNES9X_PS5_VERSION);
			break;
		case InstallResult::Failed:
			snprintf(msg, sizeof(msg), "Snes9x PS5: could not install the app in %s (see %s/logs/installer.log)",
				AppInstallDir().c_str(), OrbisRoot().c_str());
			break;
		default:
			snprintf(msg, sizeof(msg), "Snes9x PS5: nothing to install");
			break;
	}
	if (installed != InstallResult::NewerInstalled && installed != InstallResult::CantWrite &&
		!AppInstallPlace().image_pending && SyncAppMeta() > 0)
	{
		const size_t len = strlen(msg);
		snprintf(msg + len, sizeof(msg) - len, " Home screen art updated (restart the PS5 if it doesn't show).");
	}
	std::string old_ids, old_folders, same_title_folders;
	size_t old_count = 0;
	for (const migrate::OtherInstall& o : migrate::FindOtherInstalls())
	{
		if (o.title_id == SNES9X_TITLE_ID)
		{
			same_title_folders += (same_title_folders.empty() ? "" : ", ") + std::string("/data/homebrew/") + o.folder;
			continue;
		}
		old_ids += (old_ids.empty() ? "" : ", ") + o.title_id;
		old_folders += (old_folders.empty() ? "" : ", ") + std::string("/data/homebrew/") + o.folder;
		old_count++;
	}
	if (old_count > 0)
	{
		const size_t len = strlen(msg);
		snprintf(msg + len, sizeof(msg) - len,
			" Its title ID is now %s. The old %s (%s) %s left alone: to remove %s, delete %s %s (FTP), then %s "
			"(OPTIONS, Delete). Games, saves and settings are shared.",
			SNES9X_TITLE_ID, old_count == 1 ? "version" : "versions", old_ids.c_str(), old_count == 1 ? "is" : "are",
			old_count == 1 ? "it" : "them", old_count == 1 ? "its folder" : "their folders", old_folders.c_str(),
			old_count == 1 ? "its icon" : "their icons");
	}
	if (!same_title_folders.empty())
	{
		const size_t len = strlen(msg);
		snprintf(msg + len, sizeof(msg) - len,
			" Another folder holds this app too (%s): two folders for %s give a second icon. Delete it (FTP), then "
			"the extra icon (OPTIONS, Delete).",
			same_title_folders.c_str(), SNES9X_TITLE_ID);
	}
	OrbisLog("[installer] %s", msg);
	g_ready_message = msg;
#endif

	// the helper: runs until the console is turned off, or returns at once when one already runs
	if (!jailbreak::ServeHelper(OnReady))
	{
#ifndef SNES9X_HELPER_ONLY
		ProsperoNotify("%s", g_ready_message.c_str());
#endif
		OrbisLog("[helper] another helper is running: leaving");
	}
	ProsperoNotifyFlush();
	OrbisLogClose();
	return 0;
}

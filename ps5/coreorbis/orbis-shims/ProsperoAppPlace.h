// Snes9x PS5 2.10 (from Snes9x PS5 1.0 and Media Player Classic PS5 1.5, its ShadowMount+ fix): where the app the
// console runs really is, for the in-app update (done by the helper) and the installer payload (ProsperoAppPlace.cpp).
//
// ShadowMount+ (github.com/drakmor/ShadowMountPlus) registers a homebrew title by mounting its folder -- found in
// /data/homebrew, /data/etaHEN/games, /mnt/usbN/homebrew, /mnt/extN/homebrew, its custom scan paths, nested
// PS5/<any>/<TITLE_ID> folders, manual.lst, or inside an image (.ffpkg, .exfat, .ffpfs) it mounted under
// /mnt/shadowmnt -- on /system_ex/app/<TITLE_ID> with nullfs, and the console runs the app from there. So the
// folder to update is that mount's source, not always /data/homebrew/<TITLE_ID>; and the files are written through
// the mount (/system_ex/app/<TITLE_ID>/...) when it can be written, so that the mounted view sees them at once.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

struct AppPlace
{
	std::string mount;  // "/system_ex/app/<TITLE_ID>" when the console runs the app from a mount of another folder
	std::string source; // the folder the console runs (the mount's source); default_dir when there is no mount
	std::string dir;    // where the files are written: the mount, else the source (whichever accepts a file)
	std::string fs;     // the source's file system ("ufs", "exfatfs", "nullfs"...), "" when unknown
	bool writable = true;
	bool image = false;  // the source is inside an image mounted by ShadowMount+ (under /mnt/shadowmnt)
	bool unsure = false; // ShadowMount+ names a folder that isn't there now: nothing is written anywhere
	bool fallback = false; // 2.10 review: a mount that can't be read as ShadowMount+'s: default_dir, as 2.9 wrote
	bool via_link = false; // no mount yet: the folder from ShadowMount+'s link (/user/app/<TITLE_ID>/mount.lnk)
	bool image_pending = false; // that folder is in an image ShadowMount+ hasn't mounted yet (not written)
	std::string reason; // !writable: why, a sentence for the user (English; ProsperoPlaceText.h, translated by the app)
	std::string log;    // what was found, for boot.log
};

// The rules (never worse than 2.9, which always wrote default_dir; an image is never written):
// - the mount's top layer must be nullfs (through it the files are written); under another layer (a backport's
//   unionfs) the nullfs beneath gives the folder, written directly, never through the layer on top;
// - the source named by the mount (f_mntfromname, cut at 87 bytes by the system) is used only when it is whole,
//   its sce_sys/param.json names the app and its eboot.bin is the one the mount shows; otherwise only the mount;
// - every folder written has a sce_sys/param.json naming the app;
// - a read-only mount of a folder whose own file system takes files: the folder itself is written (2.10 review);
// - statfs(2) failing: getfsstat(2); neither answering, a mount that isn't nullfs and has no nullfs layer, or a source
//   that can't be identified: default_dir, as 2.9 (2.10 review: it wrote nothing before);
// - no mount (a payload autoloaded before ShadowMount+ has mounted the app): ShadowMount+'s link
//   /user/app/<TITLE_ID>/mount.lnk names the folder (not there: nothing written, no second copy; in an image -- under
//   /mnt/shadowmnt, or mount_img.lnk beside it -- never written; a link that isn't an absolute path is ignored).
// default_dir: /data/homebrew/<TITLE_ID> (where the payload installs when nothing else runs the app).
// Host tests: SNES9X_SYSTEM_EX stands for /system_ex, SNES9X_USER_APP for /user/app, SNES9X_SHADOWMNT
// for /mnt/shadowmnt, SNES9X_HOST_MOUNTS for
// the mount table ("<mount point>|<fs type>|<from>|ro or rw;..." in mount order; a path takes the longest mount point
// it is in, the last of a point's mounts), SNES9X_HOST_NO_STATFS / SNES9X_HOST_NO_GETFSSTAT make those fail.
AppPlace FindAppPlace(const std::string& title_id, const std::string& default_dir);

// Snes9x PS5: the /data/snes9x folder layout and the boot log (orbis-shims/orbis_paths.cpp).
//
//   /data/snes9x/roms         ROMs (.sfc .smc .swc .fig .bs .st .zip); sub-folders are browsable
//   /data/snes9x/saves        battery saves (.srm), one per game
//   /data/snes9x/states       save states (.000 .. .009)
//   /data/snes9x/screenshots  screenshots (.png when built with libpng, else nothing)
//   /data/snes9x/cheats       cheat files (.cht), loaded with the game
//   /data/snes9x/patches      IPS/UPS/BPS patches named after the ROM
//   /data/snes9x/bios         BS-X.bin, STBIOS.bin (Satellaview, Sufami Turbo)
//   /data/snes9x/logs         boot.log (this run) and boot.prev.log (the run before); installer.log
//   /data/snes9x/snes9x-ps5.ini  the frontend's settings
//
// USB drives are searched too: /mnt/usbN/snes9x/roms (N = 0..7) and /mnt/extN/snes9x/roms.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#ifndef ORBIS_ROOT_DEFAULT
#define ORBIS_ROOT_DEFAULT "/data/snes9x"
#endif

// Root folder (ORBIS_ROOT_DEFAULT; the host tests point it elsewhere with SNES9X_PS5_ROOT).
const std::string& OrbisRoot();
// <root>/<sub>, created at boot by OrbisPathsInit.
std::string OrbisDir(const char* sub);
// Creates the folder tree. Returns false when the root itself can't be created (no /data access).
bool OrbisPathsInit();
// Folders that hold ROMs and exist right now (internal first, then USB drives).
std::vector<std::string> OrbisRomRoots();

bool OrbisIsDir(const std::string& path);
bool OrbisIsFile(const std::string& path);
bool OrbisMkdirs(const std::string& path);

// <root>/logs/<name>.log (the run before kept as <name>.prev.log): boot.log for the emulator, installer.log
// for the installer/helper payload. Every line also goes to stdout. Lines logged before the file is open
// (before the jailbreak shows /data to the app) are kept and written first.
void OrbisLogOpen(const char* name = "boot");
void OrbisLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void OrbisLogClose();
// Debug logs on or off: the "Debug logs" setting (debug_logs= in snes9x-ps5.ini, off unless it says 1).
// OrbisLogOpen reads it, so the app, the installer and the helper all follow it; while off, OrbisLog writes
// nothing (no file, no stdout) and the log files of earlier runs are left as they are.
void OrbisLogSetEnabled(bool on);
// Reads the setting again and applies it (the helper, which keeps running, calls this for each request).
void OrbisLogRefresh();
bool OrbisLogEnabled();
// The open log's file descriptor, for the crash handler's signal-safe write(); -1 before it is open.
int OrbisLogFd();

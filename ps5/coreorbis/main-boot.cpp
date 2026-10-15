// Snes9x PS5: the app's entry point (eboot.bin).
//
// 1.6: Snes9x runs as the dashboard app itself, a native PS5 title built the way PS5SX2 builds its eboot
// (ps5/coreorbis/link-vk.sh): the console gives the controller only to the title in front, and 1.0-1.5,
// a payload started by the ELF loader next to the system UI, never got it. Snes9xPS5.elf is now the
// installer and the helper (installer/installer_main.cpp), like PS5SX2's installer and helper payloads.
//
// Like PS5SX2's main-boot.cpp it first asks to leave the sandbox (ProsperoJailbreak.h: /data and USB drives),
// then brings the PS5 layer up in order -- log, folders, video, audio, pads -- and hands over to the
// frontend: the shelf, the game, the pause menu, and back. It ends through the system
// (sceSystemServiceLoadExec("exit")), as PS5SX2 learned it must.
//
//   eboot.bin [rom]     with a ROM path, the game starts at once (no shelf; the host build's tests use it)
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoInput.h"
#include "ProsperoJailbreak.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "fe_emu.h"
#include "fe_covers.h"
#include "fe_menu.h"
#include "fe_prefetch.h"
#include "fe_shelf.h"
#include "fe_settings.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>

#ifndef SNES9X_PS5_VERSION
#define SNES9X_PS5_VERSION "dev"
#endif

namespace
{
void WritePid()
{
	const std::string path = OrbisRoot() + "/pid.txt";
	if (FILE* f = fopen(path.c_str(), "w"))
	{
		fprintf(f, "%d\n", int(getpid()));
		fclose(f);
	}
}

// One game, until the player leaves it. Returns false when the player chose "quit Snes9x".
bool PlayGame(const std::string& rom)
{
	if (!emu::LoadGame(rom))
	{
		ProsperoNotify("Snes9x: could not open %s", rom.c_str());
		fe::MessageBox("Could not open the game", rom);
		return true;
	}
	emu::Osd("%s", emu::GameName().c_str());
	for (;;)
	{
		switch (emu::RunFrame())
		{
			case emu::FrameResult::Continue:
				break;
			case emu::FrameResult::Quit:
				emu::CloseGame();
				return false;
			case emu::FrameResult::OpenMenu:
				switch (fe::PauseMenu())
				{
					case fe::PauseAction::Resume:
						emu::AfterMenu();
						ps5video::FillRect(0, 0, ps5video::kWidth, ps5video::kHeight, ps5video::Rgb(0, 0, 0));
						ps5video::InvalidateSnes();
						break;
					case fe::PauseAction::BackToList:
						emu::CloseGame();
						return true;
					case fe::PauseAction::Quit:
						emu::CloseGame();
						return false;
				}
				break;
		}
	}
}

// The end of the app: through the system, then _exit() if it never comes (PS5SX2's OrbisExitApp: on the
// console a title's exit() or return from main ends in SIGSYS).
[[noreturn]] void ExitApp(int status)
{
	OrbisLog("[boot] exit %d", status);
	ProsperoNotifyFlush();
	OrbisLogClose();
	fflush(stdout);
	fflush(stderr);
	const int rc = sceSystemServiceLoadExec("exit", nullptr);
	if (rc == 0)
		for (int i = 0; i < 100; i++)
			usleep(100 * 1000);
	_exit(status);
}

struct Args
{
	int argc;
	char** argv;
	int status;
};

int Run(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	crashlog::Install();
	S9X_STAGE(Boot, "start");
	OrbisLog("[boot] Snes9x PS5 %s (built %s %s), pid %d", SNES9X_PS5_VERSION, __DATE__, __TIME__, int(getpid()));

	// Covers first, as PS5SX2 (orbis_frontend_prefetch_covers, 30 s, before its jailbreak): HTTPS works here,
	// and on the 1.6.1 console it failed on the shelf, after the request below (fe_prefetch.h).
	const fe::PrefetchResult prefetch = fe::PrefetchCovers(30.0);

	// out of the sandbox, like PS5SX2: before it the app sees no /data and no USB drives
	S9X_STAGE(Boot, "jailbreak");
	std::string how;
	const bool freed = jailbreak::RequestForSelf(how);
	OrbisLog("[boot] jailbreak: %s", freed ? how.c_str() : "nobody answered");
	const bool have_data = OrbisPathsInit();
	OrbisLogOpen(); // also writes the lines above
	fe::SavePrefetched(prefetch);
	fe::RememberPrefetch(prefetch);
	// the shelf downloads nothing after the jailbreak (PS5SX2's download_usb_only); without one (the host
	// tests, no helper) it still does
	fe::SetShelfDownloads(!freed);
	if (!have_data)
		OrbisLog("[boot] can't create %s: no access to /data", OrbisRoot().c_str());
	else
		WritePid();
	fe::Config().Load();

	S9X_STAGE(Boot, "video");
	if (!ps5video::Init())
	{
		OrbisLog("[boot] video init failed");
		ProsperoNotify("Snes9x PS5: video failed to start (see %s/logs/boot.log)", OrbisRoot().c_str());
		return 1;
	}
	OrbisLog("[boot] splash screen hidden: %x", unsigned(sceSystemServiceHideSplashScreen()));
	S9X_STAGE(Boot, "audio");
	if (!ps5audio::Init())
		OrbisLog("[boot] audio init failed: running without sound");
	S9X_STAGE(Boot, "pad");
	if (!ps5input::Init())
		OrbisLog("[boot] pad init failed");

	if (!have_data)
	{
		// the jailbreak didn't happen: say so on the screen (the pad works, this is the title in front)
		fe::MessageBox("Snes9x PS5 has no access to /data",
			"Send Snes9xPS5-v" SNES9X_PS5_VERSION ".elf with the Payload Manager (it unlocks the app), then open Snes9x PS5 again.");
		ps5input::Shutdown();
		ps5audio::Shutdown();
		ps5video::Shutdown();
		return 2;
	}

	S9X_STAGE(Boot, "emu init");
	if (!emu::InitCore(1, argv))
	{
		ProsperoNotify("Snes9x PS5: the emulator failed to start");
		ps5audio::Shutdown();
		ps5video::Shutdown();
		return 1;
	}

	std::string rom = (argc > 1 && argv[1] && OrbisIsFile(argv[1])) ? argv[1] : "";
	for (;;)
	{
		if (rom.empty())
			rom = fe::Shelf();
		if (rom.empty())
			break; // OPTIONS on the shelf
		const bool again = PlayGame(rom);
		rom.clear();
		if (!again)
			break;
	}

	OrbisLog("[boot] leaving");
	fe::ShelfShutdown();
	fe::Config().Save();
	emu::DeinitCore();
	ps5input::Shutdown();
	ps5audio::Shutdown();
	ps5video::Shutdown();
	return 0;
}

void* RunThread(void* p)
{
	Args* a = static_cast<Args*>(p);
	a->status = Run(a->argc, a->argv);
	return nullptr;
}
} // namespace

int main(int argc, char** argv)
{
	// The emulator and the shelf run on a thread with a stack of their own (8 MiB): a title's main thread
	// gets a small one (PS5SX2 runs PCSX2 on a 16 MiB worker for the same reason).
	Args args = {argc, argv, 1};
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
	pthread_t t;
	if (pthread_create(&t, &attr, RunThread, &args) == 0)
		pthread_join(t, nullptr);
	else
		args.status = Run(argc, argv);
	pthread_attr_destroy(&attr);
	ExitApp(args.status);
}

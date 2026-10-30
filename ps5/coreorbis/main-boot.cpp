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
#include "ProsperoUpdate.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "fe_emu.h"
#include "fe_i18n.h"
#include "fe_covers.h"
#include "fe_7z.h"
#include "fe_menu.h"
#include "fe_prefetch.h"
#include "fe_ra.h"
#include "fe_shelf.h"
#include "fe_settings.h"
#include "netfs.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <stdexcept>
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
		// a ROM on a share says why (no answer, wrong password...); netfs's sentences carry no password
		std::string why = netfs_is_network_path(rom.c_str()) ? netfs_last_error() : std::string();
		// 2.10: the shelf opens from the saved list before it has looked at the folders: a game deleted (or a drive
		// unplugged) since is still listed for a moment. Say so plainly; the shelf then checks the folders.
		if (why.empty() && !netfs_is_network_path(rom.c_str()) && !OrbisIsFile(rom))
			why = fe::T("The file is no longer there.");
		// a .7z says what is wrong with it (encrypted, too big...)
		const fe::sevenzip::Error zerr = fe::sevenzip::LastLoadError();
		if (zerr != fe::sevenzip::Error::None)
			why = std::string(fe::T(fe::sevenzip::Message(zerr))) + (why.empty() ? "" : " " + why);
		ProsperoNotify(fe::T("Snes9x: could not open %s"), (rom + (why.empty() ? "" : " - " + why)).c_str());
		fe::MessageBox(fe::T("Could not open the game"), why.empty() ? rom : rom + "\n\n" + why);
		return true;
	}
	emu::Osd("%s", emu::GameName().c_str());
	unsigned frames = 0;
	for (;;)
	{
		switch (emu::RunFrame())
		{
			case emu::FrameResult::Continue:
				// a share that was lost mid-game: the notice comes once; the game goes on from what it has in memory
				if (++frames % 60 == 0)
					for (const std::string& ev : netfs_take_events())
					{
						OrbisLog("[net] %s", ev.c_str());
						ProsperoNotify("Snes9x: %s", ev.c_str());
					}
				break;
			case emu::FrameResult::Quit:
				emu::CloseGame();
				return false;
			case emu::FrameResult::BackToList:
				emu::CloseGame();
				return true;
			case emu::FrameResult::OpenCheats:
				// 2.11: RetroAchievements' hardcore mode: no cheats (and no pause through the cheat list)
				if (ra::HardcoreActive())
				{
					emu::Osd("%s", fe::T("Hardcore mode: cheats are off"));
					emu::AfterMenu();
					break;
				}
				fe::CheatMenu();
				emu::AfterMenu();
				ps5video::FillRect(0, 0, ps5video::kWidth, ps5video::kHeight, ps5video::Rgb(0, 0, 0));
				ps5video::InvalidateSnes();
				break;
			case emu::FrameResult::OpenMenu:
			{
				// 2.11: in hardcore, RetroAchievements limits how often the game may be paused (pausing over and over
				// would be a slow motion): rc_client_can_pause
				int wait = 0;
				if (!ra::CanPause(&wait))
				{
					emu::Osd(fe::T("Hardcore mode: you can pause again in %d s"), wait);
					emu::AfterMenu();
					break;
				}
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
}

// Once a day the helper looks for a newer release on GitHub (ProsperoUpdate.h); when there is one, it says so in a
// notification (Settings, Updates installs it). <root>/update-check.txt holds the time of the last look.
void AutoCheckUpdates()
{
	const std::string stamp = OrbisRoot() + "/update-check.txt";
	long long last = 0;
	if (FILE* f = fopen(stamp.c_str(), "r"))
	{
		if (fscanf(f, "%lld", &last) != 1)
			last = 0;
		fclose(f);
	}
	const long long now = (long long)time(nullptr);
	if (now - last < 24 * 3600 && now >= last)
		return;
	std::string reply;
	if (!jailbreak::UpdateRequest("autocheck", reply))
		return; // no helper of ours: nothing to ask
	OrbisLog("[update] daily check asked of the helper: %s", reply.c_str());
	if (FILE* f = fopen(stamp.c_str(), "w"))
	{
		fprintf(f, "%lld\n", now);
		fclose(f);
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

// 2.10: C++ exceptions checked at the start, with the debug logs on (as Stella PS5 1.0.1 / 1.0.2): one throw from our
// code, one from inside libc++ (std::stoi) and a failed allocation's std::bad_alloc, each caught by type. Up to 2.9 the
// eboot took the personality routine and the type_info vtables from the system's libSceLibcInternal next to its own
// static __cxa_throw and unwinder (Stella PS5 1.0 crashed at its first throw with that mix), and operator new stopped the
// app with SIGILL. boot.log says "exceptions: ok" when they work; a crash here names this stage.
__attribute__((noinline)) int ThrowForSelfTest(int v)
{
	if (v > 0)
		throw std::runtime_error("self-test");
	return v;
}

void ExceptionSelfTest()
{
	if (!OrbisLogEnabled())
		return;
	S9X_STAGE(Boot, "exceptions self-test");
	OrbisLog("[boot] exceptions: self-test (a throw and its catch)");
	OrbisLogSync();
	volatile int one = 1;
	bool ours = false, libcxx = false;
	try
	{
		ThrowForSelfTest(one);
	}
	catch (const std::runtime_error& e)
	{
		ours = strcmp(e.what(), "self-test") == 0;
	}
	try
	{
		volatile int n = std::stoi(std::string(one ? "not a number" : "1"));
		(void)n;
	}
	catch (const std::invalid_argument&)
	{
		libcxx = true;
	}
	// The console's build only: the host's operator new is the system's, and its sanitizer stops at a request this big.
	bool oom = true;
	const char* oom_how = "not tried on the host";
#ifdef __PROSPERO__
	oom = false;
	oom_how = "missed";
	try
	{
		// over PTRDIFF_MAX: the allocator refuses it itself, without asking the kernel for anything
		volatile size_t huge = SIZE_MAX / 2 + 1;
		void* p = ::operator new(huge);
		::operator delete(p);
	}
	catch (const std::bad_alloc&)
	{
		oom = true;
		oom_how = "caught";
	}
#endif
	OrbisLog("[boot] exceptions: %s (our throw %s, libc++'s std::invalid_argument %s, operator new's std::bad_alloc %s)",
		ours && libcxx && oom ? "ok" : "NOT OK", ours ? "caught" : "missed", libcxx ? "caught" : "missed", oom_how);
	OrbisLogSync();
}

#ifndef __PROSPERO__
// The host tests (SNES9X_HOST_CRASH_TEST=1): a fault with rbp holding no frame, as -O3 code leaves it, so the crash
// report is checked where a frame-pointer walk would fault (ProsperoCrash.cpp)
[[noreturn]] __attribute__((noinline)) void CrashForTest()
{
	S9X_STAGE(Boot, "crash test");
	__asm__ volatile("movabsq $0x00007f00deadbee8, %rbp\n\tmovq (%rbp), %rax");
	__builtin_unreachable();
}
#endif

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
#ifndef __PROSPERO__
	if (const char* e = getenv("SNES9X_HOST_CRASH_TEST"); e && *e == '1')
		CrashForTest();
#endif
	ExceptionSelfTest();
	fe::SavePrefetched(prefetch);
	fe::RememberPrefetch(prefetch);
	// the shelf downloads nothing after the jailbreak (PS5SX2's download_usb_only); without one (the host
	// tests, no helper) it still does
	fe::SetShelfDownloads(!freed);
	if (!have_data)
		OrbisLog("[boot] can't create %s: no access to /data", OrbisRoot().c_str());
	else
	{
		WritePid();
		AutoCheckUpdates();
	}
	fe::Config().Load();
	// 2.11: RetroAchievements: with a saved login, a quiet login in the background (nothing at all without one)
	if (have_data)
		ra::Init();

	if (have_data)
	{
		// network shares (shares.txt): connecting happens in the background, the shelf picks the games up when it is done
		netfs_set_log([](const std::string& l) { OrbisLog("[net] %s", l.c_str()); });
		const int shares = netfs_load(OrbisRoot() + "/shares.txt");
		OrbisLog("[net] %d share(s) in shares.txt", shares);
		int shown = 0;
		for (const std::string& n : netfs_notices())
		{
			OrbisLog("[net] shares.txt: %s", n.c_str());
			if (shown++ < 3)
				ProsperoNotify("Snes9x: shares.txt: %s", n.c_str());
		}
		netfs_start();
	}

	S9X_STAGE(Boot, "video");
	if (!ps5video::Init())
	{
		OrbisLog("[boot] video init failed");
		ProsperoNotify(fe::T("Snes9x PS5: video failed to start (see %s/logs/boot.log)"), OrbisRoot().c_str());
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
		char send[512];
		snprintf(send, sizeof(send), fe::T("Send Snes9xPS5-v%s.elf with the Payload Manager (it unlocks the app), then open Snes9x PS5 again."),
			SNES9X_PS5_VERSION);
		fe::MessageBox(fe::T("Snes9x PS5 has no access to /data"), send);
		ps5input::Shutdown();
		ps5audio::Shutdown();
		ps5video::Shutdown();
		return 2;
	}

	S9X_STAGE(Boot, "emu init");
	if (!emu::InitCore(1, argv))
	{
		ProsperoNotify("%s", fe::T("Snes9x PS5: the emulator failed to start"));
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
	ra::Shutdown();
	fe::ShelfShutdown();
	fe::Config().Save();
	emu::DeinitCore();
	netfs_stop();
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

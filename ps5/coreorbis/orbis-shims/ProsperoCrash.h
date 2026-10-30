// Snes9x PS5: a crash printer and stage markers, so a fault names where it happened (ProsperoCrash.cpp).
//
// 1.6 reached the shelf on the console and then died with "PPSA99009 crashed before KStuff was paused",
// with no line after "[covers] ... to download" in boot.log: an unhandled fault, and nothing said where.
// This installs signal handlers (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP, and SIGSYS since 2.10) on a separate signal stack
// (so a stack overflow can still be reported), modelled on PS5SX2's orbis-shims/ProsperoCrash.cpp. On a
// fault it writes one block to boot.log: the signal, the fault address, rip, the current stage of each
// thread group, and a short list of return addresses, each as an offset from an anchor symbol so the
// address maps straight into build/app/snes9x-pie.elf with llvm-addr2line.
//
// SPDX-License-Identifier: MIT
#pragma once

namespace crashlog
{
// Which part of the program a thread is in; the handler prints the latest of each. Setting one is a single
// pointer store, cheap enough to leave on the hot path.
enum class Area
{
	Boot,  // main thread, bring-up
	Shelf, // main thread, the game list
	Cover, // the cover worker
	Pool,  // the shelf's render threads
	Emu,   // the frontend's thread, a game's start (2.10)
	Count
};

// Install the handlers (once). Logs the anchor address used to map offsets back to the ELF.
void Install();

// The handler runs on an alternate signal stack, so a stack overflow still gets its report. That stack is
// per thread: each thread arms its own (BigThread and the audio thread do; the installing thread gets one in
// Install). Returns what DisarmThread frees when the thread ends; a no-op on the host.
void* ArmThread();
void DisarmThread(void* stack);

// Record where a thread group is now. `where` must be a string literal / long-lived pointer.
void Stage(Area area, const char* where);
} // namespace crashlog

// Short form used on the hot paths.
#define S9X_STAGE(area, where) ::crashlog::Stage(::crashlog::Area::area, where)

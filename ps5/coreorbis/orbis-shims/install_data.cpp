// Snes9x PS5: the dashboard app built into Snes9xPS5.elf (ProsperoInstall.h).
//
// The Makefile passes the five files' paths: eboot.bin (the launcher, which itself carries the emulator),
// sce_sys/param.json, sce_sys/icon0.png (the Snes9x PS5 icon, ps5/app/sce_sys/icon0.png),
// sce_sys/pic0.dds + pic1.dds (the home screen backgrounds) and sce_module/libc.prx (the C runtime module a native title carries, as PS5SX2's app folder). Only the final Snes9xPS5.elf links this file.
//
// SPDX-License-Identifier: MIT

#include "ProsperoInstall.h"

#define S9X_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const unsigned char sym##_begin[];                                                              \
	extern "C" const unsigned char sym##_end[];

S9X_INCBIN(s9x_app_eboot, APP_EBOOT)
S9X_INCBIN(s9x_app_param, APP_PARAM)
S9X_INCBIN(s9x_app_icon0, APP_ICON0)
S9X_INCBIN(s9x_app_pic0, APP_PIC0)
#ifndef APP_PIC1_IS_PIC0
S9X_INCBIN(s9x_app_pic1, APP_PIC1)
#else
// the two backgrounds are the same image (the Makefile compared them): built in once
#define s9x_app_pic1_begin s9x_app_pic0_begin
#define s9x_app_pic1_end s9x_app_pic0_end
#endif
S9X_INCBIN(s9x_app_libc, APP_LIBC)

const EmbeddedAppFile* EmbeddedAppFiles()
{
	static const EmbeddedAppFile files[] = {
		{"sce_sys/param.json", s9x_app_param_begin, s9x_app_param_end},
		{"sce_sys/icon0.png", s9x_app_icon0_begin, s9x_app_icon0_end},
		{"sce_sys/pic0.dds", s9x_app_pic0_begin, s9x_app_pic0_end},
		{"sce_sys/pic1.dds", s9x_app_pic1_begin, s9x_app_pic1_end},
		{"sce_module/libc.prx", s9x_app_libc_begin, s9x_app_libc_end},
		{"eboot.bin", s9x_app_eboot_begin, s9x_app_eboot_end},
		{nullptr, nullptr, nullptr},
	};
	return files;
}

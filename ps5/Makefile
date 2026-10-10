# Snes9x PS5
#   make ps5    -> build/ps5/Snes9xPS5.elf   the payload to send with the PS5 Payload Manager / ELF loader: it
#                  installs or updates the dashboard app /data/homebrew/$(TITLE_ID)/ (Snes9x itself, icon
#                  included) and stays running as the helper that lets the app reach /data
#   make app    -> build/app/$(TITLE_ID)/     the app folder on its own (eboot.bin, sce_module/, sce_sys/)
#   make host   -> build/host/                Linux builds of the same code (PS5 calls faked by host/sce_host.cpp)
#   make test   -> host builds + tests/run-tests.sh
#   make dist   -> build/dist/Snes9xPS5-v$(VERSION).elf + snes9x-ps5-v$(VERSION)-src.zip (release names)
#   make send   -> sends Snes9xPS5.elf to $(PS5_HOST):$(PS5_PORT) with the SDK's prospero-deploy
#
# 1.6 follows PS5SX2's layout: eboot.bin is the emulator, a native PS5 title (the console gives the controller
# to the title in front only), and the payload is its installer and helper. Build order:
#   1. build/ps5/Snes9xPS5-helper.elf   the helper alone (payload); the app sends it to the ELF loader itself
#   2. build/app/$(TITLE_ID)/            eboot.bin = Snes9x + the helper from step 1 (native title, signed);
#                                        sce_module/libc.prx; sce_sys/param.json, icon0.png, pic0.dds, pic1.dds
#   3. build/ps5/Snes9xPS5.elf           installer + helper, with the app folder from step 2 built in
#
# Dependencies: the ps5-payload-dev SDK (v0.42 or newer, https://github.com/ps5-payload-dev/sdk) in
# $(PS5_PAYLOAD_SDK), with clang/lld 18 on the host. zlib is vendored in third_party/zlib.

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION ?= 2.5
TITLE_ID ?= PPSA99009
APP_OUT := build/app/$(TITLE_ID)

ROOT := ..
ZLIB := third_party/zlib

CORE_SRC := apu/apu.cpp apu/bapu/dsp/sdsp.cpp apu/bapu/smp/smp.cpp apu/bapu/smp/smp_state.cpp \
	bsx.cpp c4.cpp c4emu.cpp cheats.cpp cheats2.cpp clip.cpp conffile.cpp controls.cpp cpu.cpp \
	cpuexec.cpp cpuops.cpp crosshairs.cpp dma.cpp dsp.cpp dsp1.cpp dsp2.cpp dsp3.cpp dsp4.cpp \
	fxinst.cpp fxemu.cpp gfx.cpp globals.cpp memmap.cpp obc1.cpp msu1.cpp ppu.cpp stream.cpp \
	sa1.cpp sa1cpu.cpp screenshot.cpp sdd1.cpp sdd1emu.cpp seta.cpp seta010.cpp seta011.cpp \
	seta018.cpp snapshot.cpp snes9x.cpp spc7110.cpp srtc.cpp tile.cpp tileimpl-n1x1.cpp \
	tileimpl-n2x1.cpp tileimpl-h2x1.cpp sha256.cpp bml.cpp movie.cpp fscompat.cpp loadzip.cpp
CORE_C_SRC := unzip/unzip.c unzip/ioapi.c

# the app (eboot.bin): the emulator and its frontend
EMU_SRC := coreorbis/main-boot.cpp \
	coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoVideo.cpp coreorbis/orbis-shims/ProsperoAudio.cpp \
	coreorbis/orbis-shims/ProsperoInput.cpp coreorbis/orbis-shims/ProsperoJailbreak.cpp \
	coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoCrt.cpp \
	frontend/fe_emu.cpp frontend/fe_menu.cpp frontend/fe_settings.cpp frontend/fe_text.cpp \
	frontend/fe_games.cpp frontend/fe_http.cpp frontend/fe_covers.cpp frontend/fe_shelf.cpp \
	frontend/fe_prefetch.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp
# the payloads: the helper, and the installer (= helper + install)
# (the helper also downloads the covers in the background: fe_coverworker.cpp, with the app's HTTPS code)
HELPER_SRC := coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoHelper.cpp coreorbis/orbis-shims/ProsperoCrash.cpp \
	frontend/fe_http.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp frontend/fe_tlshttp.cpp \
# the helper's own HTTPS (fe_tlshttp.cpp): Mbed TLS 3.6 (Apache-2.0), with fe_mbedtls_config.h on its default settings
MBEDTLS := frontend/third_party/mbedtls
MBEDTLS_SRC := $(sort $(wildcard $(MBEDTLS)/library/*.c))
MBEDTLS_FLAGS := -I$(MBEDTLS)/include -Ifrontend '-DMBEDTLS_USER_CONFIG_FILE="fe_mbedtls_config.h"'
INSTALLER_SRC := installer/installer_main.cpp $(HELPER_SRC) coreorbis/orbis-shims/ProsperoInstall.cpp

ZLIB_SRC := $(wildcard $(ZLIB)/*.c)

INCS := -I$(ROOT) -I$(ROOT)/apu -I$(ROOT)/apu/bapu -I$(ROOT)/unzip -I$(ZLIB) \
	-Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Ifrontend
DEFS := -DRIGHTSHIFT_IS_SAR -DZLIB -DUNZIP_SUPPORT -DSNES9X_PS5_VERSION='"$(VERSION)"' -DSNES9X_TITLE_ID='"$(TITLE_ID)"'
WARN := -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-missing-braces \
	-Wno-char-subscripts -Wno-deprecated-declarations -Wno-sign-compare -Wno-unused-value \
	-Wno-parentheses -Wno-unknown-warning-option
PORT_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers

# the No-Intro name table built into fe_games.o; stb_image in fe_covers.o has warnings of its own
GAMEDB_DEFS := -DGAMEDB_TSV='"$(abspath frontend/data/snes-nointro.tsv)"'
%/port/frontend/fe_games.o: EXTRA_FLAGS = $(GAMEDB_DEFS)
%/port/frontend/fe_covers.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-but-set-variable -Wno-implicit-fallthrough -DCOVER_MANIFEST_TXT='"$(abspath frontend/data/snes-cover-index.txt)"' -DTITLE_MANIFEST_TXT='"$(abspath frontend/data/snes-title-index.txt)"' -DSNAP_MANIFEST_TXT='"$(abspath frontend/data/snes-snap-index.txt)"'
%/port/frontend/fe_covers.o: frontend/data/snes-cover-index.txt frontend/data/snes-title-index.txt frontend/data/snes-snap-index.txt
%/port/frontend/fe_games.o: frontend/data/snes-nointro.tsv
# the UI fonts (PS5SX2's: PCSX2's Roboto Regular and PromptFont) built into fe_text.o
%/port/frontend/fe_text.o: EXTRA_FLAGS = -DFE_FONT_DIR='"$(abspath frontend/assets/fonts)"' -Wno-error -Wno-sign-compare -Wno-unused-function
%/port/frontend/fe_text.o: frontend/assets/fonts/Roboto-Regular.ttf frontend/assets/fonts/promptfont.otf \
	frontend/assets/fonts/fa-brands-400.otf
# the helper's HTTPS client: Mbed TLS's headers, and Mozilla's CA list built in
%/port/frontend/fe_tlshttp.o: EXTRA_FLAGS = $(MBEDTLS_FLAGS) -DCACERT_PEM='"$(abspath frontend/data/cacert.pem)"'
%/port/frontend/fe_tlshttp.o: frontend/data/cacert.pem frontend/fe_mbedtls_config.h
# the helper payload is installer_main.cpp without the install step
%/port/installer/helper_main.o: EXTRA_FLAGS = -DSNES9X_HELPER_ONLY

# ---------------- PS5 ----------------
PS5_CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
PS5_OUT := build/ps5
# The PS5's CPU is Zen 2 (PS5SX2 builds with the same -march).
PS5_OPT := -O3 -fPIC -march=znver2 -fno-strict-aliasing
# the payloads: notifications, and the kernel access the helper needs (libkernel_sys: sceKernelGetAppInfo)
# the payload SDK's network libraries (its http2_get sample's), for the helper's cover downloads
PS5_PAYLOAD_LIBS := -lkernel_sys -lSceNet -lSceNetCtl -lSceSsl -lSceHttp2 -lpthread

PS5_CORE_OBJ := $(patsubst %.cpp,$(PS5_OUT)/core/%.o,$(CORE_SRC)) $(patsubst %.c,$(PS5_OUT)/core/%.o,$(CORE_C_SRC))
PS5_EMU_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(EMU_SRC))
PS5_MBEDTLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(PS5_OUT)/mbedtls/%.o,$(MBEDTLS_SRC))
PS5_HELPER_OBJ := $(PS5_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(HELPER_SRC)) $(PS5_MBEDTLS_OBJ)
PS5_INSTALLER_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(INSTALLER_SRC)) $(PS5_MBEDTLS_OBJ)
PS5_ZLIB_OBJ := $(patsubst $(ZLIB)/%.c,$(PS5_OUT)/zlib/%.o,$(ZLIB_SRC))

ps5: $(PS5_OUT)/Snes9xPS5.elf

$(PS5_OUT)/core/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(WARN) -c -o $@ $<

$(PS5_OUT)/core/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(DEFS) $(INCS) -w -c -o $@ $<

$(PS5_OUT)/mbedtls/%.o: $(MBEDTLS)/library/%.c frontend/fe_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(PS5_CC) -Os -fPIC -march=znver2 $(MBEDTLS_FLAGS) -c -o $@ $<

$(PS5_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(PS5_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(PS5_OUT)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -O2 -fPIC -march=znver2 -DHAVE_UNISTD_H -I$(ZLIB) -w -c -o $@ $<

# 1. the helper payload (built into the app, which sends it to the ELF loader when no helper answers)
$(PS5_OUT)/Snes9xPS5-helper.elf: $(PS5_HELPER_OBJ)
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)

$(PS5_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(PS5_OUT)/Snes9xPS5-helper.elf
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(PS5_OUT)/Snes9xPS5-helper.elf)"' -c -o $@ $<

# 3. the installer + helper, with the app folder built in
APP_FILES = $(APP_OUT)/eboot.bin $(APP_OUT)/sce_sys/param.json $(APP_OUT)/sce_sys/icon0.png $(APP_OUT)/sce_sys/pic0.dds \
	$(APP_OUT)/sce_sys/pic1.dds \
	$(APP_OUT)/sce_module/libc.prx
APP_DATA_DEFS = -DAPP_EBOOT='"$(abspath $(APP_OUT)/eboot.bin)"' -DAPP_PARAM='"$(abspath $(APP_OUT)/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath $(APP_OUT)/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath $(APP_OUT)/sce_sys/pic0.dds)"' \
	-DAPP_PIC1='"$(abspath $(APP_OUT)/sce_sys/pic1.dds)"' \
	-DAPP_LIBC='"$(abspath $(APP_OUT)/sce_module/libc.prx)"'

$(PS5_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h $(APP_FILES)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(APP_DATA_DEFS) -c -o $@ $<

$(PS5_OUT)/Snes9xPS5.elf: $(PS5_INSTALLER_OBJ) $(PS5_OUT)/install_data.o
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)
	@echo "built $@ ($$(stat -c %s $@) bytes)"

$(PS5_OUT)/Snes9xPS5-stripped.elf: $(PS5_OUT)/Snes9xPS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip -o $@ $<

strip: $(PS5_OUT)/Snes9xPS5-stripped.elf

# the release files, named with the version: build/dist/Snes9xPS5-v$(VERSION).elf and the sources zip
DIST := build/dist
dist: $(PS5_OUT)/Snes9xPS5.elf
	@mkdir -p $(DIST)
	cp $< $(DIST)/Snes9xPS5-v$(VERSION).elf
	cd ../.. && rm -f $(abspath $(DIST))/snes9x-ps5-v$(VERSION)-src.zip && \
		zip -qr $(abspath $(DIST))/snes9x-ps5-v$(VERSION)-src.zip $(notdir $(abspath ..)) \
		-x '*/ps5/build/*' '*/proto/native/build/*' '*.o'
	@ls -l $(DIST)

send: $(PS5_OUT)/Snes9xPS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $<

# ---------------- the app (eboot.bin = Snes9x) ----------------
# 2. A native PS5 title linked the way PS5SX2 links its eboot (ps5/coreorbis/link-vk.sh): the boilerplate's
# app_crt.cpp (_start -> _init_env -> main) and app_cpp_runtime.cpp, the emulator's objects (the same ones,
# compiled once), a PIE laid out by ps5-pie.ld (+ PS5SX2's ehframe.ld) against the SDK's system-module stubs
# and its static libc++/libc++abi/libunwind/libc, every defined symbol local (PS5SX2's genmap.py result:
# localize.map); then ps5-native-tool link (SCE dynamic tables, process parameters) and self --sign.
# The folder carries sce_module/libc.prx, the C runtime module a native title needs: 1.4/1.5 had none and
# the console stopped the app with PRX_SCE_MODULE_LOAD_ERROR (0xa0020102).
NATIVE := proto/native
NATIVE_TOOL := $(NATIVE)/build/host/ps5-native-tool
LIBC_BUILDER := $(NATIVE)/build/host/libc-builder
LLD ?= ld.lld
NATIVE_CLANG = PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) sh $(NATIVE)/tooling/prospero-clang18
SDK_LIB := $(PS5_PAYLOAD_SDK)/target/lib
# every system-module stub but the two PS5SX2 leaves out (link-vk.sh)
APP_STUBS = $(filter-out %/libScePosixForWebKit.so %/libSceGLSlimVSH.so,$(sort $(wildcard $(SDK_LIB)/*.so)))

$(NATIVE_TOOL): $(NATIVE)/tooling/native/native_app_builder.cpp $(NATIVE)/tooling/native/elf_object.cpp \
		$(NATIVE)/tooling/native/self_container.cpp $(NATIVE)/tooling/native/sce_module_writer.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -o $@ $^ -lz
HOST_CXX_NATIVE ?= clang++

$(LIBC_BUILDER): $(NATIVE)/tooling/native/libc_builder.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -Wall -Wextra -Werror -o $@ $<

# sce_module/libc.prx, deterministic: it must match the published digest (proto/native/libc.prx.sha256)
build/app/libc.prx: $(LIBC_BUILDER) $(NATIVE_TOOL) $(wildcard $(NATIVE)/tooling/native/runtime/*.txt)
	@mkdir -p $(dir $@)
	$(LIBC_BUILDER) $(NATIVE)/tooling/native/runtime/api-surface.txt $(NATIVE)/tooling/native/runtime/imports.txt build/app/libc.raw.elf
	$(NATIVE_TOOL) self --sign --in build/app/libc.raw.elf --out $@
	cd build/app && sha256sum --check --strict $(abspath $(NATIVE)/libc.prx.sha256)

$(APP_OUT)/sce_module/libc.prx: build/app/libc.prx
	@mkdir -p $(dir $@)
	cp $< $@

app: $(APP_FILES)

build/app/app_crt.o build/app/app_cpp_runtime.o: build/app/%.o: $(NATIVE)/tooling/native/%.cpp
	@mkdir -p $(dir $@)
	$(NATIVE_CLANG) -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
		-c -o $@ $<

EBOOT_OBJS = build/app/app_crt.o build/app/app_cpp_runtime.o $(PS5_CORE_OBJ) $(PS5_EMU_OBJ) $(PS5_ZLIB_OBJ) \
	$(PS5_OUT)/helper_data.o
build/app/snes9x-pie.elf: $(EBOOT_OBJS) $(NATIVE)/tooling/native/ps5-pie.ld $(NATIVE)/tooling/native/ehframe.ld \
		$(NATIVE)/tooling/native/app-symbols.map $(NATIVE)/tooling/native/localize.map
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
		-T $(NATIVE)/tooling/native/ps5-pie.ld -T $(NATIVE)/tooling/native/ehframe.ld --eh-frame-hdr \
		--version-script $(NATIVE)/tooling/native/app-symbols.map --version-script $(NATIVE)/tooling/native/localize.map \
		-e _start --no-dynamic-linker -o $@ $(EBOOT_OBJS) \
		--as-needed $(APP_STUBS) -L $(SDK_LIB) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a $(SDK_LIB)/libc.a -lpthread

build/app/eboot.elf: build/app/snes9x-pie.elf $(NATIVE_TOOL)
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(SDK_LIB) --module-sdk 0x02000009 \
		--companion-sdk 0x08050001 --file-name eboot.elf
	python3 tools/prepare_console_elf.py --input $@ --output build/app/eboot-console-ready.elf

$(APP_OUT)/eboot.bin: build/app/eboot.elf $(NATIVE_TOOL)
	@mkdir -p $(dir $@)
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F
	$(NATIVE_TOOL) self --inspect --file $@

$(APP_OUT)/sce_sys/param.json: app/sce_sys/param.json
	@mkdir -p $(dir $@)
	sed 's/PPSA99009/$(TITLE_ID)/g' $< > $@

$(APP_OUT)/sce_sys/%.png: app/sce_sys/%.png
	@mkdir -p $(dir $@)
	cp $< $@

# pic0.dds (the home screen's background while the icon is selected) and pic1.dds (the launch background):
# 3840x2160 BC7_UNORM DX10 DDS without mipmaps, made from app/sce_sys/background-source.png with the
# ps5-native-app-boilerplate's tools/prepare-assets.sh --background (pinned bc7enc_rdo).
$(APP_OUT)/sce_sys/%.dds: app/sce_sys/%.dds
	@mkdir -p $(dir $@)
	cp $< $@

# ---------------- host ----------------
HOST_CC ?= gcc
HOST_CXX ?= g++
HOST_OUT := build/host
HOST_SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_OPT := -O1 -g -mavx2 -fno-strict-aliasing
# The Snes9x core reads unaligned words and shifts by negative amounts on purpose (x86 only): not our bugs.
HOST_CORE_NOSAN := -fno-sanitize=alignment,shift

HOST_CORE_OBJ := $(patsubst %.cpp,$(HOST_OUT)/core/%.o,$(CORE_SRC)) $(patsubst %.c,$(HOST_OUT)/core/%.o,$(CORE_C_SRC))
HOST_EMU_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(EMU_SRC) host/sce_host.cpp)
HOST_MBEDTLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(HOST_OUT)/mbedtls/%.o,$(MBEDTLS_SRC))
HOST_HELPER_OBJ := $(HOST_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(HELPER_SRC) host/sce_host.cpp) \
	$(HOST_MBEDTLS_OBJ)
HOST_INSTALLER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(INSTALLER_SRC) host/sce_host.cpp) $(HOST_MBEDTLS_OBJ)
HOST_ZLIB_OBJ := $(patsubst $(ZLIB)/%.c,$(HOST_OUT)/zlib/%.o,$(ZLIB_SRC))

host: $(HOST_OUT)/snes9x-ps5-host $(HOST_OUT)/snes9x-ps5-installer $(HOST_OUT)/snes9x-ps5-helper

$(HOST_OUT)/core/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(HOST_CORE_NOSAN) -include cstdint $(DEFS) $(INCS) $(WARN) -c -o $@ $<

$(HOST_OUT)/core/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) -w -c -o $@ $<

$(HOST_OUT)/mbedtls/%.o: $(MBEDTLS)/library/%.c frontend/fe_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -g1 $(MBEDTLS_FLAGS) -c -o $@ $<

$(HOST_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -DHAVE_UNISTD_H -I$(ZLIB) -w -c -o $@ $<

# The host installer carries a stand-in app folder (tests/fake-eboot.bin, tests/fake-libc.prx + the real
# param/icon/pic); the host app carries the host helper, which it hands to a fake ELF loader in the tests.
HOST_APP_DEFS = -DAPP_EBOOT='"$(abspath tests/fake-eboot.bin)"' -DAPP_PARAM='"$(abspath app/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath app/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath app/sce_sys/pic0.dds)"' \
	-DAPP_PIC1='"$(abspath app/sce_sys/pic1.dds)"' \
	-DAPP_LIBC='"$(abspath tests/fake-libc.prx)"'

$(HOST_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h \
		tests/fake-eboot.bin tests/fake-libc.prx app/sce_sys/param.json app/sce_sys/icon0.png app/sce_sys/pic0.dds app/sce_sys/pic1.dds
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(HOST_APP_DEFS) -c -o $@ $<

$(HOST_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(HOST_OUT)/snes9x-ps5-helper
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(HOST_OUT)/snes9x-ps5-helper)"' -c -o $@ $<

$(HOST_OUT)/snes9x-ps5-helper: $(HOST_HELPER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/snes9x-ps5-installer: $(HOST_INSTALLER_OBJ) $(HOST_OUT)/install_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/snes9x-ps5-host: $(HOST_CORE_OBJ) $(HOST_EMU_OBJ) $(HOST_ZLIB_OBJ) $(HOST_OUT)/helper_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

test: host
	bash tests/run-tests.sh

clean:
	rm -rf build

.PHONY: ps5 host test send strip app dist clean
.SECONDARY:

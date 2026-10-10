#!/bin/bash
# Snes9x PS5 host tests: the real port code (main-boot, shims, frontend, Snes9x core; the installer and the
# helper) on Linux, with the PS5 calls faked by host/sce_host.cpp. A scripted pad drives the app; flips are
# de-tiled to PPM and checked.
set -u
cd "$(dirname "$0")/.."
BIN=$PWD/build/host/snes9x-ps5-host
INSTALLER=$PWD/build/host/snes9x-ps5-installer
HELPER=$PWD/build/host/snes9x-ps5-helper
VER=$(sed -n 's/^VERSION ?= //p' Makefile) # the release the binaries say they are
# ports nobody listens on, so the app's ordinary runs find no helper and no ELF loader
export SNES9X_HELPER_PORT=$((20000 + RANDOM % 10000)) SNES9X_ELFLDR_PORT=$((30000 + RANDOM % 10000)) SNES9X_JB_NO_OTHERS=1
CHECK="python3 $PWD/tests/check_ppm.py"
WORK=${WORK:-$(mktemp -d)}
PASS=0
FAIL=0

# pad bits
CROSS=4000; CIRCLE=2000; UP=10; DOWN=40; L2=100; L3R3=6; OPTIONS=8

ok() { echo "  ok: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
expect() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }

newroot() {
	local t=$WORK/$1
	rm -rf "$t" && mkdir -p "$t/root/roms" "$t/dump"
	echo "shader=0" >"$t/root/snes9x-ps5.ini" # the plain picture: the colour checks expect it (group 16 tests the shaders)
	echo "$t"
}

run() { # dir pad dumps [args...]
	local t=$1 pad=$2 dumps=$3
	shift 3
	SNES9X_HOST_OFFLINE=${OFFLINE-1} SNES9X_COVER_URL=${COVER_URL-} \
	SNES9X_PS5_ROOT=$t/root SNES9X_PS5_HOMEBREW=$t/homebrew SNES9X_HOST_DUMP_DIR=$t/dump SNES9X_HOST_DUMP=$dumps SNES9X_HOST_MAX_FLIPS=5000 \
		SNES9X_HOST_PAD=$pad ASAN_OPTIONS=detect_leaks=0 timeout 120 "$BIN" "$@" >"$t/out.txt" 2>&1
	echo $?
}

echo "== 1. a ROM from the command line: picture, input, quick save, quit from the pause menu"
T=$(newroot t1)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$CROSS;140:0;200:$(printf %x $((0x$L2 | 0x$UP)));205:0;220:$L3R3;225:0;240:$UP;242:0;250:$CROSS;252:0" "90,130" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00090.ppm 960 540 255 0 0 >/dev/null" "red backdrop in the middle at flip 90"
expect "$CHECK $T/dump/flip00090.ppm 100 540 0 0 0 >/dev/null" "black border outside the 4:3 picture"
expect "$CHECK $T/dump/flip00090.ppm 245 540 255 0 0 >/dev/null" "4:3 picture starts at x=240"
expect "$CHECK $T/dump/flip00130.ppm 960 540 0 255 0 >/dev/null" "Cross -> SNES B -> green backdrop"
expect "[ -f $T/root/states/test.000 ]" "L2 + Up wrote states/test.000"
expect "grep -q 'battery save' $T/root/logs/boot.log; [ \$? = 1 ]" "no battery save for a game without SRAM"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 2. shelf -> game -> back to the shelf -> quit"
T=$(newroot t2)
python3 tests/make_test_rom.py "$T/root/roms/Test Game (USA).sfc" ntsc >/dev/null
mkdir -p "$T/root/roms/RPG"
rc=$(run "$T" "0:0;40:$CROSS;42:0;150:$L3R3;152:0;160:$UP;162:0;164:$UP;166:0;170:$CROSS;172:0;200:$OPTIONS;202:0;210:$CROSS;212:0" "20,35,120,190")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ -f $T/dump/flip00020.ppm ]" "the shelf was shown"
expect "$CHECK $T/dump/flip00120.ppm 960 540 255 0 0 >/dev/null" "the game picked on the shelf runs"
expect "grep -q 'last_rom=.*Test Game (USA).sfc' $T/root/snes9x-ps5.ini" "the shelf remembers the last game"
expect "grep -q 'loading .*Test Game' $T/root/logs/boot.log" "loaded from the shelf"

echo "== 3. a 50 Hz (PAL) game paces on the audio clock"
T=$(newroot t3)
python3 tests/make_test_rom.py "$T/root/roms/pal.sfc" pal >/dev/null
start=$(date +%s.%N)
rc=$(run "$T" "0:0;300:$L3R3;302:0;310:$UP;312:0;320:$CROSS;322:0" "" "$T/root/roms/pal.sfc")
end=$(date +%s.%N)
secs=$(python3 -c "print(round($end - $start, 2))")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '50 fps, PAL' $T/root/logs/boot.log" "detected as PAL"
expect "python3 -c 'import sys; sys.exit(0 if 5.0 <= $secs <= 9.0 else 1)'" "300 frames took ${secs}s (~6 s at 50 fps)"
# no sound output (sceAudioOutOpen refused): the PAL game is paced by the clock, not left to run flat out
T=$(newroot t3b)
python3 tests/make_test_rom.py "$T/root/roms/pal.sfc" pal >/dev/null
start=$(date +%s.%N)
rc=$(SNES9X_HOST_AUDIO_FAIL=1 run "$T" "0:0;300:$L3R3;302:0;310:$UP;312:0;320:$CROSS;322:0" "" "$T/root/roms/pal.sfc")
end=$(date +%s.%N)
secs=$(python3 -c "print(round($end - $start, 2))")
expect "[ $rc = 0 ] && python3 -c 'import sys; sys.exit(0 if 5.0 <= $secs <= 9.0 else 1)'" "without sound output, 300 PAL frames still took ${secs}s (~6 s at 50 fps)"

echo "== 4. integer scale and scanlines from the settings file; load state with L2 + Down"
T=$(newroot t4)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
printf 'aspect=2\nscanlines=1\n' >>"$T/root/snes9x-ps5.ini"
rc=$(run "$T" "0:0;60:$(printf %x $((0x$L2 | 0x$UP)));62:0;80:$(printf %x $((0x$L2 | 0x$DOWN)));82:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0" "50" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00050.ppm 445 540 0 0 0 >/dev/null" "integer 4x: black left of x=448"
expect "$CHECK $T/dump/flip00050.ppm 450 540 255 0 0 >/dev/null" "integer 4x: picture from x=448"
expect "$CHECK $T/dump/flip00050.ppm 960 95 127 0 0 >/dev/null" "scanline row (4th of each line) is darker"
expect "$CHECK $T/dump/flip00050.ppm 960 93 255 0 0 >/dev/null" "other rows full brightness"
expect "grep -q 'load state 0 .*: ok' $T/root/logs/boot.log" "L2 + Down loaded the state"

echo "== 5. no ROMs: the shelf explains where to put them; OPTIONS + Cross quits, Circle doesn't"
T=$(newroot t5)
rc=$(run "$T" "0:0;30:$OPTIONS;32:0;40:$CIRCLE;42:0;60:$OPTIONS;62:0;70:$CROSS;72:0" "20")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ -f $T/dump/flip00020.ppm ]" "the empty shelf was shown"

echo "== 6. a zipped ROM; 60 Hz game paced by vsync keeps the sound fed"
T=$(newroot t6)
python3 tests/make_test_rom.py "$T/game.sfc" ntsc >/dev/null
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED); z.write(sys.argv[2],'game.sfc'); z.close()" "$T/root/roms/game.zip" "$T/game.sfc"
start=$(date +%s.%N)
rc=$(SNES9X_HOST_REALTIME=1 run "$T" "0:0;300:$L3R3;302:0;310:$UP;312:0;320:$CROSS;322:0" "100" "$T/root/roms/game.zip")
end=$(date +%s.%N)
secs=$(python3 -c "print(round($end - $start, 2))")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00100.ppm 960 540 255 0 0 >/dev/null" "the zipped game runs"
expect "python3 -c 'import sys; sys.exit(0 if 4.5 <= $secs <= 8.0 else 1)'" "300 frames at vsync took ${secs}s (~5 s at 60 Hz)"
u60=$(grep -o 'frame 60: .*underruns [0-9]*' "$T/root/logs/boot.log" | grep -o '[0-9]*$')
u240=$(grep -o 'frame 240: .*underruns [0-9]*' "$T/root/logs/boot.log" | grep -o '[0-9]*$')
under=$(( ${u240:-999} - ${u60:-0} ))
expect "[ $under -le 2 ]" "audio underruns between frames 60 and 240: $under"

echo "== 7. little video memory: 720p scan-out; memory busy at first: retries"
T=$(newroot t7)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(SNES9X_HOST_DIRECT_MAX_MIB=12 run "$T" "0:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0" "90" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'scan-out 1280x720' $T/root/logs/boot.log" "fell back to 1280x720"
expect "head -2 $T/dump/flip00090.ppm | grep -q '1280 720'" "the flips are 1280x720"
expect "$CHECK $T/dump/flip00090.ppm 640 360 255 0 0 >/dev/null" "game picture in the middle (720p)"
expect "$CHECK $T/dump/flip00090.ppm 100 360 0 0 0 >/dev/null" "4:3 border kept (720p)"
T=$(newroot t7b)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(SNES9X_HOST_DIRECT_FAIL_FIRST=9 run "$T" "0:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0" "" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'retrying (1)' $T/root/logs/boot.log && grep -q 'scan-out 1920x1080' $T/root/logs/boot.log" "retried, then got 1080p"

echo "== 8. Snes9xPS5.elf installs the app (icon and libc.prx included), stays as the helper, repairs"
T=$(newroot t8)
APP=$T/homebrew/PPSA99009
inst() { # dir -> runs the installer; it stays running as the helper when the port is free
	SNES9X_PS5_ROOT=$1/root SNES9X_PS5_HOMEBREW=$1/homebrew SNES9X_PS5_APPMETA=$1/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >>"$1/inst.txt" 2>&1
}
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/snes9x-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }
stop_helpers
inst "$T" &
expect "waitfor $T/root/logs/installer.log 'listening on 127.0.0.1'" "the installer stays running as the helper"
expect "cmp -s $APP/sce_sys/icon0.png app/sce_sys/icon0.png" "icon0.png installed (the Snes9x PS5 icon)"
expect "cmp -s $APP/sce_sys/pic0.dds app/sce_sys/pic0.dds && cmp -s $APP/sce_sys/pic1.dds app/sce_sys/pic1.dds && cmp -s $APP/sce_sys/param.json app/sce_sys/param.json" "pic0.dds/pic1.dds (background) and param.json installed"
expect "cmp -s $APP/eboot.bin tests/fake-eboot.bin" "eboot.bin installed"
expect "cmp -s $APP/sce_module/libc.prx tests/fake-libc.prx" "sce_module/libc.prx installed"
expect "grep -q 'Snes9x PS5 $VER installed. Open it from the Snes9x PS5 icon' $T/inst.txt" "install notification says to open the icon"
expect "! ls $APP/*.part $APP/sce_sys/*.part $APP/sce_module/*.part 2>/dev/null | grep -q ." "no .part files left"
inst "$T"
expect "grep -q 'is up to date' $T/root/logs/installer.log && ! grep -q 'wrote' $T/root/logs/installer.log" "sent again: nothing rewritten"
expect "grep -q 'a helper is already running' $T/root/logs/installer.log" "sent again: the second copy leaves the helper to the first"
echo broken > $APP/sce_sys/icon0.png
inst "$T"
expect "cmp -s $APP/sce_sys/icon0.png app/sce_sys/icon0.png" "a damaged icon is put back"
# ShadowMountPlus registered the title with the old art (/user/appmeta/<title>): the installer brings it up to date
META=$T/appmeta/PPSA99009
mkdir -p "$META" && echo old > "$META/pic0.png" && echo old > "$META/icon0.png" && cp app/sce_sys/param.json "$META/"
echo old > "$APP/sce_sys/pic0.png"
inst "$T"
expect "cmp -s $META/pic0.dds app/sce_sys/pic0.dds && cmp -s $META/pic1.dds app/sce_sys/pic1.dds && cmp -s $META/icon0.png app/sce_sys/icon0.png" "appmeta gets the new background (pic0/pic1.dds) and icon"
expect "[ ! -f $META/pic0.png ] && [ ! -f $APP/sce_sys/pic0.png ]" "the old pic0.png is removed"
expect "grep -q 'Home screen art updated' $T/inst.txt" "the notification says the home screen art changed"
expect "grep -q 'updated to $VER' $T/inst.txt" "update notification"
stop_helpers
expect "[ \$(find $T -path '*PPSA99203*' | wc -l) = 0 ]" "nothing written for PS5SX2 (PPSA99203)"

echo "== 9. covers: download, git-symlink, name by CRC, your own cover first, 404 remembered"
T=$(newroot t9)
SRV=$T/srv/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
python3 - "$SRV" "$T/root/covers" <<'PY'
import sys
from PIL import Image
srv, covers = sys.argv[1], sys.argv[2]
Image.new('RGB', (512, 357), (255, 0, 0)).save(srv + '/Super Mario World (USA).png')
Image.new('RGB', (512, 357), (0, 255, 0)).save(srv + '/Donkey Kong Country (USA).png')
open(srv + '/Donkey Kong Country (USA) (Rev 2).png', 'w').write('Donkey Kong Country (USA).png')
Image.new('RGB', (512, 357), (255, 255, 0)).save(srv + '/Final Fantasy III (USA).png')
Image.new('RGB', (360, 512), (0, 0, 255)).save(covers + '/Final Fantasy III (USA).png')
PY
for n in "Super Mario World (USA)" "Chrono Trigger (USA)" "Final Fantasy III (USA)"; do python3 tests/make_test_rom.py "$T/root/roms/$n.sfc" ntsc >/dev/null; done
python3 tests/make_test_rom.py "$T/root/roms/dkc.sfc" ntsc >/dev/null
python3 tests/forge_crc.py "$T/root/roms/dkc.sfc" $(grep -P "\tDonkey Kong Country \(USA\) \(Rev 2\)$" frontend/data/snes-nointro.tsv | cut -f1) >/dev/null
PORT=18080
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
# shelf order: Chrono Trigger, Donkey Kong Country, Final Fantasy III, Super Mario World
RIGHT=20
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png" SNES9X_HOST_REALTIME=1 run "$T" \
	"0:0;180:$RIGHT;182:0;230:$RIGHT;232:0;280:$RIGHT;282:0;330:$OPTIONS;332:0;340:$CROSS;342:0" "170,220,270,320")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'dkc.sfc -> \"Donkey Kong Country (USA) (Rev 2)\" (by CRC)' $T/root/logs/boot.log" "dkc.sfc named by its CRC"
expect "[ -f '$T/root/covers/Super Mario World (USA).png' ]" "a cover was downloaded into the cache"
expect "[ -f '$T/root/covers/Chrono Trigger (USA).missing' ]" "a 404 is remembered (.missing)"
expect "grep -q 'is a link to Donkey Kong Country (USA).png' $T/root/logs/boot.log" "a git-symlink cover is followed"
expect "! grep -q 'GET .*Final%20Fantasy' $T/root/logs/boot.log" "your own cover: nothing downloaded for it"
expect "! $CHECK $T/dump/flip00170.ppm 960 420 255 0 0 >/dev/null 2>&1 && ! $CHECK $T/dump/flip00170.ppm 960 420 0 255 0 >/dev/null 2>&1" "no cover online: a placeholder card"
expect "$CHECK $T/dump/flip00220.ppm 960 420 0 255 0 >/dev/null" "Donkey Kong Country shows the linked (green) cover"
expect "$CHECK $T/dump/flip00270.ppm 960 420 0 0 255 >/dev/null" "Final Fantasy III shows your own (blue) cover, not the server's"
expect "$CHECK $T/dump/flip00320.ppm 960 420 255 0 0 >/dev/null" "Super Mario World shows the downloaded (red) cover"

echo "== 10. offline: no download is tried, the shelf still works"
T=$(newroot t10)
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World (USA).sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;30:$OPTIONS;32:0;40:$CROSS;42:0" "20")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'not connected' $T/root/logs/boot.log && ! grep -q 'GET ' $T/root/logs/boot.log" "no request made without a network"

echo "== 11. the pad is shared with the system (handle 0x809b0081, as on the console)"
T=$(newroot t11)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(SNES9X_HOST_PAD_SHARED=1 SNES9X_HOST_REALTIME=1 run "$T" "0:0;100:$CROSS;140:0;300:$L3R3;302:0;310:$UP;312:0;320:$CROSS;322:0" "130" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'using 809b0081 (shared with the system)' $T/root/logs/boot.log" "the system's handle is used"
expect "[ \$(grep -c 'scePadOpen' $T/root/logs/boot.log) = 1 ]" "opened once, not every 2 seconds"
expect "$CHECK $T/dump/flip00130.ppm 960 540 0 255 0 >/dev/null" "its buttons reach the game (Cross -> green)"

echo "== 12. the app asks the helper to let it out of the sandbox"
T=$(newroot t12)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
QUIT="0:0;30:$L3R3;32:0;40:$UP;42:0;50:$CROSS;52:0"
SNES9X_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
HPID=$!
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$QUIT" "" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'Snes9x helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "the helper said yes"
expect "grep -q 'jailbreak: Snes9x helper' $T/root/logs/boot.log" "logged before /data was open, written to boot.log"
expect "grep -q 'letting it out' $T/hroot/logs/helper.log" "the helper let the app's process out"
stop_helpers
T=$(newroot t12b)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
SNES9X_HOST_JB_TITLE=PPSA99203 SNES9X_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
HPID=$!
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$QUIT" "" "$T/root/roms/test.sfc")
expect "grep -q 'title PPSA99203 is not Snes9x PS5' $T/hroot/logs/helper.log" "the helper lets out no other title"
stop_helpers

echo "== 13. no helper running: the app hands its own helper to the ELF loader, then asks it"
T=$(newroot t13)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
python3 - "$SNES9X_ELFLDR_PORT" "$T" <<'PY' &
import os, socket, subprocess, sys
port, t = int(sys.argv[1]), sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(1); s.settimeout(60)
c, _ = s.accept()
data = b''
while True:
    d = c.recv(65536)
    if not d: break
    data += d
path = t + '/received.elf'
open(path, 'wb').write(data); os.chmod(path, 0o755)
env = dict(os.environ, SNES9X_PS5_ROOT=t + '/hroot', ASAN_OPTIONS='detect_leaks=0')
p = subprocess.Popen(['timeout', '30', path], env=env, stdout=open(t + '/helper.txt', 'w'), stderr=subprocess.STDOUT)
open(t + '/helper.pid', 'w').write(str(p.pid))
PY
LPID=$!
sleep 0.5
rc=$(run "$T" "$QUIT" "" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "cmp -s $T/received.elf $HELPER" "the ELF loader got the helper built into the app"
expect "grep -q 'jailbreak: Snes9x helper (started by the app)' $T/root/logs/boot.log" "then the helper it started let it out"
wait $LPID 2>/dev/null
stop_helpers

echo "== 14. no /data even so: the screen says what to do; the screen busy at first: retries"
T=$(newroot t14)
rm -rf "$T/root" && echo "not a folder" >"$T/root" # the app can't create its folders there
rc=$(run "$T" "0:0;30:$CROSS;32:0" "20")
expect "[ $rc = 2 ]" "exit code 2 (got $rc)"
expect "grep -q 'no access to /data' $T/out.txt" "logged: no access to /data"
expect "[ -f $T/dump/flip00020.ppm ]" "a message was on the screen"
T=$(newroot t14b)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(SNES9X_HOST_VIDEO_BUSY=3 run "$T" "$QUIT" "" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \$(grep -c 'sceVideoOutOpen -> .*80290009' $T/root/logs/boot.log) = 3 ] && grep -q 'scan-out 1920x1080' $T/root/logs/boot.log" "VideoOut busy three times, then opened"
expect "grep -q 'splash screen hidden' $T/root/logs/boot.log" "the splash screen is hidden"

echo "== 15. covers in the background: the helper downloads them while the shelf runs; an older helper: the prefetch"
stop_helpers
T=$(newroot t15)
SRV=$T/srv/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(255,0,0)).save('$SRV/Super Mario World (USA).png')"
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World (USA).sfc" ntsc >/dev/null
PORT=18081
# the server answers after 2 s, so the shelf shows the card first, then the cover
SERVER_DELAY=2 python3 tests/https_server.py "$T/srv" $PORT >/dev/null 2>&1 &
SRVPID=$!
SNES9X_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/root/logs/helper.log" "listening" || sleep 1
URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png"
rc=$(OFFLINE= COVER_URL="$URL" SNES9X_HOST_REALTIME=1 run "$T" "0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "20,280")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'the helper downloads the covers while the app runs: nothing to wait for' $T/root/logs/boot.log && ! grep -q 'restarting' $T/root/logs/boot*.log" "the app starts at once: no download before /data, no restart"
expect "grep -q 'Super Mario World (USA).png' $T/root/covers/wanted.txt" "the missing cover goes to covers/wanted.txt"
expect "grep -q 'Super Mario World (USA).png -> 200' $T/root/logs/helper.log && cmp -s '$T/root/covers/Super Mario World (USA).png' '$SRV/Super Mario World (USA).png'" "the helper downloaded it into covers/"
expect "! $CHECK $T/dump/flip00020.ppm 960 420 255 0 0 >/dev/null 2>&1 && $CHECK $T/dump/flip00280.ppm 960 420 255 0 0 >/dev/null" "the shelf showed its card, then the cover once it landed"
expect "grep -q '^0 1 idle' $T/root/covers/progress.txt" "covers/progress.txt: nothing left, 1 fetched"
kill $SRVPID 2>/dev/null
stop_helpers
# an older helper (2.1's protocol: the list without "covers: background"): the prefetch before /data, as before
T=$(newroot t15b)
mkdir -p "$T/srv/Named_Boxarts" "$T/root/covers"
cp "$SRV/Super Mario World (USA).png" "$T/srv/Named_Boxarts/"
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World (USA).sfc" ntsc >/dev/null
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
printf 'Super Mario World (USA).png\thttp://127.0.0.1:%s/Named_Boxarts/Super%%20Mario%%20World%%20%%28USA%%29.png\n' $PORT >"$T/wanted.txt"
python3 - "$SNES9X_HELPER_PORT" "$T/wanted.txt" <<'PY' &
import socket, struct, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', int(sys.argv[1]))); s.listen(4); s.settimeout(60)
text = open(sys.argv[2], 'rb').read()
end = time.time() + 60
while time.time() < end:
    try:
        c, _ = s.accept()
    except OSError:
        break
    req = b''
    while len(req) < 0xA10:
        d = c.recv(0xA10 - len(req))
        if not d: break
        req += d
    if len(req) == 0xA10:
        magic, cmd, pid, ret = struct.unpack_from('<IiiI', req, 0)
        out = bytearray(req)
        if cmd == 6:
            struct.pack_into('<i', out, 12, len(text)); c.sendall(bytes(out) + text)
        elif cmd == 5:
            struct.pack_into('<i', out, 12, 0); out[16:18] = b'ok'; c.sendall(bytes(out))
    c.close()
PY
OLDPID=$!
sleep 0.5
rc=$(OFFLINE= COVER_URL="$URL" run "$T" "0:0;30:$OPTIONS;32:0;40:$CROSS;42:0" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[prefetch\] 1 of 1 fetched' $T/root/logs/boot.log && cmp -s '$T/root/covers/Super Mario World (USA).png' '$SRV/Super Mario World (USA).png'" "an older helper: the cover is prefetched before /data and saved"
kill $OLDPID $SRVPID 2>/dev/null
stop_helpers

echo "== 16. CRT shaders: CRT Easymode style by default, every shader draws, the choice is kept"
QUITAT() { echo "$1:$L3R3;$(($1 + 5)):0;$(($1 + 20)):$UP;$(($1 + 22)):0;$(($1 + 30)):$CROSS;$(($1 + 32)):0"; }
SHADERS=("Off" "CRT Easymode style" "crt-lottes" "crt-lottes-fast" "crt-1tap" "crt-2tap" "crt-hyllian-fast" "crt-nobody" "newpixie-mini" "crt-blurPi-sharp" "crt-blurPi-soft" "monoCRT" "ScaleFX + rAA + AA style")
T=$(newroot t16)
rm -f "$T/root/snes9x-ps5.ini" # a first start: no settings file yet
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 160)" "90,130" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'shader CRT Easymode style' $T/root/logs/boot.log" "a first start draws the game through CRT Easymode style"
expect "python3 - $T/dump/flip00090.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, d[1].split()); px = d[3]
R = lambda x, y: px[(y * w + x) * 3]
col = [R(960, y) for y in range(400, 460)] # down the middle: scanlines
row = [R(x, 540) for x in range(900, 960)] # across: the aperture grille
sys.exit(0 if max(col) > 150 and min(col) < 0.9 * max(col) and len(set(row)) > 1 else 1)
PY" "scanlines and a phosphor mask on the red picture"
expect "python3 -c \"import sys; d=open('$T/dump/flip00130.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3+1] for y in range(520,560)) > 150 else 1)\"" "Cross -> green through the shader"
expect "grep -q 'shader [0-9.]* ms' $T/root/logs/boot.log" "the shader's drawing time is logged"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
for s in $(seq 1 12); do
	T=$(newroot t16s$s)
	echo "shader=$s" >"$T/root/snes9x-ps5.ini"
	python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
	rc=$(run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/test.sfc")
	expect "[ $rc = 0 ] && grep -q 'shader ${SHADERS[$s]} ->' $T/root/logs/boot.log && python3 -c \"import sys; d=open('$T/dump/flip00090.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3] for y in range(500,580)) > 60 else 1)\"" "${SHADERS[$s]} draws the picture"
	expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "${SHADERS[$s]}: no sanitizer reports"
done
# ScaleFX keeps the picture's own colours: a flat red screen stays exactly red (no scanlines, no mask, no blur)
expect "$CHECK $T/../t16s12/dump/flip00090.ppm 960 540 255 0 0 2 >/dev/null && $CHECK $T/../t16s12/dump/flip00090.ppm 250 100 255 0 0 2 >/dev/null" "ScaleFX + rAA + AA style: a flat colour stays exactly that colour"
T=$(newroot t16m)
python3 tests/make_test_rom.py "$T/root/roms/Alpha (USA).sfc" ntsc >/dev/null
# Triangle -> settings, the first row is Shader: Right three times -> crt-lottes-fast; Circle -> back; Options + Cross -> quit
TRIANGLE=1000; RIGHT=20
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;40:$RIGHT;42:0;46:$RIGHT;48:0;52:$RIGHT;54:0;60:$CIRCLE;62:0;70:$OPTIONS;72:0;80:$CROSS;82:0" "50")
expect "grep -q '^shader=3$' $T/root/snes9x-ps5.ini" "the settings screen's Shader row is saved (shader=0 -> 3)"
# in a game: L3 + R3, Down six times (Save, Load, Slot, Cheats, Shortcuts, Shader), Right -> CRT Easymode style
T=$(newroot t16p)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;60:$L3R3;65:0;80:$DOWN;82:0;86:$DOWN;88:0;92:$DOWN;94:0;98:$DOWN;100:0;104:$DOWN;106:0;110:$DOWN;112:0;120:$RIGHT;122:0;130:$CIRCLE;132:0;$(QUITAT 200)" "" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ] && grep -q '^shader=1$' $T/root/snes9x-ps5.ini" "the pause menu's Shader row changes it in the game (saved)"
expect "grep -q 'shader CRT Easymode style' $T/root/logs/boot.log && [ \$(grep -c '\[video\] picture' $T/root/logs/boot.log) -le 4 ]" "the game is drawn through it; the menu doesn't flood boot.log"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 17. MSU-1: a patched ROM plays its CD-quality track (files beside it, or a .msu1 pack); without them, silence"
msu_pcm() { python3 -c "
import struct,sys
pcm=b''.join(struct.pack('<hh',v,v) for v in (4000 if (i//50)%2 else -4000 for i in range(44100)))
open(sys.argv[1],'wb').write(b'MSU1'+struct.pack('<I',0)+pcm)" "$1"; }
msu_run() { # dir -> the last "non-zero samples" count of the host's audio output
	SNES9X_HOST_AUDIO_STATS=1 run "$1" "0:0;300:$L3R3;305:0;320:$UP;322:0;330:$CROSS;332:0" "" "$1/root/roms/Sub/Msu Test.sfc" >/dev/null
	grep '\[host\] audio' "$1/out.txt" | tail -1 | sed 's/.*played, \([0-9]*\) non-zero.*/\1/'
}
T=$(newroot t17)
mkdir -p "$T/root/roms/Sub"
python3 tests/make_test_rom.py "$T/root/roms/Sub/Msu Test.sfc" msu >/dev/null
: >"$T/root/roms/Sub/Msu Test.msu"
msu_pcm "$T/root/roms/Sub/Msu Test-1.pcm"
n=$(msu_run "$T")
expect "grep -q 'Using msu file .*/roms/Sub/Msu Test.msu' $T/out.txt" "the .msu beside the ROM (in a subfolder, a name with a space) is found"
expect "[ \"${n:-0}\" -gt 100000 ]" "track 1 reaches the console's audio output (${n:-0} non-zero samples)"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
T=$(newroot t17z)
mkdir -p "$T/root/roms/Sub"
python3 tests/make_test_rom.py "$T/root/roms/Sub/Msu Test.sfc" msu >/dev/null
msu_pcm "$T/track.pcm"
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w'); z.writestr('Msu Test.msu',b''); z.write(sys.argv[2],'Msu Test-1.pcm'); z.close()" "$T/root/roms/Sub/Msu Test.msu1" "$T/track.pcm"
n=$(msu_run "$T")
expect "[ \"${n:-0}\" -gt 100000 ]" "a .msu1 pack (zip) beside the ROM plays too (${n:-0} non-zero samples)"
T=$(newroot t17n)
mkdir -p "$T/root/roms/Sub"
python3 tests/make_test_rom.py "$T/root/roms/Sub/Msu Test.sfc" msu >/dev/null
n=$(msu_run "$T")
expect "[ \"${n:-1}\" -eq 0 ]" "without the MSU-1 files the same ROM is silent (${n:-?} non-zero samples)"

echo "== 18. debug logs off: nothing written (app and helper), earlier logs kept; the setting switches them at once"
TRIANGLE=1000
SHELFQUIT_AT() { echo "$1:$OPTIONS;$(($1 + 2)):0;$(($1 + 10)):$CROSS;$(($1 + 12)):0"; }
T=$(newroot t18)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
echo "debug_logs=0" >>"$T/root/snes9x-ps5.ini"
mkdir -p "$T/root/logs" && echo "OLD RUN" >"$T/root/logs/boot.log"
rc=$(run "$T" "0:0;100:$L3R3;105:0;120:$UP;122:0;130:$CROSS;132:0" "90" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00090.ppm 960 540 255 0 0 >/dev/null" "the game runs"
expect "[ \"\$(cat $T/root/logs/boot.log)\" = 'OLD RUN' ] && [ ! -e $T/root/logs/boot.prev.log ]" "the earlier boot.log is kept as it was, nothing new written"
expect "! grep -q '^\[snes9x-ps5' $T/out.txt" "nothing on stdout either"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
# Settings (Triangle) -> Debug logs (Up four times from Shader without cheat-download rows) -> Off
T=$(newroot t18b)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;70:$UP;72:0;80:$UP;82:0;100:$CROSS;102:0;120:$CIRCLE;122:0;$(SHELFQUIT_AT 150)" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '^debug_logs=0' $T/root/snes9x-ps5.ini" "Debug logs Off saved"
expect "tail -1 $T/root/logs/boot.log | grep -q 'debug logs turned off'" "the last line of boot.log says the logs were turned off"
# and back on: logging starts again in the same boot.log
T=$(newroot t18c)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
echo "debug_logs=0" >>"$T/root/snes9x-ps5.ini"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;70:$UP;72:0;80:$UP;82:0;100:$CROSS;102:0;120:$CIRCLE;122:0;$(SHELFQUIT_AT 150)" "")
expect "grep -q '^debug_logs=1' $T/root/snes9x-ps5.ini" "Debug logs On saved"
expect "head -1 $T/root/logs/boot.log | grep -q 'debug logs turned on'" "boot.log starts at the switch (nothing from before it)"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
# the helper follows the setting too
stop_helpers
T=$(newroot t18d)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
mkdir -p "$T/hroot" && echo "debug_logs=0" >"$T/hroot/snes9x-ps5.ini"
SNES9X_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
sleep 1.5
rc=$(run "$T" "0:0;30:$L3R3;32:0;40:$UP;42:0;50:$CROSS;52:0" "" "$T/root/roms/test.sfc")
expect "grep -q 'Snes9x helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "the helper still lets the app out"
expect "[ ! -e $T/hroot/logs/helper.log ] && ! grep -q 'helper\]' $T/helper.txt" "with debug logs off the helper writes no log"
stop_helpers

echo "== 19. audit: the helper refuses unknown titles and slow clients; saves and states survive a failed write;"
echo "       Resume doesn't press B; odd library files; 720p edges; Square never loses a cover"
SQUARE=8000; RIGHT=20
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/snes9x-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }
stop_helpers
# a process whose title can't be read is not let out (fail closed)
T=$(newroot t19)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
SNES9X_HOST_JB_TITLE= SNES9X_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "0:0;30:$L3R3;32:0;40:$UP;42:0;50:$CROSS;52:0" "" "$T/root/roms/test.sfc")
expect "grep -q 'title unknown: not Snes9x PS5' $T/hroot/logs/helper.log && ! grep -q 'letting it out' $T/hroot/logs/helper.log" "a process of unknown title is refused (fail closed)"
stop_helpers
# a client sending one byte a second doesn't hold the helper
T=$(newroot t19b)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
SNES9X_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
python3 - "$SNES9X_HELPER_PORT" <<'PY' &
import socket, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
try:
    for i in range(20):
        s.send(b'x'); time.sleep(1)
except OSError:
    pass
PY
DRIP=$!
sleep 0.5
start=$(date +%s)
rc=$(run "$T" "0:0;30:$L3R3;32:0;40:$UP;42:0;50:$CROSS;52:0" "" "$T/root/roms/test.sfc")
secs=$(( $(date +%s) - start ))
expect "grep -q 'Snes9x helper (port [0-9]*): ret 0' $T/root/logs/boot.log && [ $secs -lt 15 ]" "with a slow client connected, the app is still let out at once (${secs}s)"
kill $DRIP 2>/dev/null
stop_helpers
# covers/wanted.txt as a symbolic link: not followed
T=$(newroot t19c)
mkdir -p "$T/hroot/covers"
echo "secret" >"$T/secret.txt"
ln -s "$T/secret.txt" "$T/hroot/covers/wanted.txt"
SNES9X_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
python3 - "$SNES9X_HELPER_PORT" "$T/answer.bin" <<'PY'
import socket, struct, sys
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
req = bytearray(0xA10)
struct.pack_into('<IiI', req, 0, 0xDEADBEEF, 6, 1234)
s.sendall(req)
data = b''
while True:
    d = s.recv(65536)
    if not d: break
    data += d
open(sys.argv[2], 'wb').write(data)
PY
expect "! grep -q secret $T/answer.bin" "a wanted.txt that is a symbolic link is not followed"
stop_helpers
# battery save: written; then a write that fails (full disk: files limited to 1 KiB) leaves it as it was
T=$(newroot t19d)
python3 tests/make_test_rom.py "$T/root/roms/save.sfc" sram >/dev/null
QUIT="0:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0"
rc=$(run "$T" "$QUIT" "" "$T/root/roms/save.sfc")
S=$T/root/saves/save.srm
expect "[ \"\$(stat -c %s $S 2>/dev/null)\" = 2048 ] && [ \"\$(head -c1 $S | od -An -tx1 | tr -d ' ')\" = 42 ] && [ ! -e $S.part ]" "the battery save holds what the game wrote (2 KiB, \$42)"
python3 tests/make_test_rom.py "$T/root/roms/save.sfc" sram2 >/dev/null
rc=$( (ulimit -f 1; trap '' XFSZ; run "$T" "$QUIT" "" "$T/root/roms/save.sfc") )
expect "[ \"\$(stat -c %s $S 2>/dev/null)\" = 2048 ] && [ \"\$(head -c1 $S | od -An -tx1 | tr -d ' ')\" = 42 ]" "a battery save that can't be written whole leaves the old one intact"
expect "[ ! -e $S.part ]" "and no .part is left behind"
# a state: saved; then a failed save keeps the slot's old state
T=$(newroot t19e)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
L2UP=$(printf %x $((0x$L2 | 0x$UP)))
rc=$(run "$T" "0:0;60:$L2UP;62:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0" "" "$T/root/roms/test.sfc")
cp "$T/root/states/test.000" "$T/state.before"
rc=$( (ulimit -f 2; trap '' XFSZ; run "$T" "0:0;90:$L2UP;92:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0" "" "$T/root/roms/test.sfc") )
expect "cmp -s $T/root/states/test.000 $T/state.before && [ ! -e $T/root/states/test.000.part ]" "a state that can't be written whole leaves the slot's old state intact"
rc=$(run "$T" "0:0;60:$(printf %x $((0x$L2 | 0x$DOWN)));62:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0" "" "$T/root/roms/test.sfc")
expect "grep -q 'load state 0 .*: ok' $T/root/logs/boot.log" "and it still loads"
# Resume with Cross: while Cross is still held the game doesn't see B (the picture stays red)
T=$(newroot t19f)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;100:$L3R3;102:0;120:$CROSS;200:0;250:$L3R3;252:0;260:$UP;262:0;270:$CROSS;272:0" "190" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ] && $CHECK $T/dump/flip00190.ppm 960 540 255 0 0 >/dev/null" "Cross held after Resume doesn't reach the game"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
# the library: a FIFO doesn't hang the scan; a zip entry name of 600 characters; deleted ROMs leave the CRC cache
T=$(newroot t19g)
mkfifo "$T/root/roms/pipe.sfc"
python3 tests/make_test_rom.py "$T/root/roms/a.sfc" ntsc >/dev/null
python3 tests/make_test_rom.py "$T/root/roms/b.sfc" pal >/dev/null
python3 - "$T/root/roms/long.zip" "$T/root/roms/a.sfc" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr('x' * 600 + '.txt', b'\0' * 1024)
    z.write(sys.argv[2], 'inner.sfc')
PY
SHELFQUIT="0:0;30:$OPTIONS;32:0;40:$CROSS;42:0"
rc=$(run "$T" "$SHELFQUIT" "")
expect "[ $rc = 0 ] && ! grep -q 'pipe.sfc' $T/root/logs/boot.log" "a FIFO doesn't hang the shelf, and isn't listed"
expect "grep -q 'long.zip ->' $T/root/logs/boot.log" "a zip with a 600-character entry name is still listed"
expect "grep -q '/b.sfc' $T/root/covers/crc-cache.txt" "the CRC cache holds b.sfc"
rm -f "$T/root/roms/b.sfc"
rc=$(run "$T" "$SHELFQUIT" "")
expect "! grep -q '/b.sfc' $T/root/covers/crc-cache.txt" "a deleted ROM leaves the CRC cache"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
# 720p: the picture follows the game to its edges
T=$(newroot t19h)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(SNES9X_HOST_DIRECT_MAX_MIB=12 run "$T" "0:0;100:$CROSS;140:0;160:$L3R3;162:0;170:$UP;172:0;180:$CROSS;182:0" "130" "$T/root/roms/test.sfc")
expect "$CHECK $T/dump/flip00130.ppm 170 360 0 255 0 >/dev/null && $CHECK $T/dump/flip00130.ppm 1110 360 0 255 0 >/dev/null" "720p: Cross turns the picture green to its edges"
# Square (fetch the cover again): offline, or the server without it, the cover stays; with it, it's replaced
T=$(newroot t19i)
SRV=$T/srv/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World (USA).sfc" ntsc >/dev/null
python3 -c "from PIL import Image; Image.new('RGB',(512,357),(0,0,255)).save('$T/root/covers/Super Mario World (USA).png'); Image.new('RGB',(512,357),(255,0,0)).save('$T/red.png')"
cp "$T/root/covers/Super Mario World (USA).png" "$T/blue.png"
SQ="0:0;40:$SQUARE;42:0;200:$OPTIONS;202:0;210:$CROSS;212:0"
rc=$(run "$T" "$SQ" "")
expect "cmp -s '$T/root/covers/Super Mario World (USA).png' $T/blue.png" "offline: Square keeps the cover"
PORT=18093
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png"
rc=$(OFFLINE= COVER_URL="$URL" SNES9X_HOST_REALTIME=1 run "$T" "$SQ" "")
expect "cmp -s '$T/root/covers/Super Mario World (USA).png' $T/blue.png && [ ! -e '$T/root/covers/Super Mario World (USA).refetch' ]" "the server has none (404): the cover stays"
cp "$T/red.png" "$SRV/Super Mario World (USA).png"
rm -f "$T/root/covers/"*.missing
rc=$(OFFLINE= COVER_URL="$URL" SNES9X_HOST_REALTIME=1 run "$T" "$SQ" "")
kill $SRVPID 2>/dev/null
expect "cmp -s '$T/root/covers/Super Mario World (USA).png' '$SRV/Super Mario World (USA).png' && [ ! -e '$T/root/covers/Super Mario World (USA).refetch' ]" "the server has it: the new cover replaces the old one"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 20. the helper's own HTTPS (Mbed TLS): certificate checked, kept connection, chunks, redirects, 404"
stop_helpers20() { pkill -f 'build/host/snes9x-ps5-(installer|helper)' 2>/dev/null; sleep 0.3; }
stop_helpers20
C=$WORK/t20-certs; rm -rf "$C"; mkdir -p "$C"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/ca.key" -out "$C/ca.pem" -days 30 -subj "/CN=Snes9x Test CA" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/srv.key" -out "$C/srv.csr" -subj "/CN=localhost" 2>/dev/null
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' >"$C/ext.cnf"
openssl x509 -req -in "$C/srv.csr" -CA "$C/ca.pem" -CAkey "$C/ca.key" -CAcreateserial -out "$C/srv.pem" -days 30 -extfile "$C/ext.cnf" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/other.key" -out "$C/other.pem" -days 30 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" 2>/dev/null
SRVROOT=$WORK/t20-srv; rm -rf "$SRVROOT"; mkdir -p "$SRVROOT/Repo/Named_Boxarts"
python3 -c "
from PIL import Image
for n,c in [('A b',(255,0,0)),('B',(0,255,0)),('C',(0,0,255))]: Image.new('RGB',(512,357),c).save('$SRVROOT/Repo/Named_Boxarts/'+n+'.png')"
PORT=18443
tls_case() { # name cert key mode host -> $T with the helper's run in it
	T=$(newroot "t20-$1")
	mkdir -p "$T/root/covers"
	python3 tests/https_server.py "$SRVROOT" $PORT "$2" "$3" "$4" 2>"$T/server.txt" &
	local sp=$!
	sleep 0.6
	local u="https://$5:$PORT"
	printf 'A.png\t%s/Repo/Named_Boxarts/A%%20b.png\nB.png\t%s/chunked/Repo/Named_Boxarts/B.png\nC.png\t%s/moved/Repo/Named_Boxarts/C.png\nD.png\t%s/Repo/Named_Boxarts/D.png\n' "$u" "$u" "$u" "$u" >"$T/root/covers/wanted.txt"
	SNES9X_EXTRA_CA=$C/ca.pem SNES9X_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout ${6:-6} "$HELPER" >"$T/helper.txt" 2>&1
	kill $sp 2>/dev/null; wait $sp 2>/dev/null
	stop_helpers20
}
tls_case good "$C/srv.pem" "$C/srv.key" keep localhost
H="$T/root/logs/helper.log"; D="$T/root/covers"; S="$SRVROOT/Repo/Named_Boxarts"
expect "grep -q '\[https\] Mbed TLS 3\.6\.[0-9]*: [0-9]* CA certificate(s), + the extra CA' $H" "Mbed TLS set up with Mozilla's CA list (+ the test CA)"
expect "cmp -s '$D/A.png' '$S/A b.png'" "a cover over HTTPS (Content-Length)"
expect "cmp -s '$D/B.png' '$S/B.png'" "a chunked answer"
expect "cmp -s '$D/C.png' '$S/C.png' && grep -q 'GET /moved/' $T/server.txt" "a redirect followed"
expect "[ -f '$D/D.missing' ] && [ ! -f '$D/D.png' ]" "a 404: marked missing"
expect "[ \$(grep -c 'kept connection' $H) = 3 ]" "one connection for the four requests"
expect "! grep -q '\[http\] ' $H" "the helper doesn't use libSceHttp2"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/helper.txt" "no sanitizer reports"
tls_case untrusted "$C/other.pem" "$C/other.key" keep localhost 4
H="$T/root/logs/helper.log"; D="$T/root/covers"
expect "grep -q 'Certificate verification failed' $H && [ -z \"\$(ls $D 2>/dev/null | grep -v -e wanted.txt -e progress.txt -e priority.txt)\" ]" "a certificate from an unknown CA: refused, nothing saved, nothing marked missing"
tls_case wrongname "$C/srv.pem" "$C/srv.key" keep 127.0.0.1 4
H="$T/root/logs/helper.log"; D="$T/root/covers"
expect "grep -q 'Certificate verification failed' $H && [ -z \"\$(ls $D 2>/dev/null | grep -v -e wanted.txt -e progress.txt -e priority.txt)\" ]" "a certificate for another name: refused"
tls_case closing "$C/srv.pem" "$C/srv.key" drop localhost
D="$T/root/covers"
expect "[ -f '$D/A.png' ] && [ -f '$D/B.png' ] && [ -f '$D/C.png' ]" "a server that closes kept connections: a new one each time"


echo "== 21. manually copied EarthBound (USA).cht loads with EarthBound.zip"
T=$(newroot t21)
mkdir -p "$T/root/cheats"
python3 tests/make_test_rom.py "$T/earthbound.sfc" ntsc >/dev/null
python3 - "$T/earthbound.sfc" "$T/root/roms/EarthBound.zip" <<'PY'
import sys,zipfile
with zipfile.ZipFile(sys.argv[2],"w",zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[1],"EarthBound.sfc")
PY
cat > "$T/root/cheats/EarthBound (USA).cht" <<'CHEATS'
cheats = 2
cheat0_desc = "Infinite Health"
cheat0_code = "8251-57D6"
cheat0_enable = false
cheat1_desc = "Raw Action Replay"
cheat1_code = "7E0DBE63"
cheat1_enable = false
CHEATS
rc=$(run "$T" "0:0;100:$L3R3;102:0;110:$UP;112:0;120:$CROSS;122:0" "" "$T/root/roms/EarthBound.zip")
expect "[ $rc = 0 ]" "EarthBound.zip launched and exited"
expect "grep -q 'Loaded 2 cheats from EarthBound (USA).cht' '$T/root/logs/boot.log'" "manual EarthBound (USA).cht recognized for EarthBound.zip"
expect "! grep -q 'no supported codes' '$T/root/logs/boot.log'" "EarthBound sample uses supported SNES codes"


echo "== 22. previously missing artwork is retried after URL matching improves"
T=$(newroot t22)
mkdir -p "$T/root/covers" "$T/srv/Named_Boxarts"
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World (USA).sfc" ntsc >/dev/null
python3 - "$T/srv/Named_Boxarts/Super Mario World (USA).png" <<'PY'
from PIL import Image
import sys
Image.new('RGB',(512,357),(255,0,0)).save(sys.argv[1])
PY
echo 'https://old.example.invalid/no-such-cover.png' > "$T/root/covers/Super Mario World (USA).missing"
PORT=18086
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png" SNES9X_HOST_REALTIME=1 run "$T" \
    "0:0;330:$OPTIONS;332:0;340:$CROSS;342:0" "")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "game shelf opens even when an old missing-cover marker is present"
expect "[ -f '$T/root/covers/Super Mario World (USA).png' ]" "improved artwork URL bypasses obsolete 30-day 404 marker"


echo "== 23. English-patched ZIPs download matched artwork (real shelf + local server)"
T=$(newroot t23)
mkdir -p "$T/root/roms" "$T/srv/Named_Boxarts"
python3 tests/make_test_rom.py "$T/source.sfc" ntsc >/dev/null
python3 - "$T/source.sfc" "$T/root/roms" "$T/srv/Named_Boxarts" <<'PY'
import sys,zipfile,os
from PIL import Image
aliases = [
    ("Final Fantasy 6 (ENG) # SNES", "Final Fantasy VI (Japan)"),
    ("Dragon-Ball-Z - Super Gokuden 2 (ENG) # SNES", "Dragon Ball Z - Super Gokuu Den - Kakusei Hen (Japan)"),
    ("Dragon-Ball Z - Super Butouden 3 (ENG) # SNES", "Dragon Ball Z - Super Butouden 3 (Japan)"),
    ("Dragon-Ball Z - Super Butouden (ENG) # SNES", "Dragon Ball Z - Super Butouden (Japan)"),
    ("Dragon-Ball Z - Hyper Dimension (ENG) # SNES", "Dragon Ball Z - Hyper Dimension (Japan)"),
    ("Dragon Quest 1 and 2 (ENG) # SNES", "Dragon Quest I _ II (Japan)"),
    ("Dai 3 Ji - Super Robot Taisen (ENG) # SNES", "Dai-3-ji Super Robot Taisen (Japan)"),
    ("Bahamut Lagoon (ENG) # SNES", "Bahamut Lagoon (Japan)"),
    ("Pokemon Gold & Silver", "Pokemon Gold _ Silver"),
    ("Aladdin 2000", "Aladdin 2000"),
]
for idx,(title,cover) in enumerate(aliases):
    with zipfile.ZipFile(os.path.join(sys.argv[2],title+".zip"),"w",zipfile.ZIP_DEFLATED) as z:
        z.write(sys.argv[1],"game.sfc")
    Image.new('RGB',(512,357),(20+idx*18,30,40)).save(os.path.join(sys.argv[3],cover+".png"))
PY
PORT=18087
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png" SNES9X_HOST_REALTIME=1 run "$T" \
    "0:0;780:$OPTIONS;782:0;790:$CROSS;792:0" "")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "all photographed English-patched ZIPs scanned by the shelf"
expect "grep -q '10 ROM(s)' '$T/root/logs/boot.log'" "all ten ZIP titles scanned"
for cover in \
    "Final Fantasy VI (Japan)" \
    "Dragon Ball Z - Super Gokuu Den - Kakusei Hen (Japan)" \
    "Dragon Ball Z - Super Butouden 3 (Japan)" \
    "Dragon Ball Z - Super Butouden (Japan)" \
    "Dragon Ball Z - Hyper Dimension (Japan)" \
    "Dragon Quest I _ II (Japan)" \
    "Dai-3-ji Super Robot Taisen (Japan)" \
    "Bahamut Lagoon (Japan)"; do
    expect "[ -f '$T/root/covers/$cover.png' ]" "matched art downloaded for $cover"
done
expect "[ -f '$T/root/covers/Pokemon Gold _ Silver.png' ]" "Pokemon bootleg gets its own distinct image filename"
expect "[ -f '$T/root/covers/Aladdin 2000.png' ]" "Aladdin bootleg gets its own distinct image filename"

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]

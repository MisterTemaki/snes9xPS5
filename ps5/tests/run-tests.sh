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
	SNES9X_PS5_ROOT=$t/root SNES9X_PS5_HOMEBREW=$t/homebrew SNES9X_HOST_DUMP_DIR=$t/dump SNES9X_HOST_DUMP=$dumps SNES9X_HOST_MAX_FLIPS=${MAXFLIPS-5000} \
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
APP=$T/homebrew/PPSA99922
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
META=$T/appmeta/PPSA99922
mkdir -p "$META" && echo old > "$META/pic0.png" && echo old > "$META/icon0.png" && cp app/sce_sys/param.json "$META/"
echo old > "$APP/sce_sys/pic0.png"
inst "$T"
expect "cmp -s $META/pic0.dds app/sce_sys/pic0.dds && cmp -s $META/pic1.dds app/sce_sys/pic1.dds && cmp -s $META/icon0.png app/sce_sys/icon0.png" "appmeta gets the new background (pic0/pic1.dds) and icon"
expect "[ ! -f $META/pic0.png ] && [ ! -f $APP/sce_sys/pic0.png ]" "the old pic0.png is removed"
expect "grep -q 'Home screen art updated' $T/inst.txt" "the notification says the home screen art changed"
expect "grep -q 'updated to $VER' $T/inst.txt" "update notification"
# a new build is sent over the old one: the same folder is updated, nothing new appears for the home screen
echo older-build > "$APP/eboot.bin"
inst "$T"
expect "cmp -s $APP/eboot.bin tests/fake-eboot.bin && [ \"\$(ls $T/homebrew)\" = PPSA99922 ] && [ \"\$(ls $T/appmeta)\" = PPSA99922 ]" "a new build updates /data/homebrew/PPSA99922 in place: no second title folder"
expect "grep -c 'wrote' $T/root/logs/installer.log | grep -q '^1$' && grep -q 'wrote .*eboot.bin' $T/root/logs/installer.log && ! grep -q 'wrote .*param.json' $T/root/logs/installer.log" "only eboot.bin was rewritten; param.json (title, content ID, version) was left as it is"
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

echo "== 9b. covers for fan translations: the ROM's CRC, its words, and its header each find the Japanese art"
T=$(newroot t9b)
SRV=$T/srv/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
python3 - "$SRV" <<'PY'
import sys
from PIL import Image
for name in ("Dai-4-ji Super Robot Taisen (Japan)", "Super Robot Taisen EX (Japan)", "Zelda no Densetsu - Kamigami no Triforce (Japan)"):
    Image.new('RGB', (512, 357), (255, 0, 0)).save(sys.argv[1] + "/" + name + ".png")
PY
# a file name that says nothing, forged to the CRC the extra table gives to "Super Robot Wars 4 T-Eng (2026)(Aeon Genesis)"
python3 tests/make_test_rom.py "$T/root/roms/game1.sfc" ntsc >/dev/null
python3 tests/forge_crc.py "$T/root/roms/game1.sfc" 72BBACCA >/dev/null
# a translator's name: the same words as the art
python3 tests/make_test_rom.py "$T/root/roms/Super Robot Wars EX (English).sfc" ntsc >/dev/null
# a name that says nothing, a header that does (the title is at $7FC0 of this ROM)
python3 tests/make_test_rom.py "$T/root/roms/mystery.sfc" ntsc >/dev/null
python3 - "$T/root/roms/mystery.sfc" <<'PY'
import sys
path = sys.argv[1]
rom = bytearray(open(path, 'rb').read())
rom[0x7FC0:0x7FD5] = b"ZELDANODENSETSU".ljust(21, b" ")
open(path, 'wb').write(rom)
PY
PORT=18082
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png" SNES9X_HOST_REALTIME=1 run "$T" \
	"0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'game1.sfc -> \"\"' $T/root/logs/boot.log" "game1.sfc: no name from its file name or the No-Intro table"
expect "[ -f '$T/root/covers/Dai-4-ji Super Robot Taisen (Japan).png' ]" "the CRC of a translation finds the Japanese art"
expect "[ -f '$T/root/covers/Super Robot Taisen EX (Japan).png' ]" "Super Robot Wars EX (English) finds its Japanese art by its words"
expect "[ -f '$T/root/covers/Zelda no Densetsu - Kamigami no Triforce (Japan).png' ]" "a header title finds its art"

echo "== 9c. covers a database has and libretro-thumbnails has not: found by the ROM's CRC or by its name's words"
T=$(newroot t9c)
mkdir -p "$T/srv/hasheous" "$T/srv/tgdb" "$T/srv/Named_Boxarts" "$T/root/covers"
python3 - "$T/srv" <<'PY'
import sys
from PIL import Image
# hasheous.org/api/v1/images/<id> has no file extension; TheGamesDB's is <id>-1.jpg
Image.new('RGB', (300, 400), (0, 255, 0)).save(sys.argv[1] + "/hasheous/AF768536EEED4254A29FE3F540CE879677D5FE1D", "PNG")
Image.new('RGB', (300, 400), (0, 0, 255)).save(sys.argv[1] + "/tgdb/27252-1.jpg", "JPEG")
PY
# a name that says nothing, the CRC of a dump the table has (data/snes-db-art.tsv: Aladdin 2000)
python3 tests/make_test_rom.py "$T/root/roms/mystery pirate.sfc" ntsc >/dev/null
python3 tests/forge_crc.py "$T/root/roms/mystery pirate.sfc" 752A25D3 >/dev/null
# the game's words (a database has "King of Fighters 2000, The"); and one no database lists
python3 tests/make_test_rom.py "$T/root/roms/King of Fighters 2000.sfc" ntsc >/dev/null
python3 tests/make_test_rom.py "$T/root/roms/Street Fighter III - Super Version.sfc" ntsc >/dev/null
PORT=18083
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >"$T/srv.log" 2>&1) &
SRVPID=$!
sleep 1
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png" SNES9X_DBART_HASHEOUS="http://127.0.0.1:$PORT/hasheous/" \
	SNES9X_DBART_THEGAMESDB="http://127.0.0.1:$PORT/tgdb/" SNES9X_HOST_REALTIME=1 run "$T" "0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ -n \"\$(find $T/root/covers -name '*.png' -size +100c | grep -e mystery -e Aladdin)\" ]" "the CRC of a dump the table has finds its cover (hasheous.org)"
is_jpeg() { [ "$(head -c 2 "$1" | od -An -tx1 | tr -d ' \n')" = ffd8 ]; }
expect "[ -f '$T/root/covers/King of Fighters 2000.png' ] && is_jpeg '$T/root/covers/King of Fighters 2000.png'" "the words of a name find its cover (TheGamesDB, a JPEG)"
expect "[ ! -e '$T/root/covers/Street Fighter III - Super Version.png' ]" "a game no database lists gets no cover"
expect "! grep '/hasheous/\|/tgdb/' $T/srv.log | grep -v 'AF768536EEED4254A29FE3F540CE879677D5FE1D\|27252-1.jpg' | grep -q ." "and nothing else is asked of either database"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

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
# Settings (Triangle) -> Debug logs (Up six times from Shader: Back, Repair missing covers, Controller shortcuts, Network shares, Updates, Debug logs) -> Off
T=$(newroot t18b)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;70:$UP;72:0;80:$UP;82:0;90:$UP;92:0;100:$UP;102:0;110:$CROSS;112:0;130:$CIRCLE;132:0;$(SHELFQUIT_AT 160)" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '^debug_logs=0' $T/root/snes9x-ps5.ini" "Debug logs Off saved"
expect "tail -1 $T/root/logs/boot.log | grep -q 'debug logs turned off'" "the last line of boot.log says the logs were turned off"
# and back on: logging starts again in the same boot.log
T=$(newroot t18c)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
echo "debug_logs=0" >>"$T/root/snes9x-ps5.ini"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;70:$UP;72:0;80:$UP;82:0;90:$UP;92:0;100:$UP;102:0;110:$CROSS;112:0;130:$CIRCLE;132:0;$(SHELFQUIT_AT 160)" "")
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

echo "== 24. the new title ID (PPSA99922): nothing under the old one (PPSA99009) is touched; our old version is named"
NEWID=PPSA99922; OLDID=PPSA99009
OURS="UP9000-${OLDID}_00-SNES9XPS50000001"; OTHER="UP9000-${OLDID}_00-ANOTHERAPP000001"
stop21() { pkill -f 'build/host/snes9x-ps5-(installer|helper)' 2>/dev/null; sleep 0.3; }
waitfor21() { for i in $(seq 1 80); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
oldapp() { # dir content-id -> an app folder under the old ID: ours (as 2.3 to 2.5 left it) or another app's
	mkdir -p "$1/homebrew/$OLDID/sce_sys" "$1/homebrew/$OLDID/sce_module"
	sed "s/$NEWID/$OLDID/g; s/$OURS/$2/" app/sce_sys/param.json >"$1/homebrew/$OLDID/sce_sys/param.json"
	echo old-eboot >"$1/homebrew/$OLDID/eboot.bin"; echo old-icon >"$1/homebrew/$OLDID/sce_sys/icon0.png"
}
snap() { (cd "$1/homebrew/$OLDID" && find . -type f -exec md5sum {} + | sort); }
inst21() {
	SNES9X_PS5_ROOT=$1/root SNES9X_PS5_HOMEBREW=$1/homebrew SNES9X_PS5_APPMETA=$1/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >>"$1/inst.txt" 2>&1
}
stop21
# a: our old version under the old ID: left exactly as it is, the new one installed beside it, the notification names it
T=$(newroot t21a); oldapp "$T" "$OURS"; mkdir -p "$T/appmeta/$OLDID"; echo old >"$T/appmeta/$OLDID/icon0.png"
snap "$T" >"$T/before.txt"
inst21 "$T" &
expect "waitfor21 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed, then stays as the helper"
expect "cmp -s $T/homebrew/$NEWID/eboot.bin tests/fake-eboot.bin && grep -q '\"titleId\": \"$NEWID\"' $T/homebrew/$NEWID/sce_sys/param.json" "the app installed as $NEWID"
expect "snap $T | cmp -s - $T/before.txt && grep -q old $T/appmeta/$OLDID/icon0.png" "the old folder $OLDID and its home-screen art: untouched"
expect "grep -q 'Its title ID is now $NEWID. The old version ($OLDID) is left alone: to remove it, delete its folder /data/homebrew/$OLDID' $T/inst.txt" "the notification names the old version and how to delete it"
stop21
# b: another app's folder under the old ID: untouched, and not named
T=$(newroot t21b); oldapp "$T" "$OTHER"; snap "$T" >"$T/before.txt"
inst21 "$T" &
expect "waitfor21 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed"
expect "snap $T | cmp -s - $T/before.txt && grep -q 'another app.s folder, left as it is' $T/root/logs/installer.log" "another app's $OLDID folder: untouched"
expect "! grep -q 'title ID is now' $T/inst.txt && cmp -s $T/homebrew/$NEWID/eboot.bin tests/fake-eboot.bin" "nothing said about it; ours installed as $NEWID"
stop21
# c: nothing under the old ID: a plain install
T=$(newroot t21c)
inst21 "$T" &
expect "waitfor21 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed"
expect "[ ! -e $T/homebrew/$OLDID ] && ! grep -q 'title ID is now' $T/inst.txt" "no old version: nothing made or said under $OLDID"
stop21
# d: our other copies under any folder name: two other title IDs, and a second folder for the same title; untouched, named
T=$(newroot t21d)
for id in PPSA99010 PPSA99011; do
	mkdir -p "$T/homebrew/$id/sce_sys"; sed "s/$NEWID/$id/g" app/sce_sys/param.json >"$T/homebrew/$id/sce_sys/param.json"
done
mkdir -p "$T/homebrew/snes9x-copy/sce_sys"; cp app/sce_sys/param.json "$T/homebrew/snes9x-copy/sce_sys/param.json"
snapd() { (cd "$1/homebrew" && find PPSA99010 PPSA99011 snes9x-copy -type f -exec md5sum {} + | sort); }
snapd "$T" >"$T/before.txt"
inst21 "$T" &
expect "waitfor21 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed"
expect "snapd $T | cmp -s - $T/before.txt" "the other copies' folders: untouched"
expect "grep -q 'The old versions (PPSA99010, PPSA99011) are left alone: to remove them, delete their folders /data/homebrew/PPSA99010, /data/homebrew/PPSA99011 (FTP), then their icons' $T/inst.txt" "the notification names both old versions"
expect "grep -q 'Another folder holds this app too (/data/homebrew/snes9x-copy)' $T/inst.txt" "the notification names the second folder for the same title"
expect "! grep -q 'runtime error\|AddressSanitizer' $WORK/t21a/inst.txt $WORK/t21b/inst.txt $WORK/t21c/inst.txt $T/inst.txt" "no sanitizer reports"
stop21

echo "== 25. updates from GitHub's releases (Settings, Updates): the helper checks, downloads, checks every file, installs"
T=$(newroot t22)
UPD="python3 tests/update_client.py $SNES9X_HELPER_PORT"
stop22() { pkill -f 'build/host/snes9x-ps5-(installer|helper)' 2>/dev/null; pkill -f 'http.server 1[89][0-9][0-9][0-9]' 2>/dev/null; sleep 0.3; }
waitfor22() { for i in $(seq 1 80); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop22
SRV=$T/srv; mkdir -p "$SRV" "$T/appmeta/PPSA99922"; echo old >"$T/appmeta/PPSA99922/icon0.png"
SPORT=$((18000 + RANDOM % 1900))
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the release: the installed app with a new eboot, a newer param.json, a new file; the old manifest lists a file that goes
release() { # tag contentVersion [bad]
	rm -rf "$T/rel" && mkdir -p "$T/rel" && cp -r "$T/homebrew/PPSA99922" "$T/rel/PPSA99922" && rm -f "$T/rel/PPSA99922/manifest.sha256" "$T/rel/PPSA99922/sce_sys/old.bin"
	echo "eboot $1" >"$T/rel/PPSA99922/eboot.bin"; echo "new art" >"$T/rel/PPSA99922/sce_sys/icon0.png"; echo extra >"$T/rel/PPSA99922/sce_sys/extra.bin"
	sed -i "s/\"contentVersion\": \"[0-9.]*\"/\"contentVersion\": \"$2\"/" "$T/rel/PPSA99922/sce_sys/param.json"
	python3 tools/make_release_zip.py "$T/rel/PPSA99922" PPSA99922 "$SRV/Snes9xPS5-$1-PPSA99922.zip" >/dev/null
	local sha size
	sha=$(sha256sum "$SRV/Snes9xPS5-$1-PPSA99922.zip" | cut -c1-64); size=$(stat -c %s "$SRV/Snes9xPS5-$1-PPSA99922.zip")
	[ -n "${3:-}" ] && sha=0000${sha:4}
	printf '{"tag_name":"%s","name":"Genesis Plus GX PS5 %s","assets":[{"name":"Snes9xPS5-%s.elf","size":1,"browser_download_url":"http://127.0.0.1:%s/x.elf"},{"name":"Snes9xPS5-%s-PPSA99922.zip","size":%s,"digest":"sha256:%s","browser_download_url":"http://127.0.0.1:%s/Snes9xPS5-%s-PPSA99922.zip"}]}' \
		"$1" "$1" "$1" $SPORT "$1" $size $sha $SPORT "$1" >"$SRV/latest.json"
}
# the app installed by the payload (2.6), which then stays as the helper; the updater asks the test server
SNES9X_UPDATE_API=http://127.0.0.1:$SPORT/latest.json SNES9X_PS5_ROOT=$T/root SNES9X_PS5_HOMEBREW=$T/homebrew SNES9X_PS5_APPMETA=$T/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T/inst.txt" 2>&1 &
expect "waitfor22 $T/root/logs/installer.log 'listening on 127.0.0.1'" "the payload installed 2.6 and serves as the helper"
expect "[ \"\$($UPD status 2.6 | cut -d'|' -f1)\" = idle ]" "the updater answers: idle"
release v2.6 02.060.000
expect "$UPD check 2.6 --until uptodate,failed | grep -q '^uptodate|2.6|'" "the newest release is this version: up to date"
echo gone >"$T/homebrew/PPSA99922/sce_sys/old.bin"
printf '%s *sce_sys/old.bin\n' "$(sha256sum <"$T/homebrew/PPSA99922/sce_sys/old.bin" | cut -c1-64)" >"$T/homebrew/PPSA99922/manifest.sha256"
release v9.9 99.000.000 bad
expect "$UPD check 2.6 --until available,failed | grep -q '^available|9.9|'" "a newer release (9.9): available, with its size"
cp "$T/homebrew/PPSA99922/eboot.bin" "$T/eboot.before"
expect "$UPD install 2.6 --until done,failed | grep -q \"^failed|.*SHA-256 doesn't match\"" "a download whose SHA-256 isn't GitHub's: refused"
expect "cmp -s $T/homebrew/PPSA99922/eboot.bin $T/eboot.before && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "and nothing was changed"
release v9.9 99.000.000
expect "$UPD check 2.6 --until available,failed | grep -q '^available|9.9|'" "checked again"
expect "$UPD install 2.6 --until done,failed | grep -q '^done|9.9|'" "downloaded, every file checked, installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99922/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $T/homebrew/PPSA99922/sce_sys/param.json" "the new eboot.bin and param.json in place"
expect "[ -f $T/homebrew/PPSA99922/sce_sys/extra.bin ] && [ ! -e $T/homebrew/PPSA99922/sce_sys/old.bin ] && grep -q 'sce_sys/extra.bin' $T/homebrew/PPSA99922/manifest.sha256" "a new file added, one the old manifest had removed, the manifest kept"
expect "grep -q 'new art' $T/appmeta/PPSA99922/icon0.png && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "the home screen's copy of the art refreshed; no .new files left"
expect "$UPD status 9.9 | grep -q '^done|'" "the result stays until the app is opened again"
stop22
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the old payload sent again (an autoload): the newer version is left alone
SNES9X_UPDATE_API=http://127.0.0.1:$SPORT/latest.json SNES9X_PS5_ROOT=$T/root SNES9X_PS5_HOMEBREW=$T/homebrew SNES9X_PS5_APPMETA=$T/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T/inst2.txt" 2>&1 &
expect "waitfor22 $T/inst2.txt 'a newer version is installed'" "an older payload sent again: says a newer version is installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99922/eboot.bin && grep -q 'new art' $T/appmeta/PPSA99922/icon0.png" "and leaves the update (and its art) as they are"
expect "$UPD check 9.9 --until uptodate,available,failed | grep -q '^uptodate|9.9|'" "the app says which version it is: an older helper finds it up to date"
stop22
# the app: at start the daily check (the helper finds 9.9); Settings (Triangle), Up five times to "Updates" (Back, Repair missing covers, Controller shortcuts, Network shares) (it shows
# "Install 9.9"), Cross: the helper installs it
T2=$(newroot t22b)
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
SNES9X_UPDATE_API=http://127.0.0.1:$SPORT/latest.json SNES9X_PS5_ROOT=$T2/root SNES9X_PS5_HOMEBREW=$T2/homebrew SNES9X_PS5_APPMETA=$T2/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T2/inst.txt" 2>&1 &
waitfor22 $T2/root/logs/installer.log 'listening on 127.0.0.1'
python3 tests/make_test_rom.py "$T2/root/roms/test.sfc" ntsc >/dev/null
rc=$(SNES9X_HOST_REALTIME=1 SNES9X_PS5_APPMETA=$T2/appmeta run "$T2" "0:0;30:$TRIANGLE;32:0;40:$UP;42:0;46:$UP;48:0;52:$UP;54:0;58:$UP;60:0;64:$UP;66:0;72:$CROSS;74:0;150:$CIRCLE;152:0;170:$OPTIONS;172:0;180:$CROSS;182:0" "70,140")
expect "grep -q 'daily check asked of the helper' $T2/root/logs/boot.log && [ -f $T2/root/update-check.txt ]" "at start: the daily check asked of the helper"
expect "grep -q 'update install' $T2/root/logs/installer.log && grep -q 'eboot v9.9' $T2/homebrew/PPSA99922/eboot.bin" "Cross on \"Install 9.9\": the helper installs it"
expect "grep -q 'is available: Settings, Updates' $T2/inst.txt $T2/root/logs/installer.log" "the daily check found 9.9: a notification says so"
expect "! grep -q 'runtime error\|AddressSanitizer' $T2/out.txt $T2/inst.txt $T/inst.txt $T/inst2.txt" "no sanitizer reports"
stop22

echo "== 26. a big library of unknown ROMs: the scan stays quick (the fuzzy name match looks only at likely names)"
T=$(newroot t26)
python3 - "$T/root/roms" <<'PY'
import os, random, sys
random.seed(7)
words = "Super Mario Zelda Final Fantasy Dragon Quest Kirby Star Metroid Castle Mega Man Street Fighter Donkey Kong Chrono Secret Mana".split()
for i in range(1000):
    with open(os.path.join(sys.argv[1], " ".join(random.sample(words, 3)) + " %04d (Hack) [T+Eng].sfc" % i), "wb") as f:
        f.write(b"\0" * 4096)
PY
rc=$(run "$T" "0:0;100:$OPTIONS;102:0;110:$CROSS;112:0" "")
expect "[ $rc = 0 ] && grep -q '\[games\] 1000 ROM(s)' $T/root/logs/boot.log" "1000 ROMs found"
expect "python3 - $T/root/logs/boot.log <<'PY'
import re, sys
t = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[\s*([0-9.]+)\] \[(emu|games)\] (core ready|1000 ROM)', line)
    if m: t[m.group(2)] = float(m.group(1))
sys.exit(0 if 'emu' in t and 'games' in t and t['games'] - t['emu'] < 3.0 else 1)
PY" "the scan (with a CRC of each) took under 3 seconds"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 27. netfs: share addresses and redaction, routing, the libc --wrap layer, minizip and zlib on a share, the watchdog (in-memory share)"
NETFS_OUT=$(ASAN_OPTIONS=detect_leaks=1 timeout 120 "$PWD/build/host/test-netfs" 2>&1)
NETFS_RC=$?
echo "$NETFS_OUT" | sed 's/^/  /'
expect "[ $NETFS_RC = 0 ]" "netfs: every check passes"
expect "! echo \"\$NETFS_OUT\" | grep -q 'runtime error\|Sanitizer'" "no sanitizer reports"

echo "== 28. netfs end to end: a share in shares.txt, its games on the shelf, a launch and an MSU-1 track over it"
# host/netfs_dirbackend.cpp turns every address in shares.txt into a folder on this machine; the app is the real one.
# The shelf looks for a connected share twice a second of wall time, so these runs are paced (SNES9X_HOST_REALTIME).
net_prepare() { # dir -> the share is <dir>/share, shares.txt names it
	mkdir -p "$1/share/Sub"
	python3 tests/make_test_rom.py "$1/share/Sub/Msu Test.sfc" msu >/dev/null
	: >"$1/share/Sub/Msu Test.msu"
	msu_pcm "$1/share/Sub/Msu Test-1.pcm"
	echo "smb://10.0.0.5/games" >"$1/root/shares.txt"
}
net_play() { # dir -> the last "non-zero samples" count; the game is started from the shelf, no local ROM exists
	SNES9X_HOST_REALTIME=1 MAXFLIPS=900 SNES9X_HOST_NETFS_DIR="$1/share" SNES9X_HOST_AUDIO_STATS=1 run "$1" "0:0;150:$CROSS;152:0;450:$L3R3;455:0;470:$UP;472:0;480:$CROSS;482:0" "" >/dev/null
	grep '\[host\] audio' "$1/out.txt" | tail -1 | sed 's/.*played, \([0-9]*\) non-zero.*/\1/'
}
T=$(newroot t28)
net_prepare "$T"
n=$(net_play "$T")
expect "grep -q '\[net\] 1 share(s) in shares.txt' $T/root/logs/boot.log" "shares.txt was read"
expect "grep -q '\[net\] Connected to smb://10.0.0.5/games' $T/root/logs/boot.log" "the share connected"
expect "grep -q 'Using msu file /smb/10.0.0.5/games/Sub/Msu Test.msu' $T/out.txt" "the game was launched from the share and found its .msu there"
expect "grep -q 'prefetch for Msu Test.sfc: 1/1 track heads' $T/root/logs/boot.log" "the track head was fetched before the first frame"
expect "[ \"${n:-0}\" -gt 100000 ]" "track 1 plays from the share (${n:-0} non-zero samples)"
expect "! grep -q 'audio underrun' $T/root/logs/boot.log" "and without an underrun"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
T=$(newroot t28s)
net_prepare "$T"
n=$(SNES9X_HOST_NETFS_DELAY_MS=200 net_play "$T")
expect "[ \"${n:-0}\" -gt 100000 ]" "the same with 200 ms on every read (${n:-0} non-zero samples)"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
T=$(newroot t28f)
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(SNES9X_HOST_REALTIME=1 SNES9X_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;100:$OPTIONS;102:0;110:$CROSS;112:0" "")
expect "[ $rc = 0 ] && grep -q '\[games\] 0 ROM(s)' $T/root/logs/boot.log" "a share that does not connect leaves the shelf working"
expect "grep -q '\[net\] The folder games is not on 10.0.0.5' $T/root/logs/boot.log" "and says why"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 29. netfs: a ROM on a share with a made-up file name is named by its CRC in the background, and its cover follows"
T=$(newroot t29)
SRV=$T/srv/Named_Boxarts; mkdir -p "$SRV" "$T/share"
python3 - "$SRV" <<'PY'
import sys
from PIL import Image
srv = sys.argv[1]
Image.new('RGB', (512, 357), (0, 255, 0)).save(srv + '/Donkey Kong Country (USA).png')
open(srv + '/Donkey Kong Country (USA) (Rev 2).png', 'w').write('Donkey Kong Country (USA).png')
PY
python3 tests/make_test_rom.py "$T/share/dkc.sfc" ntsc >/dev/null
python3 tests/forge_crc.py "$T/share/dkc.sfc" $(grep -P "\tDonkey Kong Country \(USA\) \(Rev 2\)$" frontend/data/snes-nointro.tsv | cut -f1) >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
PORT=18081
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
OFFLINE= COVER_URL="http://127.0.0.1:$PORT/Named_Boxarts/\${name}.png" SNES9X_HOST_REALTIME=1 MAXFLIPS=420 SNES9X_HOST_NETFS_DIR="$T/share" \
	run "$T" "0:0" "400" >/dev/null
kill $SRVPID 2>/dev/null
expect "grep -q '\[games\] naming 1 network ROM(s) by CRC' $T/root/logs/boot.log" "the pass was started for the ROM the file name could not name"
expect "grep -q 'dkc.sfc -> \"Donkey Kong Country (USA) (Rev 2)\" (by CRC)' $T/root/logs/boot.log" "the next scan names it by its CRC"
expect "grep -q '/smb/10.0.0.5/games/dkc.sfc' $T/root/covers/crc-cache.txt" "and the CRC is kept for the next start"
expect "[ -f '$T/root/covers/Donkey Kong Country (USA) (Rev 2).png' ]" "the cover of the game that came late was downloaded"
expect "$CHECK $T/dump/flip00400.ppm 960 420 0 255 0 >/dev/null" "the shelf shows that cover (green)"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 31. the Network shares screen: add, change, test and remove a share with the on-screen keyboard (Settings, Network shares)"
# tests/osk_pad.py turns the steps into a pad script (the keyboard's layout is its model). Settings is Triangle on the shelf; Up four times from
# Shader reaches Network shares (Back, Repair missing covers, Controller shortcuts, Network shares). Every run ends: Circle twice, OPTIONS, Cross.
net_pad() { python3 tests/osk_pad.py 2>/dev/null; }
net_quit='press CIRCLE 2
press OPTIONS
wait 8
press CROSS'
# a. a new share: server, share, folder with a space, user, a password that needs Shift and both kinds of symbols
T=$(newroot t31)
mkdir -p "$T/share"
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 4
press CROSS
press CROSS
field 10.0.0.5
press DOWN
field games
press DOWN
field Super Nintendo
press DOWN
field jason
press DOWN
field Zq9@pw&1
press DOWN 6
press CROSS
wait 20
$net_quit
STEPS
)
rc=$(SNES9X_HOST_NETFS_DIR="$T/share" run "$T" "$PAD" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(tail -n 1 $T/root/shares.txt)\" = 'smb://jason@10.0.0.5/games/Super%20Nintendo?password=Zq9%40pw%261' ]" "the share is written as one escaped line"
expect "grep -q '^# Snes9x PS5 network shares' $T/root/shares.txt" "after the comments the app wrote"
expect "grep -q '\[net\] shares.txt: saved smb://jason@10.0.0.5/games/Super%20Nintendo' $T/root/logs/boot.log" "the log says what was saved"
expect "grep -q '\[net\] Connected to smb://jason@10.0.0.5/games/Super%20Nintendo' $T/root/logs/boot.log" "and the share connects at once, without a restart"
expect "! grep -rqE 'Zq9|pw%26' $T/root/logs $T/out.txt" "the password is in no log and not on stdout"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
# b. change one share (its note stays), test the connection, remove the other
T=$(newroot t31b)
mkdir -p "$T/share/Sub" && echo x >"$T/share/a.sfc"
printf '# my shares\nsmb://old/x   # my note\nnfs://nas/export/snes\n' >"$T/root/shares.txt"
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 4
press CROSS
press CROSS
press DOWN 3
field bob
press DOWN 6
press CROSS
wait 120
press DOWN
press CROSS
wait 20
press DOWN
press CROSS
press DOWN 7
press CROSS
wait 8
press UP
press CROSS
wait 20
$net_quit
STEPS
)
rc=$(SNES9X_HOST_REALTIME=1 SNES9X_HOST_NETFS_DIR="$T/share" run "$T" "$PAD" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(cat $T/root/shares.txt)\" = \"\$(printf '# my shares\nsmb://bob@old/x   # my note')\" ]" "one line changed (note kept), the other was removed, the comment stays"
expect "grep -q '\[net\] test of smb://bob@old/x: Connected. The folder has 1 file and 1 folder.' $T/root/logs/boot.log" "Test connection reports what is in the folder"
expect "grep -q '\[net\] shares.txt: removed nfs://nas/export/snes' $T/root/logs/boot.log" "the removal is logged"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
# c. Save with nothing filled in changes nothing
T=$(newroot t31c)
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 4
press CROSS
press CROSS
press DOWN 10
press CROSS
wait 8
press CIRCLE
$net_quit
STEPS
)
rc=$(run "$T" "$PAD" "")
expect "[ $rc = 0 ] && ! grep -q '^smb://\|^nfs://' $T/root/shares.txt" "an empty form is not saved (and Circle on a form with no changes leaves it at once)"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 32. shelf tabs: Internal and Network apart, L1 / R1 between them, the last tab kept, Square on Network scans again"
# The tab bar sits at the top: the open tab's pill is the accent colour (78,62,170), the other one dark (32,30,58); (795,60) is
# inside the Internal pill and (1120,60) inside the Network pill (no text there). The share connects within a few seconds of wall time.
L1=400; R1=800; SQUARE=8000
tab_pills() { # dump the active tab: Internal | Network
	local f=$1 on=$2
	if [ "$on" = Internal ]; then
		$CHECK "$f" 795 60 78 62 170 >/dev/null && $CHECK "$f" 1120 60 32 30 58 >/dev/null
	else
		$CHECK "$f" 795 60 32 30 58 >/dev/null && $CHECK "$f" 1120 60 78 62 170 >/dev/null
	fi
}
T=$(newroot t32)
mkdir -p "$T/share"
python3 tests/make_test_rom.py "$T/root/roms/Local Game (USA).sfc" ntsc >/dev/null
python3 tests/make_test_rom.py "$T/share/Net Game (USA).sfc" ntsc >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(SNES9X_HOST_REALTIME=1 SNES9X_HOST_NETFS_DIR="$T/share" run "$T" "0:0;220:$R1;222:0;300:$SQUARE;302:0;380:$L1;382:0;420:$OPTIONS;422:0;430:$CROSS;432:0" "200,260,400")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "tab_pills $T/dump/flip00200.ppm Internal" "the shelf opens on the Internal tab"
expect "tab_pills $T/dump/flip00260.ppm Network" "R1 goes to the Network tab"
expect "tab_pills $T/dump/flip00400.ppm Internal" "L1 comes back"
expect "grep -q '\[shelf\] tab Network: 1 game' $T/root/logs/boot.log" "the Network tab holds the game on the share"
expect "grep -q '\[shelf\] tab Internal: 1 game' $T/root/logs/boot.log" "and the Internal tab only the local one"
expect "grep -q '\[shelf\] network tab: scan again\$' $T/root/logs/boot.log" "Square on the Network tab scans again"
expect "[ \$(grep -c '\[games\] 2 ROM' $T/root/logs/boot.log) -ge 2 ]" "and the scan found both games again"
expect "grep -q 'last_tab=0' $T/root/snes9x-ps5.ini" "the tab left on is kept (Internal)"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
T=$(newroot t32b)
mkdir -p "$T/share"
python3 tests/make_test_rom.py "$T/root/roms/Local Game (USA).sfc" ntsc >/dev/null
python3 tests/make_test_rom.py "$T/share/Net Game (USA).sfc" ntsc >/dev/null
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
echo "last_tab=2" >>"$T/root/snes9x-ps5.ini"
rc=$(SNES9X_HOST_REALTIME=1 SNES9X_HOST_NETFS_DIR="$T/share" run "$T" "0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "40,250")
expect "[ $rc = 0 ] && tab_pills $T/dump/flip00040.ppm Network" "with last_tab=2 and a share set up the shelf opens on Network, before the share is up"
expect "tab_pills $T/dump/flip00250.ppm Network" "and stays there when its games arrive"
expect "grep -q 'last_tab=2' $T/root/snes9x-ps5.ini" "and keeps the tab on quit"
T=$(newroot t32c)
python3 tests/make_test_rom.py "$T/root/roms/Local Game (USA).sfc" ntsc >/dev/null
echo "last_tab=2" >>"$T/root/snes9x-ps5.ini"
rc=$(run "$T" "0:0;60:$R1;62:0;70:$SQUARE;72:0;90:$L1;92:0;120:$OPTIONS;122:0;130:$CROSS;132:0" "40,80,110")
expect "[ $rc = 0 ] && tab_pills $T/dump/flip00040.ppm Internal" "last_tab=2 without a share set up opens on the games instead"
expect "tab_pills $T/dump/flip00080.ppm Network && grep -q '\[shelf\] tab Network: 0 game' $T/root/logs/boot.log" "R1 shows the empty Network tab"
expect "grep -q '\[shelf\] network tab: scan again\$' $T/root/logs/boot.log" "Square on it scans again, with no share to ask"
expect "tab_pills $T/dump/flip00110.ppm Internal && grep -q 'last_tab=0' $T/root/snes9x-ps5.ini" "L1 goes back and the tab is kept"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
T=$(newroot t32d)
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(SNES9X_HOST_REALTIME=1 SNES9X_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;150:$SQUARE;152:0;330:$OPTIONS;332:0;340:$CROSS;342:0" "100")
expect "[ $rc = 0 ] && tab_pills $T/dump/flip00100.ppm Network" "a share that is down leaves a shelf with no games on the Network tab"
expect "grep -q '\[shelf\] network tab: scan again, reconnecting' $T/root/logs/boot.log" "Square connects it again"
expect "[ \$(grep -c 'The folder games is not on 10.0.0.5' $T/root/logs/boot.log) -ge 2 ]" "and says why each time"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 33. cheats follow their file: a code added while the game runs is applied at once, with no restart"
# the game keeps running (paced, SNES9X_HOST_REALTIME) while this script writes <rom>.cht as an FTP upload would: all of
# it at once. The cheats write to the battery RAM ($70:0000, 1, 2), which the exit saves, so the .srm shows what ran.
T=$(newroot t33)
python3 tests/make_test_rom.py "$T/root/roms/save.sfc" sram >/dev/null # writes $42 to $70:0000 at boot
mkdir -p "$T/root/cheats"
CHT=$T/root/cheats/save.cht
putcht() { cat >"$T/new.cht" && mv "$T/new.cht" "$CHT"; }
waitlog() { for i in $(seq 1 300); do grep -q "$1" "$T/root/logs/boot.log" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
cht() { printf 'cheat\n  name: %s\n  code: %s\n  enable\n\n' "$1" "$2"; }
L2R3=104
( SNES9X_HOST_REALTIME=1 MAXFLIPS=2000 run "$T" "0:0;450:$L2R3;453:0;700:$CIRCLE;703:0;780:$L3R3;785:0;800:$UP;802:0;810:$CROSS;812:0" "" "$T/root/roms/save.sfc" >"$T/rc.txt" ) &
GAME=$!
waitlog '\[emu\] frame 60:'
expect "! grep -q '\[cheats\]' $T/root/logs/boot.log" "the game started with no cheat file"
cht "Byte 0" "700000=99" | putcht
waitlog 'reloaded: 1 cheat groups'
expect "grep -q 'reloaded: 1 cheat groups from save.cht' $T/root/logs/boot.log" "a cheat file that appeared while the game runs was loaded"
{ cht "Byte 0" "700000=99"; cht "Byte 1" "700001=77"; } | putcht
waitlog 'reloaded: 2 cheat groups'
expect "grep -q 'reloaded: 2 cheat groups from save.cht' $T/root/logs/boot.log" "a cheat added to the file was loaded"
echo "this is not a cheat file" | putcht
waitlog 'has no usable cheat'
expect "grep -q 'save.cht has no usable cheat: the 2 that run stay' $T/root/logs/boot.log" "a file with no usable cheat leaves the running cheats alone"
{ cht "Byte 0" "700000=99"; cht "Byte 1" "700001=77"; cht "Byte 2" "700002=55"; } | putcht
waitlog 'reloaded: 3 cheat groups'
expect "grep -q 'reloaded: 3 cheat groups from save.cht' $T/root/logs/boot.log" "and the file written next is loaded too"
wait $GAME
rc=$(cat "$T/rc.txt")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(head -c3 $T/root/saves/save.srm | od -An -tx1 | tr -d ' ')\" = 997755 ]" "the three cheats ran in the game (battery RAM \$99 \$77 \$55)"
expect "grep -q 'name: Byte 2' $CHT" "the cheat file still holds all three after the game closed"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 34. border packs: a game finds its border (own file name, the cover matcher), then default.png, then the built-in frame"
T=$(newroot t34)
B=$T/root/borders
python3 - "$T" <<'PY'
import sys
from PIL import Image
root = sys.argv[1] + "/root"
def border(path, win=(261, 21, 1659, 1059)):
    im = Image.new("RGBA", (1920, 1080), (0, 0, 255, 255))
    im.paste((0, 0, 0, 0), win)
    im.save(path)
import os
os.makedirs(root + "/borders/games", exist_ok=True)
border(root + "/borders/games/Super Mario World (USA).png")
border(root + "/borders/games/Super Robot Taisen EX (Japan).png", (300, 60, 1600, 1000))
border(root + "/borders/default.png", (200, 40, 1700, 1040))
PY
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World.sfc" ntsc >/dev/null
python3 tests/make_test_rom.py "$T/root/roms/Super Robot Wars EX (English).sfc" ntsc >/dev/null
python3 tests/make_test_rom.py "$T/root/roms/Unknown Game.sfc" ntsc >/dev/null
bordercase() { # ROM file name: starts the game and quits from the pause menu
	run "$T" "0:0;60:$L3R3;62:0;70:$UP;72:0;80:$CROSS;82:0" "" "$T/root/roms/$1"
}
expect "[ -d $B/games ]" "the app creates /data/snes9x/borders/games at boot"
rc=$(bordercase "Super Mario World.sfc")
expect "[ $rc = 0 ] && ! grep -q '\[border\]' $T/root/logs/boot.log" "Border Off (the default): the pack is not read, nothing is logged"
echo "border=1" >>"$T/root/snes9x-ps5.ini"
rc=$(bordercase "Super Mario World.sfc")
expect "[ $rc = 0 ]" "Auto: exit code 0 (got $rc)"
expect "grep -q '\[border\] Super Mario World -> Super Mario World (USA).png (matched): window 261,21 1398x1038' $T/root/logs/boot.log" "a ROM named loosely finds its game's border through the cover matcher"
rc=$(bordercase "Super Robot Wars EX (English).sfc")
expect "grep -q '\[border\] Super Robot Wars EX (English) -> Super Robot Taisen EX (Japan).png (matched): window 300,60 1300x940' $T/root/logs/boot.log" "a translation finds the Japanese game's border by its words"
rc=$(bordercase "Unknown Game.sfc")
expect "grep -q '\[border\] Unknown Game -> default.png (default.png): window 200,40 1500x1000' $T/root/logs/boot.log" "a game with no border of its own gets default.png"
cp "$B/games/Super Mario World (USA).png" "$B/games/Super Mario World.png"
echo "window=100,100,900,600" >"$B/games/Super Mario World.ini"
rc=$(bordercase "Super Mario World.sfc")
expect "grep -q '\[border\] Super Mario World -> Super Mario World.png (own file): window 100,100 900x600' $T/root/logs/boot.log" "games/<ROM name>.png wins, and its .ini gives the window"
echo "off=1" >"$B/games/Super Mario World.ini"
rc=$(bordercase "Super Mario World.sfc")
expect "grep -q '\[border\] Super Mario World: none, an .ini says off=1' $T/root/logs/boot.log" "off=1 in the .ini: no border for that game"
rm "$B/games/Super Mario World.ini" "$B/games/Super Mario World.png"
echo "garbage" >"$B/games/Super Mario World (USA).png"
rc=$(bordercase "Super Mario World.sfc")
expect "grep -q '\[border\] games/Super Mario World (USA).png: not a PNG' $T/root/logs/boot.log && grep -q 'Super Mario World -> default.png (default.png)' $T/root/logs/boot.log" "a broken file is logged and skipped: default.png"
rm "$B/default.png"
rc=$(bordercase "Unknown Game.sfc")
expect "[ $rc = 0 ] && grep -q '\[border\] Unknown Game -> built-in (built-in): window ' $T/root/logs/boot.log" "no default.png: the frame built into the app"
sed -i 's/^border=1/border=2/' "$T/root/snes9x-ps5.ini"
cp "$T/root/borders/games/Super Robot Taisen EX (Japan).png" "$B/default.png"
rc=$(bordercase "Super Robot Wars EX (English).sfc")
expect "grep -q '\[border\] Super Robot Wars EX (English) -> default.png (default.png)' $T/root/logs/boot.log" "Default frame: the game's own border is not looked for"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 35. borders: Off changes nothing; Auto draws the game's border around the picture (the pause menu darkens both); Default frame is built in; the Border row"
# A border is a 1920x1080 PNG with a clear window (261,21 1398x1038, the picture fills it). The test one is blue with a clear window; the
# test ROM's picture is red. The pause menu darkens the whole surface twice (a quarter of each colour: red 255 -> 63).
LEFT=80
mkborder() { # file: a blue border with a clear window, named for the game it is for
	mkdir -p "$(dirname "$1")"
	python3 -c "
import sys
from PIL import Image
im = Image.new('RGBA', (1920, 1080), (0, 0, 255, 255))
im.paste((0, 0, 0, 0), (261, 21, 1659, 1059))
im.save(sys.argv[1])" "$1"
}
border_around() { # dump: blue outside the window, the picture inside it, all the way to its edges
	local f=$1
	$CHECK "$f" 100 540 0 0 255 >/dev/null && $CHECK "$f" 250 540 0 0 255 >/dev/null && $CHECK "$f" 270 540 255 0 0 >/dev/null \
		&& $CHECK "$f" 1650 540 255 0 0 >/dev/null && $CHECK "$f" 1665 540 0 0 255 >/dev/null \
		&& $CHECK "$f" 960 15 0 0 255 >/dev/null && $CHECK "$f" 960 25 255 0 0 >/dev/null \
		&& $CHECK "$f" 960 1050 255 0 0 >/dev/null && $CHECK "$f" 960 1065 0 0 255 >/dev/null
}
# a. Border Off (the default): a border in the pack is not drawn; the picture is the 4:3 one of group 1
T=$(newroot t35a)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
mkborder "$T/root/borders/games/test.png"
rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00090.ppm 100 540 0 0 0 >/dev/null && $CHECK $T/dump/flip00090.ppm 245 540 255 0 0 >/dev/null" "Border Off: black bars, the 4:3 picture from x=240, whatever is in borders/"
expect "! grep -q 'border' $T/root/logs/boot.log" "Border Off: nothing about borders in boot.log"
# b. Auto: the game's own file (test.png for test.sfc); the pause menu darkens it with the picture; Resume restores it
T=$(newroot t35b)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
mkborder "$T/root/borders/games/test.png"
echo "border=1" >>"$T/root/snes9x-ps5.ini"
rc=$(run "$T" "0:0;60:$L3R3;65:0;100:$CIRCLE;102:0;$(QUITAT 160)" "50,90,130" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "border_around $T/dump/flip00050.ppm" "Border Auto: the game's border outside the window, the picture inside it, edge to edge"
expect "$CHECK $T/dump/flip00090.ppm 100 100 0 0 63 >/dev/null && $CHECK $T/dump/flip00090.ppm 300 100 63 0 0 >/dev/null" "the pause menu darkens the border and the picture together"
expect "border_around $T/dump/flip00130.ppm" "Resume: the border and the picture are back, undarkened"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
# c. Default frame: the built-in one, even with a border of the game's own in the pack; Auto with nothing in the pack falls back to it
def_frame=$(python3 -c "
from PIL import Image
r, g, b, a = Image.open('frontend/assets/borders/default.png').convert('RGBA').getpixel((30, 540))
print(r * a // 255, g * a // 255, b * a // 255)")
for mode in "2 with" "1 without"; do
	set -- $mode
	T=$(newroot t35c$1)
	python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
	[ "$2" = with ] && mkborder "$T/root/borders/games/test.png"
	echo "border=$1" >>"$T/root/snes9x-ps5.ini"
	rc=$(run "$T" "0:0;$(QUITAT 120)" "90" "$T/root/roms/test.sfc")
	expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 30 540 $def_frame 2 >/dev/null && $CHECK $T/dump/flip00090.ppm 960 540 255 0 0 >/dev/null" "border=$1, $2 a border of the game's own: the built-in frame around the picture"
done
# d. the Settings row on the shelf: Triangle, Down twice (Shader, Aspect ratio, Border), Right twice: Off -> Auto -> Default frame; Right wraps to Off
T=$(newroot t35d)
python3 tests/make_test_rom.py "$T/root/roms/Alpha (USA).sfc" ntsc >/dev/null
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;40:$DOWN;42:0;46:$DOWN;48:0;52:$RIGHT;54:0;58:$RIGHT;60:0;66:$CIRCLE;68:0;76:$OPTIONS;78:0;86:$CROSS;88:0" "")
expect "[ $rc = 0 ] && grep -q '^border=2$' $T/root/snes9x-ps5.ini" "Settings > Border: Right twice is Default frame (border=2)"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;40:$DOWN;42:0;46:$DOWN;48:0;52:$RIGHT;54:0;60:$CIRCLE;62:0;70:$OPTIONS;72:0;80:$CROSS;82:0" "")
expect "[ $rc = 0 ] && grep -q '^border=0$' $T/root/snes9x-ps5.ini" "Right again wraps round to Off (border=0)"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;40:$DOWN;42:0;46:$DOWN;48:0;52:$LEFT;54:0;60:$CIRCLE;62:0;70:$OPTIONS;72:0;80:$CROSS;82:0" "")
expect "[ $rc = 0 ] && grep -q '^border=2$' $T/root/snes9x-ps5.ini" "and Left from Off wraps to Default frame (border=2)"
# e. the pause menu has the row too (Resume, Save, Load, Slot, Cheats, Shortcuts, Shader, Aspect ratio, Border: Down eight times) and it works at once
T=$(newroot t35e)
python3 tests/make_test_rom.py "$T/root/roms/test.sfc" ntsc >/dev/null
mkborder "$T/root/borders/games/test.png"
pad="0:0;60:$L3R3;65:0"; t=80
for i in 1 2 3 4 5 6 7 8; do pad="$pad;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
rc=$(run "$T" "$pad;$((t + 6)):$RIGHT;$((t + 8)):0;$((t + 16)):$CIRCLE;$((t + 18)):0;$(QUITAT $((t + 70)))" "50,$((t + 60))" "$T/root/roms/test.sfc")
expect "[ $rc = 0 ] && grep -q '^border=1$' $T/root/snes9x-ps5.ini" "the pause menu's Border row sets Auto (border=1, saved)"
expect "$CHECK $T/dump/flip00050.ppm 100 540 0 0 0 >/dev/null" "before: no border"
expect "border_around $T/dump/flip$(printf %05d $((t + 60))).ppm" "after Resume the game's border is there, without restarting the game"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo "== 36. border downloads: Auto asks the helper for the game's border, it lands in borders/games and goes up while the game runs; Settings, Download all borders queues the library"
# The Bezel Project's SNES picture for the game is served by a local HTTP server (SNES9X_BORDER_URL moves the app's base address there; the
# helper just follows the address in borders/priority.txt). The first flips show the built-in frame, the later ones the downloaded border.
stop_helpers
T=$(newroot t36a)
SRV=$T/srv/GameBezels
mkborder "$SRV/Super Mario World (USA).png"
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World.sfc" ntsc >/dev/null
echo "border=1" >>"$T/root/snes9x-ps5.ini"
PORT=18136
SERVER_DELAY=1 python3 tests/https_server.py "$T/srv" $PORT >/dev/null 2>&1 &
SRVPID=$!
SNES9X_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/root/logs/helper.log" "listening" || sleep 1
export SNES9X_BORDER_URL="http://127.0.0.1:$PORT/GameBezels/"
rc=$(OFFLINE= SNES9X_HOST_REALTIME=1 run "$T" "0:0;$(QUITAT 400)" "30,380" "$T/root/roms/Super Mario World.sfc")
unset SNES9X_BORDER_URL
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[border\] Super Mario World: asking the helper for Super Mario World (USA).png' $T/root/logs/boot.log" "Auto, no border in the pack: the app asks the helper for The Bezel Project's name for the game"
expect "grep -q 'Super Mario World (USA).png' $T/root/borders/priority.txt" "borders/priority.txt names it"
expect "grep -q '\[borders\] Super Mario World (USA).png -> 200' $T/root/logs/helper.log && cmp -s '$T/root/borders/games/Super Mario World (USA).png' '$SRV/Super Mario World (USA).png'" "the helper downloaded it into borders/games/"
expect "$CHECK $T/dump/flip00030.ppm 30 540 $def_frame 2 >/dev/null" "the game started with the built-in frame"
expect "grep -q '\[border\] Super Mario World -> Super Mario World (USA).png (downloaded): window 261,21 1398x1038' $T/root/logs/boot.log" "the downloaded border is loaded while the game runs"
expect "border_around $T/dump/flip00380.ppm" "and drawn around the picture"
expect "grep -q '^0 1 idle' $T/root/borders/progress.txt" "borders/progress.txt: nothing left, 1 fetched"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"
kill $SRVPID 2>/dev/null
stop_helpers
# the next start: the file is in the pack, found at once, the helper is not asked
rm -f "$T/root/borders/priority.txt"
rc=$(run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/Super Mario World.sfc")
expect "[ $rc = 0 ] && grep -q '\[border\] Super Mario World -> Super Mario World (USA).png (matched)' $T/root/logs/boot.log && ! grep -q 'asking the helper' $T/root/logs/boot.log && [ ! -e $T/root/borders/priority.txt ]" "the next start finds the downloaded border in the pack and asks for nothing"
# border_download=0: Auto never asks
T=$(newroot t36b)
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World.sfc" ntsc >/dev/null
printf 'border=1\nborder_download=0\n' >>"$T/root/snes9x-ps5.ini"
rc=$(run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/Super Mario World.sfc")
expect "[ $rc = 0 ] && ! grep -q 'asking the helper' $T/root/logs/boot.log && [ ! -e $T/root/borders/priority.txt ] && grep -q '^border_download=0$' $T/root/snes9x-ps5.ini" "border_download=0: the helper is not asked (and the setting is kept)"
# Settings > Download all borders (the row before Back: Up twice from the top), with two ROMs of one game and one without a border
T=$(newroot t36c)
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World (USA).sfc" ntsc >/dev/null
python3 tests/make_test_rom.py "$T/root/roms/Super Mario World (Europe).sfc" pal >/dev/null
python3 tests/make_test_rom.py "$T/root/roms/Totally Unknown Homebrew.sfc" ntsc >/dev/null
export SNES9X_BORDER_URL="http://127.0.0.1:$PORT/GameBezels/"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;40:$UP;42:0;46:$UP;48:0;52:$CROSS;54:0;66:$CIRCLE;68:0;76:$OPTIONS;78:0;86:$CROSS;88:0" "")
unset SNES9X_BORDER_URL
expect "[ $rc = 0 ] && grep -q '\[border\] user asked for the borders of the library: 1 to download' $T/root/logs/boot.log" "Download all borders: one picture for the library (two ROMs share it, the unknown one has none)"
expect "[ \$(wc -l <$T/root/borders/wanted.txt) = 1 ] && grep -q '^Super Mario World (USA).png.http://127.0.0.1:$PORT/GameBezels/Super%20Mario%20World%20%28USA%29.png\$' $T/root/borders/wanted.txt" "borders/wanted.txt lists it for the helper"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "no sanitizer reports"

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]

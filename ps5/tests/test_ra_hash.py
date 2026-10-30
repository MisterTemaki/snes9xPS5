#!/usr/bin/env python3
"""test_ra_hash.py -- the RetroAchievements hash of SNES games in every container the app opens (2.11).

    test_ra_hash.py <test-ra-hash binary> <work dir>

RetroAchievements' documented rule for the SNES (docs.retroachievements.org, Game Identification; rcheevos's
rc_hash_snes): the MD5 of the ROM file, leaving out the first 512 bytes when the size is a multiple of 8 KiB plus 512 (a
copier header). The expected hashes here follow that rule with Python's hashlib, independently of rcheevos; the files
are packed the way players have them (.sfc, a headered .smc, .zip, .7z, .gz, an MSU-1 .msu1 pack) and the binary hashes
them through the app's own code (frontend/fe_rahash.cpp + rcheevos).
"""
import gzip
import hashlib
import os
import random
import subprocess
import sys
import zipfile

binary, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
rnd = random.Random(2011)
passed = failed = 0


def ra_hash(data):
    if len(data) % 8192 == 512:
        data = data[512:]
    return hashlib.md5(data).hexdigest()


def check(name, path, expect):
    global passed, failed
    out = subprocess.run([binary, path], capture_output=True, text=True).stdout.strip()
    got = out.split(" ", 1)[0]
    ok = (got == expect) if expect else got == "FAIL"
    print("  %s: %s (%s)" % ("ok" if ok else "FAIL", name, out if not ok else got))
    if ok:
        passed += 1
    else:
        failed += 1


def write(name, data):
    p = os.path.join(work, name)
    with open(p, "wb") as f:
        f.write(data)
    return p


rom = bytes(rnd.getrandbits(8) for _ in range(256 * 1024))  # a 2 Mbit ROM
header = bytes([0x80, 0x00]) + bytes(510)  # a copier (SMC) header
odd = bytes(rnd.getrandbits(8) for _ in range(0x8000 + 1024))  # 1 KiB over: not a header by RA's rule
hirom = bytes(rnd.getrandbits(8) for _ in range(3 * 1024 * 1024))  # a 24 Mbit ROM, not a power of two

expect = ra_hash(rom)
check("an unheadered .sfc: the MD5 of the file", write("game.sfc", rom), expect)
check("a headered .smc: the 512-byte header left out", write("game.smc", header + rom), expect)
check("the header rule is RA's (a multiple of 8 KiB + 512), not a guess: 1 KiB over is hashed whole",
      write("odd.sfc", odd), hashlib.md5(odd).hexdigest())
check("a 3 MiB ROM", write("big.sfc", hirom), hashlib.md5(hirom).hexdigest())
check("a 3 MiB ROM with a header", write("bigh.smc", header + hirom), hashlib.md5(hirom).hexdigest())

z = os.path.join(work, "game.zip")
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as f:
    f.writestr("readme.txt", "a readme beside the ROM")
    f.writestr("Game (USA).smc", header + rom)
check("a .zip: the ROM inside (header left out), not the archive", z, expect)
z2 = os.path.join(work, "two.zip")
with zipfile.ZipFile(z2, "w", zipfile.ZIP_STORED) as f:
    f.writestr("small.sfc", rom[:0x8000])
    f.writestr("Game.sfc", rom)
check("a .zip with two files: the biggest, as Snes9x plays it", z2, expect)

s7 = os.path.join(work, "game.7z")
src = write("in7z.smc", header + rom)
subprocess.run([sys.executable, os.path.join(here, "make_7z.py"), s7, "Game (USA).smc=" + src, "notes.txt=" + write("notes.txt", b"x" * 100)],
               check=True, capture_output=True)
check("a .7z: the ROM inside, unpacked (header left out)", s7, expect)
s7c = os.path.join(work, "stored.7z")
subprocess.run([sys.executable, os.path.join(here, "make_7z.py"), "--copy", s7c, "Game.sfc=" + write("in7z.sfc", rom)],
               check=True, capture_output=True)
check("a stored .7z", s7c, expect)

gz = os.path.join(work, "game.sfc.gz")
with gzip.open(gz, "wb") as f:
    f.write(header + rom)
check("a .gz: unpacked first", gz, expect)

msu = os.path.join(work, "pack.msu1")
with zipfile.ZipFile(msu, "w", zipfile.ZIP_STORED) as f:
    f.writestr("program.rom", rom)
    f.writestr("track-1.pcm", b"MSU1" + bytes(2000))
check("an MSU-1 pack (.msu1 with program.rom): the ROM in it", msu, expect)
nomsu = os.path.join(work, "tracks.msu1")
with zipfile.ZipFile(nomsu, "w", zipfile.ZIP_STORED) as f:
    f.writestr("track-1.pcm", b"MSU1" + bytes(4000))
check("a .msu1 pack of tracks only: no ROM, no hash", nomsu, None)
check("a file that isn't there: no hash", os.path.join(work, "missing.sfc"), None)

print("ra hash: %d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)

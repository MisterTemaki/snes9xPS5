#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/snes-extra-crc.tsv: the CRC32 of SNES dumps that No-Intro does not list (fan translations,
hacks, other dump sets) -> the name that dump goes by.

The source is Hasheous' public SNES dump (no account, no key), the same data its hash lookups answer from:
  https://hasheous.org/api/v1/Dumps/platforms/Nintendo%20Super%20Nintendo%20Entertainment%20System.zip
One JSON file per game; the ROMs are in its "ROMs" attribute with name, size and hashes.

Kept: a ROM with a CRC32 and a size, not an archive (the CRC of a .zip is not the ROM's), not behind a copier header
(the app takes the CRC without it), and not already in snes-nointro.tsv. The name is the ROM's own, minus its
extension: the app only feeds it to the art matcher (fe_artmatch.h), which strips tags and translation marks.

  python3 tools/make_extra_crc.py [--zip dump.zip] [--date 2026-10-17]   (from ps5/)
"""
import argparse
import io
import json
import re
import sys
import urllib.request
import zipfile

DUMP = "https://hasheous.org/api/v1/Dumps/platforms/Nintendo%20Super%20Nintendo%20Entertainment%20System.zip"
BAD_DUMP = re.compile(r"\[[abfhopt]\d*[A-Za-z]?\]")  # GoodTools flags: bad dump, overdump, fixed, hack, pirate, trained
ARCHIVE = re.compile(r"\.(zip|7z|rar|gz)$", re.I)
ROM_EXT = re.compile(r"\.(sfc|smc|swc|fig|bs|st|bin|mgd)$", re.I)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", help="a dump already downloaded")
    ap.add_argument("--nointro", default="frontend/data/snes-nointro.tsv")
    ap.add_argument("--out", default="frontend/data/snes-extra-crc.tsv")
    ap.add_argument("--date", required=True, help="the day the dump was fetched, for the header")
    args = ap.parse_args()

    known = set()
    with open(args.nointro, encoding="utf-8") as f:
        for line in f:
            if line[0] != "#" and "\t" in line:
                known.add(line.split("\t")[0].lower())

    if args.zip:
        data = open(args.zip, "rb").read()
    else:
        with urllib.request.urlopen(DUMP, timeout=300) as r:
            data = r.read()
    z = zipfile.ZipFile(io.BytesIO(data))

    best = {}  # crc -> (rank, name)
    for member in z.namelist():
        game = json.loads(z.read(member))
        for attr in game.get("Attributes", []):
            if attr.get("attributeName") != "ROMs":
                continue
            for rom in attr["Value"]:
                crc = (rom.get("Crc") or "").lower()
                size = rom.get("Size") or 0
                name = rom.get("Name") or ""
                if not re.fullmatch(r"[0-9a-f]{8}", crc) or size <= 0 or crc in known:
                    continue
                if ARCHIVE.search(name) or size % 1024 == 512:
                    continue
                name = ROM_EXT.sub("", name).replace("\t", " ").replace("\n", " ").strip()
                if not name:
                    continue
                rank = (bool(BAD_DUMP.search(name)), len(name), name)
                if crc not in best or rank < best[crc][0]:
                    best[crc] = (rank, name)

    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Snes9x PS5: SNES dumps No-Intro does not list (fan translations, hacks, other dump sets), by ROM CRC32\n")
        out.write("# (headerless), from the Hasheous SNES dump fetched %s; made by tools/make_extra_crc.py.\n" % args.date)
        out.write("# The name is the dump's own: only the art matcher reads it. No-Intro names are in snes-nointro.tsv.\n")
        for crc in sorted(best):
            out.write("%s\t%s\n" % (crc.upper(), best[crc][1]))
    print("%d CRCs -> %s" % (len(best), args.out), file=sys.stderr)


if __name__ == "__main__":
    main()

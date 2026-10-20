#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/snes-db-art.tsv: the games libretro-thumbnails has no cover for, but an online database does,
with where to download that cover, and the CRC32 of every dump of the game.

The source is Hasheous' public SNES dump (no account, no key), the same file make_extra_crc.py reads:
  https://hasheous.org/api/v1/Dumps/platforms/Nintendo%20Super%20Nintendo%20Entertainment%20System.zip
One JSON file per game: its name, the ids Hasheous maps it to in other databases, its cover (an IGDB image, which
Hasheous serves at https://hasheous.org/api/v1/images/<id>), and its ROMs with their hashes.

A row, tab-separated: the game's name; its cover sources, space-separated ("h<image id>" from Hasheous, "t<id>" for
TheGamesDB's front box art, https://cdn.thegamesdb.net/images/original/boxart/front/<id>-1.jpg); the CRC32 of its dumps,
comma-separated, upper case. The app (fe_dbart.h) finds the row by the ROM's CRC32 or by the words of its name.

Left out: a game with no cover source, and a game libretro-thumbnails already has: one of its dumps is in No-Intro
(snes-nointro.tsv) under a name that is in the cover list (snes-cover-index.txt), so the app finds the cover by CRC.

  python3 tools/make_db_art.py [--zip dump.zip] [--date 2026-10-17]   (from ps5/)
"""
import argparse
import io
import json
import re
import sys
import urllib.request
import zipfile

DUMP = "https://hasheous.org/api/v1/Dumps/platforms/Nintendo%20Super%20Nintendo%20Entertainment%20System.zip"
ARCHIVE = re.compile(r"\.(zip|7z|rar|gz)$", re.I)
CRC = re.compile(r"[0-9a-f]{8}")
IMAGE_ID = re.compile(r"[0-9A-Fa-f]{40}")


def load_names(path):
    with open(path, encoding="utf-8") as f:
        return {line.rstrip("\n") for line in f if line.strip() and line[0] != "#"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", help="a dump already downloaded")
    ap.add_argument("--nointro", default="frontend/data/snes-nointro.tsv")
    ap.add_argument("--covers", default="frontend/data/snes-cover-index.txt")
    ap.add_argument("--out", default="frontend/data/snes-db-art.tsv")
    ap.add_argument("--date", required=True, help="the day the dump was fetched, for the header")
    args = ap.parse_args()

    nointro = {}  # crc -> No-Intro name
    with open(args.nointro, encoding="utf-8") as f:
        for line in f:
            if line[0] != "#" and "\t" in line:
                crc, name = line.rstrip("\n").split("\t")[:2]
                nointro[crc.lower()] = name
    covers = load_names(args.covers)

    if args.zip:
        data = open(args.zip, "rb").read()
    else:
        with urllib.request.urlopen(DUMP, timeout=300) as r:
            data = r.read()
    z = zipfile.ZipFile(io.BytesIO(data))

    rows = {}  # name -> [sources, crcs]
    for member in sorted(z.namelist()):
        game = json.loads(z.read(member))
        name = (game.get("Name") or "").replace("\t", " ").replace("\n", " ").strip()
        attrs = {a.get("attributeName"): a.get("Value") for a in game.get("Attributes", [])}
        sources = []
        logo = attrs.get("Logo") or ""
        if IMAGE_ID.fullmatch(logo):
            sources.append("h" + logo.upper())
        for m in game.get("Metadata") or []:
            if m.get("Source") == "TheGamesDb" and m.get("Status") == "Mapped" and re.fullmatch(r"\d+", m.get("Id") or ""):
                sources.append("t" + m["Id"])
        crcs = set()
        for rom in attrs.get("ROMs") or []:
            crc = (rom.get("Crc") or "").lower()
            size = rom.get("Size") or 0
            if not CRC.fullmatch(crc) or size <= 0 or size % 1024 == 512 or ARCHIVE.search(rom.get("Name") or ""):
                continue  # the app takes the CRC of a ROM without a copier header, never of an archive
            crcs.add(crc)
        if not name or not sources or not crcs:
            continue
        if any(nointro.get(c) in covers for c in crcs):
            continue  # libretro-thumbnails has this game
        row = rows.setdefault(name, [[], set()])
        for s in sources:
            if s not in row[0]:
                row[0].append(s)
        row[1] |= crcs

    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Snes9x PS5: SNES games libretro-thumbnails has no cover for, with where an online database has one,\n")
        out.write("# from the Hasheous SNES dump fetched %s; made by tools/make_db_art.py.\n" % args.date)
        out.write("# name<TAB>cover sources (h = hasheous.org image id, t = TheGamesDB id)<TAB>CRC32 of its dumps\n")
        for name in sorted(rows):
            sources, crcs = rows[name]
            out.write("%s\t%s\t%s\n" % (name, " ".join(sources), ",".join(c.upper() for c in sorted(crcs))))
    print("%d games -> %s" % (len(rows), args.out), file=sys.stderr)


if __name__ == "__main__":
    main()

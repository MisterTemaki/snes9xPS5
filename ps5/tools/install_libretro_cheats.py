#!/usr/bin/env python3
"""Install SNES RetroArch .cht files under ROM filenames for snes9xPS5.

Download/extract https://github.com/libretro/libretro-database, then:
 python3 ps5/tools/install_libretro_cheats.py --roms ./my-roms \
   --database ./libretro-database/cht/'Nintendo - Super Nintendo Entertainment System' \
   --output ./ps5-cheats

Output stays in RetroArch format. snes9xPS5 imports it and saves native Snes9x BML
automatically after toggles. Does not replace the user's existing cheats unless
--force is supplied.
"""
from __future__ import annotations

import argparse
import re
import shutil
from collections import defaultdict
from pathlib import Path

ROMS = {".sfc", ".smc", ".swc", ".fig", ".bs", ".st", ".zip", ".gz", ".bin", ".mgd"}


def title_key(stem: str) -> str:
    """Discard No-Intro region tags, but do not casually merge distinct titles."""
    stem = re.sub(r"\s*[\(\[][\w, .+!&'/-]*[\)\]]", "", stem)
    return re.sub(r"[^a-z0-9]+", "", stem.lower())


def score(rom_name: str, candidate: Path) -> int:
    source = candidate.stem.lower()
    rom = rom_name.lower()
    points = 0
    if source == rom:
        points += 1000
    regions = ("usa", "europe", "japan")
    matched = [r for r in regions if f"({r})" in rom]
    if matched:
        if any(f"({r})" in source for r in matched):
            points += 100
        elif any(f"({r})" in source for r in regions):
            points -= 100
    elif "(usa)" in source:
        points += 20
    elif "(europe)" in source:
        points += 10
    # Prefer the combined cheats file when available, not the Game Genie-only subset.
    if "(game genie)" in source:
        points -= 1
    return points


def install(rom_dir: Path, db_dir: Path, output: Path, *, force: bool = False) -> tuple[int, int]:
    if not rom_dir.is_dir() or not db_dir.is_dir():
        raise ValueError("ROM folder or libretro SNES cheat folder does not exist")
    index: dict[str, list[Path]] = defaultdict(list)
    for cheat in db_dir.rglob("*.cht"):
        key = title_key(cheat.stem)
        if key:
            index[key].append(cheat)
    output.mkdir(parents=True, exist_ok=True)
    installed = missing = 0
    for rom in sorted(rom_dir.rglob("*")):
        if not rom.is_file() or rom.suffix.lower() not in ROMS:
            continue
        choices = index.get(title_key(rom.stem), [])
        if not choices:
            print(f"MISSING: {rom.name}")
            missing += 1
            continue
        candidate = max(choices, key=lambda p: (score(rom.stem, p), p.name))
        target = output / f"{rom.stem}.cht"
        if target.exists() and not force:
            print(f"EXISTS (not overwritten): {target.name}")
            continue
        shutil.copyfile(candidate, target)
        print(f"INSTALLED: {rom.name} <- {candidate.name}")
        installed += 1
    return installed, missing


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--roms", required=True, type=Path, help="Folder containing your SNES ROMs")
    parser.add_argument("--database", required=True, type=Path, help="Extracted Libretro SNES cht folder")
    parser.add_argument("--output", required=True, type=Path, help="Output folder to transfer over FTP")
    parser.add_argument("--force", action="store_true", help="Overwrite existing output cheat files")
    args = parser.parse_args()
    done, missing = install(args.roms, args.database, args.output, force=args.force)
    print(f"Done: {done} installed; {missing} ROMs did not have a clear title match")


if __name__ == "__main__":
    main()

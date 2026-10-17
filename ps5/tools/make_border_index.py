#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/snes-border-index.txt: the names of the SNES borders The Bezel Project has, so the app knows what it can
download without listing the repository (raw.githubusercontent.com has no directory listing, and the API is rate limited).

The Bezel Project keeps the SNES borders in two repositories, with no name in both: bezelproject-SNES (USA and World names,
folder retroarch/overlay/GameBezels/SNES/) and bezelproject-SFC (Japan, Europe and the other regions, folder
retroarch/overlay/GameBezels/SFC/). The index lists the first, then a "#@ SFC" line and the second: the app downloads a name
from the repository whose section it is in (fe_borderfetch.cpp). Only the names are kept (No-Intro names, one per line, no
extension, "# " comment lines first): the pictures are never bundled, the app downloads the one a game needs (ADR 006).

  python3 tools/make_border_index.py            (from ps5/; clones both trees without the pictures, a few MB each)
  python3 tools/make_border_index.py --snes-repo path/to/bezelproject-SNES --sfc-repo path/to/bezelproject-SFC
"""
import argparse
import os
import subprocess
import sys
import tempfile

SOURCES = [
    # key, repository, folder of the pictures
    ("SNES", "https://github.com/thebezelproject/bezelproject-SNES", "retroarch/overlay/GameBezels/SNES/"),
    ("SFC", "https://github.com/thebezelproject/bezelproject-SFC", "retroarch/overlay/GameBezels/SFC/"),
]


def git(cwd, *args):
    return subprocess.run(["git", "-c", "core.quotepath=false", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def names_of(repo, folder):
    """(names, sha, date): the pictures of the folder, from the tree alone (nothing is checked out)."""
    sha = git(repo, "rev-parse", "HEAD").strip()
    date = git(repo, "log", "-1", "--format=%cs").strip()
    paths = git(repo, "ls-tree", "-r", "--name-only", "-z", "HEAD", "--", folder).split("\0")
    names = sorted({p[len(folder):-4] for p in paths if p.startswith(folder) and p.endswith(".png") and "/" not in p[len(folder):]})
    bad = [n for n in names if not n or "\n" in n or "\t" in n or n.startswith("#")]
    if bad or not names:
        sys.exit("unexpected file names: %r" % bad[:3])
    return names, sha, date


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--snes-repo", help="a clone of bezelproject-SNES (the pictures need not be checked out)")
    ap.add_argument("--sfc-repo", help="a clone of bezelproject-SFC")
    ap.add_argument("--out", default="frontend/data/snes-border-index.txt")
    args = ap.parse_args()
    given = {"SNES": args.snes_repo, "SFC": args.sfc_repo}

    sections = []
    with tempfile.TemporaryDirectory() as tmp:
        for key, url, folder in SOURCES:
            repo = given[key]
            if not repo:
                repo = os.path.join(tmp, key)
                subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--no-checkout", url, repo], check=True)
            sections.append((key, url, folder) + names_of(repo, folder))

    seen = set()
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The Bezel Project SNES border filename index: " +
                ", ".join("%s %s (%s)" % (url.rsplit("/", 1)[1], sha[:7], date) for _, url, _, _, sha, date in sections) + "\n")
        f.write("# <folder><name>.png on https://raw.githubusercontent.com/thebezelproject/<repository>/master/\n")
        counts = []
        for key, url, folder, names, sha, date in sections:
            f.write("#@ %s %s%s\n" % (key, url.rsplit("/", 1)[1] + "/", folder))
            kept = [n for n in names if n not in seen]  # the first repository wins a name both have (none today)
            seen.update(kept)
            for n in kept:
                f.write(n + "\n")
            counts.append("%s %d" % (key, len(kept)))
    print("%s: %s names (%s)" % (args.out, " + ".join(counts), ", ".join("%s %s" % (k, sha[:7]) for k, _, _, _, sha, _ in sections)))


if __name__ == "__main__":
    main()

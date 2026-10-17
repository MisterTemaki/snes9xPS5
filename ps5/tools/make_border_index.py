#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/data/snes-border-index.txt: the names of the SNES borders The Bezel Project has, so the app knows what it can
download without listing the repository (raw.githubusercontent.com has no directory listing, and the API is rate limited).

The source is the list of files in https://github.com/thebezelproject/bezelproject-SNES, folder
retroarch/overlay/GameBezels/SNES/. Only the names are kept (No-Intro names, one per line, no extension, "# " comment lines
first): the pictures are never bundled, the app downloads the one a game needs (fe_borderfetch.h, ADR 006).

  python3 tools/make_border_index.py                      (from ps5/; clones the tree without the pictures, a few MB)
  python3 tools/make_border_index.py --repo path/to/clone  (a clone you already have)
"""
import argparse
import os
import subprocess
import sys
import tempfile

REPO = "https://github.com/thebezelproject/bezelproject-SNES"
FOLDER = "retroarch/overlay/GameBezels/SNES/"


def git(cwd, *args):
    return subprocess.run(["git", "-c", "core.quotepath=false", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", help="a clone of bezelproject-SNES (the pictures need not be checked out)")
    ap.add_argument("--out", default="frontend/data/snes-border-index.txt")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        repo = args.repo
        if not repo:
            repo = os.path.join(tmp, "snes")
            subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--no-checkout", REPO, repo], check=True)
        sha = git(repo, "rev-parse", "HEAD").strip()
        date = git(repo, "log", "-1", "--format=%cs").strip()
        paths = git(repo, "ls-tree", "-r", "--name-only", "-z", "HEAD", "--", FOLDER).split("\0")

    names = sorted({p[len(FOLDER):-4] for p in paths if p.startswith(FOLDER) and p.endswith(".png") and "/" not in p[len(FOLDER):]})
    bad = [n for n in names if not n or "\n" in n or "\t" in n]
    if bad or not names:
        sys.exit("unexpected file names: %r" % bad[:3])
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The Bezel Project SNES border filename index (%s, bezelproject-SNES %s)\n" % (date, sha[:7]))
        f.write("# %s<name>.png on https://raw.githubusercontent.com/thebezelproject/bezelproject-SNES/master/\n" % FOLDER)
        for n in names:
            f.write(n + "\n")
    print("%s: %d names (%s, %s)" % (args.out, len(names), date, sha[:7]))


if __name__ == "__main__":
    main()

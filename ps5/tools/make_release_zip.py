#!/usr/bin/env python3
# The app folder as a release zip, for the app's own updates (Settings, Updates; ProsperoUpdate.h) and the
# homebrew catalog: <TITLE_ID>/ with every file stored (no compression: the console reads it as it is) and
# <TITLE_ID>/manifest.sha256 ("<sha-256> *<path>" lines) that the update checks every file against.
#   make_release_zip.py <app folder> <TITLE_ID> <out.zip>
# Attach the zip to the GitHub release whose tag is the version (v1.8): the app looks for the asset ending in
# "-<TITLE_ID>.zip".
# SPDX-License-Identifier: MIT
import hashlib
import os
import sys
import zipfile

app, title, out = sys.argv[1:4]
files = []
for root, dirs, names in os.walk(app):
    dirs.sort()
    for n in sorted(names):
        full = os.path.join(root, n)
        rel = os.path.relpath(full, app).replace(os.sep, '/')
        if rel == 'manifest.sha256' or rel.endswith(('.new', '.part')):
            continue
        files.append((rel, full))
manifest = ''.join('%s *%s\n' % (hashlib.sha256(open(f, 'rb').read()).hexdigest(), rel) for rel, f in files)
stamp = (2026, 1, 1, 0, 0, 0)
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
    folders = set()
    for rel, _ in files:
        parts = rel.split('/')[:-1]
        for i in range(len(parts) + 1):
            folders.add('/'.join([title] + parts[:i]) + '/')
    for d in sorted(folders):
        info = zipfile.ZipInfo(d, stamp)
        info.external_attr = 0o40755 << 16
        z.writestr(info, b'')
    for rel, full in files:
        info = zipfile.ZipInfo(title + '/' + rel, stamp)
        info.external_attr = 0o644 << 16
        info.compress_type = zipfile.ZIP_STORED
        z.writestr(info, open(full, 'rb').read())
    info = zipfile.ZipInfo(title + '/manifest.sha256', stamp)
    info.external_attr = 0o644 << 16
    z.writestr(info, manifest)
print('%s: %d files, %d bytes, sha-256 %s' % (out, len(files) + 1, os.path.getsize(out),
                                              hashlib.sha256(open(out, 'rb').read()).hexdigest()))

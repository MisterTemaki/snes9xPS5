#!/usr/bin/env python3
"""Checks the interface's translations (frontend/fe_i18n.cpp): every entry has its five languages, no English
text twice, and every translation keeps the English text's printf specifiers, in the same order (a translated
format string is passed to printf). Exit 1 on a problem.

  check_i18n.py <fe_i18n.cpp>
"""
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
body = src[src.index("const Entry kTable[] = {"):src.index("};", src.index("const Entry kTable[] = {"))]
# a C string literal, with adjacent literals joined
lit = r'"((?:[^"\\]|\\.)*)"'
entries = []
for m in re.finditer(r"\{\s*((?:%s\s*,?\s*)+)\}" % lit, body):
    parts = re.findall(r'((?:"(?:[^"\\]|\\.)*"\s*)+)(?:,|$)', m.group(1))
    strs = ["".join(re.findall(lit, p)) for p in parts]
    entries.append(strs)
spec = re.compile(r"%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcspn])")
bad = 0
seen = set()
for e in entries:
    if len(e) != 5:
        print("not five languages:", e)
        bad += 1
        continue
    if e[0] in seen:
        print("twice:", e[0])
        bad += 1
    seen.add(e[0])
    want = spec.findall(e[0])
    for lang, t in zip(("es", "pt-BR", "fr", "de"), e[1:]):
        if spec.findall(t) != want:
            print("%s: specifiers %s, English has %s: %s" % (lang, spec.findall(t), want, e[0]))
            bad += 1
print("%d texts, %d problem(s)" % (len(entries), bad))
sys.exit(1 if bad else 0)

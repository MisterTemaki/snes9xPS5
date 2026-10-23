#!/usr/bin/env python3
"""Writes the .7z archives the 7z reader's test reads (tests/7z/, committed: the tests need no py7zr).

    pip install py7zr   (py7zr 1.1.4 and pyppmd 1.3.1 made the committed ones)
    make_7z_fixtures.py tests/7z

Every file's content is Rom(size, seed) below, which tests/test_7z.cpp (and test_netfs.cpp) compute the same way:
half xorshift32 noise (the netfs test's Blob), half a repeated line, so every coder sees literals and matches.
"""
import os
import struct
import sys
import tempfile
import zlib

import py7zr
import pyppmd

out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)


def blob(n, seed):
    out = bytearray(n)
    x = (seed * 2654435761 + 1) & 0xFFFFFFFF
    for i in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = x & 0xFF
    return bytes(out)


def rom(n, seed):
    half = n // 2
    line = b"SNES9X PS5 7Z TEST %d " % seed
    text = line * (n // len(line) + 1)
    return blob(half, seed) + text[: n - half]


def write(name, files, filters=None, password=None, header_encryption=False, dirs=()):
    path = os.path.join(out_dir, name)
    if os.path.exists(path):
        os.remove(path)
    kw = {}
    if filters is not None:
        kw["filters"] = filters
    if password is not None:
        kw["password"] = password
        kw["header_encryption"] = header_encryption
    with py7zr.SevenZipFile(path, "w", **kw) as z:
        for d in dirs:
            with tempfile.TemporaryDirectory() as empty:
                z.write(empty, d)
        for fname, data in files:
            z.writestr(data, fname)
    print("%-16s %6d bytes" % (name, os.path.getsize(path)))


def num(n):
    """A 7z NUMBER: the first byte's leading 1 bits count the bytes that follow (little endian)."""
    for extra in range(9):
        if extra == 8 or n < (1 << (8 * extra + 7 - extra)):
            first = (0xFF00 >> extra) & 0xFF
            return bytes([first | (n >> (8 * extra) if extra < 8 else first)]) + (n & ((1 << (8 * extra)) - 1)).to_bytes(extra, "little")


def write_raw(name, fname, data, method, props, packed):
    """A 7z with one file in one folder of one coder, the header not packed: py7zr writes PPMd with 7 bytes of
    properties, which 7-Zip's own decoder (5 bytes) refuses, so the PPMd archive is put together here."""
    header = bytes([0x01, 0x04, 0x06]) + num(0) + num(1) + bytes([0x09]) + num(len(packed)) + bytes([0x00])
    header += bytes([0x07, 0x0B]) + num(1) + bytes([0x00]) + num(1)
    header += bytes([len(method) | 0x20]) + method + num(len(props)) + props
    header += bytes([0x0C]) + num(len(data)) + bytes([0x00])
    header += bytes([0x08, 0x0A, 0x01]) + struct.pack("<I", zlib.crc32(data)) + bytes([0x00, 0x00])
    names = (fname + "\0").encode("utf-16-le")
    header += bytes([0x05]) + num(1) + bytes([0x11]) + num(len(names) + 1) + bytes([0x00]) + names + bytes([0x00, 0x00])
    start = struct.pack("<QQI", len(packed), len(header), zlib.crc32(header))
    sig = b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I", zlib.crc32(start)) + start
    with open(os.path.join(out_dir, name), "wb") as f:
        f.write(sig + packed + header)
    print("%-16s %6d bytes" % (name, os.path.getsize(os.path.join(out_dir, name))))


def ppmd7z(data, order, mem):
    enc = pyppmd.Ppmd7Encoder(order, mem)
    return enc.encode(data) + enc.flush()


LZMA2 = [{"id": py7zr.FILTER_LZMA2, "preset": 7}]
write("lzma.7z", [("game.sfc", rom(8000, 1))], [{"id": py7zr.FILTER_LZMA, "preset": 7}])
write("lzma2.7z", [("game.sfc", rom(8000, 2))], LZMA2)
write_raw("ppmd.7z", "game.sfc", rom(8000, 3), bytes([0x03, 0x04, 0x01]), struct.pack("<BI", 6, 16 << 20),
          ppmd7z(rom(8000, 3), 6, 16 << 20))
write("bcj.7z", [("game.sfc", rom(8000, 4))], [{"id": py7zr.FILTER_X86}] + LZMA2)
write("arm.7z", [("game.sfc", rom(8000, 5))], [{"id": py7zr.FILTER_ARM}] + LZMA2)
write("delta.7z", [("game.sfc", rom(8000, 6))], [{"id": py7zr.FILTER_DELTA, "dist": 2}] + LZMA2)
write("copy.7z", [("game.sfc", rom(8000, 7))], [{"id": py7zr.FILTER_COPY}])
# solid: one block with three files and a folder; the ROM is the biggest
write("solid.7z", [("docs/readme.txt", b"Read me.\n" * 20), ("Game (USA).sfc", rom(12000, 8)),
                   ("Game (USA) [patch].smc", rom(6000, 9))], LZMA2, dirs=["docs"])
write("unicode.7z", [("ゲーム (Japan).sfc", rom(1000, 10))], LZMA2)
# a copier header: 512 bytes before the ROM (the CRC the archive stores includes it)
write("header.7z", [("game.smc", bytes(512) + rom(8192, 11))], LZMA2)
write("empty.7z", [], LZMA2)
write("disc.7z", [("Game (Disc).cue", b'FILE "Game (Disc).bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'),
                  ("Game (Disc).bin", rom(5000, 12))], LZMA2)
write("enc-header.7z", [("game.sfc", rom(4000, 13))], password="snes", header_encryption=True)
write("enc-data.7z", [("game.sfc", rom(4000, 14))], password="snes", header_encryption=False)
write("bzip2.7z", [("game.sfc", rom(4000, 15))], [{"id": py7zr.FILTER_BZIP2}])
# a ROM over Snes9x's 16 MiB (+ 512): refused for its size
write("too-big.7z", [("huge.sfc", bytes(17 << 20))], [{"id": py7zr.FILTER_LZMA2, "preset": 9}])
# a small ROM packed solid with 65 MiB more: its block is over the 64 MiB cap
write("big-block.7z", [("game.sfc", rom(4096, 16)), ("filler.bin", bytes(65 << 20))],
      [{"id": py7zr.FILTER_LZMA2, "preset": 9}])

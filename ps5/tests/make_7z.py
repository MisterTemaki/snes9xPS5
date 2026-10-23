#!/usr/bin/env python3
"""Packs files into a .7z for the host tests, with the standard library only (no py7zr, no network): one solid LZMA2
block (or stored, --copy), the header not packed, a CRC32 for every file.

    make_7z.py [--copy] out.7z name_in_archive=file [name_in_archive=file ...]

The archives the 7z reader's own test reads (tests/7z) are made by make_7z_fixtures.py with py7zr; this one makes the
archives of the app's run-tests (a ROM the tests generate, so the archive is made at test time).
"""
import lzma
import struct
import sys
import zlib


def num(n):
    """A 7z NUMBER: the first byte's leading 1 bits count the bytes that follow (little endian)."""
    for extra in range(9):
        if extra == 8 or n < (1 << (8 * extra + 7 - extra)):
            first = (0xFF00 >> extra) & 0xFF
            return bytes([first | (n >> (8 * extra) if extra < 8 else first)]) + (n & ((1 << (8 * extra)) - 1)).to_bytes(extra, "little")


def main():
    args = sys.argv[1:]
    copy = args and args[0] == "--copy"
    if copy:
        args = args[1:]
    out, pairs = args[0], args[1:]
    files = []
    for p in pairs:
        name, path = p.split("=", 1)
        with open(path, "rb") as f:
            files.append((name, f.read()))
    data = b"".join(d for _, d in files)
    if copy:
        method, props, packed = bytes([0x00]), b"", data
    else:
        dict_size = 1 << 20  # LZMA2's dictionary property 16 = 1 MiB
        packed = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "preset": 6, "dict_size": dict_size}])
        method, props = bytes([0x21]), bytes([16])

    h = bytes([0x01, 0x04])  # Header, MainStreamsInfo
    h += bytes([0x06]) + num(0) + num(1) + bytes([0x09]) + num(len(packed)) + bytes([0x00])  # PackInfo
    h += bytes([0x07, 0x0B]) + num(1) + bytes([0x00]) + num(1)  # UnpackInfo: one folder of one coder
    h += bytes([len(method) | (0x20 if props else 0)]) + method + (num(len(props)) + props if props else b"")
    h += bytes([0x0C]) + num(len(data)) + bytes([0x00])
    h += bytes([0x08])  # SubStreamsInfo
    if len(files) != 1:
        h += bytes([0x0D]) + num(len(files))
        if len(files) > 1:
            h += bytes([0x09]) + b"".join(num(len(d)) for _, d in files[:-1])
    h += bytes([0x0A, 0x01]) + b"".join(struct.pack("<I", zlib.crc32(d)) for _, d in files) + bytes([0x00])
    h += bytes([0x00])  # end of MainStreamsInfo
    names = b"".join((n + "\0").encode("utf-16-le") for n, _ in files)
    h += bytes([0x05]) + num(len(files)) + bytes([0x11]) + num(len(names) + 1) + bytes([0x00]) + names + bytes([0x00])
    h += bytes([0x00])  # end of Header
    start = struct.pack("<QQI", len(packed), len(h), zlib.crc32(h))
    with open(out, "wb") as f:
        f.write(b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I", zlib.crc32(start)) + start + packed + h)


main()

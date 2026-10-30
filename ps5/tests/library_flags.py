#!/usr/bin/env python3
"""library_flags.py FILE TEXT FLAG -- sets FLAG (a bit of the flags byte: 1 USB, 2 network, 4 named by CRC, 8 CRC pending,
16 header says Japan) on every game of a saved game list (frontend/fe_library.h) whose path holds TEXT, and writes the
checksum again: a list as an older run of the app could have saved it. Prints how many games it changed."""
import struct
import sys
import zlib

path, text, flag = sys.argv[1], sys.argv[2].encode(), int(sys.argv[3])
d = bytearray(open(path, 'rb').read())
assert d[:8] == b'S9XLIB\r\n'
count = struct.unpack_from('<I', d, 12)[0]
at, changed = 16, 0
for _ in range(count):
    strings = []
    for _ in range(8):
        n = struct.unpack_from('<I', d, at)[0]
        strings.append(bytes(d[at + 4:at + 4 + n]))
        at += 4 + n
    if text in strings[0]:
        d[at] |= flag
        changed += 1
    at += 1 + 4
assert at == len(d) - 4
d[-4:] = struct.pack('<I', zlib.crc32(bytes(d[:-4])) & 0xffffffff)
open(path, 'wb').write(d)
print(changed)

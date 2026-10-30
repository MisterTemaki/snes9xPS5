#!/usr/bin/env python3
"""damage_state.py <state> -- rewrites a Snes9x save state (gzip) so its first block, NAM, declares 2 GiB - 64 KiB of
data (the binary length form, "NAM:-", a byte, 4 bytes big-endian, ":"): a damaged or foreign state, for the tests (group 41k). Snes9x reads
what is beyond the size it expects as junk; up to 2.10's review it allocated that junk whole (snapshot.cpp)."""
import gzip
import struct
import sys

path = sys.argv[1]
data = gzip.open(path).read()
i = data.find(b'NAM:')
if i < 0:
    sys.exit('no NAM block')
header_end = i + 11
length = int(data[i + 4:i + 10])
bad = b'NAM:-' + b'\0' + struct.pack('>I', 0x7FFF0000) + b':'
assert len(bad) == 11
out = data[:i] + bad + data[header_end:]
with gzip.open(path, 'wb') as f:
    f.write(out)
print('NAM block of %d bytes now says %d' % (length, 0x7FFF0000))

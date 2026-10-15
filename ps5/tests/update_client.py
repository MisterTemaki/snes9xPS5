#!/usr/bin/env python3
# The app's side of the update requests (kCmdUpdate, ProsperoJailbreak.h), for the tests.
#   update_client.py <port> <what> [version] [--until phase,... [--timeout s]]
# Prints the helper's answer (update::Encode: phase|latest|size|done|total|error); with --until, asks "status"
# after <what> until the phase is one of those.
import os
import socket
import struct
import sys
import time

port, what = int(sys.argv[1]), sys.argv[2]
version = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else ''
until = None
timeout = 30.0
if '--until' in sys.argv:
    until = sys.argv[sys.argv.index('--until') + 1].split(',')
if '--timeout' in sys.argv:
    timeout = float(sys.argv[sys.argv.index('--timeout') + 1])


def ask(w):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    req = struct.pack('<IiiI', 0xDEADBEEF, 7, os.getpid(), (-1337) & 0xffffffff) + \
        w.encode().ljust(0x500, b'\0') + version.encode().ljust(0x500, b'\0')
    s.sendall(req)
    data = b''
    while len(data) < len(req):
        c = s.recv(len(req) - len(data))
        if not c:
            break
        data += c
    s.close()
    if len(data) != len(req):
        return None
    ret = struct.unpack('<i', data[12:16])[0]
    msg = data[16:16 + 0x500].split(b'\0')[0].decode()
    return msg if ret == 0 else 'REFUSED ' + msg


answer = ask(what)
end = time.time() + timeout
while until and answer and answer.split('|')[0] not in until and time.time() < end:
    time.sleep(0.2)
    answer = ask('status')
print(answer)

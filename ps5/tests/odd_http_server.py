#!/usr/bin/env python3
# Servers that misbehave on purpose, for the helper's HTTP client (tests/run-tests.sh, group 46).
#   odd_http_server.py <port> bigchunk            plain HTTP: a chunked 200 whose second chunk says it is 2^64 - 8 bytes,
#                                                 then 9 MiB and the end of the connection
#   odd_http_server.py <port> downgrade <cert> <key> <to>
#                                                 HTTPS: every GET is a 302 to <to> (an http:// address)
# Each request is written to stderr ("GET <path>").
import socket
import ssl
import sys
import threading

port, mode = int(sys.argv[1]), sys.argv[2]


def handle(c):
    try:
        req = b''
        while b'\r\n\r\n' not in req:
            d = c.recv(4096)
            if not d:
                return
            req += d
        line = req.split(b'\r\n', 1)[0].decode(errors='replace')
        sys.stderr.write(line + '\n')
        sys.stderr.flush()
        if mode == 'bigchunk':
            c.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nTransfer-Encoding: chunked\r\n\r\n')
            c.sendall(b'10\r\n' + b'\x89PNG\r\n\x1a\n' + b'x' * 8 + b'\r\n')
            c.sendall(b'fffffffffffffff8\r\n')
            block = b'y' * 65536
            for _ in range(9 * 16):
                c.sendall(block)
        else:
            to = sys.argv[5]
            c.sendall(('HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\n\r\n' % to).encode())
    except OSError:
        pass
    finally:
        try:
            c.close()
        except OSError:
            pass


s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(8)
ctx = None
if mode == 'downgrade':
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(sys.argv[3], sys.argv[4])
while True:
    c, _ = s.accept()
    if ctx:
        try:
            c = ctx.wrap_socket(c, server_side=True)
        except (OSError, ssl.SSLError):
            c.close()
            continue
    threading.Thread(target=handle, args=(c,), daemon=True).start()

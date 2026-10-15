#!/usr/bin/env python3
# A stand-in for ShadowMountPlus's HTTP API (127.0.0.1, POST + JSON), for the title ID move tests.
#   fake_smp.py <port> <log> <title>=<content id>[,...] [busy]
# /api/v1/games/info: 200 with the title's content_id, or 404; /api/v1/games/uninstall: 200 (or 409 the first
# <busy> times, as when a game is in front), and the title is gone; /api/v1/scan: 200. Every request is logged.
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

port, log = int(sys.argv[1]), sys.argv[2]
titles = dict(t.split('=', 1) for t in sys.argv[3].split(',') if t)
busy = [int(sys.argv[4]) if len(sys.argv) > 4 else 0]


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def answer(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        tid = req.get('title_id', '')
        with open(log, 'a') as f:
            f.write(f'{self.path} {tid}\n')
        if self.path == '/api/v1/games/info':
            if tid in titles:
                return self.answer(200, {'status': 0, 'title_id': tid, 'content_id': titles[tid], 'path': '/data/homebrew/' + tid})
            return self.answer(404, {'status': 2, 'error': 'No such file or directory'})
        if self.path == '/api/v1/games/uninstall':
            if busy[0] > 0:
                busy[0] -= 1
                return self.answer(409, {'status': 16, 'error': 'busy', 'error_reason': 'game_active'})
            if tid not in titles:
                return self.answer(404, {'status': 2})
            del titles[tid]
            with open(log, 'a') as f:
                f.write(f'UNINSTALLED {tid}\n')
            return self.answer(200, {'status': 0, 'title_id': tid, 'source_missing': True, 'scan_queued': True,
                                     'scan_deferred': False, 'scan_deferred_reason': ''})
        if self.path == '/api/v1/scan':
            return self.answer(200, {'status': 0, 'scan_queued': True})
        return self.answer(404, {'status': 2})


HTTPServer(('127.0.0.1', port), H).serve_forever()

#!/usr/bin/env python3
"""fake_ra_server.py -- a local stand-in for retroachievements.org's dorequest.php, for the host tests (2.11).

    fake_ra_server.py <port> <config.json> <log.jsonl>

Plain HTTP on 127.0.0.1:<port> (the helper's client speaks http:// to the test host only; on the console it is HTTPS to
retroachievements.org). The answers have the format rcheevos 12.5.0's own tests record (test/test_rc_client.c):

  POST /dorequest.php  r=login2            u + p (password) or u + t (token)
                       r=achievementsets   m = the game's hash -> the game's sets, or 404 "Unknown game"
                       r=startsession      -> the unlocks the config says the player already has
                       r=awardachievement  a, h -> the new score
                       r=submitlbentry     i, s -> the rank
                       r=ping              m = rich presence
  GET  /Badge/<name>.png                   a 64 x 64 PNG

config.json: {"user": "...", "password": "...", "token": "...", "expired_token": "...",
              "games": {"<hash>": <the achievementsets answer>}, "unlocked_hardcore": [ids], "unlocked": [ids],
              "fail_awards": n (the first n awardachievement get a 503), "delay": seconds before each answer,
              "delay_first": {"<r>": seconds} (only the first request of that kind waits; 2.11 review: a slow server),
              "redirect_badges": "<url>" (every badge answers 302 to it)}

Each request is one JSON line in log.jsonl: its fields, but never the password or the token themselves -- "password_ok"
and "token_ok" say whether they were right -- and the User-Agent header.
"""
import json
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

port = int(sys.argv[1])
config = json.load(open(sys.argv[2]))
log_path = sys.argv[3]
lock = threading.Lock()
state = {"awards": 0, "score": 1234, "softcore": 56, "delayed": set()}


def png(w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


BADGE = png(64, 64, (255, 200, 0, 255))


def record(entry):
    with lock:
        with open(log_path, "a") as f:
            f.write(json.dumps(entry) + "\n")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def answer(self, status, body, ctype="application/json"):
        if config.get("delay"):
            time.sleep(float(config["delay"]))
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        record({"get": self.path, "ua": self.headers.get("User-Agent", "")})
        if self.path.startswith("/Badge/") and self.path.endswith(".png") and config.get("redirect_badges"):
            self.send_response(302)
            self.send_header("Location", config["redirect_badges"])
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path.startswith("/Badge/") and self.path.endswith(".png"):
            self.answer(200, BADGE, "image/png")
        else:
            self.answer(404, b"not found", "text/plain")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        q = {k: v[0] for k, v in parse_qs(body, keep_blank_values=True).items()}
        r = q.get("r", "")
        entry = {"r": r, "ua": self.headers.get("User-Agent", ""), "path": self.path}
        for k in ("u", "m", "g", "h", "a", "i", "s", "x", "l", "o"):
            if k in q:
                entry[k] = q[k]
        if "p" in q:
            entry["password_ok"] = q["p"] == config["password"]
        if "t" in q:
            entry["token_ok"] = q["t"] == config["token"]
            entry["token_expired"] = q["t"] == config.get("expired_token")
        record(entry)
        wait = config.get("delay_first", {}).get(r)
        if wait:
            with lock:
                first = r not in state["delayed"]
                state["delayed"].add(r)
            if first:
                time.sleep(float(wait))
        token_ok = q.get("t") == config["token"]
        if r == "login2":
            if q.get("u") == config["user"] and (q.get("p") == config["password"] or token_ok):
                return self.answer(200, {"Success": True, "User": config["user"], "Token": config["token"],
                                         "Score": state["score"], "SoftcoreScore": state["softcore"], "Messages": 0,
                                         "Permissions": 1, "AccountType": "Registered"})
            if "t" in q and q["t"] == config.get("expired_token"):
                return self.answer(401, {"Success": False, "Error": "The access token has expired. Please log in again.",
                                         "Status": 401, "Code": "expired_token"})
            if "t" in q:
                return self.answer(401, {"Success": False, "Error": "Invalid User/Token combination.", "Status": 401,
                                         "Code": "invalid_credentials"})
            return self.answer(401, {"Success": False, "Error": "Invalid User/Password combination. Please try again",
                                     "Status": 401, "Code": "invalid_credentials"})
        if not token_ok:
            return self.answer(401, {"Success": False, "Error": "Invalid User/Token combination.", "Status": 401,
                                     "Code": "invalid_credentials"})
        if r == "achievementsets":
            game = config["games"].get(q.get("m", "").lower())
            if game is None:
                return self.answer(404, {"Success": False, "Error": "Unknown game", "Code": "not_found", "Status": 404})
            return self.answer(200, game)
        if r == "startsession":
            now = int(time.time())
            return self.answer(200, {"Success": True,
                                     "HardcoreUnlocks": [{"ID": i, "When": now - 100} for i in config.get("unlocked_hardcore", [])],
                                     "Unlocks": [{"ID": i, "When": now - 100} for i in config.get("unlocked", [])],
                                     "ServerNow": now})
        if r == "awardachievement":
            with lock:
                state["awards"] += 1
                n = state["awards"]
            if n <= int(config.get("fail_awards", 0)):
                return self.answer(503, b"", "text/plain")
            hardcore = q.get("h") == "1"
            if hardcore:
                state["score"] += 5
            else:
                state["softcore"] += 5
            # what is left for the player in this mode (0 tells rcheevos the game is mastered or completed)
            mode = "have_hardcore" if hardcore else "have_softcore"
            have = state.setdefault(mode, set(config.get("unlocked_hardcore", [])) | (set() if hardcore else set(config.get("unlocked", []))))
            have.add(int(q.get("a", "0")))
            every = {a["ID"] for g in config["games"].values() for st in g.get("Sets", []) for a in st.get("Achievements", [])}
            return self.answer(200, {"Success": True, "Score": state["score"], "SoftcoreScore": state["softcore"],
                                     "AchievementID": int(q.get("a", "0")), "AchievementsRemaining": len(every - have)})
        if r == "submitlbentry":
            s = int(q.get("s", "0"))
            return self.answer(200, {"Success": True, "Response": {"Score": s, "BestScore": s,
                                     "TopEntries": [{"User": config["user"], "Score": s, "Rank": 1}],
                                     "RankInfo": {"Rank": 1, "NumEntries": "1"}}})
        if r == "ping":
            return self.answer(200, {"Success": True})
        return self.answer(400, {"Success": False, "Error": "Unknown request", "Status": 400})


ThreadingHTTPServer.allow_reuse_address = True
server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
print("fake RetroAchievements server on port %d" % port, flush=True)
server.serve_forever()

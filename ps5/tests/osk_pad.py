#!/usr/bin/env python3
"""osk_pad.py -- builds a SNES9X_HOST_PAD script for the host tests from a short list of steps on stdin, so a test can
drive the Network shares screen and its on-screen keyboard (frontend/fe_osk.cpp).

Steps, one per line ('#' starts a comment):
    frame N          the next press happens at flip N
    press NAME [n]   NAME is UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE OPTIONS L1 R1 L2 R2; n times (default 1)
    field TEXT       Cross on a text row, TEXT typed on the keyboard, OPTIONS (Done)
    wait N           N flips with no press

Prints the script, "flip:hex buttons;..." ending with the last release, and the flip after it on stderr.
The keyboard model mirrors fe_osk.cpp: four rows of keys per page, a row of six function keys, the cursor keeps
its column by position when it changes rows, Shift (L2) is for one key, Symbols (R2) stays on.
"""
import sys

BUTTONS = dict(UP=0x10, DOWN=0x40, LEFT=0x80, RIGHT=0x20, CROSS=0x4000, CIRCLE=0x2000, SQUARE=0x8000, TRIANGLE=0x1000,
               OPTIONS=0x8, L1=0x400, R1=0x800, L2=0x100, R2=0x200)
W, KEY_W, GAP = 1920, 120, 10
FN_WIDTHS = [190, 190, 400, 190, 130, 150]
LOWER = ["1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"]
UPPER = ["1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@"]
SYMBOLS = ["!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "~`"]
FN_ROW = 4


def centres(row, page):
    if row == FN_ROW:
        widths = FN_WIDTHS
    else:
        widths = [KEY_W] * len(page[row])
    total = sum(widths) + GAP * (len(widths) - 1)
    x = (W - total) // 2
    out = []
    for w in widths:
        out.append(x + w // 2)
        x += w + GAP
    return out


class Script:
    def __init__(self):
        self.frame = 0
        self.events = []
        self.row = self.col = 0
        self.shift = self.symbols = False

    def press(self, name, n=1):
        for _ in range(n):
            self.events.append((self.frame, BUTTONS[name]))
            self.events.append((self.frame + 2, 0))
            self.frame += 4

    def page(self):
        return SYMBOLS if self.symbols else UPPER if self.shift else LOWER

    def clamp(self):
        if self.row != FN_ROW:
            self.col = min(self.col, len(self.page()[self.row]) - 1)

    def go_row(self, to):
        here = centres(self.row, self.page())[self.col]
        cs = centres(to, self.page())
        self.col = min(range(len(cs)), key=lambda c: abs(cs[c] - here))
        self.row = to

    def move_to(self, row, col):
        while self.row != row:
            if row > self.row:
                self.press("DOWN")
                self.go_row((self.row + 1) % 5)
            else:
                self.press("UP")
                self.go_row((self.row + 4) % 5)
        while self.col < col:
            self.press("RIGHT")
            self.col += 1
        while self.col > col:
            self.press("LEFT")
            self.col -= 1

    def type_char(self, ch):
        if ch == " ":
            self.press("TRIANGLE")
            return
        in_lower = any(ch in r for r in LOWER)
        in_upper = any(ch in r for r in UPPER) and not in_lower
        if in_upper:
            if self.symbols:
                self.press("R2")
                self.symbols = False
            if not self.shift:
                self.press("L2")
                self.shift = True
            self.clamp()
        elif in_lower:
            if self.symbols:
                self.press("R2")
                self.symbols = False
                self.clamp()
        else:
            if not self.symbols:
                self.press("R2")
                self.symbols = True
                self.clamp()
        page = self.page()
        for r in range(4):
            if ch in page[r]:
                self.move_to(r, page[r].index(ch))
                break
        else:
            raise SystemExit("cannot type %r" % ch)
        self.press("CROSS")
        self.shift = False

    def field(self, text):
        self.press("CROSS")
        self.row = self.col = 0
        self.shift = self.symbols = False
        for ch in text:
            self.type_char(ch)
        self.press("OPTIONS")


def main():
    s = Script()
    for line in sys.stdin:
        line = line.split("#", 1)[0].strip() if not line.lstrip().startswith("field") else line.strip()
        if not line:
            continue
        cmd, _, arg = line.partition(" ")
        if cmd == "frame":
            s.frame = int(arg)
        elif cmd == "wait":
            s.frame += int(arg)
        elif cmd == "press":
            parts = arg.split()
            s.press(parts[0], int(parts[1]) if len(parts) > 1 else 1)
        elif cmd == "field":
            s.field(arg)
        else:
            raise SystemExit("unknown step: " + line)
    print("0:0;" + ";".join("%d:%x" % e for e in s.events))
    print("next flip %d" % s.frame, file=sys.stderr)


if __name__ == "__main__":
    main()

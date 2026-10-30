#!/usr/bin/env python3
"""Builds a tiny 32 KiB LoROM for the host tests (no game data needed).

The ROM turns the screen on with backdrop colour red; while SNES button B is held (read through the
auto-joypad registers in the NMI handler) the backdrop is green. So a de-tiled flip shows whether the
whole chain works: CPU, PPU, the port's scaler and tiler, and the pad -> Snes9x input mapping.

    make_test_rom.py out.sfc [ntsc|pal|msu]

sram / sram2: an NTSC ROM with 2 KiB of battery RAM that writes $42 / $43 to its first byte ($70:0000), as a game
saving does.
ra (2.11, RetroAchievements): an NTSC ROM with 2 KiB of battery RAM that writes $5A to its second byte ($70:0001) at
reset, and in each NMI counts the frames in work RAM $0011 (INC, wrapping at 256) and stores the joypad's high byte in
$0010 ($80 while B is held). The fake server's achievements read those bytes (RetroAchievements' addresses $000010,
$000011 and $020001: work RAM, then the cartridge RAM).
msu: an NTSC ROM that also starts MSU-1 track 1 at full volume, playing and repeating (write $2004/$2005 =
track 1, $2006 = volume $FF, $2007 = play + repeat), as an MSU-1 patched game does.
"""
import struct
import sys

out = sys.argv[1]
region = sys.argv[2] if len(sys.argv) > 2 else "ntsc"
msu = region == "msu"
ra = region == "ra"
sram = {"sram": 0x42, "sram2": 0x43}.get(region)
if msu or sram or ra:
    region = "ntsc"

rom = bytearray([0xFF] * 0x8000)

reset = bytes([
    0x78,                    # SEI
    0x18, 0xFB,              # CLC; XCE        native mode
    0xA9, 0x8F, 0x8D, 0x00, 0x21,  # LDA #$8F; STA $2100  forced blank
    0x9C, 0x21, 0x21,        # STZ $2121       CGRAM address 0
    0xA9, 0x1F, 0x8D, 0x22, 0x21,  # LDA #$1F; STA $2122  red (BGR555 0x001F), low byte
    0x9C, 0x22, 0x21,        # STZ $2122       high byte
    0xA9, 0x0F, 0x8D, 0x00, 0x21,  # LDA #$0F; STA $2100  screen on, full brightness
]) + (bytes([
    0xA9, 0x01, 0x8D, 0x04, 0x20,  # LDA #$01; STA $2004  MSU-1 track, low byte
    0x9C, 0x05, 0x20,        # STZ $2005       high byte: loads track 1
    0xA9, 0xFF, 0x8D, 0x06, 0x20,  # LDA #$FF; STA $2006  volume
    0xA9, 0x03, 0x8D, 0x07, 0x20,  # LDA #$03; STA $2007  play + repeat
]) if msu else b"") + (bytes([
    0xA9, sram, 0x8F, 0x00, 0x00, 0x70,  # LDA #value; STA $700000  the battery RAM's first byte
]) if sram else b"") + (bytes([
    0xA9, 0x5A, 0x8F, 0x01, 0x00, 0x70,  # LDA #$5A; STA $700001  the battery RAM's second byte
    0x64, 0x10, 0x64, 0x11,  # STZ $10; STZ $11   joypad copy, frame counter
]) if ra else b"") + bytes([
    0xA9, 0x81, 0x8D, 0x00, 0x42,  # LDA #$81; STA $4200  NMI + auto joypad read
    0xCB,                    # loop: WAI
    0x80, 0xFD,              # BRA loop
])
nmi = bytes([
    0x48,                    # PHA
    0xAD, 0x10, 0x42,        # LDA $4210       acknowledge NMI
]) + (bytes([
    0xE6, 0x11,              # INC $11         frame counter (work RAM $7E0011)
]) if ra else b"") + bytes([
    0xAD, 0x19, 0x42,        # LDA $4219       joypad 1 high byte: B Y Sel Start U D L R
]) + (bytes([
    0x85, 0x10,              # STA $10         the joypad's high byte (work RAM $7E0010)
]) if ra else b"") + bytes([
    0x29, 0x80,              # AND #$80        B
    0xF0, 0x0F,              # BEQ red
    0x9C, 0x21, 0x21,        # STZ $2121
    0xA9, 0xE0, 0x8D, 0x22, 0x21,  # green 0x03E0
    0xA9, 0x03, 0x8D, 0x22, 0x21,
    0x68, 0x40,              # PLA; RTI
    0x9C, 0x21, 0x21,        # red: STZ $2121
    0xA9, 0x1F, 0x8D, 0x22, 0x21,
    0x9C, 0x22, 0x21,
    0x68, 0x40,              # PLA; RTI
])
rom[0x0000:0x0000 + len(reset)] = reset
rom[0x0100:0x0100 + len(nmi)] = nmi

# header at $FFC0 (file offset 0x7FC0)
title = b"SNES9X PS5 TEST".ljust(21, b" ")
rom[0x7FC0:0x7FD5] = title
rom[0x7FD5] = 0x20          # LoROM, slow
rom[0x7FD6] = 0x02 if (sram or ra) else 0x00  # ROM + RAM + battery, or ROM only
rom[0x7FD7] = 0x05          # 32 KiB
rom[0x7FD8] = 0x01 if (sram or ra) else 0x00  # 2 KiB of battery RAM, or none
rom[0x7FD9] = 0x01 if region == "ntsc" else 0x02  # USA / Europe
rom[0x7FDA] = 0x00
rom[0x7FDB] = 0x00
# vectors: native NMI $FFEA, emulation RESET $FFFC
struct.pack_into("<H", rom, 0x7FEA, 0x8100)
struct.pack_into("<H", rom, 0x7FFA, 0x8100)
struct.pack_into("<H", rom, 0x7FFC, 0x8000)
# checksum
struct.pack_into("<HH", rom, 0x7FDC, 0xFFFF, 0x0000)
csum = sum(rom) & 0xFFFF
struct.pack_into("<HH", rom, 0x7FDC, csum ^ 0xFFFF, csum)

with open(out, "wb") as f:
    f.write(rom)
print(f"wrote {out} ({region})")

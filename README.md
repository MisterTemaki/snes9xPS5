![Snes9x for PS5](ps5/app/sce_sys/background-source.png)

# Snes9x PS5

**Inside the emulator, to access the main menu press L3 + R3.**

Snes9x 1.63, the Super Nintendo emulator, running on a jailbroken PS5 as a **native home-screen app** with its
own icon and background. The PS5 layer follows the layout of PS5SX2 (the PCSX2 port):

- `ps5/coreorbis` holds `main-boot.cpp`, the `orbis-shims/` and `include-orbis/`;
- `ps5/frontend` holds the user interface and the 3D game shelf;
- `ps5/proto/native` builds and signs `eboot.bin`;
- `ps5/installer` is the installer and helper payload.

Everything outside `ps5/` is the original Snes9x source, unchanged, apart from this README (the original one is
[README-Snes9x.md](README-Snes9x.md)).

> **Status (2.3):** runs on the console: the app opens from its icon, the shelf, the controller, video and sound
> work, and games play. It builds with the ps5-payload-dev SDK and passes 173 host tests, which run the same code
> on Linux with the PS5 calls simulated. If something fails, the logs in `/data/snes9x/logs/` say where.

## How it works (the PS5SX2 model)

The PS5 gives the controller only to the app in front. Versions 1.0 to 1.5 ran Snes9x as a payload next to the
system UI, so the controller never worked. Since 1.6 Snes9x follows PS5SX2's model:

| Piece | What it is | In PS5SX2 |
|---|---|---|
| `eboot.bin` in `/data/homebrew/PPSA99009/` | **Snes9x itself**, a native app opened from its icon | PCSX2 (PPSA99203) |
| `sce_module/libc.prx` | the C runtime module every native app carries | the same file, byte for byte |
| `Snes9xPS5.elf` (payload) | the **installer**: installs or updates the app, then stays running as the **helper** | PS5SX2Installer.elf + PS5SX2Helper.elf |

When it opens, the app asks the helper to let it out of its sandbox; without that an app sees neither `/data`
nor USB drives. The request is the one PS5SX2 makes:

- **Who is asked, in order:** the Snes9x helper (127.0.0.1:9083; older helpers used 9075 up to 2.1 and 9080 in
  2.2, and are left alone), then etaHEN (9028) and the daemon on port 9069.
- **If nobody answers:** the app carries a copy of the helper (`Snes9xPS5-helper.elf`), sends it to the ELF
  loader (127.0.0.1:9021) and asks again. So the icon keeps working after a reboot, as long as the ELF loader
  runs.
- **What the helper allows:** only title PPSA99009. It gives the process the system's root folder and uid 0, as
  elfldr does for payloads. No other app is touched.

Snes9x only uses `/data/snes9x/`, `/data/homebrew/PPSA99009/` and its own `/user/appmeta/PPSA99009/`. Nothing is
written to PS5SX2's folders (`/data/PCSX2`, `/data/homebrew/PPSA99203`).

## Versions

Every release carries its version in the file name: `Snes9xPS5-v2.3.elf` and `snes9x-ps5-v2.3-src.zip`
(`make dist`). When updating, replace the old ELF with the new one in your autoload or Payload Manager. In this
README, "`Snes9xPS5.elf`" always means the current release's ELF.

**2.3:** **The helper downloads the covers with its own HTTPS.** In 2.2 the helper got nothing: the console's own
HTTPS (libSceSsl) fails outside the app's sandbox, and every cover failed in a few milliseconds while the counter
went down. The helper now has its own HTTPS, as Genesis Plus GX PS5 1.6 does: Mbed TLS, with Mozilla's list of
certificate authorities, the server's certificate checked as a browser does. It also keeps one connection for
all its downloads. After updating, send `Snes9xPS5-v2.3.elf` once (or let the app start its helper itself):
2.2's helper keeps running until the console restarts, but 2.3 uses its own (port 9083).

**2.2:** **Covers download in the background.** 2.1 downloaded them before the app opened (up to 30 s with the
launch screen up) and restarted itself for new ones. Now the helper downloads them while you use the app: it
starts at once, the covers around the selection come first, and each one appears on the shelf as it lands
("Downloading covers in the background... N left"). After updating, send `Snes9xPS5-v2.2.elf` once (or let the
app start its helper itself): 2.1's helper keeps running until the console restarts, but 2.2 uses its own
(port 9080).

**2.1:** CRT shaders (CRT Easymode style by default; see [CRT shaders](#crt-shaders)) and ScaleFX + rAA + AA style; MSU-1 documented and
tested (see [MSU-1](#msu-1-cd-quality-music-in-snes-games)); a setting to turn the debug logs off; the fixes of a full code audit (helper, crash-safe saves and states, covers,
library, 720p). **2.0:** Snes9x as a native
home-screen app.

## Language

Every screen and notification of Snes9x PS5 is in English.

## Requirements

- A jailbroken PS5 with **kstuff** (or kstuff-lite) and an **ELF loader** on port 9021: elfldr, etaHEN, or the
  one PS5 Payload Manager uses.
- **ShadowMountPlus**, so the icon appears on the home screen (the same one PS5SX2 uses).
- Your own games (SNES ROMs). No games are included.

## Install and play

1. **Send `Snes9xPS5.elf`** with PS5 Payload Manager, or from a PC on the same network:
   ```sh
   nc -q0 PS5_IP 9021 < Snes9xPS5-v2.1.elf
   ```
   It installs the app in `/data/homebrew/PPSA99009/` (`eboot.bin`, `sce_module/libc.prx`, `param.json`, the
   icon and the backgrounds), shows **"Snes9x PS5 2.3 installed. Open it from the Snes9x PS5 icon on the home
   screen."** and stays running as the helper.
2. **Open the Snes9x PS5 icon.** The game shelf appears and the controller works.
3. **Copy your ROMs** (`.sfc .smc .swc .fig .bs .st .zip .gz`) to `/data/snes9x/roms`, over FTP for example, or
   to a `snes9x/roms` folder on a USB drive. Subfolders work.

Tip: put `Snes9xPS5.elf` in your autoload, as PS5SX2 recommends for its payloads.

**Updating:** send the new `Snes9xPS5.elf` once. It compares every app file with the copy it carries and rewrites
only what changed; each file is written to a temporary file and then renamed, `eboot.bin` last. The notification
says "Snes9x PS5 updated to 2.1". A deleted or damaged icon is put back the same way. A second copy sent while
the helper is already running only installs and exits.

**If "Snes9x PS5 has no access to /data" appears:** no helper answered and the ELF loader wasn't running. Send
`Snes9xPS5.elf` and open the icon again.

| Folder | Contents |
|---|---|
| `/data/snes9x/roms` | your games |
| `/data/snes9x/saves` | battery saves (`.srm`), written 3 s after the game saves and on exit; each write goes to a `.part` file that replaces the old save only once it is complete and on disk, so a power cut or a full disk never leaves a broken save |
| `/data/snes9x/states` | save states (`.000` to `.009`), written the same way: a failed save leaves the slot's old state |
| `/data/snes9x/cheats` | cheats (`.cht`, Snes9x format), loaded with the game |
| `/data/snes9x/patches` | IPS/UPS/BPS patches named after the ROM |
| `/data/snes9x/bios` | `BS-X.bin`, `STBIOS.bin` (Satellaview, Sufami Turbo) |
| `/data/snes9x/covers` | downloaded covers and your own |
| `/data/snes9x/logs` | `boot.log` (the app), `installer.log` (installer/helper), `helper.log` (the helper the app starts), and the previous session's `.prev.log` files |
| `/data/snes9x/snes9x-ps5.ini` | the menu settings |
| `/data/snes9x/snes9x.conf` | optional: Snes9x's own configuration file, for advanced options |

### The home-screen app

```
/data/homebrew/PPSA99009/
  eboot.bin              Snes9x (a native app, signed as PS5SX2's is), with the helper inside
  sce_module/libc.prx    the native app's C runtime (the same as PS5SX2's and ps5-native-app-boilerplate's)
  sce_sys/param.json     title "Snes9x PS5", ID PPSA99009
  sce_sys/icon0.png      the icon (512x512; ps5/app/sce_sys/icon0.png in the source)
  sce_sys/pic0.dds       the home-screen background while the icon is selected (3840x2160, BC7)
  sce_sys/pic1.dds       the launch background (the same image)
```

To change the icon, replace `ps5/app/sce_sys/icon0.png` (512x512 PNG) and rebuild. The background comes from
`ps5/app/sce_sys/background-source.png`, converted to `pic0.dds`/`pic1.dds` with ps5-native-app-boilerplate's
`tools/prepare-assets.sh --background`. For another title ID: `make ps5 TITLE_ID=XXXX00000`.

**Home-screen art:** ShadowMountPlus copies the art in `sce_sys` (icon, backgrounds, `param.json`) to
`/user/appmeta/PPSA99009/` only when it first registers the title, and the system keeps the art it saw at that
moment. The installer keeps that folder current on every update, but if the title was registered before the art
changed, register it again once:

1. On the home screen, select **Snes9x PS5**, press **OPTIONS** and choose **Delete** (your games, saves, states
   and covers live in `/data/snes9x/` and are not removed).
2. Send `Snes9xPS5.elf` again; it reinstalls the app folder if it is gone.
3. Wait for ShadowMountPlus to register the title; the icon comes back with the new art.

## The game shelf and covers

The start screen is a 3D shelf of game covers, like PS5SX2's. In the top-left corner, under the wordmark, is the
author's line with the GitHub mark: **github.com/MisterTemaki** (PS5SX2 shows its author's handles there).

- the selected cover sits in the middle, with a glow in the cover's own colour over the game's blurred art;
- the others are tilted on both sides, in perspective, reflected on the floor;
- the shelf slides when you change games.

- **All your games at once:** the shelf gathers the ROMs in `/data/snes9x/roms` and on USB drives
  (`snes9x/roms`), subfolders included, sorted by game name.
- **Official names:** SNES ROMs have no serial. A game is identified by its file name or, when that doesn't
  match, by the ROM's CRC32, looked up in a No-Intro table of 4268 games built into the app.
  - The CRC is taken without a copier header; for a `.zip`, the CRC stored in the zip is used.
  - Loose names (`super mario world.smc`) are recognised too.
  - CRCs are cached in `covers/crc-cache.txt`, so each ROM is read once.
- **Automatic covers, in the background:** covers come from
  [libretro-thumbnails](https://github.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System)
  (`Named_Boxarts`) over HTTPS.
  - The console's own HTTPS (`libSceHttp2`/`libSceSsl`) only works inside the app's sandbox, and the app can't
    see `/data` until it is out of it. So the **helper** downloads the covers, while you use the app, with HTTPS of
    its own (since 2.3): Mbed TLS, the server's certificate checked against Mozilla's list of certificate
    authorities, one connection kept for all the downloads.
    The app opens at once and lists the missing covers in `/data/snes9x/covers/wanted.txt`, and the ones around
    the selection in `covers/priority.txt` (fetched first); the helper downloads them one by one, and each cover
    replaces its card on the shelf as it lands. The top right corner shows "Downloading covers in the
    background... N left". The helper keeps going while the app is closed.
  - With another jailbreak daemon instead of the Snes9x helper (etaHEN, 9069), 2.1's way still works: a prefetch
    before the app asks for `/data` (30 s), and a restart when new games need covers.
  - Covers are kept in `/data/snes9x/covers/` and never downloaded twice. A cover the server doesn't have is
    marked (`.missing`) and only looked for again after 30 days; **Square** forces a new try.
  - Without a network nothing is tried and the shelf works the same.
- **Your own covers:** a `.png` or `.jpg` named after the ROM file, in `/data/snes9x/covers/` or next to the ROM,
  takes priority over downloads.
- **No cover:** the game gets a card with its title.
- **Turning downloads off:** Settings, "Download covers".

### Downloading all the covers yourself

Covers to download by hand (a zip with every cover of the system) -- **Snes9x PS5: copy them to
`/data/snes9x/covers/`**:

- **SNES, all at once (zip):** https://github.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System/archive/refs/heads/master.zip
- **SNES, one by one:** the covers can also be downloaded by hand from
  https://github.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System/tree/master/Named_Boxarts
  (open a cover, then "Download raw file").

How to use it:

1. Unzip it on the PC.
2. Copy only the `.png` files inside its **`Named_Boxarts`** folder to the covers folder (over FTP, for example).
   The zip also has title screens and in-game shots: they are not used. It is large: copying only the covers of
   your games saves space.
3. The names are already right (for example `Super Mario World (USA).png`): don't rename them. Open the app:
   games recognised by name or CRC pick their cover up at once, and nothing is downloaded for them.

Notes:

- The files keep their official names (`Super Mario World (USA).png`); the app looks for exactly that name, with
  the characters `` & * / : ` < > ? \ | " `` replaced by `_` as in the repository. `boot.log` lists the name it
  found for each game (`[games] ... -> "<name>"`).
- A few `.png` files in the repository are git links: a small text file holding the name of another cover. In the
  zip they may come out as text, not images; copy the cover they name under that file's name instead.
- One cover: `https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System/master/Named_Boxarts/<name>.png`
  (spaces as `%20`).
- A cover named after the ROM file (`covers/Mario.png` for `roms/Mario.sfc`) is used before any other, for games
  the app doesn't recognise or to use another picture.

**How it is drawn:** PS5SX2 draws its shelf on the GPU with the Vulkan driver of a private ps5vk fork. Without
that driver Snes9x PS5 draws the shelf on the CPU, still in real 3D perspective:

- each cover is a quad turned about the vertical axis, drawn column by column;
- bilinear filtering, three mipmap levels and anti-aliased edges;
- the work is split across several cores.

On the test PC a frame takes about 15 ms on 2 cores.

## Controls

**On the shelf**

| Button | Does |
|---|---|
| Left / Right (D-pad or stick) | change game (hold to speed up) |
| L1 / R1 | skip 10 games |
| Cross | play |
| Triangle | settings |
| Square | download this game's cover again (the cover it has stays until the new one has arrived) |
| OPTIONS | quit Snes9x (asks first) |

**In a game** (buttons by position, as on the SNES pad)

| PS5 | SNES |
|---|---|
| Cross / Circle / Square / Triangle | B / A / Y / X |
| L1 / R1 | L / R |
| OPTIONS | Start |
| touchpad click | Select |
| D-pad or left stick | D-pad |

| Combination | Does |
|---|---|
| L3 + R3 | pause menu (save/load state, slot, shader, aspect ratio, scanlines, FPS, sound, reset, back to the list, quit) |
| L2 + Up / Down | save / load the state in the current slot |
| L2 + Left / Right | change slot (0–9) |
| hold R2 | fast forward |

Up to 4 controllers: players 2 to 4 are the other signed-in users. With 3 or 4 controllers the multitap is turned
on in port 2. The light bar shows the player (blue, red, green, pink).

## CRT shaders

Every game starts through a CRT shader -- **CRT Easymode style** unless you pick another one. Twelve shaders are
built in: the eleven CRT ones of Genesis Plus GX PS5 and ScaleFX + rAA + AA style; they work with every game (hi-res and interlaced pictures included)
and only while a game is running (the shelf is drawn without them).

**How to use them**

1. **In a game:** press **L3 + R3**; **Shader** is the row after "State slot". **Left / Right** (or **Cross**) go
   through the list, and the paused game behind the menu shows each one at once. **Circle** goes back to the game.
2. **On the shelf:** **Triangle** opens the settings; **Shader** is the first row.
3. The choice is saved and used for every game. **Off** gives the plain picture, with the "Scanlines" option.
4. It is kept in `/data/snes9x/snes9x-ps5.ini` as `shader=<number>` (the numbers below): you can also set it there
   by hand.

| `shader=` | Shader | Look | Weight | From |
|---|---|---|---|---|
| 0 | Off | the plain picture | -- | -- |
| 1 | **CRT Easymode style** (default) | flat screen, sharp, scanlines that widen on bright colours, aperture grille | medium | written for these ports, after the look of EasyMode's crt-easymode |
| 2 | crt-lottes | curved screen, Gaussian beam, shadow mask, a little bloom | heavy | Timothy Lottes (public domain) |
| 3 | crt-lottes-fast | lighter Lottes: curved, 4-tap beam, aperture mask, tone mapping | medium | Timothy Lottes (public domain) |
| 4 | crt-1tap | very light, contrasty dynamic scanlines | light | fishku (CC0) |
| 5 | crt-2tap | crt-1tap with exact blending between two lines | light | fishku (CC0) |
| 6 | crt-hyllian-fast | sharp Catmull-Rom picture, strong scanlines, magenta/green dot mask | medium | Hyllian (MIT) |
| 7 | crt-nobody | curved screen with rounded corners, beam scanlines, magenta/green mask | heavy | Hyllian (MIT) |
| 8 | newpixie-mini | strongly curved TV, colour bleed, vignette, film tone | heavy | Mattias Gustavsson (Unlicense) |
| 9 / 10 | crt-blurPi-sharp / crt-blurPi-soft | light blur and screen-space scanlines (sharp or bilinear) | light | Oriol Ferrer Mesià (MIT) |
| 11 | monoCRT | a monochrome monitor (made for black-and-white pictures) | light | hunterk (public domain) |
| 12 | ScaleFX + rAA + AA style | not a CRT: pixel art redrawn smooth -- staircase edges become clean lines and curves, flat colours stay exact, then a light edge smoothing and deblur | **very heavy** | ScaleFX + rAA post-3x (Sp00kyFox, MIT) from the scalefx+rAA+aa-fast preset; its final steps rewritten for these ports |

**ScaleFX + rAA + AA style** is libretro's `scalefx+rAA+aa-fast` preset: ScaleFX redraws the picture at 3x (edges
interpolated up to six pixels long, only colours of the original), rAA removes the remaining stair-steps, then a
smoothing along the edges, the scale to the screen and a deblur. The preset's last three passes (FXAA, guest(r)'s
AA shader 4.0 and deblur) can't be built in (GPL, or no permission to copy), so original code with the same
purpose replaces them: the result is close, not identical. It is by far the heaviest shader: on a 2-core test PC it
takes 27-42 ms a frame on real SNES screens (Super Mario World, Zelda, Super Metroid), so on the PS5's six threads
expect roughly 9-15 ms, more in hi-res games; check `shader N ms` in `boot.log`, and if a game slows down, pick
another shader.

Which to pick: **CRT Easymode style** for a sharp, flat arcade-monitor look; **crt-hyllian-fast** for stronger
scanlines and a visible dot mask; **crt-lottes** or **crt-nobody** for a curved TV; **crt-1tap / crt-2tap** when a
game should stay as light as possible.

They come from libretro's [slang-shaders](https://github.com/libretro/slang-shaders) (`crt/`), with their default
parameters. Why these: the PS5 build draws the picture with the CPU (there is no GPU driver for homebrew apps), so
only single-pass shaders are fast enough, and only shaders whose licence fits Snes9x's (public domain, CC0,
Unlicense, MIT) can be built in. crt-easymode itself is GPL, which the Snes9x licence can't take in, so "CRT
Easymode style" is original code aiming at the same look. Multi-pass shaders (crt-royale, crt-guest-advanced, the
Mega Bezel...) need a GPU.

How they run: each shader is rewritten in C++ (`ps5/coreorbis/orbis-shims/ProsperoCrt.cpp`). What depends only on
a source line and a screen column (the horizontal filter) is computed once per source line; per screen pixel only
the vertical blend, the beam, the mask and a gamma table remain; curved screens use a per-pixel map built once per
picture size. The work is shared by up to six threads. `boot.log` says how long the shader took per frame
(`shader N ms`, with the frame counts it logs); if a heavy one (crt-lottes, crt-nobody, newpixie-mini) makes a
game slow down -- hi-res games (512 pixels wide) cost more -- pick a lighter one. Licence notices are in
`ps5/THIRD_PARTY_SHADERS.md`.

## MSU-1 (CD-quality music in SNES games)

Snes9x plays games patched for the **MSU-1**, the add-on of the SD2SNES / FXPAK flash carts that streams
CD-quality music (and, in some patches, video data) from files next to the ROM. Many hacks use it to replace a
game's soundtrack (Zelda: A Link to the Past, Super Metroid, Chrono Trigger, Final Fantasy...).

**How to use it**

1. Put the patched ROM and its MSU-1 files **in the same folder**, all with **the same name** as the ROM:
   ```
   /data/snes9x/roms/Zelda MSU/
     Zelda MSU.sfc        <- the ROM with the MSU-1 patch: start this one
     Zelda MSU.msu        <- the MSU-1 data file (it may be empty, but it must be there)
     Zelda MSU-1.pcm      <- the music tracks: -1, -2, -3...
     Zelda MSU-2.pcm
   ```
   Or put everything in one zip renamed to **`Zelda MSU.msu1`**, next to the ROM (a "pack"; the files inside keep
   their `.msu` / `-N.pcm` endings).
2. Start the ROM from the shelf as usual. The `.msu` and `.pcm` files are not listed as games.

Good to know:

- The ROM must be the **MSU-1 patched** one: the original ROM plays its own music even with the tracks beside it.
- The names must match exactly, capitals included (the PS5's file system tells them apart). The usual mistake is a
  ROM renamed after the tracks were made: rename the `.msu` and the `.pcm` files to match it.
- The tracks are 44.1 kHz 16-bit stereo `.pcm` files with an `MSU1` header, as every MSU-1 pack ships them; the
  music is mixed into the game's sound and follows the same output.
- An MSU-1 ROM can be on a USB drive as well (`snes9x/roms/...`), as long as its files are in its folder. Keep the
  ROM itself unzipped (a zipped ROM with MSU-1 files beside it has not been tried).
- The host tests play an MSU-1 track from a test ROM through to the console's sound output (files beside the ROM
  and a `.msu1` pack); it has not yet been tried with a real MSU-1 hack on a console.

## Picture and sound

- 1920x1080 output through `libSceVideoOut`, flipping on vsync. With little video memory it falls back to
  1280x720.
- Aspect ratio: **4:3** (default), **8:7** (square pixels), **integer scale** (4x, 1024x896) or **16:9**.
- A CRT shader on top (CRT Easymode style by default, see below); with the shader Off, optional plain scanlines.
- Sound through `libSceAudioOut` at 48 kHz, on its own thread.
  - Snes9x's *dynamic rate control* adjusts the sound by up to 0.5% to follow the TV's 60 Hz without crackles.
  - PAL (50 Hz) games follow the audio clock.

## Debugging (logs and crashes)

**Turning the logs on or off:** Settings (Triangle on the shelf, or L3 + R3 in a game) -> **Debug logs** (the last
row; On by default). Off stops every log at once: the app writes nothing more to `boot.log` (its last line says
the logs were turned off) or to the console output, and the next starts -- the app, the installer and the helper --
write no log files at all; the files of earlier runs are left as they are (delete them over FTP if you want). On
starts again at once, adding to `boot.log`. The setting is `debug_logs=0` / `debug_logs=1` in `snes9x-ps5.ini`.
Leave them on if you want to report a problem: with them off there is no log to send, and no `== CRASH ==` report
either.

If something fails, send the files in `/data/snes9x/logs/`: `boot.log` (the app), `installer.log` and
`helper.log`, plus the previous session's `.prev.log` files. They record every step:

- the sandbox request and who answered;
- the covers, one by one (`helper.log`: the background downloads; `boot.log`: the prefetch, when there is one);
- every `sceVideoOut*` call;
- the controller handle and its first read;
- the shelf and the cover thread, stage by stage.

Since 1.6.1, if the app dies on a signal (the PS5's "Game or App Error" screen), a `== CRASH ==` block is written
at the end of `boot.log`: the signal, the address, the **stage** each thread was in (`stage[...]`), and the return
addresses. That block points to the exact line of code that failed.

## Known limitations

- The PS and Create buttons are not reported by `scePadReadState`, so Select is on the touchpad.
- No netplay, rewind, or Snes9x video filters (hq2x, NTSC…) yet.

## Building

Requirements: the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) v0.42 or newer, clang/lld 18, and
g++ with ASan for the tests. zlib 1.3.1 is vendored in `third_party/zlib`.

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd ps5
make ps5 -j$(nproc)              # build/ps5/Snes9xPS5.elf (installer + helper, with the app inside)
make send PS5_HOST=192.168.0.10  # sends it to elfldr (port 9021)
make dist                        # build/dist/Snes9xPS5-v<version>.elf + the source zip
make app                         # only build/app/PPSA99009/, to copy by hand
make test                        # Linux builds (app, installer, helper) + 173 tests (ASan/UBSan)
```

The build has three stages:

1. `Snes9xPS5-helper.elf`, the helper alone.
2. `eboot.bin`: a PIE with the boilerplate's `app_crt.cpp` and the emulator's objects.
   - It is linked against the SDK's stubs and static libc++, with `ps5-pie.ld` + `ehframe.ld` and every symbol
     local.
   - Then it goes through `ps5-native-tool link` and `self --sign`, exactly like PS5SX2's `link-vk.sh`.
   - `libc.prx` goes with it, generated by `libc_builder` and checked against the published SHA-256
     (`proto/native/libc.prx.sha256`).
3. `Snes9xPS5.elf`, with the app folder built in.

## Source layout

- **`ps5/coreorbis/main-boot.cpp`**: the app's entry point (`eboot.bin`).
  - Prefetches covers, asks to leave the sandbox, then brings up log, folders, video, sound and pads, in that
    order.
  - Hands over to the frontend on a thread with an 8 MiB stack.
  - Exits through the system (`sceSystemServiceLoadExec("exit")`), as PS5SX2 does.
- **`ps5/installer/installer_main.cpp`**: `Snes9xPS5.elf` (installs the app and stays as the helper) and, built
  with `SNES9X_HELPER_ONLY`, `Snes9xPS5-helper.elf`.
- **`ps5/coreorbis/orbis-shims/`**: the PS5 layer.
  - `ProsperoVideo.cpp`: `libSceVideoOut`, direct memory, two scan-out buffers, AVX2 tiling, scaling of the
    SNES picture;
  - `ProsperoCrt.cpp`: the CRT shaders on the CPU, and their thread pool;
  - `ProsperoAudio.cpp`: `libSceAudioOut`, lock-free ring buffer, output thread;
  - `ProsperoInput.cpp`: `libScePad` + `libSceUserService`, up to 4 players;
  - `ProsperoJailbreak.cpp` / `ProsperoHelper.cpp`: both sides of the sandbox request and the covers list;
    `helper_data.cpp` builds the helper into the app;
  - `ProsperoInstall.cpp` + `install_data.cpp`: the app folder built into the installer, the install, and the
    `/user/appmeta` art;
  - `ProsperoCrash.cpp`: the crash printer and stage markers;
  - `ProsperoNotify.cpp`: system notifications (the same kernel toast as PS5SX2);
  - `orbis_paths.cpp`: `/data/snes9x`, USB drives and the logs.
- **`ps5/coreorbis/include-orbis/ProsperoSce.h`**: prototypes of the system functions (the SDK ships the import
  stubs but not the headers).
- **`ps5/frontend/`**: the 3D shelf (`fe_shelf.cpp`), covers (`fe_covers.cpp`, `fe_prefetch.cpp`), the library
  and No-Intro names (`fe_games.cpp`, `data/snes-nointro.tsv`), HTTPS (`fe_http.cpp`; the helper's own: `fe_tlshttp.cpp`, `fe_mbedtls_config.h`, `data/cacert.pem`, with
  Mbed TLS 3.6.7 in `third_party/mbedtls`), the helper's background downloads (`fe_coverworker.cpp`,
  `fe_coverfetch.cpp`), text with PS5SX2's fonts
  (`fe_text.cpp`), menus, settings, and `fe_emu.cpp`, which connects the Snes9x core to the PS5 layer.
- **`ps5/proto/native/`**: ps5-native-app-boilerplate's tools (BlackBearReloaded, GPL-3.0), taken from PS5SX2
  and PS5_Vulkan (mihawk-99):
  - `ps5-native-tool`, for linking and signing;
  - `app_crt.cpp`;
  - `ps5-pie.ld`;
  - `libc_builder.cpp` and its manifests.
- **`ps5/app/sce_sys/`**: param.json, icon and backgrounds.
- **`ps5/host/sce_host.cpp`** and **`ps5/tests/`**: the PS5 functions implemented on Linux, and the tests. They
  check the picture, controller, save states, PAL, zip, sound, covers, install, helper and sandbox request, and
  the helper's HTTPS against a local server with a test certificate authority (a certificate from another
  authority or for another name refused, chunked answers, redirects, the connection kept).

## License and credits

- **Snes9x**: the Snes9x license (`LICENSE` at the root: personal, non-commercial use, with source).
- **New code in `ps5/`**: MIT.
- **ps5-payload-dev SDK** (John Törnblom): toolchain, CRT, kernel access and import stubs (GPLv3+).
  - The payloads are linked with the SDK's CRT;
  - the app is linked with the SDK's static libc++.
- **ps5-native-app-boilerplate** (BlackBearReloaded, GPL-3.0-or-later): `ps5-native-tool`, `app_crt.cpp`,
  `app_cpp_runtime.cpp`, `ps5-pie.ld` and the `libc.prx` generator, via PS5SX2 and PS5_Vulkan (mihawk-99).
- **PS5SX2** (Spyros): the PS5 layer's pattern (Prospero* shims, kernel toast, `/data` layout, the sandbox request,
  the cover prefetch and exiting through the system).
- The VideoOut tiling and setup follow the SDK's SDL2 port (zlib license).
- **zlib** (Jean-loup Gailly and Mark Adler): zlib license.
- **Mbed TLS** 3.6.7 ([github.com/Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls), the Mbed TLS
  contributors): Apache-2.0 (`ps5/frontend/third_party/mbedtls/LICENSE`).
- **Mozilla's CA certificate list** (`ps5/frontend/data/cacert.pem`, as packaged by
  [certifi](https://github.com/certifi/python-certifi) 2026.07.22): MPL-2.0.
- **stb_image / stb_image_resize2 / stb_truetype** (Sean Barrett): public domain or MIT, the same as PS5SX2.
- **CRT shaders** from libretro's [slang-shaders](https://github.com/libretro/slang-shaders), rewritten for the CPU:
  crt-lottes and crt-lottes-fast (Timothy Lottes, public domain), crt-1tap and crt-2tap (fishku, CC0), monoCRT
  (hunterk, public domain), newpixie-mini (Mattias Gustavsson, Unlicense), crt-hyllian-fast and crt-nobody
  (Hyllian, MIT), crt-blurPi (Oriol Ferrer Mesià, MIT). Their notices are in `ps5/THIRD_PARTY_SHADERS.md`.
  "CRT Easymode style" is original code; the look it follows is EasyMode's crt-easymode. ScaleFX and rAA post-3x
  (Sp00kyFox, MIT), from the scalefx+rAA+aa-fast preset; its last steps are original code.
- **UI fonts**, the same as PS5SX2's (which takes them from PCSX2), in `frontend/assets/fonts/` with their
  licenses:
  - **Roboto Regular** (Google, Apache 2.0);
  - **PromptFont** (Yukari "Shinmera" Hafner, SIL OFL 1.1);
  - **Font Awesome Brands** (Fonticons, Inc.; font SIL OFL 1.1, icons CC BY 4.0), for the GitHub mark on the
    shelf.
- **Covers:** [libretro-thumbnails](https://github.com/libretro-thumbnails), downloaded on the console, not
  included. **Names and CRCs:** [libretro-database](https://github.com/libretro/libretro-database) (No-Intro).
- **Port:** [github.com/MisterTemaki](https://github.com/MisterTemaki).

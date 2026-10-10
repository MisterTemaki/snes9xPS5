# Manual SNES cheats and repaired artwork downloads

The PS5 emulator **does not download cheats**. The unavailable cheat downloader, full cheat-database download menu, automatic cheat requests and cheat helper thread have all been removed. Existing manually copied cheats are preserved on disk, and the cheat manager continues to support toggling/saving individual codes.

## Manually installing cheats

1. Obtain a SNES `.cht` file from the community-maintained [Libretro SNES cheat database](https://github.com/libretro/libretro-database/tree/master/cht/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System) or from your existing files.
2. Copy the file to `/data/snes9x/cheats/` using FTP, or put it beside the ROM. You may also copy the Libretro SNES files into `/data/snes9x/cheats/database/` or `/data/snes9x/cheats/Nintendo - Super Nintendo Entertainment System/`.
3. Launch the game. Open **Pause > Cheat manager** (or your configured shortcut). Toggle a supported cheat with **Cross**.

The emulator matches the game name against the file's title. For example, `EarthBound.zip` loads `EarthBound (USA).cht` without renaming. Manually installed cheat groups can be rechecked from the in-game menu without rebooting; the game cheat toggles are written to a local native format.

The PC-side helper `ps5/tools/install_libretro_cheats.py` still matches files already downloaded to your computer and creates a folder ready for FTP transfer. It does **not** access the internet.

## Automatic cover art only

The 3D game shelf continues to download cover images, entirely separately from cheats. It uses a verified filename index for:

- **Box art** (3,689 entries)
- **Title-screen screenshots** (3,664 entries)
- **In-game screenshots** (3,748 entries)

For games without a recognized box image, title screens and screenshots are fallback artwork, not equivalent to a box cover. Fuzzy title matching tolerates punctuation/region tags and small spelling differences. The downloader now writes its priority queue using the resolved artwork filename (not the unmatched original ROM name). A cached 404 from an older URL is retried automatically when a newly matched image URL differs.

**Square** retries the selected cover; **R3** retries missing covers across the shelf; or choose **Settings > Repair missing covers**. Automatic artwork remains controlled by **Settings > Download covers**. ROMs with unrelated names, obscure hacks, or games for which Libretro has no image may still show placeholders. Manual artwork named after the ROM (PNG/JPG) in `/data/snes9x/covers/` takes precedence.

To download covers on a PC instead, use the [Libretro SNES thumbnails repository](https://github.com/libretro-thumbnails/Nintendo_-_Super_Nintendo_Entertainment_System). Copy images from `Named_Boxarts` into `/data/snes9x/covers/`.

## Debugging covers

Enable **Settings > Debug logs** and check `/data/snes9x/logs/boot.log` and `helper.log`. The cover helper remains required for **online cover downloading**; removing the cheat downloader does not remove the existing cover helper.

## English translation cover names (v2.5)

For ROM archives named like `Bahamut Lagoon (ENG) # SNES.zip`, the artwork resolver removes the collection suffix and uses a verified alias for the original SNES release. Translated Japanese titles automatically reuse the **original Japanese game artwork** from Libretro, without renaming the ZIP.

The mapped aliases include Final Fantasy VI, Dragon Ball Z: Super Gokuu Den 2 (Kakusei Hen), Super Butouden and Super Butouden 3, Hyper Dimension, Dragon Quest I & II, Dai-3-ji Super Robot Taisen and Bahamut Lagoon. For these games the artwork index already has real matching filenames.

**Bootlegs need separate artwork:** `Pokemon Gold & Silver` and `Aladdin 2000` do not have distinct artwork entries in the current Libretro SNES box/title/screenshot lists. They should not be mapped to unrelated official Pokémon or Aladdin games. A matching PNG or JPG under `/data/snes9x/covers/<ROM ZIP name without .zip>.png` will still override the placeholder.

The renderer never modifies the ROM file or the manual cheat lookup, and still handles alternate matching filenames conservatively.

# Automatic SNES cheat downloads and cover repairs

The PS5's native emulator cannot use its ordinary libSceHttp2 connection after leaving the app sandbox. All cheat downloads therefore run in the **same separate helper payload** that handles cover downloads, using the tested Mbed TLS client. Requests and progress are exchanged through atomic files under `/data/snes9x/cheats`.

## Automatic downloads

**Auto-download missing cheats is ON by default.** The first time a ROM starts, its No-Intro/CRC title is looked up in the embedded Libretro SNES database. If a matching entry is found and no per-game cheat file already exists, the emulator queues a background download for the running helper. Open the Cheat Manager after the download completes to load the new codes. Disable this behavior through **Settings > Auto-download missing cheats**.

The installer must start the **new helper payload**. If an older Snes9x helper is still resident on the PS5, fully restart the console, then load the new installer ELF again before opening the dashboard icon. Otherwise cheat requests may stay in the `queued` state.

## PS5 controls

| Where | Action |
|---|---|
| 3D game shelf | **R2** – download cheats for the selected game |
| 3D game shelf | **R3** – retry all currently missing covers |
| Settings > Repair missing covers | Requeue missing art, with title-screen/screenshot fallback |
| 3D game shelf | **Square** – retry the selected cover |
| Settings > Cheat downloads | download missing cheats for all installed games or the entire SNES database |
| In a game > pause > Cheat manager | **Options** – open cheat downloads |
| In the cheat manager with no codes | **Cross** – open cheat downloads |

The cheat download screen shows **queued, downloading, completed and offline** states, with a processed/saved/failed counter. Selecting the full SNES collection and pressing Cross now immediately queues the download and displays a confirmation; the Libretro index currently contains **2,773** .cht files.

## Destination paths

- For a single ROM: `/data/snes9x/cheats/<ROM basename>.cht`.
- For all installed ROMs: the same per-ROM path, using No-Intro/CRC title matching when available.
- The full repository: `/data/snes9x/cheats/database/<original Libretro filename>.cht`. When you launch a game, a matching downloaded file is copied from the library cache to the ROM's own cheat path, if no file already exists.
- Job request: `/data/snes9x/cheats/download-request.txt`, consumed by the helper.
- Job status: `/data/snes9x/cheats/download-status.txt`.

The downloader **never overwrites** an existing game's `.cht`, preserving activated codes. Once a new file arrives, open the Cheat Manager to load it. If the file was installed from the full database cache, restarting the game loads it automatically. Individual code ON/OFF state is saved in Snes9x's native format.

The source is the public **Libretro SNES cheats** directory:
https://github.com/libretro/libretro-database/tree/master/cht/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System

The embedded list is from the upstream directory tree at SHA `6b1f8a463a28d8c3450901844c13112d469e22be`. It has been included locally as `ps5/frontend/data/snes-cheat-index.txt`, and is independent of GitHub API rate limits. Cheat downloads use upstream raw file URLs with encoded filenames. Exact game title/region/revision matches are prioritized, but not every ROM has matching codes or art.

## Fixed downloads and fuzzy title matching

The newer build displays a direct **queued/error** confirmation when pressing Cross in Cheat downloads. Its helper periodically writes `/data/snes9x/cheats/download-worker.ready`. If that heartbeat is missing/stale, the menu reports **Helper NOT READY** and explains that the PS5 must be restarted and the new installer ELF sent again; it will no longer silently accept a request that cannot be processed. Other failures (busy queue, unwritable path, no database match) are also presented on screen.

The emulator now uses a verified snapshot of the **3,689 Libretro SNES box-art filenames** and conservative fuzzy matching (region tags, punctuation, spaces, dashes and small spelling variations), falling back to title/screenshot art as before. Example: `Kidou Butoden G-Gundam.zip` resolves to `Kidou Butouden G Gundam (Japan)`. This corrects common misspellings without changing ROM filenames. Ambiguous or too-dissimilar matches are rejected to avoid showing art from unrelated games.

## Covers

The normal helper will continue automatically downloading missing box art. **R3** retries missing covers in bulk, including previously unsuccessful downloads, and queues them again. When official `Named_Boxarts` lacks an image, the downloader also tries the official Libretro `Named_Titles` and `Named_Snaps` for that game, in that order. These are fallback pictures, **not guaranteed box-art replacements**.

A game without a No-Intro match is attempted by its ROM filename as a best effort. If no official matching image exists, the card remains a placeholder. You can still place a custom `<ROM basename>.png` or `.jpg` under `/data/snes9x/covers/`, or beside the ROM.

## Limitations

The **full database** mode makes thousands of separate HTTPS requests and may consume significant time. Downloads continue in the helper while you use the console, and do not run as downloads on the UI thread. There is no cancellation control yet; the full download uses the explicit **Download ALL SNES cheats** menu item and starts with one press of Cross.

This is a source update only until CI validates the new helper and installer. Test cheat importing, repeated code toggles, menu controls, and covers on a PS5 before treating it as production-ready.

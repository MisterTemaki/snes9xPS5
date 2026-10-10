# SNES cheats and controller shortcuts (PS5)

## In-game controls

- **Pause menu:** L3 + R3 (default, now configurable)
- **Open cheats directly:** L2 + R3 (default)
- **Return directly to the game shelf:** Touchpad + Options (default)
- **Configure all three bindings:** Pause menu > Controller shortcuts; or Triangle from the game shelf > Settings > Controller shortcuts
- **Cheat manager:** Pause menu > Cheat manager, or the configured cheat shortcut.
- Within Cheat manager: D-pad to select, Cross/Left/Right to enable or disable an individual code group; L1/R1 to page; Triangle to enable all; Square to disable all; Circle to return.
- Each change is applied immediately and saved to the game's local native `.cht` file. You do not have to restart the game.
- The shortcuts are distinct combinations selected from a list. The editor skips combinations that are already assigned to another action.
- All three choices persist in `/data/snes9x/snes9x-ps5.ini` as `shortcut_pause`, `shortcut_list`, `shortcut_cheats`.

## Download cheat codes

The public Libretro SNES cheats collection:
https://github.com/libretro/libretro-database/tree/master/cht/Nintendo%20-%20Super%20Nintendo%20Entertainment%20System

Download the database repository ZIP (GitHub > Code > Download ZIP) and extract it on your PC.
The directory to use is `libretro-database/cht/Nintendo - Super Nintendo Entertainment System`.
RetroArch `.cht` syntax is **different** from Snes9x `.cht` syntax; this port detects and imports both.
The console also recognizes region-tagged and similarly named cheat filenames, so exact renaming is no longer required. For a single game:
```
/data/snes9x/roms/Super Mario World.sfc
/data/snes9x/cheats/Super Mario World.cht
```
The cheat manager will show the named Libretro cheats; default-disabled codes stay OFF until selected. Changing a code converts/persists the local file in native Snes9x format.

### Bulk-match cheat files to your ROM library

Use the included **PC-side** installer. It does not download ROMs or alter your console:
```bash
python3 ps5/tools/install_libretro_cheats.py \
  --roms "/path/to/your/snes-roms" \
  --database "/path/to/libretro-database/cht/Nintendo - Super Nintendo Entertainment System" \
  --output "./ps5-cheats"
```
Copy the generated `ps5-cheats/*.cht` into your console's `/data/snes9x/cheats/` folder over FTP. Keep file names unchanged.
Only title matches are copied; match the ROM region for best results. The PC-side copy tool refuses to overwrite existing output files unless `--force` is specified.
Code compatibility depends on the exact ROM revision and cheat type.

**No bundled third-party cheat dump:** Codes remain in the original community-maintained Libretro repository, under its own license/attribution. This project provides a match-and-copy helper and a runtime reader.

**Cheat downloading has been removed from the PS5 menus.** Use FTP or the included PC-side match-and-copy tool for any new files; the installed cheat manager does not need network access.

## Diagnostics

Enable Debug logs in Settings and check `/data/snes9x/logs/boot.log` for
`[cheats] Loaded ...`, `[cheats] imported ...`, `[cheats] saved ...`. A loaded file does not necessarily mean every code is compatible.

## Developer checks

```bash
python3 -m unittest discover -s ps5/tests -p 'test_libretro_import.py'
make -C ps5 -j2 test
make -C ps5 -j2 ps5   # requires the PS5 payload SDK and its libc++/libc runtimes
```
The GitHub Actions workflow builds/tests on Linux and uploads a PS5 installer ELF only if the PS5 build job succeeds. Real-console input/cheat tests are still required.

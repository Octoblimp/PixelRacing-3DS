# PixelRacing-3DS

A native Nintendo 3DS port of *3D Pixel Racing* (Vidia, 2010/2011), built from a functional
decompilation of the iOS 1.1 release, with the Wii version's tilt steering and banner.

**No game data is included.** The build reads your own copies of the game:
- the iOS IPA (`iOS Files & Source/*.ipa`)
- the WiiWare title installed in Dolphin (decrypted contents under
  `Dolphin Emulator/Wii/title/00010001/57334645/content`) and the WAD (for the banner)

## Status
Boots, runs the menus and plays races with CPU racers in Azahar, driven by the physical
buttons (no on-screen touch buttons). Sound plays through ndsp. The bottom screen shows the
3D Pixel Racing logo whenever the game draws nothing there. See `docs/formats/` for the
reversed file formats and `docs/TRANSLATION.md` for how the game code is translated.

## Layout
| Path | What |
|---|---|
| `3DS Files/source/game/` | the translated iOS game code (`m_<addr>.c`, `tick.c`, `engine.c`, `game_main.c`) |
| `3DS Files/source/platform/` | 3DS platform layer: GL ES 1.1 on citro3d, input, sound, UDS |
| `3DS Files/include/` | `ios.h` (image access macros), generated `protos.h`, platform headers |
| `3DS Files/meta/` | `app.rsf`, the icon and the HOME Menu banner (rebuilt from the Wii banner) |
| `3DS Files/romfs/` | generated from your IPA by `tools/build_romfs.py` (not tracked) |

## Building
```
python -I tools/extract_ios.py
python -I tools/banner/make_banner.py --out "3DS Files/meta"
python -I tools/build_romfs.py --out "3DS Files/romfs" --tex3ds C:/devkitPro/tools/bin/tex3ds.exe
powershell -ExecutionPolicy Bypass -File tools/build.ps1          # -> 3DS Files/PixelRacing-3DS.3dsx
powershell -ExecutionPolicy Bypass -File tools/build.ps1 cia      # -> 3DS Files/PixelRacing-3DS.cia
```
devkitPro's Makefile template can't run from a path with spaces, so `tools/build.ps1`
substitutes drive letters around the build. The debug log
(`sdmc:/PixelRacing_debug.txt` for the CIA, `sdmc:/3ds/PixelRacing_debug.txt` for the .3dsx,
previous run in `PixelRacing_debug_prev.txt`) is on by default;
`DEBUG=0` builds it out. `make_banner.py` also renders the bottom-screen logo
that `build_romfs.py` picks up, so run it first.

Controls: in a race the Circle Pad and the D-pad steer, A gas, B brake, R nitro, START pause; in
the menus A/B are OK/back. SELECT opens the port's controls menu on the bottom screen (Circle Pad
or tilt steering, stick sensitivity and dead zone, tilt range, which buttons are gas, brake and
nitro), saved to `sdmc:/3ds/PixelRacing3D/controls.cfg` (`source/game/controls.c`).

## Tools
| Script | What it does |
|---|---|
| `tools/extract_ios.py` | IPA -> `build/ios_raw/` resources + `build/bin/3d_pixel_racing.armv7` |
| `tools/build_romfs.py` | `build/ios_raw/` -> the RomFS (tex3ds textures, models, tracks, audio, text) |
| `tools/build.ps1` | builds `3DS Files` (see above) |
| `tools/run_azahar.ps1` | boots the .3dsx in Azahar, plays a key script and saves screenshots |
| `tools/capture_audio.ps1` | records the PC's audio output (WASAPI loopback) to check the port's sound |
| `tools/pngio.py` | PNG reader/writer that decodes Apple CgBI ("crushed") PNGs |
| `tools/wii_unpack.py` | Dolphin-decrypted Wii contents -> `build/wii/` (LZ77 0x10/0x11, U8) |
| `tools/dol2elf.py` | Wii DOL -> PPC ELF for Ghidra |
| `tools/ghidra/ExportFunctions.java` | headless Ghidra post-script: function table + pseudo-C (kept local) |

Run every Python tool with `python -I`. Requirements: devkitPro (libctru, citro3d, citro2d, tex3ds),
Python 3, Ghidra 12, makerom and bannertool (in `tools/3ds-bin/`, not tracked).

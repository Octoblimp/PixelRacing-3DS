# PixelRacing-3DS

A native Nintendo 3DS port of *3D Pixel Racing* (Vidia, 2010/2011), built from a functional
decompilation of the iOS 1.1 release, with the Wii version's tilt steering and banner.

**No game data is included.** The build reads your own copies of the game:
- the iOS IPA (`iOS Files & Source/*.ipa`)
- the WiiWare title installed in Dolphin (decrypted contents under
  `Dolphin Emulator/Wii/title/00010001/57334645/content`) and the WAD (for the banner)

## Status
Early work: asset pipeline and analysis. See `docs/formats/` for the reversed file formats.

## Tools
| Script | What it does |
|---|---|
| `tools/extract_ios.py` | IPA -> `build/ios_raw/` resources + `build/bin/3d_pixel_racing.armv7` |
| `tools/build_romfs.py` | `build/ios_raw/` -> `build/romfs/` (tex3ds textures, models, tracks, audio, text) |
| `tools/pngio.py` | PNG reader/writer that decodes Apple CgBI ("crushed") PNGs |
| `tools/wii_unpack.py` | Dolphin-decrypted Wii contents -> `build/wii/` (LZ77 0x10/0x11, U8) |
| `tools/dol2elf.py` | Wii DOL -> PPC ELF for Ghidra |
| `tools/ghidra/ExportFunctions.java` | headless Ghidra post-script: function table + pseudo-C (kept local) |

Run every Python tool with `python -I`. Requirements: devkitPro (libctru, citro3d, citro2d, tex3ds),
Python 3, Ghidra 12, makerom and bannertool (in `tools/3ds-bin/`, not tracked).

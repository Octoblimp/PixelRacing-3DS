# 3D Pixel Racing for Nintendo 3DS

An unofficial, fan-made Nintendo 3DS port of *3D Pixel Racing* (Vidia, 2010/2011). It is built
from a functional decompilation of the iOS 1.1 release, and adds the Wii version's tilt
steering and HOME Menu banner, stereo 3D, and local wireless races between 3DS consoles.

Not affiliated with, or endorsed by, the makers or rights holders of *3D Pixel Racing*, or by
Nintendo.

## Features

The whole iOS game: every race mode, all 10 tracks, weather, pits and fuel, CPU racers,
championships, unlocks and the help pages. On top of that, the 3DS port adds:

### Stereo 3D

- Real stereoscopic 3D on the top screen: push up the 3D slider.
- Its own page in the game's Settings (**Settings > 3D**):
  - **3D Depth:** 50% to 150% of the normal strength.
  - **Pop-out:** Low / Normal / High, how far the track comes out of the screen.
  - **3D while tilting:** off by default, because turning the console breaks the 3D picture.
  - **HUD Depth:** floats the position, laps and lap times in front of the race.

### Two screens

- **Top screen:** the race, with your position, laps and best lap.
- **Bottom screen:** in a race, a dashboard with the speed, track map, rev dial with the gear,
  and the fuel gauge. Outside a race it shows the help pages, the wireless lobby, or the 3D
  Pixel Racing logo.
- The help pages (START on the title screen or main menu) are redrawn with the 3DS's buttons,
  and their Next / Back / Exit can be pressed with A / B / START or tapped.
- Pausing shows the frozen race behind "Abort game?".

### Controls

- **Physical buttons only.** There are no touch buttons on screen, and touching the bottom
  screen in a race does nothing.
- **Tilt steering**, like the Wii version, using the console's motion sensor. It reads how far
  you lean the console, so it works however far back you hold it. The horizon rolls with you
  while steering by tilt.
- **Settings > Controls** page:
  - Circle Pad or tilt steering (the D-pad always steers too).
  - Stick sensitivity and dead zone, and tilt range.
  - Which buttons are gas, brake and turbo.
  - A STEERING bar on the bottom screen to try the stick or tilt before racing.

### Local wireless races

- 2 to 4 consoles in the same room, no internet needed. Every player picks their own car;
  the host picks the mode, track and options.
- Each player sees their own car full screen, with the normal one-player dashboard (there is
  no split-screen).
- The race stays in step on every console. If one leaves or switches off, the others carry on.

### Made for the 3DS

- **Old and New 3DS / 2DS.** 60 fps on a New 3DS. The Old 3DS runs at 30 fps with the game
  still running at full speed, inside the standard 64 MB of memory.
- **HOME Menu banner:** the animated banner, jingle and icon, rebuilt from the Wii version's.
- **Saving:** the CIA keeps its progress in the title's own save data, like a retail game. It
  is backed up, moved and deleted with the game.
- **Debug log** that can be turned on in a settings file (see below).

## Controls

| In a race | |
|---|---|
| Circle Pad or D-pad left/right | steer (or tilt the console, if chosen in Settings > Controls) |
| A | gas |
| B | brake / reverse |
| R | turbo (nitro) |
| D-pad up/down | change the view |
| START | pause |

In the menus A is OK and B goes back. START on the title screen or main menu opens the help
pages (A next, B back, START exit; Next / Back / Exit can also be tapped). SELECT in the menus
opens the wireless race panel.

The game's Settings screen has two extra pages at the end of its carousel:

- **Controls:** Circle Pad or tilt steering, stick sensitivity and dead zone, tilt range, and
  which buttons are gas, brake and turbo.
- **3D:** depth, pop-out, 3D while tilting, HUD depth.

## Installing

- **CIA:** install with a CIA installer such as FBI on a 3DS running custom firmware, then
  start it from the HOME Menu. Installing a newer CIA over an older one keeps your progress.
- **.3dsx:** copy it to `sdmc:/3ds/` and start it from the Homebrew Launcher. The .3dsx keeps
  its progress in `sdmc:/3ds/PixelRacing3D/`.

## Wireless races

1. Everyone goes into the menus (any screen after the title) and presses **SELECT**.
2. One player picks **Host**; the others pick **Join** and choose that host from the list.
3. Close the panel with SELECT and go through the menus as usual. Everyone picks their own
   car; the host picks the mode, track and options.
4. The race starts on every console once everyone has started.

If a console leaves or switches off mid-race, the others carry on after about 3 seconds.
Download Play is not supported: every player needs the game installed.

## Settings and the debug log

The port's settings live on the SD card in `sdmc:/3ds/PixelRacing3D/`:

- `controls.cfg`: written by Settings > Controls and Settings > 3D.
- `config.txt`: written on first start. To turn on the **debug log**, change `debug_log 0`
  to `debug_log 1` on a PC and start the game again. The log is written to
  `sdmc:/PixelRacing_debug.txt` (CIA) or `sdmc:/3ds/PixelRacing_debug.txt` (.3dsx), and the
  previous run's log is kept as `PixelRacing_debug_prev.txt`. It is off by default.

When filing a bug, turning the log on and attaching it helps a lot: it records what the game
was doing, frame rate and memory use, and where the game was if it froze.

## Building from source

This repository holds the port's source and tools only. **No game data is included**: the
build needs your own copy of the game.

- The iOS 1.1 IPA, in `iOS Files & Source/`
- For the banner: the WiiWare title's decrypted contents (from Dolphin) and its WAD

Requirements: devkitPro (libctru, citro3d, citro2d, tex3ds), Python 3, and makerom and
bannertool in `tools/3ds-bin/`.

```bash
python -I tools/extract_ios.py
python -I tools/banner/make_banner.py --out "3DS Files/meta"
python -I tools/make_help_pages.py
python -I tools/build_romfs.py --out "3DS Files/romfs" --tex3ds C:/devkitPro/tools/bin/tex3ds.exe
powershell -ExecutionPolicy Bypass -File tools/build.ps1        # 3DS Files/PixelRacing-3DS.3dsx
powershell -ExecutionPolicy Bypass -File tools/build.ps1 cia    # 3DS Files/PixelRacing-3DS.cia
```

devkitPro's Makefile can't run from a path with spaces, so `tools/build.ps1` maps drive letters
around the build. The debug log is built in but off until `config.txt` turns it on;
`make DEBUG=0` leaves it out altogether. `make_banner.py` also renders the bottom-screen logo
that `build_romfs.py` picks up, so run it first.

### Layout

| Path | What |
|---|---|
| `3DS Files/source/game/` | the translated iOS game code (`m_<addr>.c`, `tick.c`, `engine.c`, `game_main.c`) and the port's own menus (`controls.c`, `opt_menu.c`, `net_*.c`) |
| `3DS Files/source/platform/` | 3DS platform layer: OpenGL ES 1.1 on citro3d, input, sound, wireless (UDS) |
| `3DS Files/include/` | `ios.h` (image access macros), generated `protos.h`, platform headers |
| `3DS Files/meta/` | `app.rsf`, the icon and the HOME Menu banner |
| `decomp/` | function prototypes and the symbol table used for the translation |
| `docs/` | `TRANSLATION.md` (how the game code is translated) and the reversed file formats |

### Tools

| Script | What it does |
|---|---|
| `tools/extract_ios.py` | IPA -> extracted resources and the armv7 executable |
| `tools/build_romfs.py` | extracted resources -> the RomFS (tex3ds textures, models, tracks, audio, text) |
| `tools/make_help_pages.py` | redraws the help pages with the 3DS's buttons |
| `tools/make_menu_props.py` | voxel props for the Controls and 3D settings pages |
| `tools/banner/make_banner.py` | HOME Menu banner, icon and bottom-screen logo from the Wii banner |
| `tools/build.ps1` | builds the .3dsx or the CIA |
| `tools/run_azahar.ps1` | boots the .3dsx in Azahar, plays a key script and saves screenshots |
| `tools/pngio.py` | PNG reader/writer that also decodes Apple CgBI ("crushed") PNGs |
| `tools/wii_unpack.py`, `tools/dol2elf.py` | Wii contents and DOL -> files and an ELF for Ghidra |

Run every Python tool with `python -I`.

## Legal

*3D Pixel Racing*, its code, graphics, music, sounds and text belong to their respective rights
holders. This is a non-commercial fan project. The source code here is a reimplementation for
the 3DS; to build it you need a copy of the game you own.

Nintendo 3DS is a trademark of Nintendo.

## AI Disclosure

Claude Opus 5.5 and Claude Fable 5.2 were used to help make this project possible.
Specifically, they decompiled the iOS game in full (and the Wii version's tilt steering and
banner) and ported it to the 3DS, and helped me debug in Azahar, then I tested it by myself
on real hardware (on a New 3DS and an Old 3DS).

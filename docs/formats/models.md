# Model formats (iOS 3D Pixel Racing 1.1)

Each model ships as a pair: `<name>.smd` + `<name>.bin`, plus its texture `<name>.png`.
The game code refers to `.smd` paths (Wii-style directories such as `/models/faces/face_%02d.smd`).
The iOS bundle is flat and holds both files. Verified over all 179 pairs: the `.bin` is a byte-exact
binary bake of the `.smd`. The port ships only the `.bin` (plus `road_00.smd` for reference).

## `.smd`: Valve StudioMDL text, version 1
```
version 1
nodes
  0 "SMDImport"  -1
end
skeleton
time 0
  0 px py pz rx ry rz
end
triangles
<material>                       # repeated per triangle: material line + 3 vertex lines
  bone px py pz nx ny nz u v [links bone weight ...]
end
```
Every model has one node/bone. Material names are mostly the placeholder `a`. The texture is always `<model>.png`.
`road_00` uses `road_00.ifl` (an image-file list: an animated or selectable road texture).

## `.bin`: "SMD Reference bin File", little-endian
| Offset | Size | Field |
|---|---|---|
| 0x00 | 24 | magic `"SMD Reference bin File  "` |
| 0x18 | u32 | version = 1 |
| 0x1C | u32 | vertex count N (= 3 × triangles) |
| 0x20 | 28 | node name `"SMDImport"`, NUL-padded |
| 0x3C | s32 | parent = -1 |
| 0x40 | 6×f32 | skeleton time 0: pos xyz, rot xyz |
| 0x58 | 8 | material (spaces) |
| 0x60 | N×36 | vertices |

Vertex (36 bytes): `s32 bone; f32 pos[3]; f32 normal[3]; f32 uv[2]`. File size is always `96 + 36·N`.
These are non-indexed triangle lists, as drawn by `glDrawArrays(GL_TRIANGLES)` on iOS.

Some models contain coplanar, same-facing triangles that overlap an earlier one (inside a model,
and a car's `_access` / `_window` parts against its `_body`, which the game draws first with the
same transform). With GL_LESS the earlier triangle wins; the 3DS GPU z-fights them instead.
`tools/smd_dedupe.py` (run by `tools/build_romfs.py`) cuts the hidden parts out at build time.

3DS: load the vertex block as is. The attribute buffer starts at +0x64 (skipping the bone int) with a 36-byte stride:
3 floats position, 3 floats normal, 2 floats UV, then one padding word. Alternatively, repack at load time.

## Textures
All 244 PNGs are Apple CgBI ("crushed"): raw deflate, BGRA, premultiplied alpha. `tools/pngio.py`
decodes them. `tools/build_romfs.py` pads 28 NPOT images (UI pages 480×320, fonts, gauges, buttons)
to power-of-two and records `name w h potw poth fmt` in `romfs:/textures.txt`. The GL layer scales
texture coordinates for those by `w/potw`, `h/poth`.

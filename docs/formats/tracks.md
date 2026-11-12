# Track formats (iOS 3D Pixel Racing 1.1)

Ten tracks, `map_00` … `map_09`, loaded via `%s/%s.map`, `%s/%s_grnd.pbm`, `%s/%s_road.pbm`, `%s/%s_tree.pbm`
and `map_XX_objs.txt`. Field meanings marked (?) still need confirming against the loader in Ghidra (#19).

## `map_XX.map` (1202 bytes)
`u8 width (20), u8 height (20)`, then 3 layers of `width × height` bytes, row-major:
1. **Road tiles**: 0 = none, 1 = straight horizontal, 2 = straight vertical, 3/4/5/6 = corners,
   8 = crossing, 9 = water/lake (?). Tile textures `road_00 … road_09` (?).
2. **Terrain height**: 0 = flat, 0x28–0x2D = raised hills (?).
3. **Objects**: 0 = none, otherwise an index into `_objs.txt` (values 0x11–0x22 seen; base offset still to confirm) (?).

## `map_XX_{grnd,road,tree}.pbm`
These are actually binary **PPM (P6)**, 20×20, maxval 255: one RGB colour per tile for the ground,
the road and the trees/scenery. The ground and trees are greens/browns per track, and the road is near-white
(tinted for snow/night).

## `map_XX_objs.txt`
Tab-separated rows: `model  type  count`. The first block lists the near models (`obj_NN`), and a second block
repeats them as `obj_NN_b` (the low-detail/far versions). `-` rows are empty slots. Per-track scenery
models `obj_00`–`obj_35` are in the RomFS.

## Text
- `lang_ENG.txt`: CRLF-separated UI strings, looked up by line index (`language.m`).
- `sys_msg_{ENG,FRA,SPA}.txt`: Wii NAND save-error messages (`NAND_01` …). They prove the iOS build
  kept the Wii save path. The port rewrites them for 3DS save data.

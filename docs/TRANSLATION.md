# Translating the iOS game code to C

This is the rulebook for turning Ghidra's pseudo-C of the iOS armv7 binary into the port's
`3DS Files/source/game/*.c`. The goal is a **functional decompilation**: the same behaviour and data
layout as the original. Byte-matching compiled code is not a goal. Read `3DS Files/include/ios.h` first.

## Inputs (all generated locally, not in git)
| Path | What |
|---|---|
| `decomp/raw/c/<addr>_<name>.c` | Ghidra pseudo-C per function (iOS armv7, softfp) |
| `decomp/raw/dump/00005ac4.txt` | the 17 KB game tick (`FUN_00005ac4`), disassembly + pseudo-C |
| `decomp/raw/asm/<addr>.s` | Thumb-2 disassembly per function. **Ground truth.** Literal-pool loads are annotated with their value (`; [00012e14] = 0x41a00000 (20f)`), as are string literals and call targets (`; _glEnable`, `; FUN_...`) |
| `decomp/raw/consts.txt` | `TEXT addr u32 float`: literal-pool words. `PTR addr -> target "string"`: pointer slots |
| `decomp/signatures.tsv` | softfp float-parameter / float-return hints per function |
| `decomp/raw_wii/c/*.c` | Ghidra pseudo-C of the **Wii** build of the same game (PowerPC, big-endian, clean float code). Use it to understand float math and intent. Find a function's counterpart through a distinctive string, constant or call shape. The iOS code is authoritative where the two differ (touch UI, iOS menus) |
| `3DS Files/include/protos.h` | generated prototypes for every game function (`fn_<addr>`) |

## Output conventions
- One file per module, `3DS Files/source/game/m_<start addr>.c` (the tick is `tick.c`, the engine core `engine.c`).
  `#include "ios.h"` and nothing game-specific beyond it.
- Every function `FUN_xxxxxxxx` becomes `fn_xxxxxxxx`, not static, in address order. Put a
  one-line comment above each: `/* 0x0000a184 -- loads the save slot (guess) */`.
- Module-private helpers may be `static` with a module prefix. No new globals for game state:
  game state lives in the image (`G_*` macros). Small `static` platform caches are fine.
- **Prototypes:** if `protos.h` is wrong for a function **your module defines** (a float param,
  a pointer, the return type, the arg count), write the right one in
  `decomp/protos/<module>.tsv` (`addr<TAB>prototype`, e.g.
  `0000d800	void fn_0000d800(int car, float x, float y, float z, int flags)`). `tools/check_module.sh`
  regenerates `protos.h`. Never edit `protos.h` by hand, and never change prototypes of
  functions other modules own: call them as declared and list any you think are wrong in your report.
- Integers vs pointers: both are 32-bit on iOS and the 3DS. Prefer real pointer types in your
  own code; cast at calls to `int`-typed prototypes.
- LF line endings, 4-space indent, ~100 columns.

## Reading Ghidra's softfp/NEON output
Clang compiled scalar float math into 2-lane NEON ops on d-registers. Ghidra shows it as:

| Ghidra | Meaning |
|---|---|
| `CONCAT44(hi, lo)` | a d-register; **lane 0 = `lo`** (the low 32 bits). The high lane is almost always junk |
| `FloatVectorMult(a,b,2,0x20)` / `Add` / `Sub` / `Neg` / `Absolute` | lane-wise float ops. Take lane 0: `lo(a) * lo(b)` |
| `FixedToFP(CONCAT44(_, x),0,…)` | int → float (`vcvt.f32.s32` or `.u32`: check the .s for signedness) |
| `FPToFixed(x,0,…)` | float → int, truncating toward zero (`vcvt.s32.f32`) |
| `(int)uVar13` stored through `*(int *)p` | storing the **float bits** of lane 0: write `AT_F32(p, 0) = value` |
| `in_d8`…`in_d15`, `in_s9`, `extraout_r1` | register values Ghidra could not track; usually junk high lanes or callee-saved temps. Work out the real value from the .s |
| `___divsi3(a,b)`, `___modsi3(a,b)`, `___udivdi3` | `a / b`, `a % b`, unsigned 64-bit divide |
| `___sprintf_chk(buf, 0, n, fmt, …)` | `snprintf(buf, n, fmt, …)` (or `sprintf` when `n` is -1) |
| `___strcpy_chk`, `___strcat_chk` | `strcpy`, `strcat` |
| `DAT_0000xxxx` below `0x3b000` | a **text literal**: write the value (from consts.txt / the .s comment), usually a float like `20.0f` |
| `&DAT_0000xxxx` / `&UNK_0000xxxx` used as a number | an **integer constant** that happens to be a text address (e.g. `&DAT_00008074` is `GL_VERTEX_ARRAY` 0x8074, `&UNK_00007530 + n * 3000` is `30000 + n * 3000`) |
| `DAT_0003xxxx`…`DAT_0008xxxx` | a global in the image: `G_S32(0x3fa24)`, `G_F32(…)`, … pick the type from use |
| `PTR_DAT_0003d2cc` | the **address** `0x4268c` (consts.txt `PTR`): `G(0x4268c)` |
| `*DAT_0003f13c = 1` (a slot in `0x3e92c`–`0x3f3a0` dereferenced) | the slot holds the address of another global. The .s comment shows it (`; =0x0003f13c (ptr slot -> 0x000xxxxx)`). Write the target directly: `G_U8(0x0xxxxx) = 1`. `G_PTR(0x3f13c)` also works (the table is relocated at load) |
| `movw rX,#lo / movt rX,#hi / add rX, pc` | PC-relative address. The .s comment gives the result (`; =0x0003babe "%s_%02d%s"`) |

When the pseudo-C is unclear (it often is in float-heavy code), read the .s: follow
`vldr`/`vmov`/`vmul.f32`/`vadd.f32`/`vcmpe`+`vmrs` (float compares) directly. A `vcmpe` +
`vmrs APSR_nzcv, fpscr` + `bgt/ble/bmi/bpl` pair is a float comparison. Watch NaN-sensitive
choices (`bmi` vs `blt`), but plain `<`/`>` is fine.

## Known globals (add what you find to your report)
| Address | Meaning |
|---|---|
| `0x4268c` | touch table: 10 × 0x14 bytes `{u8 active; float x@4, y@8, prevx@0xc, prevy@0x10}` in virtual-screen units |
| `0x42754` | accelerometer `float[3]` (`(int)(g·90)` as float; x = steering tilt) |
| `0x40368` / `0x40374` | virtual screen width / height (int) |
| `0x3e8c0` | frame rate (20). Only drawFrame uses it; the game ticks at 60 Hz |
| `0x4060c` | texture table (slot `0x3e960`), indexed by the LoadTPL id |
| `0x3e9dc` | the integer 180000 (not a pointer, though it sits in the slot table) |

## Things that are not plain game logic
- **Strings**: write literals. `__assert_rtn(...)` → drop it, or keep the condition as
  `if (!(cond)) sys_log("assert: ...")`. **Never copy the file paths in assert strings** (they
  contain the original developer's home directory).
- **`objc_msgSend` / NSBundle / NSString** in game code: resource paths go through
  `ios_res_path()`, documents through `ios_doc_dir()`; anything else, leave a
  `/* TODO(platform): ... */` and report it.
- **`mach_absolute_time`** → `ios_ms()`. **`rand`/`srand`** keep as is.
- **GL calls** stay GL calls (`gles.h` implements them on the 3DS GPU). Do not adapt screen
  sizes or layouts: the engine layer maps the iOS virtual screen onto the 3DS screens.
- **Sound** (`0x36330`–`0x37120`, Apple's SoundEngine), **language/save/files**
  (`0x35df8`–`0x36330`), **MTX/VEC/model** (`0x37148`–`0x38c04`) and **engine helpers**
  (`0x3ccc`–`0x5ac4`) are owned by other modules. Call them through their `fn_` prototypes.

## Checking your work
```sh
sh tools/check_module.sh "3DS Files/source/game/m_0a184.c"
```
This must compile with no errors and no warnings you can fix. Before you finish, re-read every
function against its `.s` for control flow (every branch/case present), constants (every
literal), and global offsets. This is the review the module gets, so be thorough.

## Report
At the end, report:
1. each function: address, a short role name (e.g. `Race_UpdateLapCounter`), confidence;
2. the prototype changes you wrote, and any wrong prototypes of other modules' functions you
   noticed;
3. the globals you identified (address → meaning), especially ones shared with other modules;
4. TODOs left in the code and why.

/* Shared base for the translated iOS game code (src/game/ *.c).
 *
 * MEMORY IMAGE. The iOS binary keeps all game state in fixed globals. They are not given C
 * names here: the translation addresses them through an image of the iOS address range
 * [IMG_LO, IMG_HI) -- __cstring/__const and the whole __DATA segment -- loaded at boot from
 * romfs:/ios_image.bin (tools/build_romfs.py cuts it from the user's own binary). The image's
 * pointer table at 0x3e92c..0x3f3a0 (globals that hold the address of another global, e.g.
 * *0x3e960 = texture table) is relocated at load, so G_PTR() of such a slot is a real pointer.
 * __common/__bss start zeroed, as on iOS.
 *
 *     Ghidra:  DAT_0003fa24 = 3;          ->  G_S32(0x3fa24) = 3;
 *              *(float *)(DAT_0003f15c+8) ->  ((float *)G_PTR(0x3f15c))[2]
 *              &DAT_00042754              ->  G(0x42754)
 *
 * Text-segment literals (DAT_0000xxxx below 0x3b000, consts.txt "TEXT") are compile-time
 * constants: write the value. PTR_ entries (consts.txt "PTR") are addresses: G(target).
 *
 * Pointers are 32-bit on both iOS (armv7) and the 3DS, so a pointer read from / stored into
 * the image keeps its layout; structs inside the image keep the iOS offsets.
 *
 * FUNCTIONS are named fn_<8-digit hex address> with the exact prototype from protos.h
 * (generated) or the module's own header. Imports (gl*, sinf, sprintf...) keep their names. */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gles.h"
#include "sys.h"

#define IMG_LO 0x3b000u
#define IMG_HI 0x8c000u

extern uint8_t *g_img;

#define G(a) ((void *)(g_img + ((uint32_t)(a) - IMG_LO)))
#define G_U8(a) (*(uint8_t *)G(a))
#define G_S8(a) (*(int8_t *)G(a))
#define G_U16(a) (*(uint16_t *)G(a))
#define G_S16(a) (*(int16_t *)G(a))
#define G_U32(a) (*(uint32_t *)G(a))
#define G_S32(a) (*(int32_t *)G(a))
#define G_F32(a) (*(float *)G(a))
#define G_PTR(a) (*(void **)G(a))
/* iOS address of a pointer into the image (for code that compares addresses). */
#define IMG_ADDR(p) ((uint32_t)((const uint8_t *)(p) - g_img) + IMG_LO)

/* Typed access through a base pointer + byte offset (struct fields at iOS offsets). */
#define AT_U8(p, o) (*(uint8_t *)((uint8_t *)(p) + (o)))
#define AT_S8(p, o) (*(int8_t *)((uint8_t *)(p) + (o)))
#define AT_U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define AT_S16(p, o) (*(int16_t *)((uint8_t *)(p) + (o)))
#define AT_U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define AT_S32(p, o) (*(int32_t *)((uint8_t *)(p) + (o)))
#define AT_F32(p, o) (*(float *)((uint8_t *)(p) + (o)))
#define AT_PTR(p, o) (*(void **)((uint8_t *)(p) + (o)))

/* Bit casts (softfp code moves floats through integer registers). */
static inline float u2f(uint32_t u) { union { uint32_t u; float f; } c = {.u = u}; return c.f; }
static inline uint32_t f2u(float f) { union { uint32_t u; float f; } c = {.f = f}; return c.u; }

/* Wii-style 3x4 matrix used by the MTX* shim (row-major, translation in column 3). */
typedef float Mtx34[3][4];
typedef struct { float x, y, z; } Vec3;

/* Platform services the game calls instead of iOS frameworks (src/platform). */
void ios_image_init(void);              /* game_main.c */
void *ios_alloc(uint32_t size);         /* linear (GPU-readable) heap; CxAlloc32Mem1 */
void ios_free(void *p);
/* Path of a bundle resource: "foo.png" -> "romfs:/foo.t3x" (textures) or "romfs:/foo". */
const char *ios_res_path(const char *name, char *buf, int bufsize);
/* Directory for the port's own files (controls.cfg) on the SD card, without trailing slash. */
const char *ios_doc_dir(void);
/* Directory for the game's save file ("NSDocumentDirectory", sg.m), without trailing slash:
 * the CIA's own save data, else the SD card. ios_save_commit() after writing a file. */
const char *ios_save_dir(void);
void ios_save_commit(void);
uint32_t ios_ms(void);                  /* mach_absolute_time in ms */

#include <protos.h> /* angle brackets: tools/check_module.sh points -I at a private copy */

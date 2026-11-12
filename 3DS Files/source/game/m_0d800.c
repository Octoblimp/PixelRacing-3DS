/* m_0d800.c -- terrain height/surface query, race setup (asset loading, opponent roster,
 * camera/viewport setup, race-state reset), track-select preview. iOS range
 * [0x0000d800, 0x00012a68).
 *
 * Shared state used here (iOS addresses):
 *   0x48ae0 car array, G_S32(0x3e9c8) cars of 0x678 bytes; 0x559e0 race-restart backup copy
 *   0x44000 height grid (byte, x100 = world height), 0x44400 surface-kind index grid,
 *   0x44800 road tile type grid, 0x46400 section (racing-line order) grid: 20 x 20, [ix*20+iz]
 *   0x45640 racing line: {float ix, float iz} per section, G_S32(0x3f948) sections
 *   0x44590 4 lane slots of 0x34 bytes (+0x14 claimant car, +0x18, +0x1c holder car)
 *   0x47544 roster: per car 6 ints {car model, colour, face, team, flag, char name[4]}
 */
#include "ios.h"

#define CAR_SIZE 0x678
#define CAR(i) ((uint8_t *)G(0x48ae0) + (i) * CAR_SIZE)
#define CAR_BAK(i) ((uint8_t *)G(0x559e0) + (i) * CAR_SIZE)
#define NCARS G_S32(0x3e9c8)
#define NPLAYERS G_S32(0x3e9b0)

/* Game-mode ids live in globals (compared against the current mode G_S32(0x3f9dc)). */
#define MODE_CUR G_S32(0x3f9dc)
#define MODE_A G_S32(0x3ebe0) /* 1 */
#define MODE_B G_S32(0x3ebe4) /* 2 */
#define MODE_C G_S32(0x3ebe8) /* 3 */
#define MODE_D G_S32(0x3ebec) /* 4 */
#define MODE_E G_S32(0x3f9d8)

#define TRACK_DIR G_S32(0x3e9d0) /* +1 / -1: track driven forwards or reversed */
#define NSECT G_S32(0x3f948)     /* racing-line section count */
#define GRID_MAXX G_S32(0x3f8a4)
#define GRID_MAXZ G_S32(0x3f8a0)

#define GRID_STRIDE 0x14
#define GRID_HGT ((uint8_t *)G(0x44000))  /* slot 0x3f09c */
#define GRID_KIND ((uint8_t *)G(0x44400)) /* slot 0x3f0a8: index into KIND_TAB */
#define GRID_TILE ((uint8_t *)G(0x44800)) /* slot 0x3f0b8 */
#define GRID_SECT ((uint8_t *)G(0x46400)) /* slot 0x3f0c8 */
#define KIND_TAB ((int32_t *)G(0x473b4))  /* slot 0x3f154 */
#define LINE_PTS ((float *)G(0x45640))    /* slot 0x3f0c0 */
#define LANES ((uint8_t *)G(0x44590))     /* slot 0x3f0ac: 4 x 0x34 */
#define LANE_SIZE 0x34
#define ROSTER ((int32_t *)G(0x47544))    /* slot 0x3f168: 6 ints per car */

#define TILE 812.0f
#define HALF_TILE 406.0f

/* Clamp to [0, hi] the way the game does (negative -> 0, then min). */
static int m0d800_clamp(int v, int hi)
{
    if (v < 0)
        return 0;
    return v > hi ? hi : v;
}

/* Height grid cell (clamped), in grid units (x100 = world units). */
static float m0d800_hgt(int ix, int iz)
{
    return (float)GRID_HGT[m0d800_clamp(ix, GRID_MAXX) * GRID_STRIDE +
                           m0d800_clamp(iz, GRID_MAXZ)];
}

/* Road tile type of a cell (clamped). */
static int m0d800_tile(int ix, int iz)
{
    return GRID_TILE[m0d800_clamp(ix, GRID_MAXX) * GRID_STRIDE + m0d800_clamp(iz, GRID_MAXZ)];
}

/* Surface bump id written through fn_0000d800's out parameter. */
static int m0d800_bump(float v)
{
    return (int)v % 10 + 1;
}

/* 0x0000d800 -- terrain height at (x, z) for a car (or -1); writes a surface bump id to *out */
float fn_0000d800(int car, float x, float y, float z, int *out)
{
    int ix = (int)((x + HALF_TILE) / TILE);
    int iz = (int)((z + HALF_TILE) / TILE);
    float x0 = (float)ix * TILE - HALF_TILE;
    float z0 = (float)iz * TILE - HALF_TILE;
    float lx = x - x0, lz = z - z0;
    int inside = (ix | iz) >= 0 && ix <= GRID_MAXX && iz <= GRID_MAXZ;
    int kind = KIND_TAB[inside ? GRID_KIND[ix * GRID_STRIDE + iz] : 0];
    int tile = m0d800_tile(ix, iz);
    int sect = inside ? GRID_SECT[ix * GRID_STRIDE + iz] : 0;
    int last = NSECT - 1;
    int finale = sect >= last && G_S32(0x3f8e0) != 0;
    float hc = m0d800_hgt(ix, iz);
    float h = 0.0f, adj = 0.0f, e = 0.0f;

    /* base height and bump id from the road tile shape */
    switch (tile) {
    case 0:
    case 10:
        h = hc * 100.0f;
        *out = m0d800_bump(lz);
        break;
    case 1:
    case 2: {
        /* ramp along z (tile 1) or x (tile 2) towards a higher neighbour */
        int along_x = tile == 2;
        float l = along_x ? lx : lz;
        float s = l * 100.0f / TILE;
        float hn = along_x ? m0d800_hgt(ix + 1, iz) : m0d800_hgt(ix, iz + 1);
        float hp = along_x ? m0d800_hgt(ix - 1, iz) : m0d800_hgt(ix, iz - 1);
        int tn = along_x ? m0d800_tile(ix + 1, iz) : m0d800_tile(ix, iz + 1);
        int tp = along_x ? m0d800_tile(ix - 1, iz) : m0d800_tile(ix, iz - 1);
        float edge = finale ? 20.0f : 180.0f;
        float side = along_x ? lz : lx;

        if (hn > hc && tn != 7 && tn != 8)
            h = hc * 100.0f + s * (hn - hc);
        else if (hp > hc && tp != 7 && tp != 8)
            h = hp * 100.0f - s * (hp - hc);
        else
            h = hc * 100.0f;
        if (side < edge || side > TILE - edge)
            *out = m0d800_bump(along_x ? lx : lz);
        break;
    }
    case 3:
        if (lx < 180.0f)
            *out = m0d800_bump(lz);
        if (lz > 632.0f)
            *out = m0d800_bump(lx);
        if (lx > 632.0f && lz < 180.0f)
            *out = m0d800_bump(lz + lx);
        h = hc * 100.0f;
        break;
    case 4:
        if (lx < 180.0f)
            *out = m0d800_bump(lz);
        if (lz < 180.0f)
            *out = m0d800_bump(lx);
        if (lx > 632.0f && lz > 632.0f)
            *out = m0d800_bump(lz + lx);
        h = hc * 100.0f;
        break;
    case 5:
        if (lx > 632.0f)
            *out = m0d800_bump(lz);
        if (lz > 632.0f)
            *out = m0d800_bump(lx);
        if (lx < 180.0f && lz < 180.0f)
            *out = m0d800_bump(lz + lx);
        h = hc * 100.0f;
        break;
    case 6:
        if (lx > 632.0f)
            *out = m0d800_bump(lz);
        if (lz < 180.0f)
            *out = m0d800_bump(lx);
        if (lx < 180.0f && lz > 632.0f)
            *out = m0d800_bump(lz + lx);
        h = hc * 100.0f;
        break;
    case 7: {
        /* bridge crossing: upper deck along z, lower road along x */
        int a = (int)(m0d800_hgt(ix + 1, iz) * 100.0f);
        int b = (int)(m0d800_hgt(ix, iz + 1) * 100.0f);
        float mid = hc * 100.0f + (float)((b - a) / 2);
        if (y <= mid) {
            h = (float)a;
            if (lz < 180.0f || lz > 632.0f)
                *out = m0d800_bump(lx);
            if (car >= 0)
                AT_S32(CAR(car), 0x6c) = 1;
        } else {
            h = (float)b;
            if (lx < 180.0f || lx > 632.0f)
                *out = m0d800_bump(lz);
            if (lx < 50.0f || lx > 762.0f)
                h = (float)(b + 30);
        }
        break;
    }
    case 8: {
        int a = (int)(m0d800_hgt(ix + 1, iz) * 100.0f);
        int b = (int)(m0d800_hgt(ix, iz + 1) * 100.0f);
        float mid = hc * 100.0f + (float)((a - b) / 2);
        if (y < mid) {
            h = (float)b;
            if (lx < 180.0f || lx > 632.0f)
                *out = m0d800_bump(lz);
            if (car >= 0)
                AT_S32(CAR(car), 0x6c) = 1;
        } else {
            h = (float)a;
            if (lz < 180.0f || lz > 632.0f)
                *out = m0d800_bump(lx);
            if (lz < 50.0f || lz > 762.0f)
                h = (float)(a + 30);
        }
        break;
    }
    default:
        h = 0.0f;
        break;
    }

    /* raise the floor near a higher neighbouring cell (kerb/wall slope), 40 units wide */
    if (sect < last || G_S32(0x3f8e0) == 0) {
        float hn;
        if (lx < 40.0f) {
            hn = m0d800_hgt(ix - 1, iz);
            adj = hc < hn ? (40.0f - lx) * ((hn - hc) * 100.0f) / 40.0f : 0.0f;
        }
        if (lx > 772.0f) {
            hn = m0d800_hgt(ix + 1, iz);
            if (hc < hn)
                adj = (lx - 772.0f) * ((hn - hc) * 100.0f) / 40.0f;
        }
        if (lz < 40.0f) {
            hn = m0d800_hgt(ix, iz - 1);
            if (hc < hn)
                adj = (40.0f - lz) * ((hn - hc) * 100.0f) / 40.0f;
        }
        if (lz > 772.0f) {
            hn = m0d800_hgt(ix, iz + 1);
            if (hc < hn)
                adj = (lz - 772.0f) * ((hn - hc) * 100.0f) / 40.0f;
        }
    }

    /* extra height from the cell's surface kind (kerbs, walls, jumps) */
    switch (kind) {
    case 6:
        if (tile == 1) {
            if (lx < 50.0f || lx > 762.0f) {
                float t = (float)(int)(lz + (float)TRACK_DIR * HALF_TILE);
                if (t > 356.0f && t < 456.0f)
                    e = 100.0f;
            }
        } else if (tile == 2) {
            float t = (float)(int)(lx + (float)TRACK_DIR * -HALF_TILE);
            if (t > 356.0f && t < 456.0f && (lz < 50.0f || lz > 762.0f))
                e = 100.0f;
        }
        break;
    case 7:
        if (sect < last || G_S32(0x3f8e0) == 0) {
            if (lx > 772.0f && (tile == 1 || tile == 5 || tile == 6))
                e = 100.0f;
            if (lx < 40.0f && (tile == 1 || tile == 3 || tile == 4))
                e = 100.0f;
            if (lz > 772.0f && (tile == 2 || tile == 3 || tile == 5))
                e = 100.0f;
            if (lz < 40.0f && (tile == 2 || tile == 4 || tile == 6))
                e = 100.0f;
        }
        break;
    case 8:
    case 11:
        if (sect < last) {
            if (tile == 1) {
                if ((lx < 40.0f || lx > 772.0f) && lz > 356.0f && lz < 456.0f)
                    e = 100.0f;
            } else if (tile == 2) {
                if (lx > 356.0f && lx < 456.0f && (lz < 40.0f || lz > 772.0f))
                    e = 100.0f;
            }
        }
        break;
    case 9:
        if (G_S32(0x3f97c) > 0) {
            /* jump ramps: 3 objects of 0xac bytes at 0x47f98 (slot 0x3f364) */
            float v = (float)((int)(lz + lx) % 2 + 1);
            uint8_t *o = G(0x47f98);
            for (int i = 0; i < G_S32(0x3f97c); i++, o += 0xac) {
                float zc, y0, ox;
                if (AT_S32(o, 0x24) == 0 || AT_S32(o, 0x04) != iz)
                    continue;
                zc = AT_F32(o, 0x18);
                y0 = AT_F32(o, 0x14);
                if (!(zc - 50.0f < z && z < zc + 50.0f && y0 < y && y < y0 + 100.0f))
                    continue;
                ox = AT_F32(o, 0x10);
                if (AT_S32(o, 0x20) != 0 && ox + (float)AT_S32(o, 0x08) < x) {
                    if (x < ox + (float)AT_S32(o, 0x0c))
                        e = (80.0f - fabsf(z - zc)) * 50.0f / 30.0f;
                    else
                        e = v;
                } else {
                    e = v;
                }
            }
        }
        break;
    default:
        break;
    }

    /* last sections of a pit/lane race: lane slots on the finishing straight */
    if (G_S32(0x3f8e0) != 0 && car >= 0 && G_U8(0x3fab8) == 0 && sect >= last) {
        uint8_t *c = CAR(car);
        int first, end, i;
        float pos[3], a[3], b[3], cp[3];
        int zin = lz < 732.0f && lz > 80.0f;
        int xin = lx < 732.0f && lx > 80.0f && tile == 2;

        if (sect == NSECT) {
            first = 0;
            end = 2;
        } else if (sect == last) {
            first = 2;
            end = 4;
        } else {
            first = 0;
            end = 4;
        }
        if (end < first + 1)
            end = first + 1;
        pos[0] = x0 + HALF_TILE;
        pos[1] = h;
        pos[2] = z0 + HALF_TILE;
        for (i = first; i != end; i++) {
            uint8_t *lane = LANES + i * LANE_SIZE;
            float e0 = e, d;
            int hit = -1;

            fn_0000fb78(i, tile, (int)(uintptr_t)a, (int)(uintptr_t)b);
            fn_00037dec(pos, a, a);
            cp[0] = AT_F32(c, 0x2bc);
            cp[1] = a[1];
            cp[2] = AT_F32(c, 0x2c4);
            d = fn_00037fa4(cp, a);
            if (d < 100.0f) {
                e = 100.0f;
                if (AT_S32(c, 0x70) == 0)
                    continue;
            }
            if (tile == 1) {
                if (zin) {
                    if (TRACK_DIR == -1) {
                        if (lx < 160.0f && (i | 2) == 2)
                            hit = car;
                        if (lx > 652.0f && (i | 2) == 3)
                            hit = car;
                    } else {
                        if (lx < 160.0f && (i | 2) == 3)
                            hit = car;
                        if (lx > 652.0f && (i | 2) == 2)
                            hit = car;
                    }
                }
            } else if (xin) {
                if (TRACK_DIR == 1) {
                    if (lz < 180.0f && (i | 2) == 3)
                        hit = car;
                    if (lz > 632.0f && (i | 2) == 2)
                        hit = car;
                } else {
                    if (lz < 180.0f && (i | 2) == 2)
                        hit = car;
                    if (lz > 632.0f && (i | 2) == 3)
                        hit = car;
                }
            }
            if (hit >= 0) {
                AT_S32(c, 0x78) = i;
                if (AT_S32(c, 0x70) == 0 && AT_S32(lane, 0x14) == -1)
                    AT_S32(lane, 0x14) = car;
            }
            if ((unsigned)(AT_S32(c, 0x84) - 1) < 9 && AT_S32(c, 0x74) >= 0) {
                int k = AT_S32(c, 0x74);
                AT_S32(c, 0x74) = -1;
                AT_S32(LANES + k * LANE_SIZE, 0x1c) = -1;
                AT_S32(c, 0x70) = 0;
            }
            e = d < 10.0f ? AT_F32(c, 0x7c) : e0;
        }
        if (AT_S32(c, 0x30) != -1) {
            int j = AT_S32(c, 0x78);
            if (j >= 0) {
                AT_S32(c, 0x74) = j;
                AT_S32(LANES + j * LANE_SIZE, 0x1c) = car;
            } else {
                int k = AT_S32(c, 0x74);
                if (k >= 0 && AT_S32(LANES + k * LANE_SIZE, 0x14) == car) {
                    AT_S32(c, 0x74) = -1;
                    AT_S32(LANES + k * LANE_SIZE, 0x1c) = -1;
                    AT_S32(c, 0x70) = 0;
                }
            }
        }
        adj = 0.0f;
    }

    {
        float floor_h = hc * 100.0f + adj;
        if (floor_h > h)
            h = floor_h;
    }
    return h + e;
}

/* 0x0000f9a8 -- point a car at the racing-line section delta ahead of its checkpoint
 * (heading), and refresh its grid section and tile */
void fn_0000f9a8(int car, int delta)
{
    uint8_t *c = CAR(car);
    int idx = AT_S32(c, 0x120) + delta;
    int ix, iz, inside, sect;
    float tgt[3], neg[3], dir[3];

    if (idx > NSECT)
        idx = 1;
    else if (idx <= 0)
        idx = NSECT;
    tgt[0] = (float)(int)((float)(int)LINE_PTS[idx * 2] * TILE);
    tgt[1] = 0.0f;
    tgt[2] = (float)(int)((float)(int)LINE_PTS[idx * 2 + 1] * TILE);
    neg[0] = -AT_F32(c, 0x2b0);
    neg[1] = 0.0f;
    neg[2] = -AT_F32(c, 0x2b8);
    fn_00037dec(tgt, neg, dir);
    if (fn_00037e54(dir) > 0.0f) {
        fn_000379a8(dir, dir);
        AT_F32(c, 0x244) = atan2f(dir[0], dir[2]) * 57.2958f;
    } else {
        AT_F32(c, 0x244) = 0.0f;
    }

    ix = AT_S32(c, 0x44);
    iz = AT_S32(c, 0x48);
    inside = (ix | iz) >= 0 && ix <= GRID_MAXX && iz <= GRID_MAXZ;
    sect = inside ? GRID_SECT[ix * GRID_STRIDE + iz] : 0;
    AT_S32(c, 0x80) = sect;
    AT_S32(c, 0x84) = sect;
    AT_S32(c, 0x64) = m0d800_tile(ix, iz);
}

/* 0x0000fb78 -- lane-slot offset (off) and rotation (rot) for lane i on a straight tile */
void fn_0000fb78(int i, int tile, int off_, int rot_)
{
    float *off = (float *)(uintptr_t)off_;
    float *rot = (float *)(uintptr_t)rot_;
    float w = (i & 1) ? -312.30768f : 312.30768f;

    rot[0] = rot[1] = rot[2] = 0.0f;
    off[0] = off[1] = off[2] = 0.0f;
    if (tile == 2) {
        rot[1] = (float)(TRACK_DIR * -90);
        off[2] = (float)TRACK_DIR * w;
        off[0] = 0.0f;
    } else if (tile == 1) {
        rot[1] = (float)(TRACK_DIR * 90 - 90);
        off[0] = (float)TRACK_DIR * w;
        off[2] = 0.0f;
    }
}

/* 0x0000fc28 -- 2D pass: full viewport, draw texture 0xa4 (256x256), probe pixel (0,0x80)
 * into the RGBA buffer at 0x3eb8c */
void fn_0000fc28(void)
{
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    fn_00005068(1);
    fn_00004f24();
    fn_00003f7c(0.0f, 0.0f, 1.0f, 1.0f);
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    fn_00005354(0xa4, 0.0f, 0.0f, 256.0f, 256.0f, 0.95f, 0);
    fn_00003f24(0, 0x80, (int)(uintptr_t)G(0x3eb8c));
}

/* 0x0000fc98 -- 3D setup: light position/direction, depth test, clear colour, race
 * projection (depends on the number of split screens) */
void fn_0000fc98(void)
{
    float *lpos = G(0x4829c);
    float *ldir = G(0x482a8);
    float w, h, fov, far_z;

    lpos[0] = 1000.0f;
    lpos[1] = 4000.0f;
    lpos[2] = 1000.0f;
    ldir[0] = -1000.0f;
    ldir[1] = -4000.0f;
    ldir[2] = -1000.0f;
    fn_000379a8(ldir, ldir);
    glEnable(0xb71); /* GL_DEPTH_TEST */
    fn_000042b0(1, 0x201, 1);
    fn_00003da4((int)G_U32(0x3eb78));

    w = (float)G_S32(0x40368);
    h = (float)G_S32(0x40374);
    fov = 90.0f;
    switch (NPLAYERS) {
    case 3:
        far_z = 3900.0f;
        break;
    case 4:
        far_z = 3800.0f;
        break;
    case 2:
        fov = 55.0f;
        h = (float)G_S32(0x40378);
        far_z = 4000.0f;
        break;
    default:
        far_z = 4500.0f;
        break;
    }
    fn_00037c78(G(0x42878), fov, w / h, 10.0f, far_z);
}

/* 0x0000fe34 -- pack race settings and records into the save buffer, write "RaceData" */
void fn_0000fe34(void)
{
    uint8_t *buf = *(uint8_t **)G(0x477ec); /* slot 0x3f184 */
    int i;

    buf[0] = (uint8_t)(int)G_F32(0x3e9b8);
    buf[1] = (uint8_t)G_S32(0x3e9e0);
    buf[2] = (uint8_t)G_S32(0x3ea20);
    buf[3] = (uint8_t)G_S32(0x3e9e4);
    buf[4] = (uint8_t)G_S32(0x3f8bc);
    buf[5] = (uint8_t)(int)G_F32(0x3e9bc);
    buf[6] = (uint8_t)G_S32(0x3f8ac);
    for (i = 0; i < 10; i++)
        buf[10 + i] = (uint8_t)((int32_t *)G(0x481f4))[i];
    for (i = 0; i < 11; i++)
        buf[0x1e + i] = (uint8_t)((int32_t *)G(0x4819c))[i];
    for (i = 0; i < 10; i++)
        buf[0x32 + i] = (uint8_t)((int32_t *)G(0x481c8))[i];
    if (G_S32(0x3f8c4) == 0)
        fn_000361d4("RaceData", buf, 0x80);
}

/* 0x0000ff60 -- LoadTPL(id, path, flags): load a texture into texture slot id */
void fn_0000ff60(int id, const char *name, int flags)
{
    char buf[256];

    if (id > 0x103) {
        sys_log("assert: LoadTPL id<260 (%d)\n", id);
        return;
    }
    if (id < 0) {
        sys_log("assert: LoadTPL id>=0 (%d)\n", id);
        return;
    }
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    fn_00005660(name, buf);
    fn_00004df0(id, (int)(uintptr_t)buf, flags);
}

/* 0x0000fff0 -- load a text RGB image ("name w h ? rgbrgb...") into one of the 20x20 RGBA
 * map layers (mode 0: 0x43800, 1: 0x46800, 2: 0x45000), mirrored horizontally */
void fn_0000fff0(const char *name, int mode)
{
    char *p = NULL;
    char tag[12];
    int w = 0, hgt = 0, third = 0, r, col;
    size_t len;
    uint8_t *base;
    const uint8_t *src;

    fn_00005784(&p, 0, name);
    sscanf(p, "%s", tag);
    len = strlen(tag);
    sscanf(p + len, "%d", &w);
    sscanf(p + len + 3, "%d", &hgt);
    sscanf(p + len + 6, "%d", &third);
    src = (const uint8_t *)p + len + 0xb;
    for (r = 0; r < hgt; r++) {
        int wmax = G_U8(0x3e9a4);
        for (col = 0; col < w; col++, src += 3) {
            uint8_t *d;
            if (!(col < wmax && r < G_U8(0x3e9a8)))
                continue;
            if (mode == 2)
                base = G(0x45000); /* slot 0x3f0bc */
            else if (mode == 1)
                base = G(0x46800); /* slot 0x3f0cc */
            else if (mode == 0)
                base = G(0x43800); /* slot 0x3f098 */
            else
                continue;
            d = base + r * 0x50 + (wmax - 1 - col) * 4;
            d[0] = src[0];
            d[1] = src[1];
            d[2] = src[2];
            d[3] = 0xff;
        }
    }
}

/* 0x00010160 -- reset the lane/pit state: race-finale flag, per-car lane flags, lane slots */
void fn_00010160(void)
{
    int i;

    G_S32(0x3f8e0) = (MODE_CUR == MODE_C || MODE_CUR == MODE_E) ? G_S32(0x3ea20) : 0;
    G_U8(0x3f9b8) = 0;
    G_U8(0x3f9bc) = 0;
    G_S32(0x3f9c0) = 0;
    G_S32(0x3f9c4) = 0;
    for (i = 0; i < NCARS; i++) {
        uint8_t *c = CAR(i);
        AT_S32(c, 0x114) = 0;
        AT_S32(c, 0x108) = 0;
        AT_S32(c, 0x10c) = 0;
        AT_S32(c, 0x104) = 0;
    }
    for (i = 0; i < 4; i++) {
        uint8_t *l = LANES + i * LANE_SIZE;
        AT_S32(l, 0x00) = 0;
        AT_S32(l, 0x04) = 0;
        AT_S32(l, 0x08) = 0;
        AT_S32(l, 0x0c) = 0;
        AT_S32(l, 0x10) = 0;
        AT_S32(l, 0x20) = 0;
        AT_S32(l, 0x24) = 0;
        AT_S32(l, 0x28) = 0;
        AT_S32(l, 0x2c) = 0;
        AT_S32(l, 0x30) = 0;
        AT_S32(l, 0x14) = -1;
        AT_S32(l, 0x18) = -1;
        AT_S32(l, 0x1c) = -1;
    }
}

/* Start a looping sound from its beginning. */
static void m0d800_sound_restart(int id)
{
    fn_00036190(id, 1);
    fn_0003613c(id, 0, 0);
    fn_000360bc(id);
}

/* 0x0001026c -- race start: load assets, timers, car models/colours, sounds, split-screen
 * viewports, restart backup, music */
void fn_0001026c(void)
{
    uint8_t *vp = G(0x5c160); /* per-viewport block, 0x80 bytes each from +0x3c8 */
    int32_t *roster = ROSTER;
    int i, n, t;
    uint8_t restart;

    G_S32(0x3fad0) = 0x7d;
    fn_00003da4((int)G_U32(0x3eb78)); /* the binary also passes 0xffffff */
    G_U32(0x482bc) = 0;
    G_F32(0x482b4) = -0.1f;
    G_U8(0x402f4) = 0;
    G_F32(0x482b8) = 2.2f;
    G_U8(0x3fab4) = 1;
    G_F32(0x3e9f4) = 0.5f;
    fn_000110cc();
    G_U8(0x3f8f8) = 0;
    G_S32(0x3f8e4) = 0;
    G_S32(0x3f8d0) = 0;
    G_S32(0x3f980) = 0;

    /* time limit (ms) */
    t = 3599999;
    if (MODE_CUR == MODE_D) {
        float par = (float)(int)((float *)G(0x3ead4))[G_S32(0x3f8b0)];
        t = (int)((par + (3.0f - G_F32(0x3e9b8)) * 3.0f) * 1000.0f);
    }
    G_S32(0x3f8d8) = t;
    G_S32(0x3f8dc) = -1;

    n = NCARS;
    if (n > 0) {
        for (i = 0; i < n; i++)
            AT_S32(CAR(((int32_t *)G(0x476c4))[i]), 0xf4) = i + 1;
        for (i = 0; i < NCARS; i++) {
            uint8_t *c = CAR(i);
            int32_t *ent = roster + i * 6;
            float par;
            fn_0000bf74(i, ent[0], ent[2]);
            par = (float)(int)((float *)G(0x3ead4))[G_S32(0x3f8b0)];
            AT_F32(c, 0x124) =
                (par + (float)(G_S32(0x3e9d4) - AT_S32(c, 0x90)) / G_F32(0x3e9b8) +
                 (G_F32(0x3e9b8) * -2.0f + 7.0f) * 3.0f) *
                1000.0f;
        }
    }
    G_S32(0x3e9dc) = 900000;
    if (MODE_CUR != MODE_E && MODE_CUR != MODE_C)
        G_S32(0x3e9dc) = (int)((float *)G(0x3ead4))[G_S32(0x3f8b0)] * 3000 + 30000;

    for (i = 0; i < NCARS; i++) {
        uint8_t *c = CAR(i);
        int32_t *ent = roster + i * 6;
        fn_00011018((int)(uintptr_t)(c + 0x394), ent[1]);
        AT_S32(c, 0x0) = ((int32_t *)G(0x470a0))[ent[0]]; /* body model slot */
        AT_S32(c, 0x4) = ((int32_t *)G(0x47218))[ent[2]]; /* face model slot */
        AT_S32(c, 0x8) = ((int32_t *)G(0x47258))[ent[0]];
    }
    G_U8(0x3fab0) = 0;

    /* engine and skid loops per player */
    for (i = 0; i < NPLAYERS; i++) {
        m0d800_sound_restart(((int32_t *)G(0x46e40))[i]);
        m0d800_sound_restart(((int32_t *)G(0x46e88))[i]);
        AT_S32(vp, 0x408 + i * 0x80) = 0;
        AT_S32(vp, 0x410 + i * 0x80) = i;
        AT_F32(vp, 0x418 + i * 0x80) = 90.0f;
    }
    if (MODE_CUR == MODE_B)
        for (i = 0; i < NPLAYERS; i++)
            m0d800_sound_restart(((int32_t *)G(0x46e78))[i]); /* siren */

    fn_0000fc98();
    fn_0000fc28();

    /* split-screen layout: render-target sizes and normalised viewport rects */
    if (NPLAYERS == 2) {
        G_F32(0x3e9c4) = 3000.0f;
        G_U16(0x3eba0) = 0x80;
        G_U16(0x3ebac) = 0x100;
        G_U16(0x3ebb0) = 0x100;
        G_U16(0x3eb9c) = 0x80;
        AT_F32(vp, 0x48) = 1.0f;
        AT_F32(vp, 0x58) = 0.5f;
        AT_F32(vp, 0x2c) = 0.0f;
        AT_F32(vp, 0x3c) = 0.5f;
        AT_F32(vp, 0x4c) = 1.0f;
        AT_F32(vp, 0x5c) = 0.5f;
        AT_F32(vp, 0x38) = 0.0f;
        AT_F32(vp, 0x28) = 0.0f;
    } else if (NPLAYERS == 1) {
        G_F32(0x3e9c4) = 3500.0f;
        G_U16(0x3ebac) = 0x100;
        G_U16(0x3ebb0) = 0x100;
        G_U16(0x3eb9c) = 0x100;
        G_U16(0x3eba0) = 0x100;
        AT_F32(vp, 0x48) = 1.0f;
        AT_F32(vp, 0x58) = 1.0f;
        AT_F32(vp, 0x2c) = 0.3f;
        AT_F32(vp, 0x3c) = 0.0f;
        AT_F32(vp, 0x4c) = 0.4f;
        AT_F32(vp, 0x5c) = 0.25f;
        AT_F32(vp, 0x38) = 0.0f;
        AT_F32(vp, 0x28) = 0.0f;
    } else if (NPLAYERS >= 3) {
        G_F32(0x3e9c4) = 2500.0f;
        G_U16(0x3ebac) = 0x80;
        G_U16(0x3ebb0) = 0x80;
        G_U16(0x3eb9c) = 0x80;
        G_U16(0x3eba0) = 0x80;
        AT_F32(vp, 0x48) = 0.5f;
        AT_F32(vp, 0x58) = 0.5f;
        AT_F32(vp, 0x2c) = 0.5f;
        AT_F32(vp, 0x3c) = 0.0f;
        AT_F32(vp, 0x4c) = 0.5f;
        AT_F32(vp, 0x5c) = 0.5f;
        AT_F32(vp, 0x30) = 0.0f;
        AT_F32(vp, 0x40) = 0.5f;
        AT_F32(vp, 0x50) = 0.5f;
        AT_F32(vp, 0x60) = 0.5f;
        AT_F32(vp, 0x34) = 0.5f;
        AT_F32(vp, 0x44) = 0.5f;
        AT_F32(vp, 0x54) = 0.5f;
        AT_F32(vp, 0x64) = 0.5f;
        AT_F32(vp, 0x38) = 0.0f;
        AT_F32(vp, 0x28) = 0.0f;
    }
    G_U32(0x3eba4) = G_U16(0x3eb9c) >> 1;
    G_U32(0x3eba8) = G_U16(0x3eba0) >> 1;

    /* 4 viewport render textures (descriptors at 0x44660, slot 0x3f0b0) via texture 0x99 */
    for (i = 0; i < 4; i++) {
        uint8_t *desc = (uint8_t *)G(0x44660) + i * 0x20;
        AT_S32(vp, 0x414 + i * 0x80) = 0;
        memcpy((uint8_t *)G(0x4060c) + 0x99 * 0x20, desc, 0x20);
        fn_00004dac(0x99);
        fn_00004838((int)(uintptr_t)desc, (int)(G_U32(0x3f854) & 0xffff),
                    (int)(G_U32(0x3f858) & 0xffff), 0);
        ((uint8_t *)G(0x446e0))[i] = 0; /* slot 0x3f0b4 */
        AT_F32(vp, 0x3c8 + i * 0x80) = AT_F32(CAR(i), 0x2c0) + 600.0f;
    }
    G_U8(0x3fee4) = 0;
    G_U8(0x3f8d4) = 0;
    G_U32(0x3fee8) = 0;
    G_S32(0x3ea24) = NPLAYERS > 2 ? 1 : 2;
    fn_000042e0();
    fn_0000ff60(0x97,
                NPLAYERS >= 3 ? "/pics/font_3_c.png" : "/pics/font_3_b.png", 0);
    G_U8(0x3f9e0) = 0;
    G_U8(0x3f9e4) = 0;
    G_U8(0x3f810) = 0;

    /* restart: restore cars and jump objects from the backup, else take a backup */
    restart = G_U8(0x3f98c);
    for (i = 0; i < NCARS; i++) {
        if (restart)
            memcpy(CAR(i), CAR_BAK(i), CAR_SIZE);
        else
            memcpy(CAR_BAK(i), CAR(i), CAR_SIZE);
    }
    for (i = 0; i < 3 * 0xac; i += 0xac) {
        if (restart)
            memcpy((uint8_t *)G(0x47f98) + i, (uint8_t *)G(0x47acc) + i, 0xac);
        else
            memcpy((uint8_t *)G(0x47acc) + i, (uint8_t *)G(0x47f98) + i, 0xac);
    }
    G_S32(0x3f988) = 0;
    if (restart) {
        G_U8(0x3f990) = 0;
        G_S32(0x3f984) = 1;
    } else {
        int b = (G_U8(0x3f88c) & 1) != 0;
        G_U8(0x3f990) = !b;
        G_S32(0x3f984) = b;
    }
    if (G_S32(0x3e9e0) != 0 || (G_U8(0x3f928) & 1))
        fn_00010cbc();

    G_U8(0x3f92c) = 0;
    G_S32(0x3e9f0) = 1;
    G_F32(0x3e9f4) = 0.0f;
    G_F32(0x3e9f8) = 0.1f;
    G_S32(0x3e9fc) = 1;
    G_F32(0x3ea00) = 0.0f;
    G_F32(0x3ea04) = 0.0125f;
    G_S32(0x3ea08) = 1;
    G_F32(0x3ea0c) = 0.0f;
    G_F32(0x3ea10) = 0.05f;
    G_U32(0x5c164) = 0;
    G_S32(0x3ea14) = 1;
    G_F32(0x3ea18) = 0.0f;
    G_F32(0x3ea1c) = 0.05f;
    if (MODE_CUR == MODE_A)
        fn_00010d70(-1, 0, 1);
    G_U8(0x3fab8) = 0;
    G_U8(0x3fab4) = 1;
    G_U8(0x3f81c) = G_S32(0x3f848) == 5 && G_S32(0x3f8ac) == 0;
    fn_000209e8();
    fn_000042e0();
    G_U32(0x3f828) = G_U32(0x42758);
}

/* 0x00010cbc -- stop the current music track, pick a different random one and loop it */
void fn_00010cbc(void)
{
    int32_t *music = G(0x46e50); /* slot 0x3f0d4: sound handle per track */
    char *path = G_PTR(0x3f358);
    int r;

    if (G_S32(0x3e9ec) >= 0) {
        fn_0003611c(music[G_S32(0x3e9ec)]);
        fn_00036138(); /* the binary passes track + 0x1e */
    }
    G_S32(0x3e9e8) = -1;
    do
        r = fn_00004400(7);
    while (G_S32(0x3e9e8) == r);
    snprintf(path, 0x100, "sound/music_%02d_b.wav", r);
    G_S32(0x3e9ec) = r;
    music[r] = fn_00036014(0x3c, path); /* the binary also passes 0x1f */
    m0d800_sound_restart(music[r]);
}

/* 0x00010d70 -- give car the race lead marker (+3 bonus when bonus == 1), or with car < 0
 * reset it and park the spectator car on racing-line point sect */
void fn_00010d70(int car, int bonus, int sect)
{
    int cur = G_S32(0x3f9b0);

    if (car >= 0) {
        uint8_t *c;
        int i;
        if (G_S32(0x3f9ac) > 0)
            return;
        if (cur >= 0 && AT_S32(CAR(cur), 0xf4) != 0)
            return;
        memset(G_PTR(0x3f0c4), 0, 0x3fc);
        if (cur >= 0)
            G_S32(0x3f9b4) = cur;
        for (i = 0; i < NCARS; i++)
            AT_S32(CAR(i), 0x104) = 0;
        G_S32(0x3f9b0) = car;
        c = CAR(car);
        AT_S32(c, 0x104) = 1;
        G_S32(0x3f9ac) = 30;
        if (AT_S32(c, 0x54) < 3)
            AT_S32(c, 0x54)++;
        if (bonus == 1) {
            const char *label = *(char **)G_PTR(0x3f2c0);
            AT_S32(c, 0x11c) += 3;
            sprintf((char *)c + 0x12c, "%s", label);
            sprintf((char *)c + 0x14c, "+3");
            sprintf((char *)c + 0x16c, " ");
            sprintf((char *)c + 0x18c, " ");
            sprintf((char *)c + 0x1ac, " ");
            AT_U32(c, 0x1cc) = G_U32(0x3eb7c);
            AT_U32(c, 0x1d0) = G_U32(0x3eb84);
            AT_U32(c, 0x1d4) = G_U32(0x3eb7c);
            AT_U32(c, 0x1d8) = G_U32(0x3eb7c);
            AT_U32(c, 0x1dc) = G_U32(0x3eb7c);
            AT_S32(c, 0x128) = (int)(G_F32(0x3f8ec) + 2000.0f);
        }
    } else {
        float *v = G_PTR(0x3f138);
        uint8_t *c;
        int surf = 0;
        memset(G_PTR(0x3f0c4), 0, 0x3fc);
        v[0] = LINE_PTS[sect * 2] * TILE;
        v[2] = LINE_PTS[sect * 2 + 1] * TILE;
        v[1] = 0.0f;
        v[1] = fn_0000d800(-1, v[0], v[1], v[2], &surf);
        G_S32(0x3f9b0) = -1;
        c = CAR(NCARS);
        AT_F32(c, 0x2bc) = v[0];
        AT_F32(c, 0x2c0) = v[1];
        AT_F32(c, 0x2c4) = v[2];
        AT_F32(c, 0x2b0) = v[0];
        AT_F32(c, 0x2b4) = v[1];
        AT_F32(c, 0x2b8) = v[2];
    }
}

/* 0x00011018 -- colour n of the 5x5x5 palette into dst {r, g, b, 0xff} (black -> dark grey) */
void fn_00011018(int dst_, int n)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)dst_;
    uint8_t r = (uint8_t)((n % 5) * 0xff / 4);
    uint8_t g = (uint8_t)((n / 5 % 5) * 0xff / 4);
    uint8_t b = (uint8_t)((n / 0x19 % 5) * 0xff / 4);

    if (r < 30 && g < 30 && b < 30)
        r = g = b = 30;
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = 0xff;
}

/* SmdLoadModel with the global "keep vertex data" flag the binary passes as 6th argument. */
static void m0d800_smd(int slot, const char *path, float scale, int bounds, int box)
{
    fn_0003836c(slot, path, scale, bounds, box, G_U8(0x3e99c) & 1);
}

/* 0x000110cc -- load race assets: every roster car and face, then (once) HUD textures,
 * race sounds and shared effect models */
void fn_000110cc(void)
{
    int i;

    for (i = 0; i < NCARS; i++) {
        fn_00011450(ROSTER[i * 6 + 0]);
        fn_000116f4(ROSTER[i * 6 + 2]);
    }
    if (G_U8(0x3f89c) & 1)
        return;
    G_U8(0x3f89c) = 1;
    fn_0000ff60(0xa6, NPLAYERS < 3 ? "/pics/gauge.png" : "/pics/gauge_s.png", 0);
    fn_0000ff60(0xab, "/pics/wii_wheel.png", 0);
    fn_0000ff60(0xb2, "/pics/wgauge.png", 0);
    fn_0000ff60(0xb1, "/pics/led.png", 0);
    fn_0000ff60(0xaf, "/pics/fuel.png", 0);
    fn_0000ff60(0xa5, "/pics/needle.png", 0);
    if (G_S32(0x3ebc0) < 0) {
        /* the binary passes a third argument 0xf to every fn_00036014 call */
        G_S32(0x3ebc0) = fn_00036014(0x17, "sound/check_point.wav");
        G_S32(0x3ebc4) = fn_00036014(0x16, "sound/final_lap.wav");
        G_S32(0x3ebc8) = fn_00036014(0x18, "sound/another_lap.wav");
        G_S32(0x3ebd8) = fn_00036014(0x1c, "sound/carcrash.wav");
        G_S32(0x3ebbc) = fn_00036014(0x15, "sound/start.wav");
        for (i = 0; i < 2; i++) {
            ((int32_t *)G(0x46e88))[i] = fn_00036014(i + 5, "sound/skid.wav");      /* 0x3f0dc */
            ((int32_t *)G(0x46e40))[i] = fn_00036014(i + 10, "sound/engine_02.wav"); /* 0x3f0d0 */
            ((int32_t *)G(0x46e78))[i] = fn_00036014(i + 15, "sound/siren.wav");     /* 0x3f0d8 */
        }
    }
    if (fn_00038c04(0x73) == 0)
        m0d800_smd(0x73, "/models/others/wheel_01.smd", u2f(0x4028f5c3), 0, 0);
    m0d800_smd(0x7e, "/models/others/obj_2d.smd", u2f(0x40228f5c), 1, 1);
    fn_0000ff60(0x7e, "/models/others/smoke.png", 0);
    fn_0000ff60(0x85, "/models/others/water_rain.png", 0);
    m0d800_smd(0x80, "/models/others/head_lights.smd", u2f(0x3fa28f5c), 0, 0);
    fn_0000ff60(0x80, "/models/others/head_lights.png", 0);
    m0d800_smd(0x90, "/models/others/obj_2d.smd", u2f(0x40228f5c), 0, 0);
    fn_0000ff60(0x90, "/models/others/shadow.png", 0);
    m0d800_smd(0x81, "/models/others/head_lights.smd", u2f(0x3fa28f5c), 0, 0);
    fn_0000ff60(0x81, "/models/others/lamp_lights.png", 0);
    m0d800_smd(0x7d, "/models/faces/small.smd", u2f(0x3fd8bf25), 0, 0);
    fn_0000ff60(0x7d, "/models/faces/small.png", 0x10);
    m0d800_smd(0x83, "/models/others/obj_2d.smd", 185.0f, 0, 0);
    fn_0000ff60(0x83, "/models/others/cloud.png", 0);
}

/* 0x00011450 -- load car model n (body, accessories, lights, window, far LOD, wheel texture)
 * into 7 consecutive model slots, once */
void fn_00011450(int n)
{
    int32_t *slots = G(0x470a0); /* slot 0x3f0f4: first model slot per car, -1 = not loaded */
    char base[64], smd[256], png[256];
    int id;

    snprintf(base, sizeof(base), "/models/car_%02d", n);
    if (slots[n] != -1)
        return;
    id = G_S32(0x3eb24) * (n - 1) + G_S32(0x3eb30);
    snprintf(smd, sizeof(smd), "%s_body.smd", base);
    m0d800_smd(id, smd, u2f(0x3fa28f5c), 1, 0);
    snprintf(png, sizeof(png), "%s_body.png", base);
    fn_0000ff60(id, png, 0x10);
    snprintf(smd, sizeof(smd), "%s_access.smd", base);
    m0d800_smd(id + 1, smd, u2f(0x3fa28f5c), 1, 0);
    snprintf(png, sizeof(png), "%s_access.png", base);
    fn_0000ff60(id + 1, png, 0x10);
    snprintf(smd, sizeof(smd), "%s_rear_light.smd", base);
    m0d800_smd(id + 2, smd, u2f(0x3fa28f5c), 0, 0);
    snprintf(smd, sizeof(smd), "%s_front_light.smd", base); /* done twice for car 11 */
    m0d800_smd(id + 3, smd, u2f(0x3fa28f5c), 0, 0);
    snprintf(smd, sizeof(smd), "%s_window.smd", base);
    m0d800_smd(id + 4, smd, u2f(0x3fa28f5c), 0, 0);
    snprintf(smd, sizeof(smd), "%s_far.smd", base);
    m0d800_smd(id + 5, smd, u2f(0x4013c825), 1, 1);
    snprintf(png, sizeof(png), "/models/car_01/car_01_far.png");
    fn_0000ff60(id + 5, png, 0x10);
    snprintf(png, sizeof(png), "%s_wheel_01.png", base);
    fn_0000ff60(id + 6, png, 0);
    slots[n] = id;
}

/* 0x000116f4 -- load driver face model n into its model slot, once */
void fn_000116f4(int n)
{
    int32_t *slots = G(0x47218); /* slot 0x3f134 */
    char smd[256], png[256];
    int id;

    if (slots[n] != -1)
        return;
    id = G_S32(0x3eb28) * (n - 1) + G_S32(0x3eb34);
    snprintf(smd, sizeof(smd), "/models/faces/face_%02d.smd", n);
    m0d800_smd(id, smd, u2f(0x3f872b02), 0, 0);
    snprintf(png, sizeof(png), "/models/faces/face_%02d.png", n);
    fn_0000ff60(id, png, 0x10);
    slots[n] = id;
}

/* Car class used to pick opponents of the same class as the players. */
static int m0d800_car_class(int model)
{
    switch (model) {
    case 1: case 2: case 3: case 5: case 6: case 9: case 10:
        return 0;
    case 4: case 8:
        return 1;
    case 7:
        return 2;
    case 11:
        return 3;
    default:
        return 4;
    }
}

/* 0x000117b0 -- build the opponent roster: team balance, car count, AI car models, faces,
 * names and colours distinct from the cars before them */
void fn_000117b0(void)
{
    static const int8_t same_class[7] = {1, 2, 3, 5, 6, 9, 10};
    int32_t *ros = ROSTER;
    const char *names = G(0x47180); /* slot 0x3f124: 20 x char[4] */
    int name_i = fn_00004400(20);
    int col = fn_00004400(125);
    int players = NPLAYERS;
    int same, i;

    G_S32(0x3e9ac) = players;
    if (MODE_CUR == MODE_B) {
        /* team race: pad the smaller team with AI cars */
        int t1 = 0, t0 = 0, diff, k;
        for (i = 0; i < players; i++) {
            if (ros[i * 6 + 3] == 1)
                t1++;
            else
                t0++;
        }
        G_S32(0x3f9c8) = t1;
        G_S32(0x3f9cc) = t0;
        diff = (int)fabsf((float)(t0 - t1));
        if (t1 != t0)
            G_S32(0x3e9ac) = NPLAYERS + diff;
        if (t1 > t0) {
            for (k = 0; k < diff; k++)
                ros[(NPLAYERS + k) * 6 + 3] = 0;
            G_S32(0x3f9cc) = diff + t0;
        } else if (t0 > t1) {
            for (k = 0; k < diff; k++)
                ros[(NPLAYERS + k) * 6 + 3] = 1;
            G_S32(0x3f9c8) = t1 + diff;
        }
    } else if (MODE_CUR == MODE_A || MODE_CUR == MODE_D) {
        int v = players + (G_S32(0x3f8bc) == 0);
        if (v <= 1)
            v++;
        G_S32(0x3e9ac) = v;
    }
    if (MODE_CUR == MODE_C || MODE_CUR == MODE_E) {
        if (G_S32(0x3f8bc) != 0)
            NCARS = G_S32(0x3e9ac);
        if (NCARS <= 1) {
            G_S32(0x3e9ac) = 2;
            NCARS = 2;
        }
    } else if (G_S32(0x3f8bc) != 0) {
        NCARS = G_S32(0x3e9ac);
    }

    players = NPLAYERS;
    same = 1;
    for (i = 0; i < players - 1; i++)
        same = same && m0d800_car_class(ros[i * 6]) == m0d800_car_class(ros[(i + 1) * 6]);

    for (i = players; i < NCARS; i++) {
        int32_t *ent = ros + i * 6;
        int r = fn_00004400(2);

        ent[4] = 1;
        name_i = (r + name_i + 1) % 20;
        sprintf((char *)&ent[5], "%s", names + name_i * 4);

        /* car model */
        if (same) {
            int m0 = ros[0];
            if (m0 == 4 || m0 == 8) {
                ent[0] = fn_00004400(2) == 0 ? 4 : 8;
            } else if ((unsigned)(m0 - 1) < 3 || m0 == 5 || m0 == 6 || m0 == 9 || m0 == 10) {
                r = fn_00004400(7);
                if ((unsigned)r <= 6)
                    ent[0] = same_class[r];
            } else if (m0 == 11 || m0 == 7) {
                ent[0] = m0;
            } else {
                ent[0] = fn_00004400(11) + 1;
            }
        } else if (MODE_CUR == MODE_B) {
            ent[0] = 11;
            do
                ent[0] = fn_00004400(11) + 1;
            while (ent[0] == 11);
        } else {
            ent[0] = fn_00004400(11) + 1;
        }
        ent[2] = fn_00004400(10) + 1; /* face */

        /* colour: random walk until far enough from every earlier car; team cars get the
         * fixed colour of their car model instead */
        while (ent[3] == 0) {
            int ok = 1, k;
            col += fn_00004400(125);
            ent[1] = col;
            for (k = 0; k < i; k++) {
                uint8_t a[4], b[4];
                float va[3], vb[3];
                fn_00011018((int)(uintptr_t)a, ent[1]);
                fn_00011018((int)(uintptr_t)b, ros[k * 6 + 1]);
                va[0] = a[0];
                va[1] = a[1];
                va[2] = a[2];
                vb[0] = b[0];
                vb[1] = b[1];
                vb[2] = b[2];
                if (fn_00037fa4(va, vb) < (float)(NCARS * -5 + 120))
                    ok = 0;
            }
            if (ok)
                goto next;
        }
        switch (ent[0]) {
        case 1: col = 55; break;
        case 2: col = 10; break;
        case 3: col = 110; break;
        case 4: col = 30; break;
        case 5: col = 31; break;
        case 6: case 8: col = 9; break;
        case 7: col = 24; break;
        case 9: col = 62; break;
        case 10: col = 2; break;
        case 11: col = 4; break;
        default: col = 0; break;
        }
        ent[1] = col;
    next:;
    }
}

/* 0x00011cf8 -- team colour (palette index) for car model, team and alternate flag */
int fn_00011cf8(int model, int alt, int team)
{
    switch (model) {
    case 0: return alt ? 0x37 : team ? 0x7c : 0x04;
    case 1: return alt ? 0x0a : team ? 0x7c : 0x1f;
    case 2: return alt ? 0x6e : team ? 0x1f : 0x13;
    case 3: return alt ? 0x1e : team ? 0x13 : 0x7c;
    case 4: return alt ? 0x1f : team ? 0x04 : 0x13;
    case 5: return alt ? 0x09 : team ? 0x0a : 0x09;
    case 6: return alt ? 0x18 : team ? 0x23 : 0x69;
    case 7: return alt ? 0x09 : team ? 0x54 : 0x7c;
    case 8: return alt ? 0x3e : team ? 0x0e : 0x3e;
    case 9: return alt ? 0x02 : team ? 0x02 : 0x37;
    case 10: return alt ? 0x04 : team ? 0x7c : 0x69;
    default: return 0;
    }
}

/* 0x00011dbc -- load menu/front-end assets: UI sounds, menu textures, logo, star lights,
 * the road tile used on the track-select preview */
void fn_00011dbc(void)
{
    char tiles[64], maps[64], smd[256], png[256];

    /* the binary passes a third argument 0xf to every fn_00036014 call */
    G_S32(0x3ebcc) = fn_00036014(0x19, "sound/swish.wav");
    G_S32(0x3ebb8) = fn_00036014(0x14, "sound/engine_start.wav");
    G_S32(0x3ebd0) = fn_00036014(0x1a, "sound/pitstop.wav");
    G_S32(0x3ebd4) = fn_00036014(0x1b, "sound/turbo.wav");
    fn_0000ff60(0xa8, "/pics/back.png", 0);
    fn_0000ff60(0xaa, "/pics/buttons.png", 0);
    fn_0000ff60(0x98, "/pics/controller.png", 0);
    fn_0000ff60(0xa2, "/models/objs/sun_flares.png", 0);
    fn_0000ff60(0x97, "/pics/font_3_b.png", 0);
    fn_0000ff60(0xb0, "/pics/speaker.png", 0);
    fn_0000ff60(0x9a, "/pics/line.png", 0);
    fn_0000ff60(0xae, "/models/car_01/car_01_window.png", 0x10);
    m0d800_smd(0x82, "/models/logos/logo_micro.smd", u2f(0x3f58bf25), 1, 0);
    fn_0000ff60(0x82, "/models/logos/logo_micro.png", 0x10);
    G_U8(0x3f91c) = 1;
    m0d800_smd(0x84, "/models/others/obj_2d.smd", u2f(0x40a28f5c), 0, 0);
    fn_0000ff60(0x84, "/models/others/star_lights.png", 0);
    snprintf(tiles, sizeof(tiles), "models/map_tiles");
    snprintf(maps, sizeof(maps), "models/maps");
    snprintf(smd, sizeof(smd), "%s/road_00.smd", tiles);
    m0d800_smd(((int32_t *)G(0x47770))[0], smd, u2f(0x40228f5c), 1, 1); /* slot 0x3f17c */
    snprintf(png, sizeof(png), "%s/road_02.png", tiles);
    fn_0000ff60(((int32_t *)G(0x47770))[0xb], png, 0);
    m0d800_smd(0x7f, "/models/others/obj_2d.smd", u2f(0x40228f5c), 1, 1);
}

/* 0x00012024 -- set up the single-screen track-select preview race (camera, projection,
 * light matrix, preview car, music) and the preview captions */
void fn_00012024(void)
{
    uint8_t *vp = G(0x5c160);
    uint8_t *c = CAR(0);
    float *a = G(0x47f94); /* slot 0x3f360 */
    float *b = G(0x47254); /* slot 0x3f140 */
    int idx, preview;

    G_U8(0x402f4) = 1;
    G_U32(0x482bc) = 0;
    G_U8(0x3fab0) = 1;
    G_F32(0x482b8) = 2.2f;
    NPLAYERS = 1;
    G_F32(0x482b4) = -0.1f;
    G_S32(0x3e9ac) = 1;
    G_S32(0x3f9fc) = 0;
    G_S32(0x3fa00) = 0;
    fn_0000fc98();
    fn_0000fc28();
    G_U16(0x3eb9c) = 0x100;
    G_U16(0x3eba0) = 0x100;
    AT_F32(vp, 0x28) = 0.0f;
    AT_F32(vp, 0x38) = 0.0f;
    AT_F32(vp, 0x48) = 1.0f;
    AT_F32(vp, 0x58) = 1.0f;
    AT_F32(vp, 0x2c) = 0.0f;
    AT_F32(vp, 0x3c) = 0.0f;
    AT_F32(vp, 0x4c) = 1.0f;
    AT_F32(vp, 0x5c) = 1.0f;
    G_U32(0x3eba4) = 0x80;
    G_U32(0x3eba8) = 0x80;
    G_U16(0x3ebac) = 0x100;
    *a = -1500.0f;
    *b = -1500.0f;
    G_U16(0x3ebb0) = 0x100;
    fn_00037d0c(G(0x428f8), -1500.0f, 1500.0f, -1500.0f, 1500.0f, 10.0f, 7000.0f);
    fn_00037d5c(G(0x428b8), -*a, *a, *b, -*b, 0.5f, 0.5f, 0.5f, 0.5f);

    G_U8(0x3fedc) = 0;
    G_U8(0x3fed8) = 0;
    AT_F32(c, 0x2b0) = 0.0f;
    AT_F32(c, 0x2b4) = 0.0f;
    G_U8(0x3fee0) = 0;
    AT_F32(c, 0x2b8) = 0.0f;
    AT_F32(c, 0x2bc) = AT_F32(c, 0x2b0);
    AT_F32(c, 0x2c0) = AT_F32(c, 0x2b4);
    AT_F32(c, 0x2c4) = 0.0f;
    AT_F32(vp, 0x3c4) = AT_F32(c, 0x2b0);
    AT_F32(vp, 0x3c8) = AT_F32(c, 0x2b4);
    AT_F32(vp, 0x3cc) = AT_F32(c, 0x2b8);
    AT_S32(vp, 0x410) = 0;
    AT_S32(vp, 0x490) = 0;
    G_S32(0x3f8e8) = 0;
    G_S32(0x3f8d0) = 0;
    G_F32(0x3ea00) = 0.0f;
    G_F32(0x3e9f4) = 0.0f;
    G_F32(0x3ea18) = 0.0f;
    G_F32(0x3ea0c) = 0.0f;
    idx = G_S32(0x3fa00) + 1;
    AT_S32(c, 0x0) = ((int32_t *)G(0x470a0))[idx];
    AT_S32(c, 0x4) = ((int32_t *)G(0x47218))[idx];
    AT_S32(c, 0x8) = ((int32_t *)G(0x47258))[idx];
    G_U8(0x3f924) = 1;
    fn_000042e0();
    if (G_S32(0x3fa24) != 4)
        fn_00010cbc();
    G_U8(0x3f990) = 0;
    G_S32(0x3f984) = 0;
    G_S32(0x3f988) = 0;
    fn_0000bf74(0, 1, ROSTER[2]);
    G_U8(0x3fab8) = 1;
    G_F32(0x3e998) = 2400.0f;
    preview = G_S32(0x470d4); /* *slot 0x3f0fc: selected track */
    G_S32(0x3f088) = 0x960;
    G_U8(0x3fab4) = 0;
    if (G_S32(0x3fa24) == 0x1e)
        fn_00012464(preview);
    if (G_S32(0x3fa24) == 5) {
        G_S32(0x3ec00) = 1;
        G_S32(0x3f99c) = -1;
        G_F32(0x3ebdc) = 1.0f;
        snprintf(G_PTR(0x3f148), 0x20, "%s", *(char **)G_PTR(0x3f1a8));
        *(int32_t *)G_PTR(0x3f15c) = 0x78;
        fn_000128a8(preview);
    }
    if (G_S32(0x3fa24) == 4) {
        G_S32(0x3ec00) = 1;
        G_S32(0x3f99c) = -1;
        G_F32(0x3ebdc) = 1.0f;
        snprintf(G_PTR(0x3f148), 0x20, "%s", *(char **)G_PTR(0x3f1a8));
        *(int32_t *)G_PTR(0x3f15c) = 0x82;
        fn_000128a8(preview);
    }
}

/* 0x00012464 -- track-select captions (title, track name, direction) and mode icon, then
 * draw the track preview */
void fn_00012464(int track)
{
    char *title = G_PTR(0x3f148);
    char *name = G_PTR(0x3f100);
    char *sub = G_PTR(0x3f160);
    int a = G_U8(0x3f894) & 1;

    G_S32(0x3ec00) = 0;
    if ((G_U8(0x3f920) & 1) == 0) {
        if (a)
            snprintf(title, 0x20, "%s", *(char **)G_PTR(0x3f24c));
        else
            snprintf(title, 0x20, " ");
        snprintf(sub, 0x20, " ");
        G_F32(0x3ebdc) = 1.0f;
        G_S32(0x3f99c) = -1;
    } else {
        const char *s;
        if (a || (G_U8(0x3f880) & 1)) {
            if (G_U8(0x3f928) & 1)
                snprintf(title, 0x20, "%s", *(char **)G_PTR(0x3f24c));
            else
                snprintf(title, 0x20, "%s %d/%d", *(char **)G_PTR(0x3f2a8),
                         (int)G_S32(0x3f8b4) + 1, (int)G_S32(0x3e9cc));
        } else if (G_U8(0x3fac0)) {
            snprintf(title, 0x20, "%s", *(char **)G_PTR(0x3f208));
        } else if (G_U8(0x3f90c) & 1) {
            snprintf(title, 0x20, "%s", *(char **)G_PTR(0x3f2d8));
        } else {
            snprintf(title, 0x20, " ");
        }

        switch (G_S32(0x3f8b8)) {
        case 1: s = *(char **)G_PTR(0x3f270); break;
        case 2: s = *(char **)G_PTR(0x3f20c); break;
        case 3: s = *(char **)G_PTR(0x3f2b0); break;
        case 4: s = *(char **)G_PTR(0x3f2ec); break;
        default: s = *(char **)G_PTR(0x3f2f8); break;
        }
        snprintf(name, 0x14, "%s", s);

        if ((G_U8(0x3f928) & 1) == 0 && G_U8(0x3fabc) != 0) {
            const char *track_name = (const char *)G_PTR(0x3f19c) + G_S32(0x3f8b0) * 0x10;
            const char *dir = *(char **)G_PTR(TRACK_DIR == 1 ? 0x3f274 : 0x3f2b8);
            snprintf(sub, 0x20, "%s (%s,%s)", track_name, dir, name);
        } else {
            snprintf(sub, 0x20, " ");
        }

        if (MODE_CUR == MODE_E)
            G_S32(0x3f99c) = 0x79;
        else if (MODE_CUR == MODE_C)
            G_S32(0x3f99c) = 0x76;
        else if (MODE_CUR == MODE_B)
            G_S32(0x3f99c) = 0x77;
        else if (MODE_CUR == MODE_A)
            G_S32(0x3f99c) = 0x7b;
        else if (MODE_CUR == MODE_D)
            G_S32(0x3f99c) = 0x74;
        G_F32(0x3ebdc) = 0.5f;
    }
    *(int32_t *)G_PTR(0x3f15c) = 0x78;
    fn_000128a8(track);
}

/* Copy track preview texture descriptor into texture slot 0x95. */
static void m0d800_preview_tex(int track)
{
    memcpy((uint8_t *)G(0x4060c) + 0x95 * 0x20, (uint8_t *)G_PTR(0x3f198) + track * 0x20, 0x20);
}

/* 0x000128a8 -- draw the track preview map (texture slot 0x95) in two passes */
void fn_000128a8(int track)
{
    uint8_t *tint = G(0x44190); /* slot 0x3f0a0: RGBA */
    int v, alpha;

    fn_00038a78(2);
    fn_00038a88(2);
    fn_00012a68(track, 1);
    m0d800_preview_tex(track);
    fn_00038a78(2);
    fn_00038a88(2);
    G_S32(0x3ea24) = 2;
    if (G_S32(0x3fa24) == 4) {
        G_S32(0x3ea24) = 3;
        v = 0xff;
        alpha = 0x9b;
    } else {
        v = G_S32(0x3fa24) == 0x1e ? 0x3c : 0x2d;
        alpha = 0xcd;
    }
    tint[0] = (uint8_t)v;
    tint[1] = (uint8_t)v;
    tint[2] = (uint8_t)v;
    tint[3] = (uint8_t)alpha;
    fn_00004fb4(v, v, v, alpha);
    fn_00015e4c(track, 1, 0, 0, 0, 3, 0);
    m0d800_preview_tex(track);
    fn_00038a78(3);
    fn_00038a88(3);
    if (G_S32(0x3fa24) == 4) {
        if ((G_U8(0x3f91c) & 1) == 0)
            return;
    } else if (G_S32(0x3fa24) == 5) {
        if ((G_U8(0x3f918) & 1) == 0)
            return;
    }
    fn_00012a68(track, 0);
}

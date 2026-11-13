/* m_2d550.c -- per-car physics step (gears, rpm, steering, forces, wheels against terrain and
 * walls, skid marks / smoke, pit-garage scripting), music fade, front-end menu input, player
 * table reset, button auto-repeat, rpm update and skid-mark spawner.
 * iOS range [0x0002d550, 0x000312cc).
 *
 * Shared state used here (iOS addresses):
 *   0x48ae0 car array, NCARS (G_S32(0x3e9c8)) cars of 0x678 bytes. Wheels: car + 0x398 + w*0x5c
 *   0x5c210 per-player input, 0x14 bytes (see INP_* below); 0x5c160 + i*0x24 touch/UI bytes
 *   0x5c160 + i*0x80 + 0x408 camera mode (+0x40c saved mode)
 *   0x5c700 particle ring, 35 x 0x48 bytes, index G_S32(0x5c17c)
 *   0x44800 / 0x46400 20x20 byte grids: road tile type / racing-line order
 *   0x44590 track objects (pit garages), 4 x 0x34 bytes
 *   0x47544 player table, 0x18 bytes per player {type, ?, ?, ?, ?, char name[4]}
 *   0x3f980 music volume, 0x3fa24 current front-end screen, 0x3fa28 requested screen
 *   0x3fa60..0x3fa78 one-frame button flags written by fn_00030bf4 */
#include "ios.h"
#include "opt_menu.h"
#include "net_game.h"

#define CAR_SIZE 0x678
#define CAR(i) ((uint8_t *)G(0x48ae0) + (i) * CAR_SIZE)
#define NCARS G_S32(0x3e9c8)
#define NHUMAN G_S32(0x3e9b0) /* human players (1) */

/* Game-mode ids live in globals (compared against the current mode G_S32(0x3f9dc)). */
#define MODE_CUR G_S32(0x3f9dc)
#define MODE_A G_S32(0x3ebe0) /* 1 */
#define MODE_B G_S32(0x3ebe4) /* 2 */
#define MODE_C G_S32(0x3ebe8) /* 3 */
#define MODE_D G_S32(0x3ebec) /* 4 */
#define MODE_E G_S32(0x3f9d8)

/* 20x20 byte grids, row stride 20, cells of 812 units. */
#define GRID_TILE ((uint8_t *)G(0x44800))  /* slot 0x3f0b8: road tile type */
#define GRID_ORDER ((uint8_t *)G(0x46400)) /* slot 0x3f0c8: racing-line order */
#define GRID_MAXZ G_S32(0x3f8a0)
#define GRID_MAXX G_S32(0x3f8a4)

/* Per-player input block (0x5c160 + 0xb0 + i * 0x14). */
#define INPUT(i) ((uint8_t *)G(0x5c210) + (i) * 0x14)
#define INP_BRAKE 0x0   /* u8: brake (AI: set by the pit script) */
#define INP_ACCEL 0x1   /* u8 */
#define INP_DOWN 0x2    /* u8: shift down / reverse */
#define INP_UP 0x3      /* u8: shift up */
#define INP_DOWN_P 0x4  /* u8: previous INP_DOWN (edge detect) */
#define INP_UP_P 0x5    /* u8: previous INP_UP */
#define INP_NITRO 0x6   /* u8 */
#define INP_7 0x7       /* u8 */
#define INP_NITRO_L 0x8 /* u8: nitro latch */
#define INP_STEER 0xc   /* float */
#define INP_REV 0x10    /* u8: player reverse request */
#define UI(i) ng_pad(i) /* 3DS: was 0x5c160 + i*0x24, see net_game.h */

/* Particle ring. */
#define PART_MAX 34
#define PART(n) ((uint8_t *)G(0x5c700) + (n) * 0x48)

#define DEG2RAD u2f(0x3c8efa39) /* 0.0174533f */
#define RAD2DEG u2f(0x42652ee1) /* 57.29578f */

#define M2D_P2I(p) ((int)(intptr_t)(p))

/* Car field access (c is the current car). */
#define CF(o) AT_F32(c, o)
#define CI(o) AT_S32(c, o)

/* Sign the way the game computes it: -1, 0 or 1 (NaN -> -1). */
static float m2d_sgn(float x)
{
    if (!(x >= 0.0f))
        return -1.0f;
    return (float)(x > 0.0f);
}

static double m2d_sgnd(float x)
{
    if (x < 0.0f)
        return -1.0;
    return (double)(x > 0.0f);
}

/* Clamp to [0, hi]: negative -> 0, then min. */
static int m2d_clamp(int v, int hi)
{
    if (v < 0)
        return 0;
    return v > hi ? hi : v;
}

/* Road tile under world position (x, z). */
static int m2d_tile(int ix, int iz)
{
    return GRID_TILE[m2d_clamp(ix, GRID_MAXX) * 20 + m2d_clamp(iz, GRID_MAXZ)];
}

/* Racing-line distance from car c to car o (laps * line length + section). */
static int m2d_line_gap(const uint8_t *c, const uint8_t *o)
{
    return G_S32(0x3f948) * (AT_S32(o, 0x90) - AT_S32(c, 0x90)) + AT_S32(o, 0x84) -
           AT_S32(c, 0x84);
}

/* Advances the particle ring and returns the new slot. */
static uint8_t *m2d_part_next(void)
{
    int n = G_S32(0x5c17c) + 1;
    G_S32(0x5c17c) = n;
    if (n > PART_MAX) {
        G_S32(0x5c17c) = 0;
        n = 0;
    }
    return PART(n);
}

/* 0x0002d550 -- per-car physics step: gears/rpm, nitro, steering, forces, wheels against the
 * terrain and walls, skid marks / smoke, pit-garage scripting (guess) */
void fn_0002d550(int i)
{
    uint8_t *c = CAR(i);
    uint8_t *in = INPUT(i);
    void *mtx = c + 0x334;
    float *vel = &CF(0x2f8);
    float *pos2 = &CF(0x2bc);
    float L[3] = {0.0f, 0.0f, 0.0f}; /* normalised velocity (uninitialised in the original) */
    float T[3], O[3], off[3];
    bool flag_a, flag_b;
    float spd0, rpm0, d14, s22, s24;
    int collided;

    /* --- race-role flags ------------------------------------------------------------- */
    flag_a = CI(0x10c) != 0 || (MODE_CUR == MODE_A && CI(0x104) != 0);
    flag_b = CI(0x108) != 0 ||
             (MODE_CUR == MODE_A && CI(0x104) == 0 && i < G_S32(0x3e9ac));
    if (MODE_CUR == MODE_B) {
        uint8_t *o = CAR(CI(0x118));
        if (AT_S32(o, 0x114) >= G_S32(0x3e9d8) || AT_S32(o, 0xf8) != 0)
            CI(0x118) = -1;
    }
    if (MODE_CUR == MODE_D && i < G_S32(0x3e9ac) && (G_U8(0x3f8f8) & 1)) {
        if (CI(0xf4) == 0)
            CF(0x124) += -16.5f;
        if (CF(0x124) < 0.0f) {
            CF(0x124) = 0.0f;
            CI(0xf4) = CI(0x9c);
        }
    }

    /* --- counters --------------------------------------------------------------------- */
    CI(0xfc) = (int)(G_F32(0x3ea00) + G_F32(0x3e9f4) + G_F32(0x3ea0c));
    if (CI(0xd0) != CI(0x9c) && CI(0xd4) >= 51) {
        CI(0xd4) = 2;
    } else if (!(CI(0xd0) == CI(0x9c) && CI(0xd4) > 50)) {
        CI(0xd4) += 2;
        if (CI(0xd4) == 50)
            CI(0xd0) = CI(0x9c);
    }
    if (CI(0xe8) >= 1)
        CI(0xe8)--;
    if (CI(0xec) >= 1)
        CI(0xec)--;

    /* --- rotation matrix (Y, Z, X order) and its inverse ------------------------------- */
    G_U8(0x42760) = 'Y';
    G_U8(0x42768) = 'X';
    G_U8(0x42764) = 'Z';
    fn_00004620(M2D_P2I(c + 0x334), M2D_P2I(c + 0x2c8));
    fn_0003759c(c + 0x334, c + 0x364);
    G_U8(0x42760) = 'Z';
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';

    /* --- grid cell -------------------------------------------------------------------- */
    {
        int cx = (int)((CF(0x2b0) + 406.0f) / 812.0f);
        int cz = (int)((CF(0x2b8) + 406.0f) / 812.0f);
        CI(0x44) = cx;
        CI(0x48) = cz;
        if (CI(0x4c) != cx || CI(0x50) != cz) {
            CI(0x4c) = cx;
            CI(0x50) = cz;
            CI(0x68) = CI(0x64);
            CI(0x64) = m2d_tile(cx, cz);
        }
    }

    /* --- wheel summaries -------------------------------------------------------------- */
    CI(0x34) = CI(0x3dc) + CI(0x438) + CI(0x494) + CI(0x4f0); /* wheels in the air */
    CI(0x38) = CI(0x3dc) != 0 && CI(0x438) != 0;
    CI(0x3c) = CI(0x3e4) + CI(0x440) + CI(0x49c) + CI(0x4f8); /* wheels on a surface */

    /* --- speed, forward / side vectors, heading of travel ----------------------------- */
    spd0 = fn_00037e54(vel);
    CF(0x254) = spd0;
    CF(0x2e0) = 0.0f;
    CF(0x2e4) = 0.0f;
    CF(0x2e8) = 1.0f;
    fn_00037fe8(mtx, &CF(0x2e0), &CF(0x2e0));
    CF(0x2ec) = 1.0f;
    CF(0x2f0) = 0.0f;
    CF(0x2f4) = 0.0f;
    fn_00037fe8(mtx, &CF(0x2ec), &CF(0x2ec));
    CF(0x264) = fn_00037e80(vel, &CF(0x2e0));
    CF(0x268) = fn_00037e80(vel, &CF(0x2ec));
    if (!(fn_00037e54(vel) > 0.0f)) {
        CF(0x248) = CF(0x244);
    } else {
        float s;
        fn_000379a8(vel, L);
        s = m2d_sgn(CF(0x264));
        CF(0x248) = atan2f(L[0] * s, L[2] * s) * RAD2DEG;
    }

    /* --- engine rpm ------------------------------------------------------------------- */
    rpm0 = CF(0x238);
    if (rpm0 < 80.0f)
        CF(0x23c) = rpm0;
    else
        CF(0x23c) += (CF(0x23c) - 80.0f) / -20.0f;
    if (G_S32(0x3f8e0) != 0) {
        if (CF(0x20) > 0.0f && CF(0x23c) > 10.0f)
            CF(0x20) += CF(0x23c) / -1.75e6f;
        else if (CF(0x20) < 0.0f)
            CF(0x20) = 0.0f;
    }

    /* --- gears ------------------------------------------------------------------------ */
    if ((G_U8(0x3f8f8) & 1) && CI(0xf4) == 0 && (unsigned)(CI(0x70) - 2) >= 2) {
        int g0 = CI(0x28), ng, shift = -1;
        float r18 = rpm0;

        if (CI(0x2c) != 0) { /* automatic */
            if (CI(0x30) == -1) {
                if (in[INP_REV]) {
                    ng = 0;
                    CI(0x28) = 0;
                } else {
                    shift = g0;
                    if (g0 <= 1) {
                        shift = 2;
                        CI(0x28) = 2;
                    }
                }
            } else if (in[INP_DOWN] && g0 <= 2 && g0 != 0) {
                CI(0x28) = 0;
                CF(0x238) = 0.0f;
                r18 = 0.0f;
                ng = 0;
            } else if (!in[INP_DOWN] && g0 == 0) {
                CI(0x28) = 1;
                CF(0x238) = 0.0f;
                r18 = 0.0f;
                ng = 1;
            } else if (g0 == 1) {
                ng = 1;
                if (rpm0 > 50.0f) {
                    ng = 2;
                    CI(0x28) = 2;
                }
            } else {
                ng = g0;
                if (g0 >= 2)
                    shift = g0;
            }
            if (shift >= 0) {
                if (rpm0 > (float)(90 - 3 * shift) && shift <= 6) {
                    ng = shift + 1;
                    CI(0x28) = ng;
                } else if (rpm0 < (float)(3 * shift + 20) && shift >= 3) {
                    ng = shift - 1;
                    CI(0x28) = ng;
                } else {
                    ng = shift;
                }
            }
        } else { /* manual */
            uint8_t f = 0;
            ng = g0;
            if (in[INP_UP]) {
                f = 1;
                if (in[INP_UP_P] != 1 && g0 <= 6) {
                    ng = g0 + 1;
                    CI(0x28) = ng;
                }
            }
            in[INP_UP_P] = f;
            if (!in[INP_DOWN]) {
                in[INP_DOWN_P] = 0;
            } else if (in[INP_DOWN_P] != 1) {
                if (ng >= 2) {
                    ng--;
                    CI(0x28) = ng;
                } else if (ng >= 1 && spd0 < 5.0f) {
                    ng--;
                    CI(0x28) = ng;
                }
                in[INP_DOWN_P] = 1;
            }
        }

        if (CI(0x70) != 0 && ng == 0) {
            ng = 1;
            CI(0x28) = 1;
        }
        if (g0 > ng && g0 >= 2 && CI(0x5c) <= 0)
            CI(0x5c) = 15;
        {
            bool rev = in[INP_DOWN] && CI(0x2c) != 0 && ng == 0;
            if ((in[INP_ACCEL] || rev) && r18 < 100.0f && CI(0x114) < G_S32(0x3e9d8)) {
                float add;
                if (ng == 1) {
                    add = 5.0f;
                } else {
                    float g = CF(0x1e8 + ng * 4);
                    add = (2.0f * sinf((2.0f * r18 - (g + -10.0f)) * DEG2RAD) + 9.0f) /
                          (g * g / 20.0f + 1.0f);
                }
                CF(0x238) = r18 + add;
            }
        }
        if (!(CF(0x20) > 0.0f) && fn_00004400(5) == 0 && CF(0x238) > 5.0f)
            CF(0x238) += -4.0f;
    }

    /* --- nitro, pause -------------------------------------------------------------------- */
    {
        uint8_t *u = UI(i);
        bool pause = false;

        if (i < NHUMAN)
            pause = ((G_U8(0x3f810) & 1) || (u[0xa2] && u[0xa3])) && !G_U8(0x3fab8);
        if (pause) {
            if (CI(0x5c) >= 0x23b) {
                CI(0x5c) = 0;
                CI(0x60) = 0;
                fn_000360e0(G_S32(0x3ebd4));
            }
            in[INP_7] = 0;
            in[INP_NITRO] = 0;
            in[INP_NITRO_L] = 0;
            AT_S32(u, 0xa0) = 0;
            G_U8(0x3f9e0) = 1;
            fn_0003613c(((int32_t *)G(0x46e40))[i], 0, 0);
            fn_0003613c(((int32_t *)G(0x46e88))[i], 0, 0);
            if (MODE_CUR == MODE_B)
                fn_0003613c(((int32_t *)G(0x46e78))[i], 0, 0);
        } else {
            CI(0x100) = in[INP_7] != 0;
            if (!in[INP_NITRO] || CI(0x54) < 1 || CI(0x5c) > 0) {
                in[INP_NITRO_L] = 0;
            } else if (!in[INP_NITRO_L]) {
                /* nitro fired: 600 frames, panned by player slot */
                float f;
                int a, b;
                in[INP_NITRO_L] = 1;
                CI(0x5c) = 600;
                CI(0x60) = 1;
                f = (float)(uint16_t)(1 - (i + 1) % 2);
                a = (int)(f * 6000.0f);
                b = (int)((float)(unsigned)(int)(1.0f - f) * 6000.0f);
                fn_0003613c(G_S32(0x3ebd4), a, b);
                fn_000360bc(G_S32(0x3ebd4));
            }
            if (i < NHUMAN && !G_U8(0x3fab8)) {
                /* camera mode: 0xad held -> rear view (8); 0x8a cycles 0 -> 0xc/4 -> 0 */
                int32_t *m = &AT_S32((uint8_t *)G(0x5c160) + i * 0x80, 0x408);
                int v = *m;
                if (u[0xad]) {
                    if (v != 8)
                        m[1] = v;
                    v = 8;
                    *m = v;
                } else if (v == 8) {
                    v = m[1];
                    *m = v;
                }
                if (u[0x8a]) {
                    if (v == 0xc) {
                        *m = 0;
                    } else if (v == 4) {
                        *m = 0xc;
                    } else if (v == 0) {
                        if (G_U8(0x3f98c)) {
                            m[3] = 1; /* +0x414 */
                            *m = 4;
                        } else {
                            *m = 0xc;
                        }
                    }
                }
            }
        }
    }

    /* --- drive force, brake ------------------------------------------------------------ */
    if (CI(0x28) == 1) {
        CF(0x1e0) = 0.0f;
    } else {
        float g = CF(0x1e8 + CI(0x28) * 4);
        CF(0x1e0) = CF(0x238) / (g * g + 10.0f) / 50.0f;
    }
    if (in[INP_BRAKE] || CI(0xf4) >= 1) {
        CF(0x1e0) += fabsf(fn_00037e80(L, &CF(0x2e0)) / 10.0f);
        CF(0x40) += (10.0f - CF(0x40)) / 10.0f;
        CF(0x1e4) = 10.0f;
    } else {
        CF(0x40) += CF(0x40) / -3.0f;
        CF(0x1e4) += CF(0x1e4) / -10.0f;
    }

    /* --- steering ---------------------------------------------------------------------- */
    {
        float st = AT_F32(in, INP_STEER);
        if (fabsf(st) < 1.0f)
            CF(0x250) = 0.0f;
        else
            CF(0x250) = (st - m2d_sgn(st)) / 20.0f;
        if (fabsf(CF(0x250)) > 25.0f)
            CF(0x250) = CF(0x250) < 0.0f ? -25.0f : (CF(0x250) > 0.0f ? 25.0f : 0.0f);
    }
    if (spd0 > 0.0f)
        s24 = CF(0x250) * (spd0 + 1.0f) * m2d_sgn(CF(0x264)) / 40.0f;
    else
        s24 = 0.0f;
    CF(0x244) += CF(0x24c);
    CF(0x244) = fn_00038a20(CF(0x244));
    d14 = CF(0x238) - rpm0;

    /* --- forces: yaw, thrust, tilt, side grip, drive vector ---------------------------- */
    if (CI(0x34) < 4 || CI(0x30) == -1) {
        float f, n, div;

        if (CI(0x38) != 0 && CI(0x30) != -1) {
            s24 = 0.0f;
        } else if (spd0 < 1.0f) {
            s24 *= spd0;
        } else if (spd0 > 0.0f) {
            float r = fminf(1.0f, CF(0x264) / spd0);
            double d = r < -1.0f ? -1.0 : (double)r;
            CF(0x26c) = (float)(acos(d) * (double)RAD2DEG * m2d_sgnd(CF(0x268)) *
                                m2d_sgnd(CF(0x264)));
        }
        CF(0x24c) += s24 * fabsf(cosf(CF(0x26c) * DEG2RAD)) / 10.0f -
                     CF(0x24c) / (CI(0x30) != -1 ? 15.0f : 20.0f);

        /* thrust along the car's z axis */
        CF(0x304) = 0.0f;
        CF(0x308) = 0.0f;
        CF(0x30c) = d14 * CF(0x1e8 + CI(0x28) * 4) / 100.0f;
        fn_00037fe8(mtx, &CF(0x304), &CF(0x304));
        fn_00037dec(vel, &CF(0x304), vel);

        /* slope push from the body tilt */
        if ((G_U8(0x3f8f8) & 1) && CI(0x78) == -1) {
            CF(0x310) = sinf(CF(0x29c) * DEG2RAD) / -30.0f;
            CF(0x314) = 0.0f;
            CF(0x318) = sinf(CF(0x298) * DEG2RAD) / 20.0f;
        } else {
            CF(0x310) = 0.0f;
            CF(0x314) = 0.0f;
            CF(0x318) = 0.0f;
        }
        fn_00037fe8(mtx, &CF(0x310), &CF(0x310));
        fn_00037dec(vel, &CF(0x310), vel);

        /* side grip */
        if (CF(0x254) > 0.0f) {
            float base = fmaxf(0.0f, (G_F32(0x3ea18) + -3.0f) * 0.25f + 0.0f +
                                         (G_F32(0x3ea0c) + -3.0f) / 6.0f);
            if (CI(0x30) == -1) {
                float k1 = 2.5f, k2 = 10.0f;
                if (flag_b) {
                    k1 = 3.5f;
                    k2 = 9.0f;
                } else if (flag_a || (MODE_CUR == MODE_A && i < G_S32(0x3e9ac))) {
                    k1 = 3.5f;
                }
                f = k1 / (CF(0x40) + 5.0f + base + fabsf(CF(0x24c) * k2));
            } else {
                int type = AT_S32(G(0x47544), i * 0x18);
                f = 1.5f / (CF(0x40) + 4.0f + base + fabsf(CF(0x24c) * 8.0f)) *
                    G_F32(0x3ea20 + type * 12) *
                    (CF(0x3f0) + CF(0x44c) + CF(0x4a8) + CF(0x504) + 4.5f) * 0.125f;
            }
            n = -CF(0x268);
            if (fabsf(CF(0x268)) > 2.0f)
                n = CF(0x268) > -0.0f ? -2.0f : (CF(0x268) < -0.0f ? 2.0f : 0.0f);
            CF(0x31c) = n * f;
            CF(0x320) = 0.0f;
            CF(0x324) = 0.0f;
            fn_00037fe8(mtx, &CF(0x31c), &CF(0x31c));
            fn_00037dec(vel, &CF(0x31c), vel);
        }

        div = CI(0x30) == -1 ? 1.0f : (float)(CI(0x34) / 2 + 1);
        CF(0x328) = L[0] * CF(0x1e0) / div;
        CF(0x32c) = L[1] * CF(0x1e0) / div;
        CF(0x330) = L[2] * CF(0x1e0) / div;
    } else {
        CF(0x328) = 0.0f;
        CF(0x32c) = 0.0f;
        CF(0x330) = 0.0f;
    }

    /* --- rolling resistance / rubber band (s22) ----------------------------------------- */
    s22 = 0.0f;
    if (CI(0x104) != 0 && MODE_CUR == MODE_A) {
        int t = CI(0x90);
        if (CI(0x30) == -1)
            s22 = 0.001f / ((float)(6 * t + 1) + (G_F32(0x3e9b8) + -1.0f) * 6.0f) + 0.0f;
        else
            s22 = 0.001f / (float)(3 * t + 1) + 0.0f;
    }
    if (CI(0x5c) >= 1) { /* nitro running */
        CI(0x5c)--;
        s22 = fminf(0.0f, s22 + -0.003f);
        if (CI(0x5c) <= 0 && CI(0x60) != 0) {
            CI(0x60) = 0;
            CI(0x54)--;
        }
    }
    if (CF(0x58) != 0.0f && (CI(0x30) > -1 || flag_b)) {
        s22 += CF(0x58) * -0.1f;
        if (s22 > 0.0f)
            s22 = 0.0f;
    }
    {
        float acc, factor = 1.0f, F;
        int type, k;

        if (CI(0x30) == -1) {
            uint8_t *o = NULL;
            float kd = 0.0f;

            if (MODE_CUR == MODE_E || MODE_CUR == MODE_C) {
                int n = NCARS - NHUMAN, kk = i + 1 - NHUMAN;
                s22 += (G_F32(0x3e9b4) + -7.5f) / 1000.0f +
                       (float)(n - kk) * 4.0f / (float)n / 2000.0f;
                if (CI(0x9c) > NCARS - i) {
                    int v;
                    o = CAR(((int32_t *)G(0x476c4))[NCARS - i - 1]);
                    v = m2d_line_gap(c, o);
                    s22 += (v > 2 ? 3.0f : (float)(v + 1)) / -2000.0f;
                    o = NULL;
                }
            } else if (MODE_CUR == MODE_A) {
                if (i >= G_S32(0x3e9ac)) {
                    s22 += 0.005f;
                } else if (CI(0x104) == 0) {
                    o = CAR(G_S32(0x3f9b0)); /* leader */
                    kd = -6.8f;
                } else if (flag_a) {
                    s22 += (G_F32(0x3e9b4) + -6.0f) / 1000.0f;
                }
            } else if (MODE_CUR == MODE_D) {
                if (i >= G_S32(0x3e9ac))
                    s22 += 0.005f;
                else
                    s22 += (G_F32(0x3e9b4) + -7.5f) / 1000.0f;
            } else if (MODE_CUR == MODE_B) {
                if (CI(0x10c) == 0 && CI(0x108) == 0) {
                    s22 += 0.005f;
                } else if (flag_b) {
                    o = CAR(CI(0x110));
                    kd = -7.0f;
                } else if (flag_a) {
                    s22 += (float)(((double)G_F32(0x3e9b4) + -5.5) +
                                   (double)((float)CI(0x114) / -300.0f)) /
                           1000.0f;
                }
            }
            if (o != NULL) { /* rubber band towards car o */
                int v = m2d_line_gap(c, o);
                float m = G_F32(0x3e9b8) + -1.0f;
                if (!((float)v > m)) {
                    float d = fn_00037fa4(&CF(0x2b0), &AT_F32(o, 0x2b0)) / 812.0f;
                    if (d <= m)
                        m = d;
                }
                s22 += (G_F32(0x3e9b4) + kd + G_F32(0x3e9b8) * -0.5f - m) / 1000.0f;
            }
            acc = fmaxf(-0.002f, fminf(0.005f, s22)) + 0.005f;
            if (MODE_CUR == MODE_E || MODE_CUR == MODE_C)
                factor = (float)(NCARS - i - 1) * 0.2f / (float)NCARS + 1.0f;
        } else {
            acc = fmaxf(-0.002f, fminf(0.005f, s22)) + 0.005f;
        }

        /* drag against the velocity, never reversing it */
        type = AT_S32(G(0x47544), i * 0x18);
        F = factor * (1.0f / G_F32(0x3ea1c + type * 12)) * acc *
            (float)(CI(0x3e4) + CI(0x440) + CI(0x49c) + CI(0x4f8) + 1) * 0.5f;
        CF(0x310) = vel[0] * F;
        CF(0x314) = vel[1] * F;
        CF(0x318) = vel[2] * F;
        fn_00037dec(&CF(0x310), &CF(0x328), T);
        for (k = 0; k < 3; k++)
            vel[k] = fabsf(vel[k]) > fabsf(T[k]) ? vel[k] - T[k] : 0.0f;
    }

    /* --- integrate position ------------------------------------------------------------- */
    {
        float s;
        CF(0x2c0) = CF(0x274);
        off[0] = 0.0f;
        off[1] = 37.0f;
        off[2] = 0.0f;
        fn_00037fe8(mtx, off, O);
        fn_00037dec(pos2, vel, pos2);
        fn_00037dec(pos2, O, &CF(0x2b0)); /* body position = ground point + 37 up */
        CF(0x2cc) = CF(0x244);
        s = CF(0x254);
        if (!(s > 10.0f))
            s = s < 5.0f ? 0.0f : s;
        else
            s = 10.0f;
        CF(0x2a0) = (float)((int)(CF(0x2a0) + s) % 40); /* wheel spin angle */
        CF(0x254) = fn_00037e54(vel);
        CF(0x260) = CF(0x254) - spd0; /* acceleration */
        s = CF(0x260) * 25.0f * m2d_sgn(CF(0x264));
        if (fabsf(s) > 10.0f)
            s = s < 0.0f ? -10.0f : (s > 0.0f ? 10.0f : 0.0f);
        CF(0x25c) += (s - CF(0x25c)) / 5.0f; /* body pitch */
    }
    for (int k = 0; k < 4; k++)
        fn_0001ea38(i, k, mtx);

    /* --- wheels against the ground and the walls -------------------------------------- */
    CI(0x6c) = 0;
    CI(0x78) = -1;
    collided = 0;
    for (int w = 0; w < 4; w++) {
        uint8_t *W = c + w * 0x5c; /* wheel fields at the car offsets 0x398.. */
        float h = 0.0f, D[3];
        int sid = 0, l20c = 0;

        if (!G_U8(0x3fab8)) {
            h = fn_0000d800(i, AT_F32(W, 0x398), AT_F32(W, 0x39c), AT_F32(W, 0x3a0), &l20c);
            sid = l20c;
        }
        if (CF(0x254) > 0.5f)
            h += (float)(sid / 10);
        if (sid == 0) {
            AT_S32(W, 0x3e4) = 0;
        } else {
            AT_S32(W, 0x3e4) = 1;
            if (CF(0x254) > 0.5f) { /* skid mark */
                int ix = (int)((AT_F32(W, 0x398) + 406.0f) / 812.0f);
                int iz = (int)((AT_F32(W, 0x3a0) + 406.0f) / 812.0f);
                int tile = m2d_tile(ix, iz);
                uint32_t col;
                uint8_t *cb = (uint8_t *)&col;
                float sc;
                bool hot;
                int type;

                fn_0001cae8(&col, ix, iz);
                sc = CF(0x254) / 50.0f;
                hot = G_F32(0x3ea18) + G_F32(0x3ea0c) > 4.0f || tile == 10;
                type = hot ? 0x85 : 0x7e;
                if (tile == 10) {
                    col = 0xffffffffu;
                    sc += 2.5f;
                } else {
                    cb[1] = (uint8_t)((unsigned)cb[1] * 2 / 3);
                    cb[2] = (uint8_t)((unsigned)cb[2] * 2 / 3);
                    cb[3] = 0xff;
                    sc += 0.5f;
                }
                if (G_S32(0x3f8e0) != 0 && !hot && AT_F32(W, 0x3f0) > 0.0f)
                    AT_F32(W, 0x3f0) += -0.0005f; /* tyre wear */
                fn_00031020(i, w, col, type, sc);
            }
        }
        if (collided)
            continue;

        D[0] = AT_F32(W, 0x398) - pos2[0];
        D[1] = AT_F32(W, 0x39c) - pos2[1];
        D[2] = AT_F32(W, 0x3a0) - pos2[2];
        if (fn_00037e54(D) > 0.0f) {
            fn_000379a8(D, D);
        } else {
            D[0] = CF(0x2e0);
            D[1] = CF(0x2e4);
            D[2] = CF(0x2e8);
        }
        if (fn_00037e54(D) > 0.0f && h - CF(0x274) > 20.0f) {
            /* wall: average the blocked directions around the car into a normal */
            int a0 = (int)(atan2f(D[0], D[2]) * RAD2DEG), sum = -1, cnt = 0, n;
            float an, N[3], R[3], hh;

            for (int r = -180; r != 190; r += 10) {
                float ang = (float)(a0 + r) * DEG2RAD;
                float sx = sinf(ang) * 70.0f, cz = cosf(ang) * 70.0f;
                hh = fn_0000d800(i, pos2[0] + sx, pos2[1], pos2[2] + cz, &l20c);
                if (hh - CF(0x274) > 10.0f) {
                    sum += a0 + r;
                    cnt++;
                }
            }
            if (cnt == 0)
                cnt = 1;
            an = (float)(sum / cnt) * DEG2RAD;
            N[0] = sinf(an);
            N[1] = 0.0f;
            N[2] = cosf(an);
            fn_00037eb4(D, N, R);
            {
                float sp = CF(0x254) * 0.5f + 2.0f;
                vel[0] = R[0] * sp;
                vel[1] = R[1] * sp;
                vel[2] = R[2] * sp;
            }
            if (fn_00037e54(vel) > 0.0f)
                fn_000379a8(vel, D);
            else if (fn_00037e54(&CF(0x2e0)) > 0.0f)
                fn_000379a8(&CF(0x2e0), D);

            if (CF(0x254) > 3.0f && i < NHUMAN) { /* crash sound */
                float l = 0.0f, rr = 0.0f;
                fn_0001e804(pos2[0], pos2[1], pos2[2], i, &l, &rr);
                fn_0003613c(G_S32(0x3ebd8), (int)(l * 20000.0f), (int)(rr * 20000.0f));
                fn_000360bc(G_S32(0x3ebd8));
            }

            /* push the car out of the wall */
            n = 1;
            for (;;) {
                pos2[0] -= N[0] * 10.0f;
                pos2[1] -= N[1] * 10.0f;
                pos2[2] -= N[2] * 10.0f;
                off[0] = 0.0f;
                off[1] = 37.0f;
                off[2] = 0.0f;
                fn_00037fe8(mtx, off, O);
                fn_00037dec(pos2, O, &CF(0x2b0));
                fn_0001ea38(i, w, mtx);
                hh = fn_0000d800(i, AT_F32(W, 0x398), AT_F32(W, 0x39c), AT_F32(W, 0x3a0),
                                 &l20c);
                if (!(hh - CF(0x274) > 5.0f))
                    break;
                if (n++ >= 50)
                    break;
            }
            for (int k = 0; k < 4; k++) {
                AT_F32(c, 0x3e0 + k * 0x5c) = 0.0f;
                AT_F32(c, 0x3d8 + k * 0x5c) = AT_F32(c, 0x3d4 + k * 0x5c);
            }
            for (int k = 0; k < 4; k++)
                fn_0001ea38(i, k, mtx);
            collided = 1;
            if (CI(0x30) == -1) { /* player: count wall hits in quick succession */
                if (G_F32(0x3f8ec) - (float)CI(0xdc) < 2500.0f)
                    CI(0xe0)++;
                else
                    CI(0xe0) = 0;
                CI(0xdc) = (int)G_F32(0x3f8ec);
            }
        } else {
            /* suspension: fall under gravity or snap to the ground */
            AT_F32(W, 0x3ec) = h;
            AT_F32(W, 0x3d8) = AT_F32(W, 0x3d4);
            if ((double)(AT_F32(W, 0x3d4) + AT_F32(W, 0x3e0)) > (double)h + 0.5) {
                AT_F32(W, 0x3e0) += -0.2f;
                AT_S32(W, 0x3dc) = 1;
                AT_F32(W, 0x3d4) += AT_F32(W, 0x3e0);
            } else {
                float v = h - AT_F32(W, 0x3d4);
                AT_F32(W, 0x3d4) = h;
                AT_F32(W, 0x3e0) = fminf(3.0f, v);
                AT_S32(W, 0x3dc) = 0;
            }
        }
    }

    /* --- body height and tilt from the four wheels ------------------------------------- */
    if (CI(0x70) == 5 && CI(0x78) == -1)
        CI(0x70) = 0;
    {
        float y0 = CF(0x3d4), y1 = CF(0x430), y2 = CF(0x48c), y3 = CF(0x4e8);
        float avg = (y0 + y1 + y2 + y3) * 0.25f, e, e2, old268 = CF(0x268);

        CF(0x274) = avg;
        CF(0x270) = (CF(0x3ec) + CF(0x448) + CF(0x4a4) + CF(0x500)) * 0.25f;
        CF(0x544) = (y0 + y1) * 0.5f;
        CF(0x5a0) = (y1 + y2) * 0.5f;
        CF(0x5fc) = (y2 + y3) * 0.5f;
        CF(0x658) = (y3 + y0) * 0.5f;
        e = CF(0x544) - avg;
        CF(0x298) = (float)(m2d_sgnd(e) *
                            (acos((double)(CF(0x288) / sqrtf(CF(0x288) * CF(0x288) + e * e))) *
                             (double)RAD2DEG));
        e2 = CF(0x5a0) - avg;
        CF(0x29c) = (float)(m2d_sgnd(e2) *
                            (acos((double)(CF(0x288) / sqrtf(CF(0x288) * CF(0x288) + e2 * e2))) *
                             (double)RAD2DEG));
        CF(0x2c8) = CF(0x298) + CF(0x2a0) / 20.0f + -1.0f - CF(0x25c);
        CF(0x2d0) = CF(0x29c) - old268;
    }
    CF(0x264) = fn_00037e80(vel, &CF(0x2e0));
    CF(0x268) = fn_00037e80(vel, &CF(0x2ec));
    fn_00030fcc(i);
    CF(0x240) = fminf(110.0f, CF(0x240) + (CF(0x23c) - CF(0x240)) / 5.0f);

    /* --- tyre smoke ---------------------------------------------------------------------- */
    if (CF(0x254) > 0.5f) {
        float stat = G_F32(0x3ea0c) + G_F32(0x3ea18);
        if (fabsf(sinf(CF(0x26c) * DEG2RAD)) > 0.25f || fabsf(d14) > 10.0f ||
            (stat > 5.0f && CI(0x6c) == 0)) {
            int type = G_F32(0x3ea0c) + G_F32(0x3ea18) > 5.0f ? 0x85 : 0x7e;
            float sc = CF(0x254) / 50.0f + 0.5f;
            int w;
            if (G_F32(0x3ea18) > 4.0f)
                sc *= 0.5f;
            w = (int)(CF(0x2b0) + CF(0x2b8)) % 4;
            if (CI(0x3e4 + w * 0x5c) == 0 && G_S32(0x3f8e0) != 0 &&
                !(G_F32(0x3ea0c) + G_F32(0x3ea18) > 5.0f) && CF(0x3f0 + w * 0x5c) > 0.0f)
                CF(0x3f0 + w * 0x5c) += -0.0002f;
            fn_00031020(i, w % 2 + 2, 0x9bfafafau, type, sc);
        }
    }

    /* --- dust puff ------------------------------------------------------------------------ */
    if ((MODE_CUR == MODE_B && CI(0x114) > fn_00004400(100) * G_S32(0x3e9d8) / 25 &&
         (CI(0x10c) != 0 || CI(0x108) != 0)) ||
        (CF(0x20) == 0.0f && fn_00004400(100) == 0 && CF(0x254) > 0.0f)) {
        float sc = CF(0x254) / 50.0f;
        uint8_t *P = m2d_part_next();
        AT_S32(P, 0x30) = 0;
        AT_F32(P, 0x0) = CF(0x2b0);
        AT_F32(P, 0x4) = CF(0x2b4);
        AT_F32(P, 0x8) = CF(0x2b8);
        AT_F32(P, 0x20) = (float)fn_00004400(360);
        AT_F32(P, 0xc) = AT_F32(P, 0x0);
        AT_F32(P, 0x10) = AT_F32(P, 0x4);
        AT_F32(P, 0x14) = AT_F32(P, 0x8);
        AT_U32(P, 0x40) = 0xff000000u;
        AT_F32(P, 0x34) = sc + 0.5f;
        AT_S32(P, 0x38) = 0x7e;
    }

    /* --- pit garages on the start straight (track objects 0x44590) ----------------------- */
    if (G_U8(0x3fab8))
        return;
    fn_000312cc(i, CI(0x30));
    {
        int ix = (int)((pos2[0] + 406.0f) / 812.0f);
        int iz = (int)((pos2[2] + 406.0f) / 812.0f);
        int ord = 0, tile, l1b4 = 0;
        float h, base[3], hp50;

        if ((ix | iz) >= 0 && ix <= GRID_MAXX && iz <= GRID_MAXZ)
            ord = GRID_ORDER[ix * 20 + iz];
        if (G_S32(0x3f8e0) == 0 || i < 0 || G_U8(0x3fab8) || ord < G_S32(0x3f948) - 1)
            return;
        tile = m2d_tile(ix, iz);
        h = fn_0000d800(-1, pos2[0], pos2[1], pos2[2], &l1b4);
        base[0] = (float)ix * 812.0f + -406.0f + 406.0f;
        base[1] = h;
        base[2] = (float)iz * 812.0f + -406.0f + 406.0f;
        hp50 = h + 50.0f;

        for (int k = 0; k < 4; k++) {
            uint8_t *ob = (uint8_t *)G(0x44590) + k * 0x34;
            float A[3], Rr[3], Q[3], V[3], dist, t, hd, v;
            int st;

            if (AT_S32(ob, 0x14) != i || (CI(0x74) != -1 && CI(0x74) != k))
                continue;
            fn_0000fb78(k, tile, M2D_P2I(A), M2D_P2I(Rr));
            fn_00037dec(base, A, A);
            Q[0] = pos2[0];
            Q[1] = A[1];
            Q[2] = pos2[2];
            dist = fn_00037fa4(Q, A);
            V[0] = A[0] - pos2[0];
            V[1] = 0.0f;
            V[2] = A[2] - pos2[2];
            hd = CF(0x244);
            if (fn_00037e54(V) > 5.0f) {
                fn_000379a8(V, V);
                t = atan2f(V[0], V[2]) * RAD2DEG;
            } else {
                t = hd;
            }
            while (t - hd > 180.0f)
                t += -360.0f;
            while (hd - t > 180.0f)
                t += 360.0f;
            while (Rr[1] - hd > 180.0f)
                Rr[1] += -360.0f;
            while (hd - Rr[1] > 180.0f)
                Rr[1] += 360.0f;

            if (!(CF(0x254) < 10.0f) || CF(0x264) < 0.0f) {
                CI(0x70) = 0;
                continue;
            }
            if (!(fabsf(Rr[1] - hd) < 60.0f) && CI(0x30) != -1) {
                CI(0x70) = 0;
                continue;
            }
            st = CI(0x70);
            if (!(fabsf(t - hd) < 60.0f) && CI(0x30) != -1)
                goto leave;

            v = dist / 20.0f + 1.0f;
            if ((unsigned)(st - 3) >= 2) { /* steer towards the garage */
                float nh = t;
                if (CF(0x254) > v)
                    in[INP_BRAKE] = 1;
                CI(0x28) = 2;
                if (dist > 0.0f)
                    nh = hd + (t - hd + (t - hd)) * CF(0x254) / dist;
                CF(0x244) = nh;
                AT_F32(in, INP_STEER) = (t - nh) * 4.0f;
            }
            if (dist < 130.0f && st == 0) {
                CI(0x70) = 1;
                AT_S32(ob, 0x1c) = i;
                CI(0x7c) = 0;
            } else if ((unsigned)(st - 2) < 2) {
                /* parked: hold the car on the lift */
                pos2[0] = A[0];
                pos2[1] = A[1];
                pos2[2] = A[2];
                CF(0x2c8) = Rr[0];
                CF(0x2cc) = Rr[1];
                CF(0x2d0) = Rr[2];
                CF(0x244) = Rr[1];
                vel[0] = 0.0f;
                vel[1] = 0.0f;
                vel[2] = 0.0f;
                if (CF(0x2b4) < hp50) {
                    CF(0x7c) += 2.0f;
                } else {
                    AT_S32(ob, 0x18) = i;
                    CI(0x70) = 3;
                    if (AT_F32(ob, 0x0) >= 135.0f && AT_F32(ob, 0x0) < 270.0f) {
                        if (CF(0x20) < 1.0f)
                            CF(0x20) += 0.00725f; /* refuel */
                        else
                            CF(0x20) = 1.0f;
                    }
                    st = 3;
                }
                in[INP_BRAKE] = 1;
                goto leave;
            } else if (st != 1) {
                goto leave;
            }
            /* state 1: drive in */
            if (dist < 10.0f) {
                CI(0x70) = 2;
                continue;
            }
            if (dist < 50.0f) {
                float a = t * DEG2RAD;
                CI(0x28) = 1;
                CF(0x24c) = 0.0f;
                vel[0] = sinf(a) * v;
                vel[1] = 0.0f;
                vel[2] = cosf(a) * v;
            }
            continue;
        leave:
            if (st == 4 && dist > 100.0f) { /* drove away: free the garage */
                AT_S32(ob, 0x0) = 0;
                AT_S32(ob, 0x4) = 0;
                AT_S32(ob, 0x8) = 0;
                AT_S32(ob, 0xc) = 0;
                AT_S32(ob, 0x10) = 0;
                AT_S32(ob, 0x20) = 0;
                AT_S32(ob, 0x24) = 0;
                AT_S32(ob, 0x28) = 0;
                AT_S32(ob, 0x2c) = 0;
                AT_S32(ob, 0x30) = 0;
                AT_S32(ob, 0x14) = -1;
                AT_S32(ob, 0x18) = -1;
                CI(0x70) = 5;
                AT_S32(ob, 0x1c) = -1;
                CI(0x74) = -1;
            }
        }
    }
}

/* 0x000302d0 -- steps the music volume G_S32(0x3f980) by 50 toward target, applies it and
 * stops the sound at 0 (Ghidra folds it into fn_0002d550) */
void fn_000302d0(int snd, int target)
{
    int v = G_S32(0x3f980);

    if (v < target) {
        v += 50;
        G_S32(0x3f980) = v;
        if (v > target) {
            v = target;
            G_S32(0x3f980) = v;
        }
    } else if (v > target) {
        v -= 50;
        G_S32(0x3f980) = v;
        if (v < target) {
            v = target;
            G_S32(0x3f980) = v;
        }
    }
    fn_0003613c(snd, (uint16_t)v, (uint16_t)v);
    if (G_S32(0x3f980) <= 0)
        fn_0003611c(snd);
}

/* Toggles an int option 0 <-> 1. */
static void m2d_toggle(int32_t *p)
{
    *p = 1 - *p;
}

/* Loads entry n of the map list 0x3eafc ("map_%02d") and resets the map view. */
static void m2d_load_map(int n)
{
    char name[256];
    snprintf(name, sizeof name, "map_%02d", (int)((int32_t *)G(0x3eafc))[n]);
    fn_0000cd00(name);
    G_S32(0x3fa3c) = G_S32(0x3fa34);
    G_S32(0x3fa40) = G_S32(0x3fa38);
}

/* 0x00030318 -- front-end menu input: main menu (10), options (12), player menus (11, 25),
 * map browser (30), screens 4 / 5 (guess) */
void fn_00030318(void)
{
    int scr = G_S32(0x3fa24);

    if (!((unsigned)(scr - 10) < 3 || scr == 0x19)) {
        if (scr == 0x1e) { /* map browser */
            if (MODE_CUR == MODE_C && !(G_U8(0x3f900) & 1) && !(G_U8(0x3f904) & 1)) {
                G_U8(0x3f90c) = 1;
                if (G_U8(0x3fa6c) & 1) {
                    int n = G_S32(0x3f8b0) + 1;
                    if (n > 9)
                        n = 0;
                    G_S32(0x3f8b0) = n;
                    m2d_load_map(n);
                }
                if (G_U8(0x3fa68) & 1) {
                    int n = G_S32(0x3f8b0) - 1;
                    if (n < 0)
                        n = 9;
                    G_S32(0x3f8b0) = n;
                    m2d_load_map(n);
                }
                if (G_U8(0x3fa60) & 1) {
                    G_S32(0x3f8b8)++;
                    if (G_S32(0x3f8b8) >= 5)
                        G_S32(0x3f8b8) = 0;
                }
                if (G_U8(0x3fa64) & 1) {
                    G_S32(0x3f8b8)--;
                    if (G_S32(0x3f8b8) <= -1)
                        G_S32(0x3f8b8) = 4;
                }
                G_S32(0x3fa44) = G_S32(0x3f8b0);
                if ((G_U8(0x3fa74) & 1) && !(G_U8(0x3f900) & 1) &&
                    ((int32_t *)G(0x481f4))[G_S32(0x3f8b0)] != 0) { /* slot 0x3f370 */
                    G_U8(0x3fee0) = 1;
                    G_S32(0x3f8f0) = 0;
                    G_U8(0x3f908) = 1;
                    G_S32(0x3f8f4) = 0xff;
                }
                if ((G_U8(0x3fa70) & 1) && !(G_U8(0x3f900) & 1)) {
                    G_S32(0x3e9d0) = -G_S32(0x3e9d0);
                    fn_0000cfc8();
                }
                return;
            }
            if ((G_U8(0x3fa70) & 1) && (G_U8(0x3f890) & 1))
                G_U8(0x3f88c) = 1;
            G_U8(0x3f90c) = 0;
            return;
        }
        if (scr == 5) {
            if (G_U8(0x3fa74) & 1) {
                G_S32(0x3fa28) = 10;
                G_S32(0x3fa2c) = 0;
            }
            return;
        }
        if (scr == 4 && (G_U8(0x3fa74) & 1)) {
            G_S32(0x3f874) = 1;
            G_S32(0x3f878) = 1;
        }
        return;
    }

    /* list menus: left / right move the cursor G_S32(0x3faa8) with a panned click */
    if ((G_U8(0x3fa6c) & 1) && G_S32(0x3fa88) <= 1) {
        G_S32(0x3faac) = 0;
        G_S32(0x3faa8)++;
        fn_0003613c(G_S32(0x3ebcc), 0, 8000);
        fn_000360bc(G_S32(0x3ebcc));
    }
    if ((G_U8(0x3fa68) & 1) && G_S32(0x3fa84) <= 1) {
        G_S32(0x3faac) = 0;
        G_S32(0x3faa8)--;
        fn_0003613c(G_S32(0x3ebcc), 8000, 0);
        fn_000360bc(G_S32(0x3ebcc));
    }
    if (G_S32(0x3faa8) > G_S32(0x3ec00) - 1)
        G_S32(0x3faa8) = 0;
    else if (G_S32(0x3faa8) <= -1)
        G_S32(0x3faa8) = G_S32(0x3ec00) - 1;
    if (scr == 0xc && optm_input()) /* the port's Controls / 3D pages (opt_menu.c) */
        return;

    if (G_U8(0x3fa74) & 1) { /* ok */
        int sel = G_S32(0x3faa8);
        if (scr > 0x18) {
            if (scr == 0x19) {
                AT_S32(G(0x47544), G_S32(0x3fa08) * 0x18 + 0x10) = sel;
                G_S32(0x3fa28) = 0x17;
                G_S32(0x3fa2c) = 0;
            }
            return;
        }
        if (scr == 10) { /* main menu: pick the game mode */
            if (G_S32(0x3f8c4) == 0 && sel == 0) {
                MODE_CUR = MODE_E;
                G_S32(0x3fa28) = 0x14;
                NCARS = 8;
                fn_00030ac4();
                sel = G_S32(0x3faa8);
            }
            if (sel == 1) {
                MODE_CUR = MODE_C;
                G_S32(0x3fa28) = 0x14;
                NCARS = 8;
                fn_00030ac4();
                sel = G_S32(0x3faa8);
            }
            if (sel == 2 && G_S32(0x3f8c4) == 0) {
                MODE_CUR = MODE_A;
                G_S32(0x3fa28) = 0x14;
                NCARS = 0xc;
                fn_00030ac4();
                sel = G_S32(0x3faa8);
            }
            if (sel == 3 && G_S32(0x3f8c4) == 0) {
                MODE_CUR = MODE_B;
                G_S32(0x3fa28) = 0x14;
                NCARS = 0xc;
                fn_00030ac4();
                sel = G_S32(0x3faa8);
            }
            if (sel == 4) {
                MODE_CUR = MODE_D;
                G_S32(0x3fa28) = 0x14;
                NCARS = 0xc;
                fn_00030ac4();
                sel = G_S32(0x3faa8);
            }
            if (sel == 5)
                G_S32(0x3fa28) = 0xc;
            G_S32(0x3fa2c) = 0;
            return;
        }
        if (scr == 0xb) {
            fn_00030ac4();
            return;
        }
        if (scr != 0xc)
            return;
        switch (sel) { /* options */
        case 0:
            m2d_toggle(&G_S32(0x3e9e0));
            break;
        case 1:
            m2d_toggle(&G_S32(0x3f8bc));
            break;
        case 2: /* cycles 3, 5, 6, 7, 9, 10 */
            switch (G_S32(0x3e9e4)) {
            case 3:
                G_S32(0x3e9e4) = 5;
                break;
            case 5:
                G_S32(0x3e9e4) = 6;
                break;
            case 6:
                G_S32(0x3e9e4) = 7;
                break;
            case 7:
                G_S32(0x3e9e4) = 9;
                break;
            case 9:
                G_S32(0x3e9e4) = 10;
                break;
            default:
                G_S32(0x3e9e4) = 3;
                break;
            }
            break;
        case 3: /* float 1, 2, 3 */
            G_F32(0x3e9b8) += 1.0f;
            if (G_F32(0x3e9b8) > 3.0f)
                G_F32(0x3e9b8) = 1.0f;
            break;
        case 4:
            m2d_toggle(&G_S32(0x3ea20));
            break;
        case 5: /* float 0, 1, 2, 3 */
            G_F32(0x3e9bc) += 1.0f;
            if (G_F32(0x3e9bc) > 3.0f)
                G_F32(0x3e9bc) = 0.0f;
            break;
        case 6:
            m2d_toggle(&G_S32(0x3f8ac));
            break;
        default:
            break;
        }
        return;
    }

    if (!(G_U8(0x3fa70) & 1)) /* back */
        return;
    if (scr == 0xb) {
        G_S32(0x3fa28) = 10;
    } else if (scr == 0x19) {
        G_S32(0x3fa28) = 0x15;
    } else if (scr == 0xc) {
        fn_0000fe34();
        G_S32(0x3fa28) = 10;
    } else {
        return;
    }
    G_S32(0x3fa2c) = 0;
}

/* 0x00030ac4 -- single-player setup: one human, player table entries "1UP".., clears flag
 * +0xc of the other players, requests screen 20 (guess) */
void fn_00030ac4(void)
{
    if (G_S32(0x3f8c4) != 0 && G_S32(0x3faa8) != 0)
        return;
    NHUMAN = 1;
    G_S32(0x3fa08) = 0;
    G_S32(0x3fa0c) = 0;
    if (G_S32(0x3f8a8) != 1) {
        int n = G_S32(0x3f8a8);
        if (n <= 0) {
            do {
                uint8_t *e = (uint8_t *)G(0x47544) + n * 0x18;
                AT_S32(e, 0x0) = 1;
                AT_S32(e, 0x4) = 0;
                AT_S32(e, 0x8) = 1;
                AT_S32(e, 0xc) = 0;
                AT_S32(e, 0x10) = 1;
                n++;
                sprintf((char *)e + 0x14, "%dUP", n);
            } while (n < NHUMAN);
        }
        G_S32(0x3f8a8) = NHUMAN;
    }
    if (NHUMAN < NCARS) {
        int end = NHUMAN + 1 > NCARS ? NHUMAN + 1 : NCARS;
        for (int k = NHUMAN; k < end; k++)
            AT_S32(G(0x47544), k * 0x18 + 0xc) = 0;
    }
    AT_S32(CAR(0), 0x108) = 0;
    G_S32(0x3fa28) = 0x14;
    G_S32(0x3fa2c) = 0;
}

/* Held-button auto-repeat: fires on press, then every frame once held for 20 frames. */
static void m2d_repeat(bool held, uint32_t counter, uint32_t flag, uint32_t speed)
{
    if (!held) {
        G_S32(counter) = 0;
        return;
    }
    if (G_S32(counter) == 0) {
        G_U8(flag) = 1;
        G_F32(speed) = 5.0f;
    } else if (G_S32(counter) > 19) {
        G_U8(flag) = 1;
        return;
    }
    G_S32(counter)++;
}

/* Edge-triggered button with a hold-off counter. */
static void m2d_press(bool held, uint32_t counter, uint32_t flag, int holdoff)
{
    if (held) {
        if (G_S32(counter) == 0) {
            G_U8(flag) = 1;
            G_S32(counter) = holdoff;
        }
    } else if (G_S32(counter) >= 1) {
        G_S32(counter)--;
    }
}

/* Eases a scroll speed back down to 1. */
static void m2d_ease(uint32_t a)
{
    if (G_F32(a) > 1.0f)
        G_F32(a) += -0.5f;
    if (G_F32(a) < 1.0f)
        G_F32(a) = 1.0f;
}

/* 0x00030bf4 -- menu animation angles and the one-frame button flags 0x3fa60..0x3fa78 from
 * the touch buttons of player 0 and the active player (repeating arrows, back, ok) */
void fn_00030bf4(void)
{
    uint8_t *B, *P;

    G_F32(0x3faac) += 0.5f;
    if (G_F32(0x3faac) > 360.0f)
        G_F32(0x3faac) = 0.0f;
    G_F32(0x3fa10) += 0.1f;
    if (G_F32(0x3fa10) > 360.0f)
        G_F32(0x3fa10) = 0.0f;
    G_U8(0x3fa60) = 0;
    G_U8(0x3fa64) = 0;
    G_U8(0x3fa68) = 0;
    G_U8(0x3fa6c) = 0;
    G_U8(0x3fa70) = 0;
    G_U8(0x3fa74) = 0;
    G_U8(0x3fa78) = 0;
    if (!(G_U8(0x3f924) & 1))
        return;

    B = G(0x5c160);
    P = B + G_S32(0x3fa08) * 0x24;
    m2d_repeat((B[0xab] | P[0xab]) != 0, 0x3fa84, 0x3fa68, 0x3fa98); /* left */
    m2d_repeat((B[0xac] | P[0xac]) != 0, 0x3fa88, 0x3fa6c, 0x3fa9c); /* right */
    m2d_repeat((B[0xae] | P[0xae]) != 0, 0x3fa7c, 0x3fa60, 0x3faa0);
    m2d_repeat((B[0xad] | P[0xad]) != 0, 0x3fa80, 0x3fa64, 0x3faa4);
    m2d_press((B[0x83] | P[0x83]) != 0, 0x3fa8c, 0x3fa70, 1); /* back */
    m2d_press((B[0x84] | P[0x84]) != 0, 0x3fa90, 0x3fa74, 1); /* ok */
    m2d_press((B[0x7c] | P[0x7c]) != 0, 0x3fa94, 0x3fa78, 5);
    m2d_ease(0x3fa98);
    m2d_ease(0x3fa9c);
    m2d_ease(0x3faa0);
    m2d_ease(0x3faa4);
}

/* 0x00030fcc -- engine rpm from the forward speed and gear ratio (neutral: decays) */
void fn_00030fcc(int car)
{
    uint8_t *c = CAR(car);

    if (CI(0x28) == 1)
        CF(0x238) += CF(0x238) / -20.0f;
    else
        CF(0x238) = fabsf(CF(0x264) / CF(0x1e8 + CI(0x28) * 4) * 100.0f);
}

/* 0x00031020 -- drops a skid-mark / smoke particle (type 0x7e or 0x85) at a wheel once it is
 * far enough from the previous one */
void fn_00031020(int car, int wheel, unsigned color, int type, float scale)
{
    uint8_t *W = CAR(car) + wheel * 0x5c;
    float dx, dz, thr;
    uint8_t *P;

    if (AT_S32(W, 0x3dc) != 0) /* in the air */
        return;
    if (type != 0x85 && type != 0x7e)
        return;
    dx = AT_F32(W, 0x3c8) - AT_F32(W, 0x398);
    dz = AT_F32(W, 0x3d0) - AT_F32(W, 0x3a0);
    if (NHUMAN < car)
        thr = 2500.0f;
    else
        thr = type == 0x85 ? 650.0f : 1250.0f;
    if (!(dx * dx + dz * dz > scale * thr))
        return;

    P = m2d_part_next();
    AT_S32(P, 0x30) = 0;
    AT_F32(P, 0x0) = AT_F32(W, 0x398);
    AT_F32(P, 0x4) = AT_F32(W, 0x39c);
    AT_F32(P, 0x8) = AT_F32(W, 0x3a0);
    AT_F32(P, 0xc) = AT_F32(P, 0x0);
    AT_F32(P, 0x10) = AT_F32(P, 0x4);
    AT_F32(P, 0x14) = AT_F32(P, 0x8);
    AT_F32(P, 0x20) = (float)fn_00004400(360);
    if (type == 0x85) {
        AT_U32(P, 0x40) = G_F32(0x3ea18) > 4.0f ? G_U32(0x3eb7c) : color;
        AT_F32(P, 0x34) = (G_F32(0x3ea0c) + G_F32(0x3ea18) + -5.0f) * scale + 1.0f;
    } else {
        AT_U32(P, 0x40) = color;
        AT_F32(P, 0x34) = scale;
    }
    AT_S32(P, 0x38) = type;
    AT_F32(W, 0x3c8) = AT_F32(P, 0x0);
    AT_F32(W, 0x3cc) = AT_F32(P, 0x4);
    AT_F32(W, 0x3d0) = AT_F32(P, 0x8);
}

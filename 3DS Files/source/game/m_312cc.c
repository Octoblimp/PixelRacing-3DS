/* m_312cc: per-car race logic (checkpoints, laps, split times, respawn, slipstream, human and
 * AI driving input), menu helpers, race-start asset loading, points and engine sounds.
 * Module range [0x000312cc, 0x00035df8). */
#include "ios.h"
#include "opt_menu.h"
#include "net_game.h"

/* Car array: 0x678 bytes per car. */
#define M312_CAR(i) ((uint8_t *)G(0x48ae0) + (i) * 0x678)
#define M312_NCARS G_S32(0x3e9c8)
#define M312_NPTS G_S32(0x3f948)   /* racing-line points (1-based) */
#define M312_CLOCK G_F32(0x3f8ec)  /* race clock, ms */
#define M312_PTS ((float *)G(0x45640)) /* racing line: {row, col} per point, in tiles */
#define M312_MAXROW G_S32(0x3f8a4)
#define M312_MAXCOL G_S32(0x3f8a0)

/* Game modes (values held in the slot table). */
#define M312_MODE G_S32(0x3f9dc)
#define M312_MODE_A G_S32(0x3ebe0) /* 1 */
#define M312_MODE_B G_S32(0x3ebe4) /* 2 */
#define M312_MODE_C G_S32(0x3ebe8) /* 3 */
#define M312_MODE_D G_S32(0x3ebec) /* 4 */
#define M312_MODE_E G_S32(0x3f9d8)

#define M312_WHITE G_U32(0x3eb7c)
#define M312_HILITE G_U32(0x3eb84)

/* Per-car control block: 0x14 bytes {u8 brake, u8 accel, u8 [2], u8 [3], .., u8 [6], u8 [7],
 * .., float steer @0xc, u8 skid @0x10}. */
#define M312_CTL(i) ((uint8_t *)G(0x5c210) + (i) * 0x14)
/* Per-pad input block: 0x24 bytes, base 0x5c160 (analog at 0x8c/0x90, buttons at 0xa0..0xa8). */
#define M312_PAD(i) ng_pad(i) /* 3DS: was 0x5c160 + i*0x24, see net_game.h */

static int m312cc_clamp(int v, int hi)
{
    if (v < 0)
        return 0;
    return v > hi ? hi : v;
}

/* Track tile kind at (row, col), clamped into the grid. */
static int m312cc_tile(int row, int col)
{
    return ((uint8_t *)G(0x44800))[m312cc_clamp(row, M312_MAXROW) * 20 +
                                   m312cc_clamp(col, M312_MAXCOL)];
}

static float m312cc_sign(float v)
{
    if (v < 0.0f)
        return -1.0f;
    return (float)(v > 0.0f);
}

static void m312cc_fmt_time(char *dst, int t)
{
    sprintf(dst, "%02d:%02d:%02d", (t / 60000) % 60, (t / 1000) % 60, (t / 10) % 100);
}

/* Blank the five on-screen message lines of a car and reset their colours. */
static void m312cc_clear_msgs(uint8_t *c)
{
    int k;
    for (k = 0; k < 5; k++)
        strcpy((char *)c + 0x12c + k * 0x20, " ");
    for (k = 0; k < 5; k++)
        AT_U32(c, 0x1cc + k * 4) = M312_WHITE;
}

/* "+mm:ss:cc " (or "(+laps)+mm:ss:cc ") gap to the car ahead `o` on time field `off`. */
static void m312cc_gap(char *dst, uint8_t *c, int o, int off)
{
    uint8_t *co = M312_CAR(o);
    char buf[32];
    int d = (int)fabsf((float)(AT_S32(c, off) - AT_S32(co, off)));
    int laps;

    m312cc_fmt_time(buf, d);
    laps = AT_S32(co, 0x8c) - AT_S32(c, 0x8c);
    if (laps >= 1 && AT_S32(co, 0x84) > AT_S32(c, 0x84))
        sprintf(dst, "(+%d)+%s ", laps, buf);
    else
        sprintf(dst, "+%s ", buf);
}

/* Split time `stage` (1 or 2) reached: time field `off` (0xc0 / 0xc4), bonus string slot. */
static void m312cc_split(uint8_t *c, int car, int off, int stage, uint32_t bonus_str)
{
    float clock = M312_CLOCK;
    char *s1 = (char *)c + 0x12c;
    char *s2 = (char *)c + 0x14c;

    m312cc_clear_msgs(c);
    AT_S32(c, off) = (int)clock;
    m312cc_fmt_time(s1, AT_S32(c, off) - AT_S32(c, 0xbc));
    if (AT_S32(c, 0x9c) <= 1) {
        AT_S32(c, 0xcc) = -1;
        strcpy(s2, " ");
    } else {
        int o = ((int *)G(0x476c4))[AT_S32(c, 0x9c) - 2]; /* car one place ahead */
        AT_S32(c, 0xcc) = o;
        if (o < 0)
            strcpy(s2, " ");
        else
            m312cc_gap(s2, c, o, off);
    }
    if (G_S32(0x3e9b0) == 2) { /* split screen: one line only */
        strcpy(s1, s2);
        strcpy(s2, " ");
    }
    AT_U32(c, 0x1cc) = 0xffffffffu;
    AT_U8(c, 0x1d0) = 0xff;
    AT_U8(c, 0x1d1) = 0;
    AT_U8(c, 0x1d2) = 0;
    AT_U8(c, 0x1d3) = 0xff;
    AT_S32(c, 0x128) = (int)(clock + 3000.0f);
    AT_S32(c, 0x94) = stage;
    if (AT_S32(c, 0x98) == stage - 1) {
        AT_S32(c, 0x98) = stage;
        if (M312_MODE == M312_MODE_A && AT_S32(c, 0x104) != 0) {
            AT_S32(c, 0x11c) += 3;
            strcpy(s2, (const char *)G_PTR(bonus_str));
            strcpy((char *)c + 0x16c, "+3");
            AT_U32(c, 0x1d4) = M312_HILITE;
        }
    }
    if (G_S32(0x3e9b0) > car && !(G_U8(0x3f928) & 1)) {
        fn_00034810(G_S32(0x3ebc0), car, 14000);
        fn_000360bc(G_S32(0x3ebc0));
    }
}

/* "Wrong way" message. */
static void m312cc_wrong_way(uint8_t *c)
{
    AT_S32(c, 0x128) = (int)(M312_CLOCK + 2000.0f);
    strcpy((char *)c + 0x12c, " ");
    strcpy((char *)c + 0x14c, " ");
    sprintf((char *)c + 0x16c, "%s", (const char *)G_PTR(0x47e50));
    strcpy((char *)c + 0x18c, " ");
    strcpy((char *)c + 0x1ac, " ");
    AT_U32(c, 0x1cc) = M312_WHITE;
    AT_U32(c, 0x1d0) = M312_WHITE;
    AT_U32(c, 0x1d8) = M312_WHITE;
    AT_U32(c, 0x1dc) = M312_WHITE;
    AT_U32(c, 0x1d4) = M312_HILITE;
}

/* Normalise angle `a` (degrees) to within 180 of `ref`. */
static float m312cc_near(float a, float ref)
{
    while (a - ref > 180.0f)
        a += -360.0f;
    while (ref - a > 180.0f)
        a += 360.0f;
    return a;
}

/* Bring the car's body heading (0x248) within 180 of its travel heading (0x244), then return
 * the reference heading blended between them by speed (body at speed, travel when slow). */
static float m312cc_ref_heading(uint8_t *c)
{
    float h = AT_F32(c, 0x248), t = AT_F32(c, 0x244), k;

    if (h - t > 180.0f) {
        h += -360.0f;
        AT_F32(c, 0x248) = h;
    } else if (t - h > 180.0f) {
        h += 360.0f;
        AT_F32(c, 0x248) = h;
    }
    k = fminf(1.0f, (40.0f - AT_F32(c, 0x254)) / 40.0f);
    k = fmaxf(0.0f, k);
    return fn_00038a20(h * (1.0f - k) + t * k);
}

/* Advance the look-ahead point (0x80) along the racing line while it gets further away but
 * stays within `lim`, at most 5 points ahead of the checkpoint. */
static void m312cc_look_ahead(uint8_t *c, int tile, float lim)
{
    int npts = M312_NPTS, n;
    float a[3], b[3], d, d2;

    n = AT_S32(c, 0x84) + (AT_S32(c, 0x120) << ((unsigned)(tile - 7) < 2));
    if (n > npts)
        n = 1;
    else if (n < 1)
        n = npts;
    AT_S32(c, 0x80) = n;
    a[0] = M312_PTS[n * 2] * 812.0f;
    a[1] = 0.0f;
    a[2] = M312_PTS[n * 2 + 1] * 812.0f;
    b[0] = AT_F32(c, 0x2b0);
    b[1] = 0.0f;
    b[2] = AT_F32(c, 0x2b8);
    d = fn_00037fa4(a, b);
    if (isnan(d) || !(d < lim))
        return;
    for (;;) {
        n = AT_S32(c, 0x80) + AT_S32(c, 0x120);
        npts = M312_NPTS;
        if (n > npts)
            n = 1;
        else if (n < 1)
            n = npts;
        AT_S32(c, 0x80) = n;
        a[0] = M312_PTS[n * 2] * 812.0f;
        a[1] = 0.0f;
        a[2] = M312_PTS[n * 2 + 1] * 812.0f;
        d2 = fn_00037fa4(a, b);
        if (d2 < d || !(d2 < lim))
            break;
        d = d2;
        if (AT_S32(c, 0x80) - AT_S32(c, 0x84) >= 5)
            break;
    }
}

/* Heading (degrees) from the car to (tx, tz); returns 0 if the car is exactly there. */
static int m312cc_heading_to(uint8_t *c, float tx, float tz, float *ang)
{
    float t[3], p[3], v[3];

    t[0] = tx;
    t[1] = 0.0f;
    t[2] = tz;
    p[0] = -AT_F32(c, 0x2b0);
    p[1] = 0.0f;
    p[2] = -AT_F32(c, 0x2b8);
    fn_00037dec(t, p, v);
    if (!(fn_00037e54(v) > 0.0f))
        return 0;
    fn_000379a8(v, v);
    *ang = atan2f(v[0], v[2]) * 57.2958f;
    return 1;
}

/* Heading towards (tx, tz) and the reference heading it is steered against. When the car sits
 * exactly on the target the original leaves a stale register as the heading; use the
 * reference instead (no steering). */
static void m312cc_aim(uint8_t *c, float tx, float tz, float *ang, float *ref)
{
    *ref = AT_F32(c, 0x244);
    if (m312cc_heading_to(c, tx, tz, ang)) {
        *ref = m312cc_ref_heading(c);
        *ang = m312cc_near(*ang, *ref);
    } else {
        *ang = *ref;
    }
}

/* Human driver: pad analog stick + buttons -> car controls (part of fn_000312cc). */
static void m312cc_human_drive(int car, uint8_t *c, int pad, int tile)
{
    uint8_t *p = M312_PAD(pad), *ctl = M312_CTL(car);
    int st = AT_S32(c, 0x70), n;
    float sx, sy, v, sgn, steer_in, lim, tx, tz, ang, ref, diff, k, steer, spd;

    if ((unsigned)(st - 1) < 3) {
        /* in the pit box: hold the brake, accelerate out when the pit stop is done */
        ctl[0] = st == 2 || st == 3;
        ctl[1] = p[0xa8] ? (st != 2 && st != 3) : 0;
        ctl[2] = 0;
        ctl[3] = 0;
        return;
    }

    sx = AT_F32(p, 0x8c);
    sy = AT_F32(p, 0x90);
    if (sx < 0.0f) {
        v = sy;
        sgn = m312cc_sign(sy);
    } else if (sy < 0.0f) {
        sgn = -1.0f;
        v = -100.0f;
    } else {
        v = sy > 0.0f ? 100.0f : 0.0f;
        sgn = (float)(v > 0.0f);
    }
    v = v * v;
    /* steering sensitivity of the player's setup (table of 12-byte entries at 0x3ea24) */
    steer_in = sgn * fabsf(v / 125.0f) *
               G_F32(0x3ea24 + AT_S32(G(0x47544), car * 0x18) * 12);

    spd = AT_F32(c, 0x254);
    lim = spd * 55.0f;
    if (spd > 5.0f && lim < 406.0f)
        lim = 406.0f;
    m312cc_look_ahead(c, m312cc_tile(AT_S32(c, 0x44), AT_S32(c, 0x48)), lim);
    n = AT_S32(c, 0x80);
    if (AT_S32(c, 0x1c) == 1)
        AT_S32(c, 0x120) = 1;
    tx = M312_PTS[n * 2] * 812.0f;
    tz = M312_PTS[n * 2 + 1] * 812.0f;
    m312cc_aim(c, tx, tz, &ang, &ref);

    /* auto-steer towards the racing line, blended with the stick by speed */
    spd = AT_F32(c, 0x254);
    diff = ang - ref;
    if (spd < 5.0f)
        diff = (diff + diff) / (spd / 100.0f + 1.0f);
    diff *= m312cc_sign(AT_F32(c, 0x264));
    k = spd * G_F32(0x3e9bc) / 66.0f;
    if (((unsigned)(tile - 1) < 2 || tile == 7 || tile == 8) && (double)k < 0.1)
        k = 0.1f;
    if (spd < 2.0f) {
        k = 0.0f;
    } else {
        if ((double)k > 0.8)
            k = 0.8f;
        if (AT_F32(c, 0x264) < 0.0f)
            k = 0.0f;
    }
    steer = (1.0f - k) * steer_in + diff * k;
    AT_F32(ctl, 0xc) = steer;
    if (fabsf(steer) > 90.0f)
        AT_F32(ctl, 0xc) = steer < 0.0f ? -90.0f : (steer > 0.0f ? 90.0f : 0.0f);

    ctl[0] = 0;
    ctl[1] = 0;
    if (p[0xa7])
        ctl[0] = p[0xa7]; /* brake */
    else if (p[0xa8])
        ctl[1] = p[0xa8]; /* accelerate */
    ctl[2] = p[0xa4];
    ctl[3] = p[0xa5];
    ctl[6] = p[0xa0];
    ctl[7] = p[0xa1];
}

/* AI driver (part of fn_000312cc). tile: track tile under the car at the start of the update;
 * b42 / b1: see fn_000312cc. */
static void m312cc_ai_drive(int car, uint8_t *c, int tile, int b42, int b1)
{
    uint8_t *ctl = M312_CTL(car);
    int brake = 0, n, npts, j, lim_i;
    float thr, lim, spd, tx, tz, ang, ref, diff, steer, base;

    /* tyre wear: brake (for the pits) when worn more than the car's allowance */
    thr = (float)(M312_NCARS - 2 - car) / 10.0f;
    if (!((double)AT_F32(c, 0x20) < (double)thr + 0.3)) {
        float wear = AT_F32(c, 0x3f0) + AT_F32(c, 0x44c) + AT_F32(c, 0x4a8) + AT_F32(c, 0x504);
        if (!(wear < thr + 0.1f))
            brake = 1;
    }

    /* 0x1c: 1 = race on the racing line, 0 = follow / chase a target car */
    if (M312_MODE == M312_MODE_E || M312_MODE == M312_MODE_C || M312_MODE == M312_MODE_D) {
        AT_S32(c, 0x1c) = 1;
    } else if (M312_MODE == M312_MODE_A) {
        AT_S32(c, 0x1c) = G_S32(0x3f9b0) == car || G_S32(0x3f9d0) <= car;
    } else if (M312_MODE == M312_MODE_B) {
        AT_S32(c, 0x1c) = AT_S32(c, 0x10c) != 0 ? 1 : AT_S32(c, 0x108) == 0;
    }

    spd = AT_F32(c, 0x254);
    lim = spd * 55.0f;
    if (spd > 5.0f && lim < 406.0f)
        lim = 406.0f;
    m312cc_look_ahead(c, tile, lim);
    n = AT_S32(c, 0x80);
    npts = M312_NPTS;

    if (AT_S32(c, 0x1c) == 1) {
        AT_S32(c, 0x120) = 1;
        tx = M312_PTS[n * 2] * 812.0f;
        tz = M312_PTS[n * 2 + 1] * 812.0f;
        if (G_S32(0x3f8e0) == 0 || brake) {
            brake = 0;
        } else {
            /* pit stop on alternate laps (or when the tyres are worn) */
            int laps = AT_S32(c, 0x8c), cp, k, fp;
            uint8_t *slot = NULL;

            if ((laps % 2 != car % 2 && !((double)AT_F32(c, 0x20) < 0.3)) ||
                !(laps < G_S32(0x3e9d4) - 1) || laps < 0) {
                brake = 0;
                goto steer;
            }
            cp = AT_S32(c, 0x84);
            if (AT_S32(c, 0x74) > -1)
                brake = AT_F32(c, 0x254) > (float)((npts - 1 - cp) * 4 + 6);
            else
                brake = 0;
            if (cp <= npts - 5)
                goto steer;
            /* claim a free pit box (4 boxes of 0x34 bytes, car index at 0x1c) */
            for (k = 0;; k++) {
                int mine = AT_S32(c, 0x74);
                if (k > 3)
                    goto steer;
                slot = (uint8_t *)G(0x44590) + k * 0x34;
                if ((mine == -1 || k == mine) &&
                    (AT_S32(slot, 0x1c) == -1 || AT_S32(slot, 0x1c) == car))
                    break;
            }
            AT_S32(slot, 0x1c) = car;
            AT_S32(c, 0x74) = k;
            if (cp <= npts - 3)
                goto steer;
            {
                float pt[3], box[3], aux[3], me[3], dist, g;
                int nx;

                fp = k <= 1 ? npts : npts - 1;
                pt[0] = M312_PTS[fp * 2] * 812.0f;
                pt[1] = 0.0f;
                pt[2] = M312_PTS[fp * 2 + 1] * 812.0f;
                /* pit box position */
                fn_0000fb78(k, tile, (int)(uintptr_t)box, (int)(uintptr_t)aux);
                fn_00037dec(pt, box, box);
                me[0] = AT_F32(c, 0x2bc);
                me[1] = box[1];
                me[2] = AT_F32(c, 0x2c4);
                dist = fn_00037fa4(me, box);
                if (AT_F32(c, 0x254) > dist / 100.0f + 2.0f)
                    brake = 1;
                nx = cp + 1;
                if (nx > npts)
                    nx = 1;
                if (nx == fp || cp == fp) {
                    g = (float)G_S32(0x3e9d0);
                    if (tile == 2) {
                        tz = (k & 1) ? box[2] - g * 406.0f : box[2] + g * 406.0f;
                        tx = box[0] - (dist * 0.5f + 50.0f) * g;
                    } else if (tile == 1) {
                        tx = (k & 1) ? box[0] - g * 406.0f : box[0] + g * 406.0f;
                        tz = box[2] + (dist * 0.5f + 50.0f) * g;
                    }
                }
            }
        }
    } else {
        /* follow the target car 0x110 */
        int t, gap;
        uint8_t *ct;
        float a[3], b[3], dist, sgn, kk, rad, near, fx, fz;

        if (M312_MODE == M312_MODE_B) {
            int ncars = M312_NCARS, rank = 0, cnt = 0;
            for (j = 0; j < ncars; j++) {
                if (AT_S32(M312_CAR(j), 0x108) == 0)
                    continue;
                rank++;
                if (j == car)
                    break;
            }
            t = -1;
            for (j = 0; j < ncars; j++) {
                uint8_t *cj = M312_CAR(j);
                if (j != car && AT_S32(cj, 0x10c) != 0 && AT_S32(cj, 0xf4) == 0) {
                    cnt++;
                    t = j;
                    if (cnt == rank)
                        break;
                }
            }
            AT_S32(c, 0x110) = t;
            if (t < 0) {
                AT_S32(c, 0xf4) = 1;
                t = 0;
                AT_S32(c, 0x110) = 0;
            }
        } else {
            if (M312_MODE == M312_MODE_A) {
                t = G_S32(0x3f9b0) < 0 ? M312_NCARS : G_S32(0x3f9d4);
                AT_S32(c, 0x110) = t;
            } else {
                t = AT_S32(c, 0x110);
            }
            if (t <= -1) {
                t = 0;
                AT_S32(c, 0x110) = 0;
            }
        }
        ct = M312_CAR(t);
        npts = M312_NPTS;
        gap = npts * AT_S32(c, 0x90) + AT_S32(c, 0x84) - AT_S32(ct, 0x84) -
              AT_S32(ct, 0x90) * npts;
        brake = 0;
        if (gap < -1 || (gap >= 4 && M312_MODE == M312_MODE_A && t == M312_NCARS)) {
            AT_S32(c, 0x120) = 1;
            if (gap < 1)
                goto follow;
        } else if (gap >= 2) {
            AT_S32(c, 0x120) = -1;
            goto follow;
        } else if (AT_S32(ct, 0x120) <= -1) {
            AT_S32(c, 0x120) = -1;
            if (gap > -1)
                goto follow;
        } else if (AT_S32(ct, 0x120) >= 1) {
            AT_S32(c, 0x120) = 1;
            if (gap < 1)
                goto follow;
        } else if (AT_S32(c, 0x120) == -1) {
            if (gap > -1)
                goto follow;
        } else if (AT_S32(c, 0x120) != 1 || gap < 1) {
            goto follow;
        }
        /* catching the target: brake if faster than it */
        if (AT_F32(c, 0x254) > 5.0f)
            brake = AT_F32(c, 0x254) + 5.0f > AT_F32(ct, 0x254);
    follow:
        a[0] = AT_F32(ct, 0x2b0);
        a[1] = 0.0f;
        a[2] = AT_F32(ct, 0x2b8);
        b[0] = AT_F32(c, 0x2b0);
        b[1] = 0.0f;
        b[2] = AT_F32(c, 0x2b8);
        dist = fn_00037fa4(a, b);
        ct = M312_CAR(AT_S32(c, 0x110));
        tx = M312_PTS[n * 2] * 812.0f;
        tz = M312_PTS[n * 2 + 1] * 812.0f;
        /* aim at a point in front of the target car, further ahead the faster it goes */
        sgn = m312cc_sign(AT_F32(ct, 0x264));
        kk = (AT_F32(ct, 0x254) * 6.0f + 20.0f) * sgn;
        rad = AT_F32(ct, 0x248) * 0.0174533f;
        near = 812.0f / (AT_F32(ct, 0x254) / 20.0f + 1.0f);
        fz = a[2] + cosf(rad) * kk;
        fx = a[0] + sinf(rad) * kk;
        if (!(dist > near)) {
            float w = fminf(1.0f, AT_F32(c, 0x254) / 100.0f);
            tz = tz * w + fz * (1.0f - w);
            tx = tx * w + fx * (1.0f - w);
        }
        AT_F32(c, 0x10) = fx;
        AT_S32(c, 0x14) = AT_S32(c, 0x2b4);
        AT_F32(c, 0x18) = fz;
    }

steer:
    m312cc_aim(c, tx, tz, &ang, &ref);
    ctl[0] = 0;
    ctl[1] = 0;
    if (AT_S32(c, 0x30) == -1) {
        /* swerve around the first car close in front */
        for (j = 0; j < M312_NCARS; j++) {
            uint8_t *cj = M312_CAR(j);
            float d[3], dist, a, rel, kk;

            d[0] = AT_F32(cj, 0x2bc) - AT_F32(c, 0x2bc);
            d[1] = AT_F32(cj, 0x2c0) - AT_F32(c, 0x2c0);
            d[2] = AT_F32(cj, 0x2c4) - AT_F32(c, 0x2c4);
            dist = fn_00037e54(d);
            if (j == car || !(dist < 812.0f) || !(dist > 90.0f))
                continue;
            fn_000379a8(d, d);
            a = m312cc_near(atan2f(d[0], d[2]) * 57.2958f, ref);
            rel = ref - a;
            if (!(fabsf(rel) < 250.0f / (dist + -90.0f) + 2.0f))
                continue;
            if (b1 != 1 || (AT_S32(cj, 0x10c) == 0 && AT_S32(cj, 0x104) == 0))
                kk = 45.0f;
            else if (b42 == 1)
                kk = 90.0f;
            else
                continue;
            ang += cosf(rel * 0.0174533f) * kk * m312cc_sign(rel) / (dist / 500.0f + 1.0f);
            break;
        }
    }

    spd = AT_F32(c, 0x254);
    diff = ang - ref;
    if (spd < 5.0f)
        steer = (diff + diff) / (spd / 100.0f + 1.0f) * m312cc_sign(AT_F32(c, 0x264));
    else
        steer = diff * m312cc_sign(AT_F32(c, 0x264));
    AT_F32(ctl, 0xc) = steer;
    /* 0x10: stuck / turning on the spot */
    if (fabsf(diff) > 80.0f && ctl[0x10] == 0 && AT_F32(c, 0x254) < 5.0f)
        ctl[0x10] = 1;
    if (fabsf(diff) < 60.0f && ctl[0x10] != 0)
        ctl[0x10] = 0;
    spd = AT_F32(c, 0x254);
    lim_i = (int)(spd * -1.5f + 120.0f);
    if (fabsf(AT_F32(ctl, 0xc)) > (float)lim_i)
        AT_F32(ctl, 0xc) = (float)(lim_i * (int)m312cc_sign(AT_F32(ctl, 0xc)));

    base = (float)(50 - car);
    if (!brake) {
        if (fabsf(diff) < base + spd * -0.5f || spd < 3.0f)
            ctl[1] = 1; /* accelerate */
    }
    if ((spd > 4.0f && fabsf(diff) > base + spd / -3.0f) || brake)
        ctl[0] = 1; /* brake */

    if (M312_MODE == M312_MODE_B) {
        if (AT_S32(c, 0x10c) == 0 && AT_S32(c, 0x108) == 0) {
            if (spd > 10.0f)
                ctl[1] = 0;
            if (spd > 15.0f)
                ctl[0] = 1;
        }
        if (spd > 22.0f && spd > AT_F32(M312_CAR(AT_S32(c, 0x110)), 0x254) + 1.0f)
            ctl[1] = 0;
    } else if ((M312_MODE == M312_MODE_D || M312_MODE == M312_MODE_A) &&
               G_S32(0x3e9ac) <= car) {
        if (spd > 10.0f && car != G_S32(0x3e9ac) + 1)
            ctl[1] = 0;
        if (spd > 14.0f && car != G_S32(0x3e9ac) + 1)
            ctl[0] = 1;
        if (spd > 22.0f)
            ctl[1] = 0;
    } else if (spd > 22.0f) {
        ctl[1] = 0;
    }
}

#ifdef DEBUG_LOG
/* Debug: log a car that stands still for 3 s of a running race, and when it moves again, with
 * what the race logic knows about it (looking for computer cars that stop driving). */
int g_fuel_test;
static void m312cc_watch(int car, int pad, uint8_t *c, int tile)
{
    static int s_since[16], s_logged[16], s_fuel_clk;
    int clk = (int)M312_CLOCK;
    if (car < 0 || car >= 16)
        return;
    if (g_fuel_test && car == 0 && (G_U8(0x3f8f8) & 1) && clk - s_fuel_clk >= 1000) {
        s_fuel_clk = clk;
        sys_log("fuel test: t %d fuel %.4f rpm %.1f rpm2 %.1f speed %.1f gear %d lap %d pits %d",
                clk, AT_F32(c, 0x20), AT_F32(c, 0x238), AT_F32(c, 0x23c), AT_F32(c, 0x254),
                AT_S32(c, 0x28), AT_S32(c, 0x8c), G_S32(0x3f8e0));
    }
    if (clk < s_fuel_clk)
        s_fuel_clk = 0;
    if (!(G_U8(0x3f8f8) & 1) || clk < 1000) {
        s_since[car] = clk;
        s_logged[car] = 0;
        return;
    }
    if (AT_F32(c, 0x254) > 1.0f) {
        if (s_logged[car])
            sys_log("ai watch: car %d moves again after %d ms", car, clk - s_since[car]);
        s_since[car] = clk;
        s_logged[car] = 0;
        return;
    }
    if (s_logged[car] || clk - s_since[car] < 3000)
        return;
    s_logged[car] = 1;
    {
        const uint8_t *ctl = M312_CTL(car);
        float wear = AT_F32(c, 0x3f0) + AT_F32(c, 0x44c) + AT_F32(c, 0x4a8) + AT_F32(c, 0x504);
        sys_log("ai watch: car %d STOPPED 3 s: pad %d state %d mode %d (A%d B%d C%d D%d E%d) "
                "track %d wx %d ncars %d humans %d | lap %d cp %d/%d row %d col %d tile %d | "
                "ctl brake %d gas %d steer %.0f spin %d | line %d pit %d/%d chaser %d crim %d "
                "target %d done %d | 0x20 %.2f wear %.2f off %d e0 %d 34 %d pits %d",
                car, pad, G_S32(0x3f848), M312_MODE, M312_MODE_A, M312_MODE_B, M312_MODE_C,
                M312_MODE_D, M312_MODE_E, G_S32(0x3f8b0), G_S32(0x3f8b8), M312_NCARS,
                G_S32(0x3e9ac), AT_S32(c, 0x8c), AT_S32(c, 0x84), M312_NPTS, AT_S32(c, 0x44),
                AT_S32(c, 0x48), tile, ctl[0], ctl[1], AT_F32(ctl, 0xc), ctl[0x10],
                AT_S32(c, 0x1c), AT_S32(c, 0x74), AT_S32(c, 0x78), AT_S32(c, 0x108),
                AT_S32(c, 0x10c), AT_S32(c, 0x110), AT_S32(c, 0xf4), AT_F32(c, 0x20), wear,
                AT_S32(c, 0xd8), AT_S32(c, 0xe0), AT_S32(c, 0x34), G_S32(0x3f8e0));
        sys_log("ai watch: car %d phys: pos %.0f %.0f %.0f hd %.0f mv %.0f yaw %.2f | gear %d auto %d "
                "rpm %.1f rpm2 %.1f fwd %.2f side %.2f | pitst %d 0x38 %d dmg %d brk %.2f 0x40 %.2f "
                "nitro %d slip %.2f e8 %d ground %d %d %d %d 0x7c %.1f",
                car, AT_F32(c, 0x2b0), AT_F32(c, 0x2b4), AT_F32(c, 0x2b8), AT_F32(c, 0x244),
                AT_F32(c, 0x248), AT_F32(c, 0x24c), AT_S32(c, 0x28), AT_S32(c, 0x2c),
                AT_F32(c, 0x238), AT_F32(c, 0x23c), AT_F32(c, 0x264), AT_F32(c, 0x268),
                AT_S32(c, 0x70), AT_S32(c, 0x38), AT_S32(c, 0x114), AT_F32(c, 0x1e0),
                AT_F32(c, 0x40), AT_S32(c, 0x5c), AT_F32(c, 0x58), AT_S32(c, 0xe8),
                AT_S32(c, 0x3e4), AT_S32(c, 0x440), AT_S32(c, 0x49c), AT_S32(c, 0x4f8),
                AT_F32(c, 0x7c));
    }
}
#endif

/* 0x000312cc -- per-car race update: checkpoints, laps, split times, respawn, slipstream,
 * then the driving input (pad >= 0: human pad, pad < 0: AI, pad == -2: idle) (guess) */
void fn_000312cc(int car, int pad)
{
    uint8_t *c = M312_CAR(car);
    int maxrow = M312_MAXROW, maxcol = M312_MAXCOL;
    int b42, b1, row, col, cp, ord, tile, ok, npts, clk, fp, dir, px, pz, i;
    float clock;

    /* b42: car takes part in the "chase" scoring; b1: car is a chaser (mode B/A). */
    b42 = AT_S32(c, 0x10c) != 0 ||
          (M312_MODE == M312_MODE_A && AT_S32(c, 0x104) != 0);
    b1 = AT_S32(c, 0x108) != 0 ||
         (M312_MODE == M312_MODE_A && AT_S32(c, 0x104) == 0 && car < G_S32(0x3e9ac));

    col = AT_S32(c, 0x48);
    row = AT_S32(c, 0x44);
    cp = AT_S32(c, 0x84);
    if ((col | row) < 0 || row > maxrow || col > maxcol)
        ord = 0;
    else
        ord = ((uint8_t *)G(0x46400))[row * 20 + col]; /* racing-line order of the tile */
    tile = m312cc_tile(row, col);
    npts = M312_NPTS;

    /* ---- checkpoint progress ---- */
    ok = 1;
    if ((unsigned)(tile - 7) < 2) {
        /* crossing tile: keep the checkpoint */
    } else if ((unsigned)(AT_S32(c, 0x68) - 7) < 2) {
        /* coming off a crossing tile: two points per step */
        if (ord == cp + 2) {
            AT_S32(c, 0x84) = ord;
            AT_S32(c, 0xe4) = 0;
            if (AT_S32(c, 0x30) != -1)
                AT_S32(c, 0x120) = 1;
            if (M312_MODE == M312_MODE_A && G_S32(0x3f9b0) == car) {
                ((int *)G(0x45e38))[ord - 1] = 1;
                ((int *)G(0x45e38))[ord] = 1;
                AT_S32(c, 0x11c)++;
            }
        } else if (ord == cp - 2) {
            int old = AT_S32(c, 0xe4);
            AT_S32(c, 0x84) = ord;
            AT_S32(c, 0xe4) = old + 1;
            if (old >= 1)
                m312cc_wrong_way(c);
            if (AT_S32(c, 0x30) != -1)
                AT_S32(c, 0x120) = -1;
        }
    } else if (ord == cp + 1 || ord == cp + 1 - npts) {
        AT_S32(c, 0x84) = ord;
        AT_S32(c, 0xe4) = 0;
        if (AT_S32(c, 0x30) != -1)
            AT_S32(c, 0x120) = 1;
        if (M312_MODE == M312_MODE_A && G_S32(0x3f9b0) == car) {
            ((int *)G(0x45e38))[ord] = 1;
            AT_S32(c, 0x11c)++;
        }
    } else if (ord == cp - 1 || ord == cp - 1 + npts) {
        int e = AT_S32(c, 0xe4);
        AT_S32(c, 0x84) = ord;
        if (AT_S32(c, 0x108) == 0)
            AT_S32(c, 0xe4) = ++e;
        if (e >= 2)
            m312cc_wrong_way(c);
        if (AT_S32(c, 0x30) != -1)
            AT_S32(c, 0x120) = -1;
        if (AT_S32(c, 0xe4) >= 2) {
            AT_S32(c, 0xd8) = 200;
            ok = 0;
        }
    } else if (ord == cp) {
        AT_S32(c, 0x84) = ord;
    } else {
        ok = 0; /* off the racing line */
    }

    clock = M312_CLOCK;
    if (cp != AT_S32(c, 0x84))
        AT_S32(c, 0x88) = (int)clock;
    if (!(G_U8(0x3f8f8) & 1)) /* race not running */
        goto respawn_check;

    /* ---- lap timing ---- */
    if (AT_S32(c, 0xbc) == 0)
        AT_S32(c, 0xbc) = (int)clock;
    AT_S32(c, 0xac) = (int)(clock - (float)AT_S32(c, 0xbc));
    ord = AT_S32(c, 0x84);

    if (ord > G_S32(0x3f94c) && ord < G_S32(0x3f950) && AT_S32(c, 0x94) == 0) {
        m312cc_split(c, car, 0xc0, 1, 0x47df0);
        goto respawn_check;
    }
    if (ord > G_S32(0x3f950) && ord < M312_NPTS && AT_S32(c, 0x94) == 1) {
        m312cc_split(c, car, 0xc4, 2, 0x47df4);
        goto respawn_check;
    }

    npts = M312_NPTS;
    if (cp == npts && ord == 1 && AT_S32(c, 0xf4) == 0) {
        /* crossed the finish line forwards */
        int total, laps, valid;

        AT_S32(c, 0x90)++;
        if (AT_S32(c, 0x98) == -1)
            AT_S32(c, 0x98) = 0;
        valid = (AT_S32(c, 0xac) > 30000 && AT_S32(c, 0x94) == 2 && AT_S32(c, 0xc0) != 0 &&
                 AT_S32(c, 0xc4) != 0) ||
                AT_S32(c, 0x8c) < 0;
        if (!valid)
            goto lap_done;
        valid = (M312_MODE == M312_MODE_A && AT_S32(c, 0x98) == 2) ||
                M312_MODE == M312_MODE_E || M312_MODE == M312_MODE_C ||
                M312_MODE == M312_MODE_B ||
                (M312_MODE == M312_MODE_D && G_S32(0x3e9ac) > car) || AT_S32(c, 0x8c) < 0;
        if (!valid)
            goto lap_done;

        AT_S32(c, 0x94) = 0;
        AT_S32(c, 0xbc) = (int)clock;
        AT_S32(c, 0xb0) = 150;
        AT_S32(c, 0x98) = 0;
        laps = ++AT_S32(c, 0x8c);
        if (G_S32(0x3e9b0) > car && laps >= 1 && !(G_U8(0x3f928) & 1)) {
            int snd = 0, vol = 0;
            total = G_S32(0x3e9d4);
            if (laps == total - 1) {
                snd = G_S32(0x3ebc4); /* last lap */
                vol = 14000;
            } else if (M312_MODE == M312_MODE_B) {
                snd = G_S32(0x3ebc0);
                vol = 14000;
            } else if (laps < total - 1) {
                snd = G_S32(0x3ebc8);
                vol = 16000;
            }
            if (vol) {
                fn_00034810(snd, car, vol);
                fn_000360bc(snd);
            }
        }
        laps = AT_S32(c, 0x8c);
        total = G_S32(0x3e9d4);
        if (laps >= total && M312_MODE != M312_MODE_B) {
            /* finished: record the place */
            int pos;
            if (M312_MODE == M312_MODE_D || M312_MODE == M312_MODE_A) {
                pos = AT_S32(c, 0x9c);
            } else {
                pos = 1;
                for (i = 0; i < M312_NCARS; i++)
                    if (AT_S32(M312_CAR(i), 0xf4) != 0)
                        pos++;
            }
            AT_S32(c, 0xf4) = pos;
            AT_S32(c, 0xf8) = 1;
        }
        AT_S32(c, 0xb4) = AT_S32(c, 0xac);
        if (laps < 1 || AT_S32(c, 0xc0) == 0 || AT_S32(c, 0xc4) == 0)
            goto lap_done;

        m312cc_clear_msgs(c);
        if (G_S32(0x3e9b0) != 2) {
            sprintf((char *)c + 0x12c, "%s", (const char *)G_PTR(0x47d60)); /* "lap time" */
            m312cc_fmt_time((char *)c + 0x14c, AT_S32(c, 0xb4));
        } else {
            m312cc_fmt_time((char *)c + 0x12c, AT_S32(c, 0xb4));
        }
        if (M312_MODE == M312_MODE_A && AT_S32(c, 0x104) != 0) {
            if (G_S32(0x3e9b0) == 2) {
                strcpy((char *)c + 0x14c, " ");
                AT_S32(c, 0x11c) += 3;
                strcpy((char *)c + 0x14c, "+3");
                AT_U32(c, 0x1d0) = M312_HILITE;
            } else {
                strcpy((char *)c + 0x16c, " ");
                AT_S32(c, 0x11c) += 3;
                strcpy((char *)c + 0x16c, "+3");
                AT_U32(c, 0x1d4) = M312_HILITE;
            }
        } else {
            AT_S32(c, 0xc8) = (int)clock;
            if (AT_S32(c, 0x9c) <= 1) {
                AT_S32(c, 0xcc) = -1;
            } else {
                int o = ((int *)G(0x476c4))[AT_S32(c, 0x9c) - 2];
                AT_S32(c, 0xcc) = o;
                if (o >= 0) {
                    int line = G_S32(0x3e9b0) == 2 ? 0x14c : 0x16c;
                    m312cc_gap((char *)c + line, c, o, 0xc8);
                    /* colour of that line: 0x1d0 for the 0x14c line, 0x1d4 for the 0x16c one.
                     * (line + 0x84 put the second at 0x1f0, the car's first-gear ratio: the
                     * car could then never pull away from a stop.) */
                    int col = line == 0x14c ? 0x1d0 : 0x1d4;
                    AT_U8(c, col) = 0xff;
                    AT_U8(c, col + 1) = 0;
                    AT_U8(c, col + 2) = 0;
                    AT_U8(c, col + 3) = 0xff;
                }
            }
        }
        AT_S32(c, 0x128) = (int)(clock + 3000.0f);
        if (AT_S32(c, 0xf4) == 0 && M312_MODE == M312_MODE_D && AT_S32(c, 0x8c) < total) {
            /* time attack: extra time for the next lap */
            float base = (float)(int)G_F32(0x3ead4 + G_S32(0x3f8b0) * 4);
            AT_F32(c, 0x124) += (base + (float)(total - AT_S32(c, 0x90)) / G_F32(0x3e9b8)) *
                                900.0f;
        }
        if (AT_S32(c, 0xb4) < AT_S32(c, 0xb8)) {
            AT_S32(c, 0xb8) = AT_S32(c, 0xb4); /* personal best */
            if (AT_S32(c, 0xb4) < G_S32(0x3f8d8)) {
                G_S32(0x3f8d8) = AT_S32(c, 0xb4); /* race best */
                G_S32(0x3f8dc) = car;
            }
        }
    lap_done:
        AT_S32(c, 0xd8) = 0;
    } else if (cp == 1 && ord == npts) {
        /* crossed the line backwards */
        AT_S32(c, 0x90)--;
        AT_S32(c, 0xd8) = 0;
    }

respawn_check:
    clk = (int)M312_CLOCK;
    if (ok || !(G_U8(0x3f8f8) & 1)) {
        AT_S32(c, 0xd8) = 0;
        AT_S32(c, 0xf0) = clk;
    } else {
        AT_S32(c, 0xd8) += clk - AT_S32(c, 0xf0);
        AT_S32(c, 0xf0) = clk;
        if (AT_S32(c, 0xd8) > 0x7d)
            goto respawn;
    }
    if (!(AT_S32(c, 0xe0) > 2 || ((unsigned)tile <= 10 && ((1u << tile) & 0x601))))
        goto slipstream;

respawn:
    /* off the racing line too long / off the road: put the car back on the line */
    if (AT_S32(c, 0x34) > 3 || G_U8(0x3fab8) != 0)
        goto slipstream;
    if (M312_MODE == M312_MODE_B) {
        if (AT_S32(c, 0x10c) != 0) {
            AT_S32(c, 0x114) += 10;
            if (AT_S32(c, 0x114) >= G_S32(0x3e9d8)) {
                AT_S32(c, 0x114) = G_S32(0x3e9d8);
                for (i = 0; i < G_S32(0x3e9ac); i++) {
                    uint8_t *ci = M312_CAR(i);
                    if (AT_S32(ci, 0x108) != 0 && AT_S32(ci, 0x118) == car)
                        AT_S32(ci, 0x11c) += 10;
                }
            }
        }
    } else if (M312_MODE == M312_MODE_A && G_S32(0x3f9b0) == car) {
        G_S32(0x3f9b4) = car;
        for (i = 0; i < M312_NCARS; i++)
            AT_S32(M312_CAR(i), 0x104) = 0;
        fn_00010d70(-1, 0, AT_S32(c, 0x84));
    }
    AT_S32(c, 0xe0) = 0;
    AT_S32(c, 0xe4) = 0;
    AT_S32(c, 0xd8) = 0;
    AT_S32(c, 0x70) = 0;
    if (AT_S32(c, 0x74) >= 0)
        AT_S32(G(0x44590), AT_S32(c, 0x74) * 0x34 + 0x1c) = -1; /* free the pit slot */
    AT_S32(c, 0x74) = -1;
    AT_S32(c, 0x78) = -1;
    npts = M312_NPTS;
    dir = AT_S32(c, 0x120);
    fp = cp - dir;
    if (dir == 1) {
        if (fp <= 0) {
            AT_S32(c, 0x84) = 1;
            fp = 2;
        }
    } else {
        if (fp > npts)
            AT_S32(c, 0x84) = npts;
        AT_S32(c, 0x120) = 1;
        fp = cp;
    }
    px = (int)M312_PTS[fp * 2];
    pz = (int)M312_PTS[fp * 2 + 1];
    {
        int t = m312cc_tile(px, pz);
        if (t == 7 || t == 8) {
            fp -= 1;
            if (fp < 1)
                fp = npts;
            else if (fp > npts)
                fp = 1;
            px = (int)M312_PTS[fp * 2];
            pz = (int)M312_PTS[fp * 2 + 1];
        }
    }
    {
        int out[4];
        float x = (float)px * 812.0f + (float)(car % 4) * 812.0f * 0.5f / 3.0f + -203.0f;
        float z = (float)pz * 812.0f + (float)(car / 4) * 812.0f * 0.5f / 3.0f + -203.0f;
        float y;

        AT_F32(c, 0x2bc) = x;
        AT_F32(c, 0x2c4) = z;
        AT_F32(c, 0x2b0) = x;
        AT_F32(c, 0x2b8) = z;
        AT_F32(c, 0x2b4) = 2000.0f;
        y = fn_0000d800(-1, x, 2000.0f, z, out) + 16.0f; /* ground height */
        AT_F32(c, 0x2b4) = y;
        AT_F32(c, 0x2c0) = y;
        AT_F32(c, 0x274) = y;
    }
    AT_S32(c, 0x44) = px;
    AT_S32(c, 0x48) = pz;
    AT_S32(c, 0x2f8) = 0;
    AT_S32(c, 0x2fc) = 0;
    AT_S32(c, 0x300) = 0;
    AT_S32(c, 0x254) = 0;
    AT_S32(c, 0xe8) = 200;
    AT_S32(c, 0x24c) = 0;
    fn_00030fcc(car);
    fn_0000f9a8(car, fp);

slipstream:
    /* ---- slipstream (0x58: 0..1) ---- */
    {
        float *slip = &AT_F32(c, 0x58);
        if (*slip > 0.0f) {
            *slip += -0.01f;
            if (*slip < 0.0f || AT_F32(c, 0x254) < 5.0f)
                *slip = 0.0f;
        }
        for (i = 0; i < M312_NCARS; i++) {
            uint8_t *ci = M312_CAR(i);
            float d[3], dist, a, m, ang;

            d[0] = AT_F32(ci, 0x2bc) - AT_F32(c, 0x2bc);
            d[1] = AT_F32(ci, 0x2c0) - AT_F32(c, 0x2c0);
            d[2] = AT_F32(ci, 0x2c4) - AT_F32(c, 0x2c4);
            dist = fn_00037e54(d);
            if (i == car || !(dist < 1218.0f) || !(dist > 0.0f))
                continue;
            /* same travel direction? */
            a = AT_F32(ci, 0x244);
            m = AT_F32(c, 0x244);
            a = m312cc_near(a, m);
            if (!(fabsf(m - a) < 25.0f))
                continue;
            fn_000379a8(d, d);
            ang = atan2f(d[0], d[2]);
            while (AT_F32(ci, 0x248) - AT_F32(c, 0x248) > 180.0f)
                AT_F32(ci, 0x248) += -360.0f;
            while (AT_F32(c, 0x248) - AT_F32(ci, 0x248) > 180.0f)
                AT_F32(ci, 0x248) += 360.0f;
            ang = m312cc_near(ang * 57.2958f, AT_F32(c, 0x248));
            /* the other car is ahead of us, both facing the same way, both moving */
            if (fabsf(AT_F32(c, 0x248) - ang) < 25.0f &&
                fabsf(AT_F32(ci, 0x248) - AT_F32(c, 0x248)) < 25.0f &&
                AT_F32(c, 0x254) > 10.0f && AT_F32(ci, 0x254) > 10.0f) {
                *slip += ((1218.0f - dist) / 1218.0f - *slip) / 10.0f;
                if (*slip > 1.0f)
                    *slip = 1.0f;
            }
        }
    }

#ifdef DEBUG_LOG
    m312cc_watch(car, pad, c, tile);
#endif
    if (pad == -2) {
        /* no driver: wheels straight, nothing pressed */
        uint8_t *ctl = M312_CTL(car);
        AT_F32(ctl, 0xc) = 90.0f;
        ctl[6] = 0;
        ctl[7] = 0;
        AT_U32(ctl, 0) = 0;
        return;
    }
    if (pad < 0)
        m312cc_ai_drive(car, c, tile, b42, b1);
    else
        m312cc_human_drive(car, c, pad, tile);
}

/* 0x00034810 -- play a sound panned to the player's half of a split screen (guess) */
void fn_00034810(int snd, int player, int vol)
{
    if (G_S32(0x3e9b0) < 2) {
        int v = (unsigned)vol >> 1;
        fn_0003613c(snd, v, v);
    } else {
        fn_0003613c(snd, (uint16_t)(((player + 1) % 2) * vol), (uint16_t)((player % 2) * vol));
    }
}

/* 0x0003485c -- menu animation: background rotation and a 0..255 pulse (guess) */
void fn_0003485c(void)
{
    float a = G_F32(0x5c168) + 0.1f;
    int v;

    if (a > 360.0f)
        a += -360.0f;
    G_F32(0x5c168) = a;
    v = G_S32(0x3fa30) + G_S32(0x3ebf8);
    G_S32(0x3fa30) = v;
    if (v >= 256) {
        G_S32(0x3fa30) = 255;
        G_S32(0x3ebf8) = -10;
    } else if (v <= -1) {
        G_S32(0x3fa30) = 0;
        G_S32(0x3ebf8) = 10;
    }
}

/* 0x000348d8 -- pit crew animation: advance the 4 pit boxes' wheel-change timers and finish
 * the stop when all four are done (guess) */
void fn_000348d8(void)
{
    static const float inc[4] = {2.0f, 2.5f, 2.7f, 1.8f};
    static const int field[4] = {0x3f0, 0x44c, 0x4a8, 0x504}; /* tyre wear per wheel */
    uint8_t *base = (uint8_t *)G(0x44590);
    int i, k;

    for (i = 0; i < 4; i++) {
        float *f = (float *)(base + i * 0x34);
        int car = AT_S32(base + i * 0x34, 0x18);

        if (car < 0)
            continue;
        if (f[0] < 360.0f)
            f[0] += 1.5f;
        for (k = 1; k <= 4; k++) {
            if (f[k] < 360.0f) {
                f[k] += inc[k - 1];
                if (f[k] > 320.0f && f[k] < 340.0f)
                    AT_F32(M312_CAR(car), field[k - 1]) = 1.0f; /* new tyre */
            }
        }
        if (f[0] >= 360.0f && f[1] >= 360.0f && f[2] >= 360.0f && f[3] >= 360.0f &&
            f[4] >= 360.0f && AT_S32(M312_CAR(car), 0x70) == 3) {
            AT_S32(M312_CAR(car), 0x70) = 4; /* leave the pit box */
            AT_S32(M312_CAR(car), 0x7c) = 0;
        }
    }
}

/* 0x00034a98 -- menu idle timer: after 300 ticks fall back to the attract screen (guess) */
void fn_00034a98(void)
{
    if (G_S32(0x3e9ec) < 0)
        return;
    if (G_S32(0x3f8d0) <= 300)
        G_S32(0x3f8d0)++;
    else
        G_S32(0x3e9e8) = G_S32(0x3e9ec);
    if (G_U8(0x3fab8) && G_S32(0x3fa24) == 5 && G_S32(0x3f8c4) != 0)
        G_S32(0x3f8d0) = 300;
}

static void m312cc_model(int slot, const char *smd, const char *png, float scale, int bounds)
{
    fn_0003836c(slot, smd, scale, bounds, 0, G_U8(0x3e99c) & 1);
    fn_0000ff60(slot, png, 0x10);
}

/* 0x00034b04 -- load the race/menu assets: fonts, HUD, pit crew, world, flags, props (guess) */
void fn_00034b04(void)
{
    char smd[256], png[256];
    int i;

    G_S32(0x3fa08) = 0;
    G_S32(0x3f9fc) = 0;
    G_S32(0x3fa00) = 0;
    G_S32(0x3fa0c) = 0;
    fn_00011450(1);
    fn_000116f4(4);
    fn_000116f4(1);
    fn_000350c4(1);
    fn_0000ff60(0x96, "/pics/font.png", 0);
    fn_0000ff60(0x9d, "/pics/track.png", 0);
    fn_0000ff60(0xac, "/pics/turbo.png", 0);
    G_S32(0x3ebb4) = fn_00036014(4, "sound/spray.wav"); /* original passes a 3rd arg 0xf */
    m312cc_model(0x7c, "/models/faces/helmet.smd", "/models/faces/helmet.png", 1.1545454f, 0);
    m312cc_model(0x91, "models/map_tiles/road_00.smd", "/models/map_tiles/road_01.png",
                 1.1545454f, 0);
    m312cc_model(0x92, "models/map_tiles/road_00.smd", "/models/map_tiles/road_00.png",
                 1.1545454f, 0);
    if (fn_00038c04(0x73) == 0)
        fn_0003836c(0x73, "/models/others/wheel_01.smd", 2.64f, 0, 0, G_U8(0x3e99c) & 1);
    m312cc_model(0x87, "/models/objs/obj_21.smd", "/models/objs/obj_21.png", 0.508f, 0);
    m312cc_model(0x88, "/models/others/pit_man_00.smd", "/models/others/pit_man_00.png",
                 0.635f, 1);
    m312cc_model(0x89, "/models/others/pit_man_01.smd", "/models/others/pit_man_01.png",
                 0.635f, 1);
    m312cc_model(0x78, "/models/logos/3d_logo.smd", "/models/logos/3d_logo.png", 0.84666663f, 0);
    G_U8(0x3f918) = 1;
    m312cc_model(0x86, "/models/others/world.smd", "/models/others/world.png", 1.6933333f, 0);
    m312cc_model(0x8f, "/models/others/podium.smd", "/models/others/podium.png", 1.27f, 1);
    for (i = 0; i < 10; i++) {
        int slot = ((int *)G(0x4821c))[i];
        snprintf(smd, 0xff, "/models/flags/flag_%02d.smd", i + 1);
        fn_0003836c(slot, smd, 0.508f, 1, 0, G_U8(0x3e99c) & 1);
        snprintf(png, 0xff, "/models/flags/flag_%02d.png", i + 1);
        fn_0000ff60(slot, png, 0);
    }
    fn_0000ff60(0x9f, "/pics/back_menu.png", 0);
    fn_0000ff60(0x9b, "/pics/select_color.png", 0);
    fn_0000ff60(0x9c, "/pics/curr_color.png", 0);
    m312cc_model(0x79, "/models/others/cup.smd", "/models/others/cup.png", 1.27f, 0);
    m312cc_model(0x7a, "/models/others/key.smd", "/models/others/key.png", 1.27f, 0);
    m312cc_model(0x7b, "/models/others/flag.smd", "/models/others/flag.png", 1.27f, 1);
    m312cc_model(0x8e, "/models/others/note.smd", "/models/others/note.png", 1.27f, 0);
    m312cc_model(0x8c, "/models/others/gear_box_a.smd", "/models/others/gear_box_a.png", 1.27f,
                 0);
    m312cc_model(0x8d, "/models/others/gear_box_m.smd", "/models/others/gear_box_m.png", 1.27f,
                 0);
    m312cc_model(0x74, "/models/others/clock.smd", "/models/others/clock.png", 1.27f, 0);
    m312cc_model(0x76, "/models/others/signal_3.smd", "/models/others/signal_3.png", 1.27f, 0);
    m312cc_model(0x77, "/models/others/siren.smd", "/models/others/siren.png", 2.54f, 0);
    m312cc_model(0x75, "/models/others/plate.smd", "/models/others/plate.png", 1.27f, 0);
    m312cc_model(0x8b, "/models/faces/eyes.smd", "/models/faces/eyes.png", 1.016f, 0);
    optm_load(); /* the port's Options props: Controls and 3D pages (opt_menu.c) */
    fn_00038184(0x82);
    fn_00004dac(0x82);
}

/* 0x000350c4 -- load team logo n on demand (model slot table at 0x47258) (guess) */
void fn_000350c4(int n)
{
    int *tbl = (int *)G(0x47258);
    char smd[256], png[256];
    int s;

    if (tbl[n] != -1)
        return;
    s = G_S32(0x3eb2c) * (n - 1) + G_S32(0x3eb38);
    tbl[n] = s;
    snprintf(smd, sizeof(smd), "/models/logos/logo_%02d.smd", n);
    fn_0003836c(s, smd, 0.528f, 0, 0, G_U8(0x3e99c) & 1);
    snprintf(png, sizeof(png), "/models/logos/logo_%02d.png", n);
    fn_0000ff60(s, png, 0x10);
}

/* 0x0003517c -- award points to a car for its finishing place, per game mode (guess) */
void fn_0003517c(int car, int place)
{
    uint8_t *c = M312_CAR(car);

    if (M312_MODE == M312_MODE_D) {
        float t;
        if (G_S32(0x3e9ac) <= car)
            return;
        if (G_S32(0x3f8dc) == car)
            AT_S32(c, 0xa0) += 3; /* fastest lap */
        t = AT_F32(c, 0x124) / 1000.0f;
        if (AT_S32(c, 0x30) == -1) {
            int r;
            if (AT_S32(c, 0xf4) != 0) {
                r = (int)((float)AT_S32(c, 0xa0) + t);
            } else {
                int d = (M312_NPTS - AT_S32(c, 0x84)) / 2;
                int r3 = (int)(t - (float)d);
                r = r3 < 0 ? AT_S32(c, 0xa0) : AT_S32(c, 0xa0) + r3;
            }
            AT_S32(c, 0xa0) = r + G_S32(0x3e9d4);
        } else {
            AT_S32(c, 0xa0) = (int)((float)AT_S32(c, 0xa0) + t) + AT_S32(c, 0x8c);
        }
    } else if (M312_MODE == M312_MODE_A) {
        AT_S32(c, 0xa0) += AT_S32(c, 0x11c);
    } else if (M312_MODE == M312_MODE_B) {
        if (AT_S32(c, 0x108) != 0) {
            int q = AT_S32(c, 0x11c) / 10;
            AT_S32(c, 0xa4) += q;
            AT_S32(c, 0xa0) += q;
        }
        if (AT_S32(c, 0x10c) == 0)
            return;
        if (AT_S32(c, 0x114) < G_S32(0x3e9d8)) {
            int q = (G_S32(0x3e9d8) - AT_S32(c, 0x114)) / 10;
            AT_S32(c, 0xa4) += q;
            AT_S32(c, 0xa0) += q;
        }
        if (AT_S32(c, 0xf8) == 0)
            return;
        AT_S32(c, 0xa4)++;
        AT_S32(c, 0xa0)++;
    } else {
        static const int pts[7] = {0, 10, 6, 4, 3, 2, 1};
        if ((unsigned)(place - 1) < 6)
            AT_S32(c, 0xa0) += pts[place];
    }
}

/* 0x00035450 -- engine and skid sound volume/pitch of a car on a player's channel, panned by
 * gains a (right) and b (left) (guess) */
void fn_00035450(int car, int player, float a, float b)
{
    uint8_t *c = M312_CAR(car);
    float pitch, base, k, add, v, e, w;

    pitch = AT_F32(c, 0x240) / 30.0f + 1.0f;
    base = (float)G_S32(0x3e9b0) * 0.5f + 0.1f;
    if (AT_S32(c, 0x60) != 0)
        pitch += (float)AT_S32(c, 0x5c) / 700.0f;
    if (AT_S32(c, 0x6c) != 0) {
        k = 15.0f;
        add = 20000.0f;
    } else {
        k = 10.0f;
        add = 12000.0f;
    }
    v = (AT_F32(c, 0x240) * k + add) / base;
    if (v > 32000.0f / base)
        v = 32000.0f / base;
    fn_0003613c(((int *)G(0x46e40))[player], (int)(v * b), (int)(v * a));
    fn_00036174(((int *)G(0x46e40))[player], pitch);

    e = fmaxf(0.0f, fabsf(AT_F32(c, 0x260)) * 50.0f + -5.0f) + fabsf(AT_F32(c, 0x268));
    w = e * 4000.0f / (float)(AT_S32(c, 0x3c) + AT_S32(c, 0x34) + 1) / base;
    if (w > 8000.0f / base)
        w = 8000.0f / base;
    fn_0003613c(((int *)G(0x46e88))[player], (int)(w * b), (int)(w * a));
}

/* 0x00035600 -- multiplayer setup menu input: player car (0x14/0x15), colour (0x17) and
 * name entry (0x18); state 0x3fa24, setup table 0x47544 (0x18 bytes per player) (guess) */
void fn_00035600(void)
{
    uint8_t *setup = (uint8_t *)G(0x47544);
    uint8_t *e;
    int i;

    switch (G_S32(0x3fa24)) {
    case 0x14:
        if (G_S32(0x3fa00) != G_S32(0x3f9fc))
            return;
        if (G_F32(0x3fa1c) != 1.0f)
            return;
        if ((G_U8(0x3fa6c) & 1) && G_S32(0x3fa88) < 2) {
            if (++G_S32(0x3f9fc) > 10)
                G_S32(0x3f9fc) = 0;
            fn_0002c13c(0, G_S32(0x3f9fc));
            fn_0003613c(G_S32(0x3ebcc), 0, 8000);
            fn_000360bc(G_S32(0x3ebcc));
        }
        if ((G_U8(0x3fa68) & 1) && G_S32(0x3fa84) < 2) {
            if (--G_S32(0x3f9fc) < 0)
                G_S32(0x3f9fc) = 10;
            fn_0002c13c(0, G_S32(0x3f9fc));
            fn_0003613c(G_S32(0x3ebcc), 8000, 0);
            fn_000360bc(G_S32(0x3ebcc));
        }
        e = setup + G_S32(0x3fa08) * 0x18;
        if (M312_MODE == M312_MODE_B) {
            if (G_S32(0x3fa00) == 10) {
                AT_S32(e, 0xc) = 0;
            } else {
                if (G_U8(0x3fa60) & 1)
                    AT_S32(e, 0xc) = 1;
                if (G_U8(0x3fa64) & 1)
                    AT_S32(e, 0xc) = 0;
            }
        }
        if ((G_U8(0x3fa74) & 1) &&
            (G_S32(0x3fa24) != 0x14 || M312_MODE != M312_MODE_B || G_S32(0x3f9fc) != 10) &&
            ((int *)G(0x4819c))[G_S32(0x3f9fc)] != 0) {
            AT_S32(setup + G_S32(0x3fa08) * 0x18, 0) = G_S32(0x3f9fc) + 1;
            G_S32(0x3fa28) = 0x15;
            G_S32(0x3fa2c) = 0;
            fn_0003613c(G_S32(0x3ebb8), 5000, 5000);
            fn_000360bc(G_S32(0x3ebb8));
        }
        if (!(G_U8(0x3fa70) & 1))
            return;
        if (G_S32(0x3fa08) > 0) {
            G_S32(0x3fa0c) = G_S32(0x3fa08) - 1;
            G_S32(0x3fa28) = 0x18;
            G_S32(0x3fa2c) = 0;
            return;
        }
        G_S32(0x3fa28) = 10;
        G_S32(0x3fa2c) = 0;
        break;
    case 0x15:
        G_U8(0x3fa50) = G_U8(0x3fa60) & 1;
        if (G_U8(0x3fa60) & 1)
            G_S32(0x3fa04)++;
        G_U8(0x3fa54) = G_U8(0x3fa64) & 1;
        if (G_U8(0x3fa64) & 1)
            G_S32(0x3fa04)--;
        if ((G_U8(0x3fa74) & 1) && G_S32(0x3fa2c) < 1) {
            G_S32(0x3faa8) = AT_S32(setup + G_S32(0x3fa08) * 0x18, 0x10);
            G_S32(0x3fa28) = 0x17;
            fn_0003613c(G_S32(0x3ebb4), 8000, 8000);
            fn_000360bc(G_S32(0x3ebb4));
            G_S32(0x3fa2c) = 0x2d;
        }
        if (!(G_U8(0x3fa70) & 1))
            return;
        G_S32(0x3fa28) = 0x14;
        G_S32(0x3fa2c) = 0;
        break;
    case 0x17:
        if ((G_U8(0x3fa6c) & 1) && G_S32(0x3fa88) < 2) {
            if (++G_S32(0x3f9f4) > 9)
                G_S32(0x3f9f4) = 0;
            fn_0002c13c(1, G_S32(0x3f9f4));
            fn_0003613c(G_S32(0x3ebcc), 0, 8000);
            fn_000360bc(G_S32(0x3ebcc));
        }
        G_U8(0x3fa5c) = G_U8(0x3fa6c) & 1;
        if ((G_U8(0x3fa68) & 1) && G_S32(0x3fa84) < 2) {
            if (--G_S32(0x3f9f4) < 0)
                G_S32(0x3f9f4) = 9;
            fn_0002c13c(1, G_S32(0x3f9f4));
            fn_0003613c(G_S32(0x3ebcc), 8000, 0);
            fn_000360bc(G_S32(0x3ebcc));
        }
        G_U8(0x3fa58) = G_U8(0x3fa68) & 1;
        if ((G_U8(0x3fa74) & 1) && ((int *)G(0x481c8))[G_S32(0x3f9f4)] != 0) {
            G_S32(0x3fa28) = 0x18;
            e = setup + G_S32(0x3fa08) * 0x18;
            AT_S32(e, 8) = G_S32(0x3f9f4) + 1;
            G_S32(0x3f9ec) = 0;
            G_S32(0x3f9f0) = AT_S8(e, 0x14) - 0x20;
        }
        if (!(G_U8(0x3fa70) & 1))
            return;
        G_S32(0x3faa8) = AT_S32(setup + G_S32(0x3fa08) * 0x18, 0x10);
        G_S32(0x3fa28) = 0x15;
        break;
    case 0x18:
        /* name entry: 3 letters, cursor 0x3f9ec, letter 0x3f9f0 (char - 0x20) */
        e = setup + G_S32(0x3fa08) * 0x18;
        if (G_U8(0x3fa6c) & 1) {
            if (++G_S32(0x3f9ec) > 2)
                G_S32(0x3f9ec) = 2;
            G_S32(0x3f9f0) = AT_S8(e, 0x14 + G_S32(0x3f9ec)) - 0x20;
            fn_0003613c(G_S32(0x3ebcc), 0, 8000);
            fn_000360bc(G_S32(0x3ebcc));
        }
        G_U8(0x3fa5c) = G_U8(0x3fa6c) & 1;
        if (G_U8(0x3fa68) & 1) {
            if (--G_S32(0x3f9ec) < 0)
                G_S32(0x3f9ec) = 0;
            G_S32(0x3f9f0) = AT_S8(e, 0x14 + G_S32(0x3f9ec)) - 0x20;
            fn_0003613c(G_S32(0x3ebcc), 8000, 0);
            fn_000360bc(G_S32(0x3ebcc));
        }
        if (G_U8(0x3fa64) & 1) {
            if (++G_S32(0x3f9f0) == 5)
                G_S32(0x3f9f0) = 6;
            if (G_S32(0x3f9f0) > 0x3a)
                G_S32(0x3f9f0) = 0;
            fn_0003613c(G_S32(0x3ebcc), 0, 8000);
            fn_000360bc(G_S32(0x3ebcc));
        }
        G_U8(0x3fa54) = G_U8(0x3fa64) & 1;
        if (G_U8(0x3fa60) & 1) {
            if (--G_S32(0x3f9f0) == 5)
                G_S32(0x3f9f0) = 4;
            if (G_S32(0x3f9f0) < 0)
                G_S32(0x3f9f0) = 0x3a;
            fn_0003613c(G_S32(0x3ebcc), 8000, 0);
            fn_000360bc(G_S32(0x3ebcc));
        }
        G_U8(0x3fa50) = G_U8(0x3fa60) & 1;
        if (G_U8(0x3fa74) & 1) {
            if (G_S32(0x3fa08) < G_S32(0x3e9b0) - 1) {
                G_S32(0x3fa0c) = G_S32(0x3fa08) + 1; /* next player */
                G_S32(0x3fa28) = 0x14;
            } else {
                fn_000117b0();
                for (i = 0; i < M312_NCARS; i++)
                    sprintf((char *)M312_CAR(i) + 0xc, "%s", (char *)setup + 0x14 + i * 0x18);
                G_S32(0x3fa28) = 0x1e; /* start the race */
                G_U8(0x3f900) = 0;
            }
        }
        if (!(G_U8(0x3fa70) & 1))
            return;
        G_S32(0x3fa28) = 0x17;
        break;
    default: /* 0x16: nothing */
        break;
    }
}

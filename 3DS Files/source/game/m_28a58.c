/* Module 0x00028a58..0x0002d550: in-race HUD of one viewport (positions, lap / time, speedo,
 * gauges, result tables), the floating name tags over the other cars, the bar helper, the
 * HUD-button reader, the race (re)load dispatcher and the camera update.
 * Functional translation of the iOS armv7 build (see docs/TRANSLATION.md).
 *
 * Shared state used here (iOS addresses):
 *   0x48ae0 car array, G_S32(0x3e9c8) cars of 0x678 bytes; 0x476c4 race order table
 *   0x3f9dc current game mode, compared with the mode ids in 0x3ebe0/4/8/c and 0x3f9d8
 *   0x5c160 camera/viewport block: per viewport (stride 4) w at 0x5c1a8, h at 0x5c1b8;
 *           per camera (stride 0x80, base 0x5c160 + 0x3a0) the camera record (see CAM_*)
 *   0x47e90 shared 256-byte text buffer (slot 0x3f358)
 *   0x40388 HUD touch-button table, 0x20 bytes per button
 *
 * Draw helpers (engine module / m_20f70):
 *   fn_0000526c(tex, x, y, w, h, z, u, v, du, dv, rot, cx, cy, flags)  sprite quad
 *   fn_00004fb4(r, g, b, a)                                             glColor4f(x / 255)
 *   fn_00023808(s, x, y, sx, sy, r, g, b, a, flags)                    bitmap-font string */
#include "ios.h"
#include "controls.h"
#include "net_game.h"

#define CAR_SIZE 0x678
#define CAR(i) ((uint8_t *)G(0x48ae0) + (i) * CAR_SIZE)
#define NCARS G_S32(0x3e9c8)
#define ORDER ((int *)G(0x476c4)) /* slot 0x3f16c */

#define MODE_CUR G_S32(0x3f9dc)
#define MODE_A G_S32(0x3ebe0) /* 1 */
#define MODE_B G_S32(0x3ebe4) /* 2 */
#define MODE_C G_S32(0x3ebe8) /* 3 */
#define MODE_D G_S32(0x3ebec) /* 4 */
#define MODE_E G_S32(0x3f9d8)

#define TXT ((char *)G(0x47e90)) /* slot 0x3f358 */
/* A language string: the image word at `a` holds the char * (slot -> table entry). */
#define STR(a) ((const char *)G_PTR(a))

/* Viewport size (floats) and camera records. */
#define VP_W(vp) G_F32(0x5c1a8 + (vp) * 4)
#define VP_H(vp) G_F32(0x5c1b8 + (vp) * 4)
#define CAM(c) ((uint8_t *)G(0x5c160) + (c) * 0x80)
/* Camera record offsets (relative to CAM(c)): */
#define CAM_UP 0x3a0     /* float[3] up vector */
#define CAM_EYE 0x3c4    /* float[3] eye position */
#define CAM_PREV 0x3d0   /* float[3] eye position of the previous frame */
#define CAM_FIX 0x3dc    /* float[3] trackside camera position (mode 4) */
#define CAM_ANG 0x3f4    /* float heading (degrees) */
#define CAM_FADE 0x3f8   /* float 0..255 fade level */
#define CAM_MODE 0x408   /* int camera mode 0..12 */
#define CAM_CAR 0x410    /* int followed car */
#define CAM_HOLD 0x414   /* int */
#define CAM_FOV 0x418    /* float field of view */
#define CAM_FADEIN 0x41c /* u8 fade direction */

#define D2R 0.0174533f /* 0x3c8efa39 */

/* Callees with pointer arguments (prototypes from m_1c7c0.tsv / m_0d800.tsv):
 * fn_0001da58(vp, pos, out) projects a world point to screen x, y; fn_0000d800 returns the
 * ground height (float) and writes a flag through its int * argument. */
#define M28_PROJECT(vp, in, out) fn_0001da58((vp), (in), (out))
#define M28_GROUND(car, x, y, z, flag) fn_0000d800((car), (x), (y), (z), (flag))

#define RANK ((int *)G(0x47704)) /* championship ranking table, slot 0x3f170 */

/* Line k (0..4) of a car's result table: 32-byte strings at car+0x12c, RGBA colours (bytes)
 * at car+0x1cc; car+0x128 holds the race-timer value (int ms) until which it stays visible. */
#define RES_LINE(c, k) ((char *)(c) + 0x12c + (k) * 0x20)
#define RES_COL(c, k) ((uint8_t *)(c) + 0x1cc + (k) * 4)

/* snprintf(dst, n, fmt, s) kept out of line: source and destination both live in the image,
 * which makes gcc's -Wrestrict see a possible overlap. */
static __attribute__((noinline)) void m28_fmt_str(char *dst, int n, const char *fmt, const char *s)
{
    snprintf(dst, n, fmt, s);
}

/* "%02d:%02d:%02d" (minutes, seconds, hundredths) of a time in ms, into TXT. */
static void m28_fmt_time(int t)
{
    snprintf(TXT, 0x100, "%02d:%02d:%02d", t / 60000 % 60, t / 1000 % 60, t / 10 % 100);
}

static void m28_res_cols(uint8_t *c, uint32_t col)
{
    int k;

    for (k = 0; k < 5; k++)
        AT_U32(c, 0x1cc + k * 4) = col;
}

/* 0x0002b3ce path of fn_00028a58: credits screen. Two pairs of lines (role / name) cross-fade;
 * the pair texts are taken from the 64-byte tables at 0x3ec08 and 0x3ee48. */
static void m28_credits(void)
{
    int a;

    if (G_S32(0x3fa30) > 0x80) {
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_00023ad0(3, 8, STR(0x47e1c));
    }
    a = G_S32(0x3ec04); /* fade-in level of the current pair */
    if (a >= 0xff) {
        G_S32(0x3ec04) = 0xff;
        a = 0xff;
        if (++G_S32(0x3fad8) >= 0xff) { /* hold time over: next pair */
            int idx;

            m28_fmt_str((char *)G(0x3fcda), 0xff, "%s", (const char *)G(0x3fadc));
            m28_fmt_str((char *)G(0x3fdd9), 0xff, "%s", (const char *)G(0x3fbdb));
            idx = G_S32(0x3fad4);
            m28_fmt_str((char *)G(0x3fadc), 0xff, "%s", (const char *)G(0x3ec08) + idx * 0x40);
            m28_fmt_str((char *)G(0x3fbdb), 0xff, "%s", (const char *)G(0x3ee48) + idx * 0x40);
            if (G_S32(0x3fad4) <= 6)
                G_S32(0x3fad4)++;
            G_S32(0x3ec04) = 0;
            G_S32(0x3fad8) = 0;
            a = 0;
        }
    } else {
        a += 10;
        if (a >= 0xff)
            a = 0xff;
        G_S32(0x3ec04) = a;
    }
    /* new pair fades in, old pair fades out */
    m28_fmt_str(TXT, 0x100, "%s", (const char *)G(0x3fadc));
    fn_00023808(TXT, 320.0f, 48.0f, 0.8f, 0.8f, 0xff, 0xff, 0, G_U8(0x3ec04), 4);
    m28_fmt_str(TXT, 0x100, "%s", (const char *)G(0x3fbdb));
    fn_00023808(TXT, 320.0f, 73.0f, 0.8f, 0.8f, 0xff, 0xff, 0xff, G_U8(0x3ec04), 4);
    m28_fmt_str(TXT, 0x100, "%s", (const char *)G(0x3fcda));
    fn_00023808(TXT, 320.0f, 48.0f, 0.8f, 0.8f, 0xff, 0xff, 0, (0xff - a) & 0xff, 4);
    m28_fmt_str(TXT, 0x100, "%s", (const char *)G(0x3fdd9));
    fn_00023808(TXT, 320.0f, 73.0f, 0.8f, 0.8f, 0xff, 0xff, 0xff, (0xff - a) & 0xff, 4);
}

/* 0x0002921a: big countdown / race clock (m:ss digits from texture 0x96). */
static void m28_clock(uint8_t *me, int secs, int aspect)
{
    int x = G_S32(0x4036c); /* PTR 0x3d2f4 */
    int yb = aspect == 2 ? 0x20 : 0x10;
    int min, sec;
    bool label = G_S32(0x3e9b0) < 2;

    if (secs < 0)
        secs = 0;
    min = secs / 60;
    sec = secs - min * 60;
    if (AT_S32(me, 0xf4) == 0)
        fn_00004fb4(0xff, 0xc8, 0, 0xff);
    else if (AT_S32(me, 0x8c) < G_S32(0x3e9d4))
        fn_00004fb4(0xff, 0, 0, 0xff);
    else
        fn_00004fb4(0, 0xff, 0, 0xff);
    if (G_U8(0x42804) == 0 && G_S32(0x3e9b0) == 1)
        label = false;

    /* blinks during the last 10 seconds */
    if (secs > 10 || G_S32(0x5c16c) % 128 >= 0x41) {
        float y = (float)yb;

        fn_0000526c(0x96, (float)(x - 0x2a), y, 24.0f, 40.0f, 0.95f, (float)(min % 10) * 0.1f,
                    0.0f, 0.1f, 1.0f, 0.0f, 0.0f, 0.0f, 0);
        fn_0000526c(0x96, (float)(x - 10), y, 24.0f, 40.0f, 0.95f, (float)(sec / 10) * 0.1f, 0.0f,
                    0.1f, 1.0f, 0.0f, 0.0f, 0.0f, 0);
        fn_0000526c(0x96, (float)(x + 0xe), y, 24.0f, 40.0f, 0.95f, (float)(sec % 10) * 0.1f,
                    0.0f, 0.1f, 1.0f, 0.0f, 0.0f, 0.0f, 0);
        snprintf(TXT, 0x100, ":");
        fn_00023808(TXT, (float)(x - 0x17), (float)(yb | 0xf), 1.0f, 1.0f, 0xff, 0xc8, 0, 0xc8, 0);
    }
    if (label) {
        snprintf(TXT, 0x100, "%s", STR(0x47e78));
        fn_00023808(TXT, (float)(x - 0x28), (float)(yb - 0x11), 1.19f, 1.0f, 0xff, 0xff, 0xff, 0xff,
                    0);
    }
}

/* 0x00029554: fill the end-of-race result table of `car` (drawn fading by fn_00028a58). */
static void m28_results(int car)
{
    uint8_t *me = CAR(car);
    int m = MODE_CUR;

    if (m == MODE_D) {
        int laps, bonus, t;

        if (AT_S32(me, 0xf4) == 0)
            return;
        laps = AT_S32(me, 0x8c);
        bonus = G_S32(0x3f8dc) == car ? 3 : 0; /* fastest lap */
        snprintf(RES_LINE(me, 0), 0x20, "+%2d %s", laps, STR(0x47d64));
        snprintf(RES_LINE(me, 1), 0x20, "+%2d %s", bonus, STR(0x47ce4));
        t = (int)(AT_F32(me, 0x124) / 1000.0f);
        snprintf(RES_LINE(me, 2), 0x20, "+%2d %s", t, STR(0x47e78));
        snprintf(RES_LINE(me, 3), 0x20, "--------");
        snprintf(RES_LINE(me, 4), 0x20, "=%2d     ", t + bonus + laps);
        m28_res_cols(me, G_U32(0x3eb7c));
        AT_S32(me, 0x128) = (int)(G_F32(0x3f8ec) + 2000.0f);
    } else if (m == MODE_B && AT_S32(me, 0xf4) != 0) {
        if (AT_S32(me, 0x10c) != 0) {
            snprintf(RES_LINE(me, 0), 0x20, "%s",
                     AT_S32(me, 0xf8) == 0 ? STR(0x47cec) : STR(0x47e54));
            snprintf(RES_LINE(me, 1), 0x20, "%s", STR(0x47de8));
            snprintf(RES_LINE(me, 2), 0x20, "--------");
            snprintf(RES_LINE(me, 3), 0x20, "%d", (int)AT_S32(me, 0xa4));
            snprintf(RES_LINE(me, 4), 0x20, " ");
            m28_res_cols(me, G_U32(0x3eb7c));
            AT_S32(me, 0x128) = (int)(G_F32(0x3f8ec) + 3000.0f);
        } else if (AT_S32(me, 0x108) != 0) {
            float now;

            m28_res_cols(me, G_U32(0x3eb7c));
            snprintf(RES_LINE(me, 1), 0x20, "%s", STR(0x47de8));
            snprintf(RES_LINE(me, 2), 0x20, "--------");
            snprintf(RES_LINE(me, 3), 0x20, "%d", (int)AT_S32(me, 0xa4));
            snprintf(RES_LINE(me, 4), 0x20, " ");
            now = G_F32(0x3f8ec);
            AT_S32(me, 0x128) = (int)(now + 3000.0f);
            if (now >= (float)G_S32(0x3e9dc)) /* time limit reached */
                snprintf(RES_LINE(me, 0), 0x20, "%s", STR(0x47d20));
            else if (G_S32(0x3f9c0) >= 1)
                snprintf(RES_LINE(me, 0), 0x20, "%s", STR(0x47ce0));
            else if (G_S32(0x3f9c0) == 0)
                snprintf(RES_LINE(me, 0), 0x20, "%s", STR(0x47e54));
        }
    } else if (m == MODE_A && (G_U8(0x3f8d4) & 1)) {
        m28_res_cols(me, G_U32(0x3eb7c));
        AT_U32(me, 0x1d8) = G_U32(0x3eb84);
        snprintf(RES_LINE(me, 0), 0x20, " ");
        snprintf(RES_LINE(me, 1), 0x20, "%s", STR(0x47de8));
        snprintf(RES_LINE(me, 2), 0x20, "--------");
        snprintf(RES_LINE(me, 3), 0x20, "%d", (int)AT_S32(me, 0x11c));
        snprintf(RES_LINE(me, 4), 0x20, " ");
        AT_S32(me, 0x128) = (int)(G_F32(0x3f8ec) + 3000.0f);
    }
}

/* Colour of one of the four wheel lights of the second gauge (value 0..1: red..green). */
static void m28_light_color(float v)
{
    if (v > 0.5f)
        fn_00004fb4((int)((1.0f - v) * 255.0f * 2.0f), 0xff, 0, 0xff);
    else
        fn_00004fb4(0xff, (int)((v + v) * 255.0f), 0, 0xff);
}

/* 0x00028a58 -- in-race HUD of viewport `vp` for car `car` (guess: Hud_Draw). Standings or
 * race position, clock, best lap, name tags (fn_0002b654), speedo + gear, optional second
 * gauge, mode-B bars, lives, lap counter and the fading result table. On the credits screen
 * (flag 0x3f928) it draws the credits instead. */
/* port: in a single-player race the speedometer, the speed and the second gauge are drawn on
 * the bottom screen (controls.h hud_bottom_begin); split-screen keeps them in each view. */
#define M28_BOTTOM (G_S32(0x3e9b0) == 1)

void fn_00028a58(int car, int vp)
{
    int aspect = (int)(VP_W(vp) / VP_H(vp));
    uint8_t *me = CAR(car);
    int m;

    fn_00004e68(0.0f, 0.0f, 0.0f, 1.0f, (float)aspect, 1.0f);
    if (G_U8(0x3f928) & 1) {
        m28_credits();
        return;
    }
    if (G_S32(0x3e9b0) <= vp && vp != ng_drawn_view()) { /* viewport not in use by a player */
        fn_0002b654(car, vp, aspect);
        return;
    }

    /* ---- standings (mode A) or race position ---- */
    if (MODE_CUR == MODE_A) {
        int n = G_S32(0x3e9ac);
        int base = aspect == 2 ? 0x40 : 0x41;

        fn_00005068(1);
        if (n >= 1) {
            int best = 1, step, off, j;

            for (j = 0; j < n; j++)
                if (AT_S32(CAR(j), 0x11c) >= best)
                    best = AT_S32(CAR(j), 0x11c);
            step = 0xbe / (n + 4);
            off = -step;
            for (j = 0; j < G_S32(0x3e9ac); j++, off += step) {
                int s = RANK[j];
                uint8_t *cs = CAR(s);
                float y = (float)(base + off);
                uint32_t col;

                snprintf(TXT, 0x100, s == G_S32(0x3f9b0) ? "%s&" : "%s", (const char *)(cs + 0xc));
                if (s == car) {
                    fn_00023808(TXT, 64.0f, y, 1.0f, 1.0f, 0xff, 0xff, 0, 0xff, 4);
                    col = G_U32(0x3eb84);
                } else {
                    fn_00023808(TXT, 64.0f, y, 1.0f, 1.0f, 0xff, 0xff, 0xff, 0xff, 4);
                    col = G_U32(0x3eb80);
                }
                fn_0002be1c((base | 0x14) + off, col, 1, 0x46,
                            (float)AT_S32(cs, 0x11c) / (float)best);
            }
        }
    } else {
        int prev = AT_S32(me, 0xd0), pos;
        int base = aspect == 2 ? 0x20 : 0x41;

        fn_00005068(1);
        if (AT_S32(me, 0xf4) != 0) { /* finished: final position in yellow */
            fn_00004fb4(0xff, 0xff, 0, 0xff);
            pos = prev = AT_S32(me, 0xf4);
        } else {
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
            pos = AT_S32(me, 0x9c);
        }
        m = MODE_CUR;
        if (m == MODE_E || m == MODE_A || m == MODE_C || (m == MODE_D && G_S32(0x3e9ac) >= 2)) {
            float y = (float)base;
            int total, t;
            const char *suffix;

            fn_0000526c(0x96, 64.0f, y, 48.0f, 80.0f, 0.95f, (float)(pos % 10) * 0.1f, 0.0f, 0.1f,
                        1.0f, 0.0f, 0.0f, 0.0f, 4);
            total = (m == MODE_A || m == MODE_D) ? G_S32(0x3e9ac) : NCARS;
            if (pos >= 10)
                fn_0000526c(0x96, 16.0f, y, 48.0f, 80.0f, 0.95f, (float)(pos / 10) * 0.1f, 0.0f,
                            0.1f, 1.0f, 0.0f, 0.0f, 0.0f, 4);

            /* position change: the old number grows and fades out over 50 frames */
            t = AT_S32(me, 0xd4);
            if (t <= 0x31) {
                if (prev == pos) {
                    AT_S32(me, 0xd4) = 0x32;
                } else {
                    float w = (float)(0x30 + 2 * t), h = (float)(0x50 + 2 * t);
                    float py = (float)(base - t);

                    fn_00004fb4(0xff, 0xff, 0xff, (t * -3 + 0xff) & 0xff);
                    fn_0000526c(0x96, (float)(0x40 - t), py, w, h, 0.95f,
                                (float)(prev % 10) * 0.1f, 0.0f, 0.1f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
                    if (prev >= 10)
                        fn_0000526c(0x96, (float)(0x10 - t), py, w, h, 0.95f,
                                    (float)(prev / 10) * 0.1f, 0.0f, 0.1f, 1.0f, 0.0f, 0.0f, 0.0f,
                                    4);
                }
            }
            switch (pos) { /* "st" / "nd" / "rd" / "th" */
            case 1: suffix = STR(0x47de4); break;
            case 2: suffix = STR(0x47d88); break;
            case 3: suffix = STR(0x47dcc); break;
            default: suffix = STR(0x47e24); break;
            }
            snprintf(TXT, 0x100, "%s", suffix);
            fn_00023808(TXT, 112.0f, (float)(base + 5), 1.0f, 1.0f, 0xff, 0xff, 0xff, 0xff, 4);
            snprintf(TXT, 0x100, "/%d", total);
            fn_00023808(TXT, 111.0f, (float)(base + 0x37), 1.0f, 1.0f, 0xff, 0xff, 0xff, 0xff, 4);
        }
    }

    /* ---- clock (0x2910c): time left in modes B / A (player 0 only), race time in mode D ---- */
    m = MODE_CUR;
    if (m == MODE_B || m == MODE_A) {
        int secs = (int)(((float)G_S32(0x3e9dc) - G_F32(0x3f8ec)) / 1000.0f);

        if (car == 0)
            m28_clock(me, secs, aspect);
        m28_results(car);
    } else if (m == MODE_D) {
        m28_clock(me, (int)(AT_F32(me, 0x124) / 1000.0f), aspect);
        m28_results(car);
    }

    /* ---- best lap (0x29af6) ---- */
    if (MODE_CUR != MODE_B) {
        int y = aspect == 2 ? 0xa0 : 0x100;
        int bc = G_S32(0x3f8dc), tc, hl;

        if (bc >= 0 && G_S32(0x3e9b0) <= 1) {
            m28_fmt_str(TXT, 0x100, "(%s)", (const char *)(CAR(bc) + 0xc));
            tc = bc;
        } else {
            snprintf(TXT, 0x100, "%s", STR(0x47ce4));
            tc = car;
        }
        hl = G_S32(0x3f8dc) == car ? 0 : 0xff; /* own record in yellow */
        fn_00023808(TXT, 64.0f, (float)y, 1.0f, 1.0f, 0xff, 0xff, hl, 0xff, 4);
        m28_fmt_time(AT_S32(CAR(tc), 0xb8));
        fn_00023808(TXT, 64.0f, (float)(y | 0x15), 1.0f, 1.0f, 0xff, 0xff, hl, 0xff, 4);
    }

    fn_0002b654(car, vp, aspect);

    /* ---- speedometer (0x29fa6) ---- */
    {
        int y0 = G_S32(0x3f868) / aspect + (aspect == 1 ? -280 : -248);
        int x0 = G_S32(0x3f864);
        float gx = (float)(x0 - 0xaa), gy = (float)y0;
        int g, v;

        if (M28_BOTTOM) /* the 200 x 200 dial, right half of the bottom screen */
            hud_bottom_begin((float)(x0 - 70), (float)(y0 + 100), 250.0f, 74.0f, 0.7f);
        if (G_F32(0x3ea00) > 1.0f)
            fn_00004fb4(0xff, 100, 0, 0xff);
        else
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_0000526c(0xa6, gx, gy, 200.0f, 200.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                    4);
        /* rev light, blinking above 65 */
        if (AT_F32(me, 0x238) > 65.0f && G_S32(0x5c16c) % 16 >= 9)
            fn_00004fb4(0xff, 50, 0, 0xff);
        else
            fn_00004fb4(100, 0, 0, 0xff);
        fn_0000526c(0xb1, (float)(x0 - 0x38), (float)(y0 + 0x22), 13.0f, 13.0f, 0.95f, 0.0f, 0.0f,
                    1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
        if (G_F32(0x3ea00) > 1.0f)
            fn_00004fb4(0xff, 0, 0, 0xff);
        else
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        /* needle */
        fn_0000526c(0xa5, (float)(x0 - 0x3c), (float)(y0 + 0x6e), 16.0f, 112.0f, 0.95f, 0.0f, 0.0f,
                    1.0f, 1.0f, AT_F32(me, 0x23c) + 90.0f, (float)(x0 - 0x34), (float)(y0 + 0x76),
                    4);
        fn_00004fb4(200, 200, 0xff, 0xeb);
        fn_0000526c(0xb2, gx, gy, 200.0f, 200.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                    4);

        /* gear */
        g = AT_S32(me, 0x28);
        if (g <= 0)
            snprintf(TXT, 0x100, "R");
        else if (g >= 2)
            snprintf(TXT, 0x100, "%d", g - 1);
        else
            snprintf(TXT, 0x100, "N");
        if (G_F32(0x3ea00) > 1.0f)
            fn_00023808(TXT, (float)(x0 - 0xa4), (float)(y0 + 0x10), 1.0f, 1.0f, 0xff, 0, 0, 0xff,
                        4);
        else
            fn_00023808(TXT, (float)(x0 - 0xa4), (float)(y0 + 0x10), 1.0f, 1.0f, 0, 0xff, 0, 0xff,
                        4);

        /* speed (0x3f8cc: other unit, x1.35) */
        v = (int)(AT_F32(me, 0x254) * 10.0f);
        if (G_S32(0x3f8cc) != 0)
            v = (int)((float)v * 1.35f);
        snprintf(TXT, 0x100, "%s.%03d", STR(0x47d58), v);
        if (M28_BOTTOM) { /* top left, above the map */
            hud_bottom_end();
            hud_bottom_begin(64.0f, 206.0f, 10.0f, 8.0f, 0.8f);
        }
        fn_00023808(TXT, 64.0f, aspect == 2 ? 136.0f : 206.0f, 1.0f, 1.0f, 0xff, 0, 0, 0xff, 4);
        if (M28_BOTTOM)
            hud_bottom_end();
    }

    /* ---- second gauge (0x2a39c): needle from car+0x20, four wheel lights ---- */
    if (G_S32(0x3f8e0) != 0) {
        int y2 = (G_S32(0x3f868) - 0x50) / aspect - 20 * aspect;
        int x = G_S32(0x3f864);
        float by = (float)(y2 - 0x30), f;

        if (M28_BOTTOM) /* below the dial */
            hud_bottom_begin((float)(x - 55), (float)(y2 - 3), 246.0f, 178.0f, 0.7f);
        if (G_F32(0x3ea00) > 1.0f)
            fn_00004fb4(0xff, 100, 0, 0xff);
        else
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_0000526c(0xaf, (float)(x - 0x78), by, 113.0f, 65.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 4);
        f = AT_F32(me, 0x20);
        if ((f < 0.2f && G_S32(0x5c16c) % 20 < 10) || f == 0.0f) { /* low: blinking warning */
            fn_00004fb4(0xff, 0, 0, 0xff);
            fn_0000526c(0xaf, (float)(x - 0x20), (float)(y2 - 0x10), 16.0f, 22.0f, 0.95f, 0.778f,
                        0.492f, 0.141f, 0.338f, 0.0f, 0.0f, 0.0f, 4);
            fn_0000526c(0xaf, (float)(x - 0x20), (float)(y2 - 0x10), 16.0f, 22.0f, 0.95f, 0.778f,
                        0.492f, 0.141f, 0.338f, 0.0f, 0.0f, 0.0f, 4);
        }
        if (G_F32(0x3ea00) > 1.0f)
            fn_00004fb4(0xff, 0, 0, 0xff);
        else
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_0000526c(0xa5, (float)(x - 0x1c), (float)(y2 - 3), 10.0f, 45.0f, 0.95f, 0.0f, 0.0f, 1.0f,
                    1.0f, AT_F32(me, 0x20) * 90.0f + 90.0f, (float)(x - 0x17), (float)(y2 + 2), 4);
        fn_00004fb4(200, 200, 0xff, 0xeb);
        fn_0000526c(0xb2, (float)(x - 0x40), by, 75.0f, 90.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 4);

        m28_light_color(AT_F32(me, 0x3f0));
        fn_0000526c(0xaf, (float)(x - 0x72), (float)(y2 - 0x2a), 12.0f, 22.0f, 0.95f, 0.053f,
                    0.092f, 0.106f, 0.338f, 0.0f, 0.0f, 0.0f, 4);
        m28_light_color(AT_F32(me, 0x44c));
        fn_0000526c(0xaf, (float)(x - 0x5a), (float)(y2 - 0x2a), 12.0f, 22.0f, 0.95f, 0.053f,
                    0.092f, 0.106f, 0.338f, 0.0f, 0.0f, 0.0f, 4);
        m28_light_color(AT_F32(me, 0x4a8));
        fn_0000526c(0xaf, (float)(x - 0x5a), (float)(y2 - 0xb), 12.0f, 22.0f, 0.95f, 0.053f,
                    0.092f, 0.106f, 0.338f, 0.0f, 0.0f, 0.0f, 4);
        m28_light_color(AT_F32(me, 0x504));
        fn_0000526c(0xaf, (float)(x - 0x72), (float)(y2 - 0xb), 12.0f, 22.0f, 0.95f, 0.053f,
                    0.092f, 0.106f, 0.338f, 0.0f, 0.0f, 0.0f, 4);
        if (M28_BOTTOM)
            hud_bottom_end();

        /* near the end of the racing line */
        if (AT_S32(me, 0x84) > G_S32(0x3f948) - 4) {
            snprintf(TXT, 0x100, "%s", STR(0x47db8));
            fn_00023808(TXT, 320.0f, aspect == 2 ? 120.0f : 240.0f, 1.0f, 1.0f, 0xff, 0xff, 0xff,
                        0xff, 0x34);
        }
    }

    /* ---- mode B bars (0x2a9f8): distance to the nearest target, damage ---- */
    if (MODE_CUR == MODE_B) {
        uint32_t col = G_U32(0x3eb7c);
        int yb = aspect == 2 ? 0x5e : 0x9c, yb2;
        float best = 3248.0f;
        const char *label = NULL;
        int i;

        if (AT_S32(me, 0x10c) != 0) {
            for (i = 0; i < NCARS; i++) {
                if (AT_S32(CAR(i), 0x108) != 0) {
                    float d = fn_00037fa4((float *)(me + 0x2b0), (float *)(CAR(i) + 0x2b0));

                    if (!(best < d))
                        best = d;
                }
            }
        }
        if (AT_S32(me, 0x108) != 0) {
            int t = AT_S32(me, 0x118);

            if (t >= 0 && AT_S32(CAR(t), 0x114) < G_S32(0x3e9d8) && AT_S32(CAR(t), 0xf8) == 0) {
                best = fn_00037fa4((float *)(me + 0x2b0), (float *)(CAR(t) + 0x2b0));
            } else {
                best = 3248.0f;
                for (i = 0; i < NCARS; i++) {
                    uint8_t *ci = CAR(i);

                    if (AT_S32(ci, 0x10c) != 0 && AT_S32(ci, 0x114) < G_S32(0x3e9d8) &&
                        AT_S32(ci, 0xf8) == 0) {
                        float d = fn_00037fa4((float *)(me + 0x2b0), (float *)(ci + 0x2b0));

                        if (!(best < d))
                            best = d;
                    }
                }
            }
        }
        if (AT_S32(me, 0xf8) != 0)
            fn_0002be1c(yb + 0x10, 0xff00ff00u, 1, 0x80, 1.0f);
        else
            fn_0002be1c(yb + 0x10, 0xff00ffffu, 0, 0x80, best / 3248.0f);
        snprintf(TXT, 0x100, "%s", STR(0x47d1c));
        fn_00023808(TXT, 64.0f, (float)yb, 1.0f, 1.0f, col & 0xff, (col >> 8) & 0xff,
                    (col >> 16) & 0xff, 0xff, 4);

        col = G_U32(0x3eb7c);
        fn_00004fb4(0xff, 0, 0, 0xff);
        yb2 = aspect == 2 ? 0x74 : 0xb0;
        if (AT_S32(me, 0x10c) != 0) {
            int dmg = AT_S32(me, 0x114);

            if (dmg < G_S32(0x3e9d8) || G_S32(0x5c16c) % 128 >= 0x41) {
                fn_0002be1c(yb2 + 0x10, G_U32(0x3eb84), 0, 0x80, (float)dmg / 100.0f);
                label = STR(0x47d04);
            }
        } else if (AT_S32(me, 0x108) != 0 && AT_S32(me, 0x118) >= 0) {
            fn_0002be1c(yb2 + 0x10, G_U32(0x3eb84), 0, 0x80,
                        (float)AT_S32(CAR(AT_S32(me, 0x118)), 0x114) / 100.0f);
            label = STR(0x47d44);
        }
        if (label != NULL) {
            snprintf(TXT, 0x100, "%s", label);
            fn_00023808(TXT, 64.0f, (float)yb2, 1.0f, 1.0f, col & 0xff, (col >> 8) & 0xff,
                        (col >> 16) & 0xff, 0xff, 4);
        }
    }

    /* ---- lives (0x2ada2) ---- */
    m = MODE_CUR;
    if (m == MODE_B || m == MODE_D || m == MODE_A) {
        int lx, lives = AT_S32(me, 0x54), i;
        float ly;

        if (aspect == 2) {
            ly = m == MODE_B ? 76.0f : 90.0f;
            lx = m == MODE_B ? 0x40 : 0xa0;
        } else if (m == MODE_B) {
            ly = 230.0f;
            lx = 0x40;
        } else {
            ly = m == MODE_A ? 138.0f : 140.0f;
            lx = m == MODE_A ? 0xa0 : 0x40;
        }
        if (lives >= 1) {
            float sz = (float)(0x20 / aspect);

            for (i = 1;; i++) {
                int v = AT_S32(me, 0x5c);

                if (AT_S32(me, 0x60) != 0 && v >= 1 && i == lives) { /* the life being lost */
                    int c = ((v % 5) * 50) & 0xff;

                    fn_00004fb4(0xff, c, c, v > 0xff ? 0xff : v & 0xff);
                } else {
                    fn_00004fb4(200, 200, 0xff, 0xff);
                }
                fn_0000526c(0xac, (float)((i - 1) * 0x20 / aspect + lx), ly, sz, sz, 0.95f, 0.0f,
                            0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
                fn_00004fb4(200, 200, 0xff, 0x55);
                fn_0000526c(0xac, (float)(G_S32(0x40368) - 0x4c),
                            (float)(G_S32(0x40374) - ((G_U8(0x3f83c) & 1) ? 0x90 : 0xa0)), 64.0f,
                            64.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0);
                lives = AT_S32(me, 0x54);
                if (i >= lives)
                    break;
            }
        }
    }

    /* ---- lap counter (0x2affa) ---- */
    m = MODE_CUR;
    if (m == MODE_E || m == MODE_C || m == MODE_D || m == MODE_A) {
        int laps = AT_S32(me, 0x8c) + 1;

        if (laps < 0)
            laps = 0;
        if (m == MODE_A) {
            snprintf(TXT, 0x100, "%s.%d", STR(0x47de8), (int)AT_S32(me, 0x11c));
        } else {
            int tot = G_S32(0x3e9d4);

            snprintf(TXT, 0x100, "%s.%d/%d", STR(0x47d64), laps < tot ? laps : tot, tot);
        }
        fn_00023808(TXT, 64.0f, aspect == 2 ? 116.0f : 176.0f, 1.0f, 1.0f, 0xff, 0xff, 0xff, 0xff,
                    4);
    }

    /* ---- result table (0x2b0f2), fades out during the last 500 ms ---- */
    {
        int d = (int)(G_F32(0x3f8ec) - (float)AT_S32(me, 0x128));

        if (d < 0) {
            static const int dy[5] = {0, 0x14, 0x28, 0x3c, 0x4b};
            int a = d * -255;
            int alpha = a >= 128000 ? 0xff : a / 500;
            int y0 = aspect == 2 ? 0x20 : (G_S32(0x3e9b0) == 1 ? 0x64 : 0x50);
            int k;

            /* formats the race time into TXT but never draws it (as in the original) */
            m28_fmt_time(AT_S32(me, 0xc0) - AT_S32(me, 0xbc));
            for (k = 0; k < 5; k++) {
                const uint8_t *c = RES_COL(me, k);

                if (strcmp(RES_LINE(me, k), " ") != 0)
                    fn_00023808(RES_LINE(me, k), 320.0f, (float)(y0 + dy[k]), 1.0f, 1.0f, c[0],
                                c[1], c[2], alpha & 0xff, 0x14);
            }
        }
    }
}

/* 0x0002b654 -- floating name / position tags over the other cars of viewport `vp` (guess:
 * Hud_DrawCarTags). `aspect` is the integer viewport aspect of fn_00028a58. In mode B a
 * damage bar is drawn over each tagged car. */
void fn_0002b654(int car, int vp, int aspect)
{
    void *mtx = (uint8_t *)G(0x46fa0) + vp * 0x30; /* camera matrices (slot 0x3f0ec) */
    float faspect = (float)aspect;
    int i;

    for (i = 0; i < NCARS; i++) {
        int mode = MODE_CUR;
        int c = (mode == MODE_B) ? i : ORDER[i];
        uint8_t *ci = CAR(i);
        uint8_t *cc;
        float v[3], out[3], z;
        uint32_t col;
        bool vis;

        if (c == car)
            continue;
        if (!((mode == MODE_B && (AT_S32(ci, 0x10c) != 0 || AT_S32(ci, 0x108) != 0)) ||
              (mode == MODE_E && i > AT_S32(CAR(car), 0x9c) - 4) ||
              (mode == MODE_C && i > AT_S32(CAR(car), 0x9c) - 4) ||
              (mode == MODE_D && c < G_S32(0x3e9ac)) ||
              (mode == MODE_A && c < G_S32(0x3e9ac))))
            continue;

        cc = CAR(c);
        v[0] = AT_F32(cc, 0x2b0);
        v[1] = AT_F32(cc, 0x2b4) + 50.0f;
        v[2] = AT_F32(cc, 0x2b8);
        fn_00037fe8(mtx, v, out);
        out[2] = -out[2];
        M28_PROJECT(vp, v, (float *)(cc + 0x2a4)); /* screen x, y at 0x2a4 / 0x2a8 */
        z = out[2];
        vis = z < 3000.0f && z > 150.0f;
        col = G_U32(0x3eb7c);

        if (MODE_CUR == MODE_B) {
            const char *fmt, *arg;
            float x, y, sc;

            if (!vis)
                continue;
            if (AT_S32(cc, 0x10c) != 0) {
                int full = G_S32(0x3e9d8);
                float f;

                fn_00004fb4(0, 0, 0, 0xff);
                fn_0000526c(0xa8, AT_F32(cc, 0x2a4) + -11.0f, AT_F32(cc, 0x2a8) / faspect + -3.0f,
                            22.0f, 6.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0);
                f = (float)(full - AT_S32(cc, 0x114)) / (float)full;
                f = fminf(1.0f, f);
                fn_00004fb4(0xff, (int)(f * 255.0f), 0, 0xff);
                fn_0000526c(0xa8, AT_F32(cc, 0x2a4) + -10.0f, AT_F32(cc, 0x2a8) / faspect + -2.0f,
                            f * 20.0f, 4.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0);
            }
            if (AT_S32(cc, 0x114) >= G_S32(0x3e9d8)) {
                fmt = "%s";
                arg = STR(0x47e58);
            } else if (AT_S32(cc, 0xf8) != 0) {
                fmt = "%s";
                arg = STR(0x47e68);
            } else {
                fmt = AT_S32(cc, 0x10c) != 0 ? "%s$" : "%s@";
                arg = (const char *)(cc + 0xc);
            }
            snprintf(TXT, 0x100, fmt, arg);
            x = AT_F32(cc, 0x2a4) * (float)G_S32(0x3f864) / (float)G_S32(0x40368);
            y = AT_F32(cc, 0x2a8) * (float)G_S32(0x3f868) / (float)G_S32(0x40374) / faspect +
                -16.0f;
            sc = G_S32(0x3e9b0) >= 3 ? 1.0f : 0.8f;
            fn_00023808(TXT, x, y, sc, sc, col & 0xff, (col & 0xff00) >> 8, (col >> 16) & 0xff,
                        0xff, 0x34);
        } else {
            float sx, sy, va, x, y, sc;
            int a, pos, r, g, b;

            if (!vis)
                continue;
            sx = AT_F32(cc, 0x2a4);
            if (!(sx > 0.0f) || !(sx < (float)G_S32(0x40368)))
                continue;
            sy = AT_F32(cc, 0x2a8);
            if (!(sy > 0.0f) || !(sy < (float)G_S32(0x40374)))
                continue;
            /* fade in over [150, 500), fade out over (2500, 3000) */
            if (z > 2500.0f) {
                a = (int)((3000.0f - z) * 255.0f / 500.0f);
            } else if (z < 500.0f) {
                a = (int)(z * 255.0f / 500.0f);
            } else {
                a = 0;
                if (z > 500.0f && z < 2500.0f)
                    a = 0xff;
            }
            va = VP_W(vp) / VP_H(vp);
            pos = AT_S32(cc, 0xf4);
            if (pos == 0)
                pos = AT_S32(cc, 0x9c);
            if (a > 0xff)
                a = 0xff;
            if (c < G_S32(0x3e9ac)) {
                snprintf(TXT, 0x100, "%s", (const char *)(cc + 0xc));
                r = 0xff, g = 0xff, b = 0;
            } else if (pos == 3) {
                snprintf(TXT, 0x100, "%d%s", 3, STR(0x47e64));
                r = 0xff, g = 200, b = 100;
            } else if (pos == 2) {
                snprintf(TXT, 0x100, "%d%s", 2, STR(0x47e60));
                r = 200, g = 200, b = 200;
            } else if (pos == 1) {
                snprintf(TXT, 0x100, "%d%s", 1, STR(0x47e6c));
                r = 0xff, g = 0xff, b = 0;
            } else {
                snprintf(TXT, 0x100, "%d%s", pos, STR(0x47e74));
                r = col & 0xff, g = (col & 0xff00) >> 8, b = (col >> 16) & 0xff;
            }
            if (MODE_CUR == MODE_A && AT_S32(cc, 0x104) != 0)
                strcat(TXT, "&"); /* sprintf(buf, "%s&", buf) */
            x = AT_F32(cc, 0x2a4) * (float)G_S32(0x3f864) / (float)G_S32(0x40368);
            y = AT_F32(cc, 0x2a8) * (float)G_S32(0x3f868) / (float)G_S32(0x40374) / va + -16.0f;
            if (a < 0)
                a = 0;
            sc = G_S32(0x3e9b0) >= 3 ? 1.0f : 0.8f;
            fn_00023808(TXT, x, y, sc, sc, r & 0xff, g, b & 0xff, a & 0xff, 0x34);
        }
    }
}

/* 0x0002be1c -- horizontal bar at row y, width w (guess: Hud_DrawBar): black frame, then a
 * fill of frac (clamped to 1) in colour `color` (mode 1, packed RGBA bytes) or red->yellow. */
void fn_0002be1c(int y, unsigned color, int mode, int w, float frac)
{
    fn_00004fb4(0, 0, 0, 0xff);
    fn_0000526c(0xa8, 64.0f, (float)(y - 4), (float)w, 10.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 4);
    frac = fminf(1.0f, frac);
    if (mode == 1)
        fn_00004fb4(color & 0xff, (color >> 8) & 0xff, (color >> 16) & 0xff, color >> 24);
    else
        fn_00004fb4(0xff, (int)(frac * 255.0f), 0, 0xff);
    fn_0000526c(0xa8, 65.0f, (float)(y - 2), (float)(w - 2) * frac, 6.0f, 0.95f, 0.0f, 0.0f, 1.0f,
                1.0f, 0.0f, 0.0f, 0.0f, 4);
}

/* 0x0002bf2c -- fade step: moves G_S32(0x3f8f0) toward G_S32(0x3f8f4) by 3, snapping when
 * within 5 (not a Ghidra function; called by the tick). */
void fn_0002bf2c(void)
{
    int target = G_S32(0x3f8f4);
    int v = G_S32(0x3f8f0);

    if (v < target) {
        v += 3;
        G_S32(0x3f8f0) = v;
        if (v > target - 5)
            G_S32(0x3f8f0) = target;
    } else if (v > target) {
        v -= 3;
        G_S32(0x3f8f0) = v;
        if (v < target + 5)
            G_S32(0x3f8f0) = target;
    }
}

/* 0x0002bf68 -- in-race HUD buttons: set visibility, read presses into a control bitmask
 * (guess: Hud_ReadButtons; same shape as fn_0000a184, reading the "held" byte +0x14). */
int fn_0002bf68(int car)
{
    int state = G_S32(0x3f848);
    uint8_t *b = G(0x40388); /* HUD button table, 0x20 bytes per button (PTR 0x3d2fc) */
    unsigned flags;
    int screen, pause_mode;
    uint8_t fab8;

    if (!((unsigned)(state - 1) < 2 || state == 4 || state == 6 || state == 7 || state == 5 ||
          state == 8 || state == 9 || (G_U8(0x3f9e0) & 1)))
        return 0;

    b[0x36] = 1;
    b[0x56] = 1;
    b[0x76] = 1;
    b[0x96] = 1;
    b[0xb6] = 1;
    b[0xd6] = 1;
    b[0x156] = 0;
    b[0x176] = 1;
    screen = G_S32(0x3f840);
    b[0x196] = screen == 7;
    fab8 = G_U8(0x3fab8);
    b[0x1b6] = fab8;
    b[0x1d6] = fab8;
    if (G_U8(0x3fab4) != 0 && (state == 5 || state == 8))
        pause_mode = 3;
    else
        pause_mode = 4;
    AT_S32(b, 0x17c) = pause_mode;
    b[0xf6] = state == 5;

    if (state == 5 && !(G_U8(0x3f9e0) & 1)) {
        flags = 0;
        if (b[0x34] != 0) {
            flags = 0x800;
            if (AT_F32(CAR(car), 0x264) > 1.0f)
                flags = 0x200;
        }
    } else {
        flags = b[0x34] != 0 ? 0x200 : 0;
    }
    if (b[0x54] != 0 || (b[0x1b4] != 0 && fab8 != 0))
        flags |= 0x100;
    if (b[0x74] != 0)
        flags |= 8;
    if (b[0x94] != 0)
        flags |= 4;
    flags |= b[0xb4] != 0;
    if (b[0xd4] != 0)
        flags |= 2;
    if (b[0xf4] != 0)
        flags |= 0x10;

    if (b[0x174] != 0) {
        if (pause_mode == 3 || state == 9) {
            flags |= 0x1010;
        } else {
            G_S32(0x3f84c) = state;
            G_S32(0x3f850) = screen;
            fn_0000a370(0);
        }
    }
    return (int)flags;
}

/* 0x0002c13c -- (re)loads race data guarded by the busy flag 0x3f894 (guess:
 * Race_ReloadAssets). what 0: car models + extra (car n+1), 1: faces/flags/logos (n+1),
 * 2: by *0x47e7c = n: 0 / 2 -> fn_00034b04, 1 / 2 -> fn_000110cc. */
void fn_0002c13c(int what, int n)
{
    int *sel = (int *)G(0x47e7c); /* slot 0x3f354 */

    if (G_U8(0x3f894) & 1)
        return;
    G_U8(0x3f894) = 1;
    G_U8(0x3f898) = 0;
    if (what == 2) {
        int v;

        sel[0] = n;
        fn_00004560();
        v = sel[0];
        if (v == 0) {
            G_U8(0x3f910) = 0;
            G_U8(0x3f914) = 1;
            fn_00034b04();
            v = sel[0];
            G_U8(0x3f910) = 1;
            G_U8(0x3f914) = 0;
        }
        if (v == 1) {
            G_U8(0x3f900) = 0;
            G_U8(0x3f904) = 1;
            fn_000110cc();
            v = sel[0];
            G_U8(0x3f900) = 1;
            G_U8(0x3f904) = 0;
        }
        if (v == 2) {
            G_U8(0x3f910) = 0;
            G_U8(0x3f914) = 1;
            fn_00034b04();
            G_U8(0x3f910) = 1;
            G_U8(0x3f914) = 0;
            G_U8(0x3f900) = 0;
            G_U8(0x3f904) = 1;
            fn_000110cc();
            G_U8(0x3f900) = 1;
            G_U8(0x3f904) = 0;
        }
    } else if (what == 1) {
        sel[0] = n + 1;
        fn_00004560();
        fn_000116f4(sel[0]);
    } else if (what == 0) {
        sel[0] = n + 1;
        sel[1] = n + 1;
        fn_00004560();
        fn_00011450(sel[0]);
        fn_000350c4(sel[1]);
    } else {
        return; /* flags stay 1 / 0 */
    }
    G_U8(0x3f894) = 0;
    G_U8(0x3f898) = 1;
    if (G_U8(0x3f888) & 1)
        fn_00004550();
}

/* Signed divide as the iOS runtime does it (__divsi3 returns 0 for a zero divisor). */
static int m28_div(int a, int b)
{
    return b ? a / b : 0;
}

/* Heading wrap used by the cameras: brings ang within 180 degrees of target, storing every
 * adjustment to *slot as the original does. */
static float m28_wrap(float *slot, float ang, float target)
{
    if (ang - target > 180.0f) {
        ang += -360.0f;
        *slot = ang;
    }
    if (target - ang > 180.0f) {
        ang += 360.0f;
        *slot = ang;
    }
    return ang;
}

/* Up vector tilted by the car's speed (modes 0 and 8). */
static void m28_speed_up(uint8_t *cam, float ang, float spd)
{
    float rad = (ang + -90.0f) * D2R;
    float co = cosf(rad), si = sinf(rad);

    AT_F32(cam, CAM_UP + 0) = si * spd / 500.0f;
    AT_F32(cam, CAM_UP + 4) = 1.0f;
    AT_F32(cam, CAM_UP + 8) = co * spd / 500.0f;
}

/* Up vector tilted by a quarter towards the heading ang - 90 (modes 5, 6, 9, 10, 11). */
static void m28_tilt_up(uint8_t *cam, float ang)
{
    float rad = (ang + -90.0f) * D2R;
    float co = cosf(rad), si = sinf(rad);

    AT_F32(cam, CAM_UP + 0) = si * 0.25f;
    AT_F32(cam, CAM_UP + 4) = 1.0f;
    AT_F32(cam, CAM_UP + 8) = co * 0.25f;
}

/* The default up vector, the const float[3] at 0x3ccf0. */
static void m28_const_up(uint8_t *cam)
{
    memcpy(cam + CAM_UP, G(0x3ccf0), 12);
}

/* Eye at `dist` behind the car along heading `ang`, `hgt` above it. */
static void m28_eye_behind(uint8_t *cam, uint8_t *cc, float ang, float dist, float hgt)
{
    float rad = ang * D2R;
    float co = cosf(rad), si = sinf(rad);

    AT_F32(cam, CAM_EYE + 0) = AT_F32(cc, 0x2bc) - si * dist;
    AT_F32(cam, CAM_EYE + 4) = AT_F32(cc, 0x2c0) + hgt;
    AT_F32(cam, CAM_EYE + 8) = AT_F32(cc, 0x2c4) - co * dist;
}

/* Height-grid byte (0x44000, slot 0x3f09c, 20 per row) at row clamped to [0, 0x3f8a4] and
 * column clamped to [0, 0x3f8a0]. */
static int m28_grid(int row, int col)
{
    int r = 0, c = 0;

    if (row >= 0)
        r = G_S32(0x3f8a4) < row ? G_S32(0x3f8a4) : row;
    if (col >= 0)
        c = G_S32(0x3f8a0) < col ? G_S32(0x3f8a0) : col;
    return ((uint8_t *)G(0x44000))[r * 20 + c];
}

/* 0x0002c2d8 -- per-frame camera update of camera `ci` (guess: Camera_Update). Modes:
 * 0 chase (speed-tilted up, smoothed), 1 side, 2 front, 3 orbit to 90 deg around the origin,
 * 4 trackside TV camera, 5 spinning (0x5c168 clock), 6 garage/menu orbit, 7 slow overhead
 * spin, 8 close chase, 9 slow orbit around the origin, 10 replay (0x3fa38 target),
 * 11 swinging replay, 12 cockpit. Then the fade level and (mode 1) a ground clamp. */
void fn_0002c2d8(int ci)
{
    uint8_t *cam = CAM(ci);
    int mode = AT_S32(cam, CAM_MODE);
    int c = AT_S32(cam, CAM_CAR);
    float *angp = &AT_F32(cam, CAM_ANG);
    float dist, hgt, ang, target, fade;

    memcpy(cam + CAM_PREV, cam + CAM_EYE, 12);
    /* 3DS: a multi-card race has the one-player chase camera (no split screen) */
    if (G_S32(0x3e9b0) == 1 || ng_view_car() >= 0) {
        dist = 150.0f;
        hgt = 130.0f;
    } else {
        dist = G_S32(0x3e9b0) == 2 ? 200.0f : 135.0f;
        hgt = 120.0f;
    }
    AT_F32(cam, CAM_FOV) = 90.0f;

    switch (mode) {
    case 0: { /* chase */
        uint8_t *cc = CAR(c);
        float head, d, div, co, si, t;
        int k;

        ang = *angp;
        m28_speed_up(cam, ang, AT_F32(cc, 0x268));
        head = AT_F32(cc, 0x244);
        ang = m28_wrap(angp, ang, head);
        ang = ang + (head - ang) / 10.0f;
        co = cosf(ang * D2R);
        si = sinf(ang * D2R);
        *angp = ang;
        d = dist - AT_F32(cc, 0x298);
        k = 0xfa - G_S32(0x3f8e4);
        div = (float)(k / 2 + 4);
        if (k < -1)
            div = 4.0f;
        t = AT_F32(cc, 0x2bc) - si * d;
        AT_F32(cam, CAM_EYE + 0) += (t - AT_F32(cam, CAM_EYE + 0)) * 0.5f;
        t = AT_F32(cc, 0x2c0) + (hgt + AT_F32(cc, 0x298));
        AT_F32(cam, CAM_EYE + 4) += (t - AT_F32(cam, CAM_EYE + 4)) / div;
        t = AT_F32(cc, 0x2c4) - co * d;
        AT_F32(cam, CAM_EYE + 8) += (t - AT_F32(cam, CAM_EYE + 8)) * 0.5f;
        break;
    }
    case 1:   /* side view */
    case 2: { /* front view */
        uint8_t *cc = CAR(c);

        m28_const_up(cam);
        target = AT_F32(cc, 0x244) + (mode == 1 ? -90.0f : 180.0f);
        ang = m28_wrap(angp, *angp, target);
        ang = ang + (target - ang) / 20.0f;
        *angp = ang;
        m28_eye_behind(cam, cc, ang, dist, hgt);
        break;
    }
    case 3: { /* orbit to heading 90 around the origin */
        float rad;

        m28_const_up(cam);
        ang = m28_wrap(angp, *angp, 90.0f);
        ang = ang + (90.0f - ang) / 20.0f;
        rad = ang * D2R;
        *angp = ang;
        AT_F32(cam, CAM_EYE + 0) = 0.0f - sinf(rad) * dist;
        AT_F32(cam, CAM_EYE + 4) = hgt + 0.0f;
        AT_F32(cam, CAM_EYE + 8) = 0.0f - cosf(rad) * dist;
        break;
    }
    case 4: { /* trackside TV camera */
        uint8_t *cc = CAR(c);
        float d, top, sx, sy, sz, dx, dy, dz, v[3];
        int fresh = 0, n, flag;

        m28_const_up(cam);
        d = fn_00037fa4((float *)(cc + 0x2bc), (float *)(cam + CAM_FIX));
        AT_F32(cam, CAM_FOV) = 90.0f - fminf(35.0f, fabsf(d / 50.0f));
        if (d > 4600.0f || AT_F32(cc, 0x254) < 1.0f) {
            /* pick the car to follow and put the post ahead of it on the racing line */
            const float *pt;
            int k, row, col, h;

            AT_F32(cam, CAM_FOV) = 90.0f;
            if (G_U8(0x3f928) & 1) {
                c = fn_00004400(NCARS);
            } else if (G_S32(0x3e9b0) > ci &&
                       (G_U8(0x3f98c) != 0 || AT_S32(CAM(c), CAM_HOLD) != 0)) {
                /* (the original indexes the camera record by the car number here) */
                c = ci;
            } else {
                int j = 1, prev;

                c = ORDER[0];
                if (AT_S32(CAR(c), 0xf4) != 0 && NCARS > 0) {
                    do { /* first car in race order that has not finished */
                        c = ORDER[j];
                        if (AT_S32(CAR(c), 0xf4) == 0)
                            break;
                        prev = j++;
                    } while (prev < NCARS);
                }
            }
            AT_S32(cam, CAM_CAR) = c;
            cc = CAR(c);
            k = AT_S32(cc, 0x84) + 3;
            if (k > G_S32(0x3f948))
                k -= G_S32(0x3f948);
            pt = (const float *)G(0x45640) + k * 2; /* racing line (slot 0x3f0c0) */
            row = (int)pt[0];
            col = (int)pt[1];
            h = m28_grid(row, col);
            AT_F32(cam, CAM_FIX + 0) = (float)row * 812.0f;
            AT_F32(cam, CAM_FIX + 4) = ((float)(unsigned)h + 1.0f) * 100.0f;
            AT_F32(cam, CAM_FIX + 8) = (float)col * 812.0f;
            fresh = 1;
        }
        /* eye a third of the way from the post to the car; its height is the highest ground
         * (+50) of 3 samples stepping towards the car's 0x2b0 position, at least the car's */
        sx = (AT_F32(cam, CAM_FIX + 0) + 2.0f * AT_F32(cc, 0x2bc)) / 3.0f;
        AT_F32(cam, CAM_EYE + 0) = sx;
        sz = (AT_F32(cam, CAM_FIX + 8) + 2.0f * AT_F32(cc, 0x2c4)) / 3.0f;
        AT_F32(cam, CAM_EYE + 8) = sz;
        sy = AT_F32(cam, CAM_EYE + 4);
        dx = (AT_F32(cc, 0x2b0) - sx) / 3.0f;
        dz = (AT_F32(cc, 0x2b8) - sz) / 3.0f;
        dy = (AT_F32(cc, 0x2b4) - sy) / 3.0f;
        top = -10000.0f;
        for (n = 3; n != 0; n--) {
            float g;

            flag = 0;
            g = M28_GROUND(-1, sx, sy, sz, &flag) + 50.0f;
            if (top > g)
                g = top;
            sz += dz;
            sy += dy;
            sx += dx;
            top = g;
        }
        if (AT_F32(cc, 0x2c0) > top)
            top = AT_F32(cc, 0x2c0);
        if (!fresh)
            top = AT_F32(cam, CAM_EYE + 4) + (top - AT_F32(cam, CAM_EYE + 4) + 100.0f) / 10.0f;
        AT_F32(cam, CAM_EYE + 4) = top;
        v[0] = -AT_F32(cc, 0x2bc);
        v[1] = -AT_F32(cc, 0x2c0);
        v[2] = -AT_F32(cc, 0x2c4);
        fn_00037dec((float *)(cam + CAM_EYE), v, v);
        fn_000379a8(v, v);
        *angp = atan2f(v[0], v[2]) * 57.2958f;
        break;
    }
    case 5: /* spinning around the car, 3 deg per clock unit */
        ang = *angp;
        m28_tilt_up(cam, ang);
        target = G_F32(0x5c168) * 3.0f;
        ang = m28_wrap(angp, ang, target);
        ang = ang + (target - ang) / 10.0f;
        *angp = ang;
        m28_eye_behind(cam, CAR(c), ang, 235.0f, 60.0f);
        break;
    case 6: { /* garage / menu orbit: heading from the menu selection */
        float rad;

        target = (float)(m28_div(G_S32(0x3faa8) * 360, G_S32(0x3ec00)) + 0xaa);
        ang = m28_wrap(angp, *angp, target);
        m28_tilt_up(cam, ang);
        ang = ang + (target - ang) / 10.0f;
        rad = ang * D2R;
        *angp = ang;
        AT_F32(cam, CAM_EYE + 0) += (sinf(rad) * -200.0f - AT_F32(cam, CAM_EYE + 0)) * 0.5f;
        AT_F32(cam, CAM_EYE + 4) += (80.0f - AT_F32(cam, CAM_EYE + 4)) * 0.5f;
        AT_F32(cam, CAM_EYE + 8) += (cosf(rad) * -200.0f - AT_F32(cam, CAM_EYE + 8)) * 0.5f;
        break;
    }
    case 7: /* slow spin high above the car */
        m28_const_up(cam);
        ang = *angp + 0.25f;
        *angp = ang;
        m28_eye_behind(cam, CAR(c), ang, 250.0f, 360.0f);
        break;
    case 8: { /* close chase */
        uint8_t *cc = CAR(c);
        float head;

        ang = *angp;
        m28_speed_up(cam, ang, AT_F32(cc, 0x268));
        head = AT_F32(cc, 0x244);
        ang = m28_wrap(angp, ang, head);
        ang = ang + (head - ang) / 20.0f;
        *angp = ang;
        m28_eye_behind(cam, cc, ang, 50.0f, 65.0f);
        break;
    }
    case 9: { /* slow orbit around the origin (the menu target only drives the wrap) */
        float rad;

        target = G_S32(0x3ec00) < 1
                     ? 0.0f
                     : (float)(m28_div(G_S32(0x3faa8) * 360, G_S32(0x3ec00)) + 0xaa);
        ang = m28_wrap(angp, *angp, target);
        m28_tilt_up(cam, ang);
        ang = ang + 0.125f;
        rad = ang * D2R;
        *angp = ang;
        AT_F32(cam, CAM_EYE + 0) += (sinf(rad) * -80.0f - AT_F32(cam, CAM_EYE + 0)) / 5.0f;
        AT_F32(cam, CAM_EYE + 4) += (80.0f - AT_F32(cam, CAM_EYE + 4)) / 5.0f;
        AT_F32(cam, CAM_EYE + 8) += (cosf(rad) * -80.0f - AT_F32(cam, CAM_EYE + 8)) / 5.0f;
        break;
    }
    case 10: { /* replay: heading towards 0x3fa38 + 180 */
        float step;

        ang = *angp;
        m28_tilt_up(cam, ang);
        target = G_F32(0x3fa38) + 180.0f;
        ang = m28_wrap(angp, ang, target);
        if (fabsf(G_F32(0x3fa38) - G_F32(0x47110)) > 5.0f) /* slot 0x3f120 */
            step = (target - ang) / 10.0f;
        else
            step = 0.125f;
        ang = ang + step;
        *angp = ang;
        m28_eye_behind(cam, CAR(c), ang, 235.0f, 150.0f);
        break;
    }
    case 11: { /* swinging replay camera driven by the 0x5c168 clock */
        float t = G_F32(0x5c168), h, rad;
        uint8_t *cc = CAR(c);

        m28_tilt_up(cam, *angp);
        /* (the original also wraps the heading towards 0x3fa38 + 180 here; the result is
         * overwritten below) */
        h = sinf(t * -4.0f * D2R) * 40.0f + 120.0f;
        ang = sinf(t * 3.0f * D2R) * 30.0f + 180.0f + -30.0f;
        *angp = ang;
        rad = ang * D2R;
        AT_F32(cam, CAM_EYE + 0) = AT_F32(cc, 0x2bc) - sinf(rad) * 245.0f + 100.0f;
        AT_F32(cam, CAM_EYE + 4) = AT_F32(cc, 0x2c0) + h;
        AT_F32(cam, CAM_EYE + 8) = AT_F32(cc, 0x2c4) - cosf(rad) * 245.0f;
        break;
    }
    case 12: { /* cockpit */
        uint8_t *cc = CAR(c);
        float m[12], rot[3], up[3], v[3];

        memcpy(up, G(0x3ccf0), 12);
        G_U8(0x42764) = 'Z';
        G_U8(0x42768) = 'X';
        rot[0] = AT_F32(cc, 0x298);
        rot[1] = AT_F32(cc, 0x244);
        G_U8(0x42760) = 'Y';
        rot[2] = AT_F32(cc, 0x29c) + AT_F32(cc, 0x268) * -0.5f;
        fn_00004620((int)(intptr_t)m, (int)(intptr_t)rot);
        G_U8(0x42760) = 'Z';
        G_U8(0x42764) = 'Y';
        G_U8(0x42768) = 'X';
        fn_00037fe8(m, up, (float *)(cam + CAM_UP));
        fn_000379a8((float *)(cam + CAM_UP), (float *)(cam + CAM_UP));
        v[0] = 0.0f;
        v[1] = AT_F32(cc, 0x7c) + 75.0f;
        v[2] = 10.0f;
        fn_00037fe8(m, v, v);
        *angp = AT_F32(cc, 0x244);
        fn_00037dec((float *)(cc + 0x2bc), v, v);
        memcpy(cam + CAM_EYE, v, 12);
        break;
    }
    default:
        break;
    }

    /* fade towards 255 (flag set) or 0 */
    fade = AT_F32(cam, CAM_FADE);
    if (AT_U8(cam, CAM_FADEIN) & 1) {
        if (fade < 255.0f)
            AT_F32(cam, CAM_FADE) = fade + (255.0f - fade) * 0.5f;
    } else if (fade > 0.0f) {
        AT_F32(cam, CAM_FADE) = fade + fade * -0.5f;
    }

    /* side view: keep the eye above the ground grid */
    if (mode == 1) {
        int row = (int)((AT_F32(cam, CAM_EYE + 0) + 406.0f) / 812.0f);
        int col = (int)((AT_F32(cam, CAM_EYE + 8) + 406.0f) / 812.0f);
        float h = ((float)(unsigned)m28_grid(row, col) + 1.0f) * 100.0f;
        float ey = AT_F32(cam, CAM_EYE + 4);

        if (ey < h)
            AT_F32(cam, CAM_EYE + 4) = ey + (h - ey) * 0.5f;
    }
}

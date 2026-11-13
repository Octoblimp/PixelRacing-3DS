/* m_12a68.c -- menu / scene 3D drawing: the car and prop models of the menu screens (stunt
 * intro, garage, results podium, car-select lineup, sky ring) and the visible-tile sweep of the
 * track renderer. iOS range [0x00012a68, 0x0001621c).
 *
 * Shared state used here (iOS addresses):
 *   0x3fa24 game/menu state, 0x3f9dc current game mode (compared with 0x3ebe0.. mode ids)
 *   0x474f4 int[] model slots of the menu props (G_S32(0x3ec00) of them)
 *   model node (fn_00038818): +0x270 pos xyz, +0x27c rot xyz (deg), +0x288/+0x298/+0x2a8 scale
 *     (diagonal of the node matrix), +0x28c shear/bob, +0x2ac base height
 *   0x48ae0 car 0 (0x678 bytes), reused as the "showroom" car the menus draw
 *   0x5c160 per-view camera block (0x80 bytes per player): +0x3b0 yaw (deg)
 */
#include "ios.h"
#include "opt_menu.h"

#define D2R u2f(0x3c8efa39) /* 0.0174533f */

#define STATE G_S32(0x3fa24)
#define MODE_CUR G_S32(0x3f9dc)
#define MODE_A G_S32(0x3ebe0)
#define MODE_B G_S32(0x3ebe4)
#define MODE_C G_S32(0x3ebe8)
#define MODE_D G_S32(0x3ebec)
#define MODE_E G_S32(0x3f9d8)

#define NPROPS G_S32(0x3ec00)                /* number of menu props (3) */
#define PROP_SLOT(i) (((int *)G(0x474f4))[i]) /* slot table (ptr slot 0x3f15c) */
#define SHOWCAR ((uint8_t *)G(0x48ae0))      /* car 0 */
#define ENTRY(i) ((uint8_t *)G(0x47544) + (i) * 24) /* car roster, 24-byte entries */
#define VIEW_YAW(p) G_F32(0x5c510 + (p) * 0x80)

/* Stunt-intro timer / sound latch (state 4). */
#define INTRO_T G_F32(0x3e998)
#define INTRO_SND G_S32(0x3f87c)

/* Node field access. */
#define N_F(o, off) AT_F32(o, off)

static void m12a68_scale(uint8_t *o, float s)
{
    N_F(o, 0x288) = s;
    N_F(o, 0x298) = s;
    N_F(o, 0x2a8) = s;
}

static void m12a68_draw(int slot, int tex, int filter)
{
    fn_00038a98(slot, tex, -1, 0, 0, filter);
}

/* Tint used by the prop pieces: the given colour, or the shadow colour 0x3eb88 at alpha 0x9b
 * when G_S32(0x3f8c4) is set. */
static void m12a68_tint(int r, int g, int b)
{
    if (G_S32(0x3f8c4) == 0) {
        fn_00004fb4(r, g, b, 0xff);
    } else {
        uint32_t c = G_U32(0x3eb88);
        fn_00004fb4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, 0x9b);
    }
}

/* Volume from the node height, then play (stunt intro). */
static void m12a68_play(uint32_t id_addr, float y, float mul)
{
    int v = (int)(y * mul + 8000.0f);
    fn_0003613c(G_S32(id_addr), v, v);
    fn_000360bc(G_S32(id_addr));
}

/* Paint id of a roster entry with a fixed paint (tbb at 0x14430). */
static int m12a68_fixed_paint(int car)
{
    switch (car) {
    case 0: return 0x37;
    case 1: return 10;
    case 2: return 0x6e;
    case 3: return 0x1e;
    case 4: return 0x1f;
    case 5: return 9;
    case 6: return 0x18;
    case 7: return 9;
    case 8: return 0x3e;
    case 9: return 2;
    case 10: return 4;
    default: return 0;
    }
}

static void m12a68_copy3(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, 12);
}

/* State 4 (stunt intro): run the scripted jump of prop o up to the target time
 * G_S32(0x3f088), 3 units per step (the timer lives in 0x3e998). */
static void m12a68_intro_jump(uint8_t *o, int q)
{
    float lim = (float)G_S32(0x3f088);
    float t = INTRO_T;
    float negq;

    if (!(t < lim)) {
        t = (float)(G_S32(0x3f088) - 1);
        INTRO_T = t;
    }
    if (!(t <= lim))
        return;
    negq = (float)(-q);
    do {
        INTRO_T = t + 3.0f;
        N_F(o, 0x288) = 1.0f;
        N_F(o, 0x2a8) = 1.0f;
        N_F(o, 0x270) = 0.0f;
        N_F(o, 0x27c) = 0.0f;
        N_F(o, 0x280) = negq;
        N_F(o, 0x284) = 0.0f;
        t = INTRO_T;
        if (t < 3600.0f) {
            /* ramp up: bouncing approach */
            N_F(o, 0x274) = fabsf(sinf(t * D2R) * 60.0f) + N_F(o, 0x2ac) + 20.0f;
            N_F(o, 0x278) = -60.0f - (1800.0f - t * 0.5f);
            N_F(o, 0x298) = fabsf(sinf((t + 45.0f) * D2R)) * 0.5f + 1.0f;
            if (N_F(o, 0x274) > N_F(o, 0x2ac) * N_F(o, 0x298)) {
                INTRO_SND = 0;
            } else if (INTRO_SND == 0) {
                INTRO_SND = 1;
                m12a68_play(0x3ebcc, N_F(o, 0x278), 4.0f);
            }
        } else if (t < 3735.0f) {
            if (t < 3603.0f || INTRO_SND == 0) {
                INTRO_SND = 1;
                m12a68_play(0x3ebcc, N_F(o, 0x278), 4.0f);
            }
            N_F(o, 0x298) = fabsf(sinf((t + 45.0f) * D2R)) * 0.5f + 1.0f;
            N_F(o, 0x274) = N_F(o, 0x2ac) * N_F(o, 0x298);
            N_F(o, 0x278) = (3735.0f - t) * -30.0f / 135.0f + -30.0f;
        } else if (t < 3915.0f) {
            N_F(o, 0x298) = 1.0f;
            N_F(o, 0x274) = N_F(o, 0x2ac);
            N_F(o, 0x278) = -30.0f;
            INTRO_SND = 0;
        } else if (t < 3945.0f) {
            if (INTRO_SND == 0) {
                fn_000360e0(G_S32(0x3ebd0));
                INTRO_SND = 1;
                m12a68_play(0x3ebd0, N_F(o, 0x278), 17.0f);
            }
            N_F(o, 0x284) = (t + -3915.0f) * 0.25f;
            N_F(o, 0x274) = N_F(o, 0x2ac) + sinf((t + -3915.0f) * D2R) * 15.0f;
            N_F(o, 0x278) = -30.0f;
            N_F(o, 0x298) = 1.0f;
        } else if (t < 4245.0f) {
            N_F(o, 0x284) = 7.0f;
            N_F(o, 0x274) = N_F(o, 0x2ac) + 8.0f;
            N_F(o, 0x278) = -30.0f;
            N_F(o, 0x298) = 1.0f;
            INTRO_SND = 0;
        } else if (t < 4275.0f) {
            if (INTRO_SND == 0) {
                fn_000360e0(G_S32(0x3ebd0));
                INTRO_SND = 1;
                m12a68_play(0x3ebd0, N_F(o, 0x278), 17.0f);
            }
            N_F(o, 0x284) = (4275.0f - t) * 0.25f;
            N_F(o, 0x274) = (N_F(o, 0x2ac) + 8.0f) + sinf((t + -4245.0f) * D2R) * 30.0f;
            N_F(o, 0x278) = -30.0f;
            N_F(o, 0x298) = 1.0f;
        } else if (t < 4475.0f) {
            N_F(o, 0x284) = 0.0f;
            N_F(o, 0x274) = N_F(o, 0x2ac) + 30.0f;
            N_F(o, 0x278) = -30.0f;
            N_F(o, 0x298) = 1.0f;
            if (t < 4285.0f) {
                INTRO_SND = 0;
            } else if (t < 4470.0f && INTRO_SND == 0) {
                INTRO_SND = 1;
                m12a68_play(0x3ebb8, N_F(o, 0x278), 17.0f);
            }
        } else if (t < 4935.0f) {
            float d = 4935.0f - t;
            N_F(o, 0x280) = d * d / 100.0f;
            N_F(o, 0x274) = (N_F(o, 0x2ac) + 30.0f) + sinf((t + -4575.0f) * D2R) * 15.0f;
            N_F(o, 0x278) = -30.0f;
            N_F(o, 0x298) = 1.0f;
        } else if (t < 5635.0f) {
            float d;
            if (t < 4945.0f) {
                INTRO_SND = 0;
            } else if (INTRO_SND == 0) {
                INTRO_SND = 1;
                m12a68_play(0x3ebd4, N_F(o, 0x278), 17.0f);
            }
            d = t + -4935.0f;
            N_F(o, 0x280) = 0.0f;
            N_F(o, 0x274) = N_F(o, 0x2ac) + 30.0f;
            N_F(o, 0x278) = -30.0f;
            N_F(o, 0x270) = d * d / 50.0f;
            N_F(o, 0x298) = 1.0f;
        } else {
            N_F(o, 0x280) = 0.0f;
            N_F(o, 0x274) = N_F(o, 0x2ac) + 35.0f;
            N_F(o, 0x278) = -30.0f;
            N_F(o, 0x270) = 1200.0f;
            N_F(o, 0x298) = 1.0f;
        }
        t = INTRO_T;
    } while (t <= (float)G_S32(0x3f088));
}

/* State 11: prop i (0..3) drawn as a few coloured pieces spread along its facing. */
static void m12a68_props_state11(uint8_t *o, int i, int q, int pass, int slot)
{
    float a, s, c, s4, c4, dy;

    switch (i) {
    case 0:
        m12a68_scale(o, u2f(0x3f4ccccd)); /* 0.8f */
        fn_00004fb4(0xe6, 0xe6, 0xe6, 0xff);
        break;
    case 1:
        m12a68_scale(o, 0.5f);
        a = (float)q * D2R;
        s = sinf(a);
        c = cosf(a);
        N_F(o, 0x270) = N_F(o, 0x270) - c * 20.0f;
        N_F(o, 0x278) = N_F(o, 0x278) - s * 20.0f;
        m12a68_tint(0xe6, 0xe6, 0xe6);
        m12a68_draw(slot, -1, 0);
        N_F(o, 0x270) = N_F(o, 0x270) + c * 40.0f;
        N_F(o, 0x278) = N_F(o, 0x278) + s * 40.0f;
        m12a68_tint(0xc8, 0x64, 0);
        break;
    case 2:
        m12a68_scale(o, 0.5f);
        N_F(o, 0x274) = N_F(o, 0x274) + (pass != 1 ? 40.0f : -40.0f);
        a = (float)q * D2R;
        s = sinf(a);
        c = cosf(a);
        c4 = c * 30.0f;
        s4 = s * 30.0f;
        N_F(o, 0x270) = N_F(o, 0x270) - c4;
        N_F(o, 0x278) = N_F(o, 0x278) - s4;
        m12a68_tint(0xc8, 0x64, 0);
        m12a68_draw(slot, -1, 0);
        dy = pass != 1 ? -20.0f : 20.0f;
        N_F(o, 0x274) = N_F(o, 0x274) + dy;
        N_F(o, 0x270) = N_F(o, 0x270) + c4;
        N_F(o, 0x278) = N_F(o, 0x278) + s4;
        m12a68_tint(0xe6, 0xe6, 0xe6);
        m12a68_draw(slot, -1, 0);
        N_F(o, 0x274) = N_F(o, 0x274) + dy;
        N_F(o, 0x270) = N_F(o, 0x270) + c4;
        N_F(o, 0x278) = N_F(o, 0x278) + s4;
        m12a68_tint(0, 0x64, 0xc8);
        break;
    default: /* 3 */
        m12a68_scale(o, 0.5f);
        a = (float)q * D2R;
        s = sinf(a);
        c = cosf(a);
        N_F(o, 0x270) = N_F(o, 0x270) - c * 20.0f;
        N_F(o, 0x278) = N_F(o, 0x278) - s * 20.0f;
        m12a68_tint(0xe6, 0xe6, 0xe6);
        m12a68_draw(slot, -1, 0);
        c4 = c * 40.0f;
        s4 = s * 40.0f;
        N_F(o, 0x270) = N_F(o, 0x270) + c4;
        N_F(o, 0x278) = N_F(o, 0x278) + s4;
        m12a68_tint(0xc8, 0x64, 0);
        m12a68_draw(slot, -1, 0);
        N_F(o, 0x274) = N_F(o, 0x274) + (pass != 1 ? 40.0f : -40.0f);
        N_F(o, 0x270) = N_F(o, 0x270) - c4;
        N_F(o, 0x278) = N_F(o, 0x278) - s4;
        m12a68_tint(0, 0x64, 0x32);
        m12a68_draw(slot, -1, 0);
        N_F(o, 0x270) = N_F(o, 0x270) + c4;
        N_F(o, 0x278) = N_F(o, 0x278) + s4;
        m12a68_tint(0, 0x64, 0xc8);
        break;
    }
    m12a68_draw(slot, -1, 0);
}

/* Colour of rank m of a 3-step medal stack: bronze, silver, gold. */
static void m12a68_medal_color(int m)
{
    if (m == 2)
        fn_00004fb4(0xff, 0xc8, 0, 0xff);
    else if (m == 1)
        fn_00004fb4(0xc8, 0xc8, 0xdc, 0xff);
    else if (m == 0)
        fn_00004fb4(0xa5, 0x46, 10, 0xff);
}

/* State 12: prop i (0..6) of the options / records screen. */
static void m12a68_props_state12(uint8_t *o, int i, int q, int pass, int slot, float extra)
{
    float a, s, c, lit, c20, s20, c60, s60, c30, s30;
    int k, sel;

    switch (i) {
    case 0:
        m12a68_scale(o, u2f(0x3f99999a)); /* 1.2f */
        if (G_S32(0x3e9e0) == 0)
            fn_00004fb4(0x96, 0x96, 0x96, 0x64);
        else
            fn_00004fb4(0, 0x64, 0xff, 0xff);
        m12a68_draw(slot, -1, 0);
        break;
    case 1: {
        uint8_t *o2;
        a = (float)(q + 90) * D2R;
        o2 = fn_00038818(((int *)G(0x47218))[1]);
        m12a68_copy3(o2 + 0x270, o + 0x270);
        m12a68_copy3(o2 + 0x27c, o + 0x27c);
        m12a68_scale(o2, u2f(0x3f19999a)); /* 0.6f */
        s = sinf(a);
        c = cosf(a);
        c20 = c * 20.0f;
        s20 = s * 20.0f;
        N_F(o2, 0x270) = N_F(o2, 0x270) - c20;
        N_F(o2, 0x278) = N_F(o2, 0x278) - s20;
        fn_00004fb4(0xe6, 0xe6, 0xe6, 0xff);
        m12a68_draw(((int *)G(0x47218))[1], -1, 0);
        m12a68_scale(o, u2f(0x3f19999a));
        N_F(o, 0x270) = N_F(o, 0x270) + c20;
        N_F(o, 0x278) = N_F(o, 0x278) + s20;
        if (G_S32(0x3f8bc) == 0)
            fn_00004fb4(0xc8, 0xc8, 0xff, 0xff);
        else
            fn_00004fb4(0x82, 0x82, 0x82, 0x64);
        m12a68_draw(slot, -1, 0);
        break;
    }
    case 2: /* level meter: 6 bars, G_S32(0x3e9e4) selects how many are lit */
        sel = G_S32(0x3e9e4);
        switch (sel) {
        case 3: lit = 1.0f; break;
        case 5: lit = 2.0f; break;
        case 6: lit = 3.0f; break;
        case 7: lit = 4.0f; break;
        case 9: lit = 5.0f; break;
        case 10: lit = 6.0f; break;
        default: lit = 0.0f; break;
        }
        a = (float)(q + 180) * D2R;
        s = sinf(a);
        c = cosf(a);
        N_F(o, 0x270) = N_F(o, 0x270) - c * 30.0f;
        N_F(o, 0x278) = N_F(o, 0x278) - s * 30.0f;
        N_F(o, 0x274) = N_F(o, 0x274) + 5.0f;
        c20 = c * 20.0f;
        s20 = s * 20.0f;
        c60 = c * 60.0f;
        s60 = s * 60.0f;
        for (k = 0; k < 6; k++) {
            if ((float)k < lit)
                fn_00004fb4((200 - 10 * k) & 0xff, 0xc8, (200 + 10 * k) & 0xff, 0xff);
            else
                fn_00004fb4(0x64, 0x64, 0x64, 0x64);
            m12a68_scale(o, u2f(0x3eb33333)); /* 0.35f */
            if (k <= 9 && ((1u << k) & 0x248)) { /* k == 3: start the second row */
                N_F(o, 0x274) = N_F(o, 0x274) + -20.0f;
                N_F(o, 0x270) = N_F(o, 0x270) - c60;
                N_F(o, 0x278) = N_F(o, 0x278) - s60;
            }
            m12a68_draw(slot, -1, 0);
            N_F(o, 0x270) = N_F(o, 0x270) + c20;
            N_F(o, 0x278) = N_F(o, 0x278) + s20;
        }
        break;
    case 3: /* three medals (texture 0xae), dimmed from G_F32(0x3e9b8) on */
        a = (float)(q + 240) * D2R;
        s = sinf(a);
        c = cosf(a);
        s30 = s * 30.0f;
        c30 = c * 30.0f;
        N_F(o, 0x270) = N_F(o, 0x270) - c30 * 3.0f * 0.5f;
        N_F(o, 0x278) = N_F(o, 0x278) - s30 * 3.0f * 0.5f;
        for (k = 0; k < 3; k++) {
            m12a68_medal_color(k);
            if (!((float)k < G_F32(0x3e9b8)))
                fn_00004fb4(0x64, 0x64, 0x64, 0x9b);
            m12a68_scale(o, ((float)(k + 1) * u2f(0x3e99999a) + 0.5f) / 3.0f);
            N_F(o, 0x270) = N_F(o, 0x270) + c30;
            N_F(o, 0x278) = N_F(o, 0x278) + s30;
            fn_00038a98(slot, 0xae, -1, 0, 0, 0);
        }
        break;
    case 4: { /* a car, through the car drawer 0x1e94c */
        float qf = (float)q, r, ry, rz;
        a = qf * D2R;
        c = cosf(a);
        m12a68_scale(o, u2f(0x3f4ccccd)); /* 0.8f */
        r = (float)(NPROPS * 5 + 70);
        s = sinf(a);
        ry = qf + extra;
        rz = 0.0f;
        if (pass == 1) {
            ry = ry + 0.0f;
            rz = 180.0f;
        }
        if (G_S32(0x3ea20) != 0) {
            fn_0001e94c(r * s, 0.0f, r * c, 0.0f, ry, rz, 0, pass);
        } else {
            fn_00004fb4(0x64, 0x64, 0x64, 0x9b);
            fn_0001e94c(r * s, 0.0f, r * c, 0.0f, ry, rz, 1, pass);
        }
        break;
    }
    case 5: /* three medals (plain model), dimmed from G_F32(0x3e9bc) on */
        a = (float)(q + 90) * D2R;
        s = sinf(a);
        c = cosf(a);
        s30 = s * 30.0f;
        c30 = c * 30.0f;
        N_F(o, 0x270) = N_F(o, 0x270) - c30 * 3.0f * 0.5f;
        N_F(o, 0x278) = N_F(o, 0x278) - s30 * 3.0f * 0.5f;
        for (k = 0; k < 3; k++) {
            m12a68_medal_color(k);
            if (!((float)k < G_F32(0x3e9bc)))
                fn_00004fb4(0x64, 0x64, 0x64, 0x9b);
            m12a68_scale(o, u2f(0x3edddddd)); /* 0.4333f */
            N_F(o, 0x270) = N_F(o, 0x270) + c30;
            N_F(o, 0x278) = N_F(o, 0x278) + s30;
            m12a68_draw(slot, -1, 0);
        }
        break;
    default: { /* 6: rocking prop with a flat shadow copy (model 0x92) */
        uint8_t *sh;
        m12a68_scale(o, u2f(0x3e19999a)); /* 0.15f */
        N_F(o, 0x27c) = 0.0f;
        N_F(o, 0x280) = 0.0f;
        N_F(o, 0x284) = cosf(G_F32(0x3faac) * 4.0f * D2R) * 15.0f;
        N_F(o, 0x274) = N_F(o, 0x274) + -25.0f;
        sh = fn_00038818(0x92);
        memcpy(sh + 0x288, o + 0x288, 36);
        m12a68_copy3(sh + 0x270, o + 0x270);
        m12a68_copy3(sh + 0x27c, o + 0x27c);
        if (G_S32(0x3f8ac) != 0)
            fn_00004fb4(0x1e, 0x1e, 0x1e, 0x64);
        else
            fn_00004fb4(10, 0x14, 0, 0xff);
        fn_000042b0(1, 0x201, 0);
        m12a68_draw(0x92, -1, 0);
        fn_000042b0(1, 0x201, 1);
        if (G_S32(0x3f8ac) != 0)
            fn_00004fb4(0x1e, 0x1e, 0x1e, 0x64);
        else
            fn_00004fb4(0x64, 0x64, 0x64, 0xff);
        m12a68_draw(slot, -1, 0);
        break;
    }
    }
}

/* Section 2: the car lineup of the menu states 10/11/12 (circle of NPROPS cars around the
 * centre) and 20/21 (two cars side by side). */
static void m12a68_lineup(int player, int pass)
{
    uint8_t *car = SHOWCAR;
    uint8_t *e = NULL;
    int n, j, k, lod, paint;
    float fog, px, pz;

    fn_00038a78(3);
    fn_00038a88(3);
    G_S32(0x3f938) = 0;
    G_S32(0x3f93c) = 4;
    G_S32(0x3f940) = 5;
    n = NPROPS;
    fog = G_F32(0x3fa1c);
    G_F32(0x5c21c) = 80.0f;
    if ((unsigned)(STATE - 10) >= 3)
        n = 2;
    if (STATE == 10)
        AT_S32(car, 0x108) = 0;
    lod = fog > 0.5f ? 2 : 0;
    for (j = 0; j < n; j++) {
        float x0 = (float)(160 * j);
        int xoff = 150 * j - 75;

        if (MODE_CUR == MODE_B) {
            e = ENTRY(G_S32(0x3fa08));
            if (AT_S32(e, 0xc) != 0) {
                AT_S32(car, 0x108) = 1;
                AT_S32(car, 0x10c) = 0;
            } else {
                AT_S32(car, 0x108) = 0;
                AT_S32(car, 0x10c) = 1;
            }
        }
        if ((unsigned)(STATE - 10) <= 2) {
            float a = (float)((360 * j) / n) * D2R;
            float rot = x0 + G_F32(0x3fa10);
            float c = cosf(a), s = sinf(a);
            AT_F32(car, 0x2cc) = rot;
            AT_F32(car, 0x244) = rot;
            G_F32(0x3ebf0) = s * 300.0f;
            G_F32(0x3ebf4) = c * 300.0f;
            px = s * 300.0f;
            pz = c * 300.0f;
        } else {
            px = (float)xoff;
            pz = 0.0f;
            AT_F32(car, 0x2cc) = x0;
            AT_F32(car, 0x244) = x0;
        }
        AT_F32(car, 0x2b0) = px;
        AT_F32(car, 0x2b8) = pz;
        fn_00004620((int)(car + 0x334), (int)(car + 0x2c8));
        for (k = 0; k < 4; k++)
            fn_0001ea38(0, k, car + 0x334);
        if (MODE_CUR == MODE_B) {
            if (STATE == 21) {
                if (AT_S32(e, 0xc) != 0)
                    fn_00011018((int)(car + 0x394), AT_S32(e, 4));
            } else if (STATE == 20 || STATE == 11) {
                if (STATE == 11)
                    AT_S32(car, 0x108) = 1;
                if (AT_S32(e, 0xc) == 0)
                    paint = fn_00011cf8(G_S32(0x3fa00), 0, j % 2);
                else
                    paint = m12a68_fixed_paint(G_S32(0x3fa00));
                fn_00011018((int)(car + 0x394), paint);
            }
        } else if (STATE != 21) {
            fn_00011018((int)(car + 0x394), fn_00011cf8(G_S32(0x3fa00), 0, j % 2));
        }
        AT_S32(car, 0xfc) = lod;
        if (((int *)G(0x4819c))[G_S32(0x3fa00)] == 0) { /* locked car: grey paint */
            car[0x394] = 0x37;
            car[0x395] = 0x37;
            car[0x396] = 0x37;
            car[0x397] = 0xff;
        }
        fn_00038a78(3);
        fn_00038a88(3);
        if (pass == 1)
            fn_0001ed34(0, 0, player, 1, 0, 1, 1, 1, 0, fog);
        else
            fn_0001ed34(0, 0, player, 1, 0, 3, 2, 0, 0, fog);
    }
}

/* Section 3: the sky ring of sprites (model 0x84) around the camera, plus the large glow
 * billboards (texture 0xa2) on the first pass. Axis letters 0x42760.. select the rotation order. */
static void m12a68_sky(int pass)
{
    uint8_t *o;
    int view, base, m;
    float v1[3], v2[3];

    G_U8(0x42764) = 'X';
    G_U8(0x42760) = 'Y';
    G_U8(0x42768) = 'Z';
    view = G_S32(0x470d4);
    o = fn_00038818(0x84);
    N_F(o, 0x27c) = G_F32(0x47100);
    N_F(o, 0x280) = G_F32(0x47104) + 180.0f;
    N_F(o, 0x284) = 0.0f;
    fn_0001e8c0(-1.0f);
    fn_000042b0(0, 0x201, 0);
    glBlendFunc(0x302, 1);
    glDisable(0xb50);
    for (base = -10; base != 350; base += 90) {
        for (m = 0; m < 3; m++) {
            int ang = base + 10 * m;
            float a = (float)(ang + 45) * D2R;
            float c, s;
            m12a68_scale(o, 5.0f);
            c = cosf(a);
            s = sinf(a);
            N_F(o, 0x270) = G_F32(0x470f4) + s * 500.0f;
            N_F(o, 0x274) = G_F32(0x470f8) + 150.0f;
            N_F(o, 0x278) = G_F32(0x470fc) + c * 500.0f;
            if (pass == 1)
                N_F(o, 0x274) = -N_F(o, 0x274);
            fn_00004fb4(0xff, 0xff, 0xff, 200);
            N_F(o, 0x284) = 0.0f;
            fn_000042b0(1, 0x201, 0);
            if (STATE != 4)
                fn_00038a98(0x84, -1, -1, 0, 0, 2);
            N_F(o, 0x284) = G_F32(0x47104);
            fn_00037fe8((uint8_t *)G(0x46fa0) + G_S32(0x470d4) * 48, (float *)(o + 0x270), v1);
            v1[2] = -v1[2];
            fn_0001da58(view, (float *)(o + 0x270), v2);
            if (pass == 0 && m == 0) {
                float a2 = (float)ang * D2R;
                N_F(o, 0x284) = G_F32(0x47104) * 0.5f;
                m12a68_scale(o, 50.0f);
                c = cosf(a2);
                s = sinf(a2);
                N_F(o, 0x270) = G_F32(0x470f4) + s * 500.0f;
                N_F(o, 0x274) = G_F32(0x470f8) + 100.0f;
                N_F(o, 0x278) = G_F32(0x470fc) + c * 500.0f;
                fn_00004fb4(0xff, 0xff, 0xff, 100);
                fn_0001e8c0(-1.0f);
                fn_000042b0(0, 0x201, 0);
                fn_00038a98(0x84, 0xa2, -1, 0, 0, 2);
            }
        }
    }
    fn_0001e8c0(G_F32(0x3e9f4)); /* slot holds 0.5f */
    glEnable(0xb50);
    glBlendFunc(0x302, 0x303);
    fn_000042b0(1, 0x201, 1);
    G_U8(0x42760) = 'Z';
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';
}

/* Load roster entry e into the showroom car. Returns 0 when it has no body/wheel model. */
static int m12a68_load_entry(uint8_t *car, uint8_t *e)
{
    int tmp;

    if (MODE_CUR == MODE_B) {
        if (AT_S32(e, 0xc) != 0) {
            AT_S32(car, 0x108) = 1;
            AT_S32(car, 0x10c) = 0;
        } else {
            AT_S32(car, 0x108) = 0;
            AT_S32(car, 0x10c) = 1;
        }
    }
    tmp = 0;
    fn_0000d800(0, AT_F32(car, 0x2bc), AT_F32(car, 0x2c0), AT_F32(car, 0x2c4), &tmp);
    G_F32(0x5c21c) = 80.0f;
    AT_S32(car, 0) = ((int *)G(0x470a0))[AT_S32(e, 0)];
    fn_00011018((int)(car + 0x394), AT_S32(e, 4));
    AT_S32(car, 4) = ((int *)G(0x47218))[AT_S32(e, 8)];
    return AT_S32(car, 0) >= 1 && AT_S32(car, 4) >= 1;
}

/* Results podium: up to 4 finishers (order table 0x47060) on their steps, medals over the
 * first three, then the podium model 0x8f. */
static void m12a68_podium(int player, int pass, float fog)
{
    uint8_t *car = SHOWCAR;
    uint8_t *o;
    int ncar, n, j, k;

    ncar = (MODE_CUR == MODE_C || MODE_CUR == MODE_E) ? G_S32(0x3e9c8) : 0;
    if (MODE_CUR == MODE_A || MODE_CUR == MODE_B || MODE_CUR == MODE_D)
        ncar = G_S32(0x3e9ac);
    n = ncar > 4 ? 4 : ncar;
    for (j = 0; j < n; j++) {
        int r = ((int *)G(0x47060))[j];
        float y, h, f;

        if (!m12a68_load_entry(car, ENTRY(r)))
            continue;
        AT_F32(car, 0x2cc) = (float)((r * 360) / n + 30);
        AT_F32(car, 0x244) = AT_F32(car, 0x2cc);
        if (j == 0) {
            AT_F32(car, 0x2cc) = 30.0f;
            AT_F32(car, 0x244) = 30.0f;
            G_F32(0x3ebf0) = 0.0f;
            G_F32(0x3ebf4) = 0.0f;
            AT_F32(car, 0x270) = 109.0f;
        } else if (j == 3) {
            uint8_t *rec = (uint8_t *)G(0x44590);
            AT_F32(car, 0x2cc) = -30.0f;
            AT_F32(car, 0x244) = -30.0f;
            G_F32(0x3ebf0) = 0.0f;
            G_F32(0x3ebf4) = 130.0f;
            AT_F32(car, 0x270) = 15.0f;
            AT_S32(rec, 0x18) = 0;
            fn_0001d388(0, 0.0f, 0.0f, 130.0f, 0.0f, -30.0f, 0.0f, pass);
            if (!(AT_F32(rec, 0) < 360.0f)) {
                AT_S32(rec, 0x00) = 0;
                AT_S32(rec, 0x04) = 0;
                AT_S32(rec, 0x08) = 0;
                AT_S32(rec, 0x0c) = 0;
                AT_S32(rec, 0x10) = 0;
                AT_S32(rec, 0x20) = 0;
                AT_S32(rec, 0x24) = 0;
                AT_S32(rec, 0x28) = 0;
                AT_S32(rec, 0x2c) = 0;
                AT_S32(rec, 0x30) = 0;
            }
        } else if (j == 2) {
            AT_F32(car, 0x2cc) = 45.0f;
            AT_F32(car, 0x244) = 45.0f;
            G_F32(0x3ebf0) = -110.0f;
            G_F32(0x3ebf4) = 0.0f;
            AT_F32(car, 0x270) = 59.0f;
        } else if (j == 1) {
            AT_F32(car, 0x2cc) = -45.0f;
            AT_F32(car, 0x244) = -45.0f;
            G_F32(0x3ebf0) = 110.0f;
            G_F32(0x3ebf4) = 0.0f;
            AT_F32(car, 0x270) = 84.0f;
        }
        y = AT_F32(car, 0x270);
        if (pass == 1) {
            y = -y;
            AT_F32(car, 0x270) = y;
        }
        AT_F32(car, 0x274) = y;
        AT_F32(car, 0x3d4) = y;
        AT_F32(car, 0x430) = y;
        AT_F32(car, 0x48c) = y;
        AT_F32(car, 0x4e8) = y;
        AT_F32(car, 0x3ec) = y;
        AT_F32(car, 0x448) = y;
        AT_F32(car, 0x4a4) = y;
        AT_F32(car, 0x500) = y;
        AT_F32(car, 0x2b0) = G_F32(0x3ebf0);
        AT_F32(car, 0x2b4) = y + 37.0f;
        AT_F32(car, 0x2b8) = G_F32(0x3ebf4);
        fn_00004620((int)(car + 0x334), (int)(car + 0x2c8));
        for (k = 0; k < 4; k++)
            fn_0001ea38(0, k, car + 0x334);
        AT_S32(car, 0xfc) = 2;
        fn_00038a78(3);
        fn_00038a88(3);
        if (pass == 1)
            fn_0001ed34(0, r, player, j != 3, 1, 1, 1, 1, 0, fog);
        else
            fn_0001ed34(0, r, player, j != 3, 1, 3, 2, 0, 0, fog);
        if (j > 2)
            continue;
        /* medal over the car (model 0x79, texture 0xae) */
        o = fn_00038818(0x79);
        m12a68_copy3(o + 0x27c, car + 0x2c8);
        m12a68_copy3(o + 0x270, car + 0x2b0);
        h = (AT_F32(car, 0x2b4) + 70.0f) - (float)(5 * j);
        N_F(o, 0x274) = h;
        if (j == 0) {
            h = h + sinf(G_F32(0x3f9a8) * D2R) * 5.0f;
            N_F(o, 0x274) = h;
        } else if (j == 2) {
            N_F(o, 0x280) = N_F(o, 0x280) + sinf(G_F32(0x3f9a8) * D2R) * 30.0f;
        } else {
            N_F(o, 0x27c) = N_F(o, 0x27c) + sinf(G_F32(0x3f9a8) * D2R) * 30.0f;
        }
        if (pass == 1)
            N_F(o, 0x274) = h - h; /* sic: 0 (h - h in the original) */
        if (j == 0)
            fn_00004fb4(0xff, 0xc8, 0, 0xff);
        if (j == 1)
            fn_00004fb4(0xc8, 0xc8, 0xdc, 0xff);
        if (j == 2)
            fn_00004fb4(0xa5, 0x46, 10, 0xff);
        f = (float)(j + 1);
        m12a68_scale(o, (f * u2f(0x3e99999a) + u2f(0x3e99999a)) / f);
        fn_00038a98(0x79, 0xae, -1, 0, 0, 2);
    }
    o = fn_00038818(0x8f);
    N_F(o, 0x270) = 0.0f;
    if (pass == 1) {
        N_F(o, 0x27c) = 180.0f;
        N_F(o, 0x274) = -1.0f - N_F(o, 0x2ac);
    } else {
        N_F(o, 0x27c) = 0.0f;
        N_F(o, 0x274) = N_F(o, 0x2ac) + 1.0f;
    }
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    fn_00038a98(0x8f, -1, -1, 0, 0, 2);
}

/* Garage: all roster cars on a circle, the turntable model 0x86, a ring of 10 trophies
 * (slots 0x4821c, unlocked flags 0x481f4) bobbing around it, and the selection pointer 0x87. */
static const float m12a68_ring_yaw[10] = { 80, 0, 10, 70, 300, 240, 270, 110, 170, 230 };
static const float m12a68_ring_pitch[10] = { -15, -30, 15, -35, 10, -10, -30, 5, -40, -40 };
static const float m12a68_ptr_x[10] = { -15, -30, 15, -35, 10, -10, -30, 5, -40, -40 };
static const float m12a68_ptr_z[10] = { 440, 360, 370, 430, 660, 600, 630, 470, 530, 590 };

static void m12a68_garage(int player, int pass, float fog)
{
    uint8_t *car = SHOWCAR;
    uint8_t *o;
    int cnt, idx, k, sel;
    float x, z, v[3], A, B, C, c, ny, L, sc, R;

    cnt = G_S32(0x3e9ac);
    for (idx = 0; idx < G_S32(0x3e9ac); idx++) {
        int a;
        float af, s;

        if (!m12a68_load_entry(car, ENTRY(idx)))
            continue;
        a = (360 * idx) / cnt;
        af = (float)a * D2R;
        c = cosf(af);
        AT_F32(car, 0x2cc) = (float)(a + 30);
        AT_F32(car, 0x244) = AT_F32(car, 0x2cc);
        R = (float)(G_S32(0x3e9ac) * 6 + 100);
        s = sinf(af);
        G_F32(0x3ebf0) = s * R;
        G_F32(0x3ebf4) = c * R;
        AT_F32(car, 0x2b0) = s * R;
        AT_F32(car, 0x2b4) = 42.0f;
        AT_F32(car, 0x2b8) = c * R;
        fn_00004620((int)(car + 0x334), (int)(car + 0x2c8));
        for (k = 0; k < 4; k++)
            fn_0001ea38(0, k, car + 0x334);
        AT_S32(car, 0xfc) = 2;
        fn_00038a78(3);
        fn_00038a88(3);
        if (pass == 1)
            fn_0001ed34(0, idx, player, 1, 1, 1, 1, 1, 0, fog);
        else
            fn_0001ed34(0, idx, player, 1, 1, 3, 2, 0, 0, fog);
    }

    o = fn_00038818(0x86);
    N_F(o, 0x270) = 0.0f;
    if (pass == 1) {
        N_F(o, 0x27c) = 180.0f;
        N_F(o, 0x274) = -120.0f;
    } else {
        N_F(o, 0x27c) = 0.0f;
        N_F(o, 0x274) = 120.0f;
    }
    N_F(o, 0x278) = 0.0f;
    N_F(o, 0x280) = 0.0f;
    N_F(o, 0x284) = 0.0f;
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    fn_00038a98(0x86, -1, -1, 0, 0, 2);

    for (k = 0; k < 10; k++) {
        float a = (G_F32(0x3fa4c) + (float)(35 * k)) * D2R;
        float bob = sinf(a) * 0.125f;
        float yaw, pitch, a1, a2, y;
        int slot = ((int *)G(0x4821c))[k];

        c = cosf(a);
        yaw = m12a68_ring_yaw[k];
        pitch = (c + c) + m12a68_ring_pitch[k];
        a1 = yaw * D2R;
        a2 = pitch * D2R;
        x = sinf(a1) * 68.0f * cosf(a2);
        z = cosf(a1) * 68.0f * cosf(a2);
        o = fn_00038818(slot);
        N_F(o, 0x270) = x;
        N_F(o, 0x278) = z;
        if (pass == 1) {
            pitch = 180.0f - pitch;
            y = sinf(a2) * 68.0f + -120.0f;
        } else {
            y = sinf(a2) * -68.0f + 120.0f;
        }
        N_F(o, 0x27c) = pitch;
        N_F(o, 0x274) = y;
        N_F(o, 0x280) = yaw;
        N_F(o, 0x284) = 0.0f;
        N_F(o, 0x28c) = bob;
        if (((int *)G(0x481f4))[k] != 0)
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        else
            fn_00004fb4(0x32, 0x32, 0x32, 0x96);
        fn_00038a98(((int *)G(0x4821c))[k], -1, -1, 0, 0, 2);
    }
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    G_F32(0x3fa4c) = G_F32(0x3fa4c) + 1.0f;

    /* selection pointer, flying towards the selected trophy */
    o = fn_00038818(0x87);
    sel = G_S32(0x3fa44);
    if ((unsigned)sel <= 9) {
        x = m12a68_ptr_x[sel];
        z = m12a68_ptr_z[sel];
    } else {
        x = 0.0f;
        z = 360.0f;
    }
    G_F32(0x4710c) = x;
    G_F32(0x47110) = z;
    v[0] = x - G_F32(0x3fa34);
    v[1] = 0.0f;
    v[2] = z - G_F32(0x3fa38);
    if (!(fn_00037e54(v) > 1.0f))
        return;
    A = G_F32(0x3fa34);
    C = G_F32(0x3fa3c);
    c = cosf(fabsf(A - C) / (fabsf(G_F32(0x4710c) - C) + 1.0f) * 180.0f * D2R);
    N_F(o, 0x27c) = 0.0f;
    B = G_F32(0x3fa38);
    ny = (float)G_S32(0x471d4);
    N_F(o, 0x280) = ((B + 90.0f) + (float)(G_S32(0x471d4) * 90)) + c * -30.0f * ny;
    N_F(o, 0x284) = 0.0f;
    L = G_F32(0x47540);
    sc = L / 50.0f + 0.1f;
    sc = fminf(1.0f, sc);
    m12a68_scale(o, sc);
    N_F(o, 0x270) = sinf(G_F32(0x3fa38) * D2R) * (G_F32(0x47540) + 65.0f) *
                    cosf(G_F32(0x3fa34) * D2R);
    N_F(o, 0x278) = cosf(G_F32(0x3fa38) * D2R) * (G_F32(0x47540) + 65.0f) *
                    cosf(G_F32(0x3fa34) * D2R);
    if (pass == 1) {
        N_F(o, 0x27c) = 180.0f;
        N_F(o, 0x274) = sinf(G_F32(0x3fa34) * D2R) * (G_F32(0x47540) + 65.0f) + -120.0f;
    } else {
        N_F(o, 0x274) = sinf(G_F32(0x3fa34) * -D2R) * (G_F32(0x47540) + 65.0f) + 120.0f;
    }
    G_U8(0x42760) = 'Y';
    G_U8(0x42764) = 'Z';
    G_U8(0x42768) = 'X';
    fn_00038a98(0x87, -1, -1, 0, 0, 2);
    G_U8(0x42760) = 'Z';
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';
}

/* State 23: the single prop (PROP_SLOT(0)) swaying, with a blinking highlight copy (model 0x8b)
 * when entry G_S32(0x3f9f8) of the flag table 0x481c8 is set. */
static void m12a68_state23(int pass)
{
    uint8_t *o, *o2;
    float yaw, roll, w;
    int m, alpha, *lit;

    o = fn_00038818(PROP_SLOT(0));
    N_F(o, 0x27c) = 5.0f;
    yaw = ((G_F32(0x47104) + sinf(G_F32(0x3faac) * D2R) * 10.0f) + 180.0f) + 0.0f;
    N_F(o, 0x280) = yaw;
    N_F(o, 0x284) = 0.0f;
    N_F(o, 0x270) = 0.0f;
    N_F(o, 0x278) = 0.0f;
    N_F(o, 0x274) = 70.0f - sinf(G_F32(0x3faac) * D2R) * 10.0f;
    m12a68_scale(o, 1.0f);
    roll = 0.0f;
    m = G_S32(0x3f9f8) % 3;
    if (m == 0 || m == 1 || m == 2) {
        w = sinf(sinf(G_F32(0x5c168) * 100.0f * D2R) * 30.0f * D2R) * 20.0f;
        if (m == 0) {
            roll = w + 0.0f;
            N_F(o, 0x284) = roll;
        } else if (m == 1) {
            N_F(o, 0x27c) = w + 5.0f;
        } else {
            yaw = yaw + w;
            N_F(o, 0x280) = yaw;
        }
    }
    if (pass == 1) {
        N_F(o, 0x280) = -yaw;
        N_F(o, 0x284) = 180.0f - roll;
        N_F(o, 0x274) = sinf(G_F32(0x3faac) * D2R) * 10.0f + -40.0f;
    }
    alpha = (int)(G_F32(0x3fa1c) * 255.0f);
    lit = (int *)G(0x481c8);
    if (lit[G_S32(0x3f9f8)] == 0)
        fn_00004fb4(0x37, 0x37, 0x37, alpha);
    else
        fn_00004fb4(0xe6, 0xe6, 0xe6, alpha);
    m12a68_scale(o, 1.0f);
    fn_00038a98(PROP_SLOT(0), -1, -1, 0, 0, 2);
    if (lit[G_S32(0x3f9f8)] == 0)
        return;
    o2 = fn_00038818(0x8b);
    m12a68_scale(o2, u2f(0x3f8147ae)); /* 1.01f */
    m12a68_copy3(o2 + 0x27c, o + 0x27c);
    m12a68_copy3(o2 + 0x270, o + 0x270);
    if (G_S32(0x3f9e8) <= -1) {
        int r = fn_00004400(1000);
        int v = G_S32(0x3f9e8);
        if (r > 980 && G_S32(0x3f9f8) != 1 && G_S32(0x3f9f8) != 5 && v <= -16) {
            v = 15;
            G_S32(0x3f9e8) = 15;
        }
        G_S32(0x3f9e8) = v - 1;
    } else {
        fn_00004fb4(0xd2, 0xd2, 0xd2, (int)(G_F32(0x3fa1c) * 255.0f));
        fn_00038a98(0x8b, -1, -1, 0, 0, 2);
        G_S32(0x3f9e8) = G_S32(0x3f9e8) - 1;
    }
}

/* 0x00012a68 -- draws the 3D part of the menu screens for one view: the menu props (state
 * 4/5/10/11/12/25/30), the car lineup, the sky ring, the garage / results podium, the state-23
 * prop and the floating title model. pass 1 is the mirrored (reflection) pass. */
void fn_00012a68(int player, int pass)
{
    int st = STATE;
    int i;

    /* Section 1: menu props on a circle */
    if (((unsigned)(st - 10) < 2 || st == 4 || st == 5 || st == 30 || st == 12 || st == 25) &&
        NPROPS >= 1) {
        for (i = 0; i < NPROPS; i++) {
            int q = (360 * i) / NPROPS;
            int slot = PROP_SLOT(i);
            uint8_t *o = fn_00038818(slot);
            float extra = 0.0f, old;

            if (STATE == 4) {
                m12a68_intro_jump(o, q);
            } else {
                float qf;
                if (i == G_S32(0x3faa8))
                    extra = G_F32(0x3faac);
                m12a68_scale(o, 1.0f);
                qf = (float)q;
                if (NPROPS == 1) {
                    N_F(o, 0x270) = 0.0f;
                    N_F(o, 0x278) = 0.0f;
                } else {
                    float a = qf * D2R;
                    float c = cosf(a), s = sinf(a);
                    N_F(o, 0x270) = (float)(NPROPS * 5 + 70) * s;
                    N_F(o, 0x278) = (float)(NPROPS * 5 + 70) * c;
                }
                N_F(o, 0x280) = (qf + extra) + 180.0f;
                N_F(o, 0x284) = 0.0f;
                if (STATE == 5 || STATE == 30) {
                    N_F(o, 0x274) = 110.0f;
                    N_F(o, 0x27c) = -10.0f;
                } else {
                    N_F(o, 0x274) =
                        sinf(((float)(30 * i) + G_F32(0x3faac)) * -D2R) * 10.0f + 60.0f;
                    N_F(o, 0x27c) = -30.0f;
                }
            }
            if (pass == 1) {
                N_F(o, 0x280) = 180.0f - N_F(o, 0x280);
                N_F(o, 0x284) = N_F(o, 0x284) + 180.0f;
                N_F(o, 0x274) = -N_F(o, 0x274);
            }
            old = N_F(o, 0x27c);
            N_F(o, 0x27c) = -old;
            if (STATE == 12) {
                if (optm_prop(o, i, pass, slot)) /* the port's rows and pages (opt_menu.c) */
                    ;
                else if (i <= 6)
                    m12a68_props_state12(o, i, q, pass, slot, extra);
            } else if (STATE == 11) {
                N_F(o, 0x27c) = old;
                if (i <= 3)
                    m12a68_props_state11(o, i, q, pass, slot);
            } else {
                if (STATE == 10 && G_S32(0x3f8c4) != 0 && !(i <= 5 && ((1u << i) & 0x32))) {
                    uint32_t c = G_U32(0x3eb88);
                    fn_00004fb4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, 0x9b);
                } else {
                    fn_00004fb4(0xe6, 0xe6, 0xe6, 0xff);
                }
                m12a68_draw(slot, -1, 0);
            }
        }
    }

    /* Section 2: car lineup */
    st = STATE;
    if ((unsigned)(st - 10) < 3 || st == 20 || st == 21)
        m12a68_lineup(player, pass);

    /* Section 3: sky ring */
    m12a68_sky(pass);

    /* Sections 4/5: results podium or garage */
    if (G_U8(0x3fabc) != 0 && !(G_U8(0x3f928) & 1)) {
        int show = G_S32(0x3fac4);
        float fog;
        fn_00038a78(3);
        fn_00038a88(3);
        G_S32(0x3f938) = 0;
        G_S32(0x3f93c) = 4;
        G_S32(0x3f940) = 10;
        fog = G_F32(0x3fa1c);
        if (show != 0)
            m12a68_podium(player, pass, fog);
        else
            m12a68_garage(player, pass, fog);
    }

    /* Section 6 */
    if (STATE == 23)
        m12a68_state23(pass);

    /* Section 7: the floating title model G_S32(0x3f99c), first pass only */
    if (G_S32(0x3f99c) == -1 || G_S32(0x3fac4) != 0)
        return;
    if ((STATE == 30 && G_U8(0x3fabc) == 0) || pass != 0)
        return;
    {
        uint8_t *o = fn_00038818(G_S32(0x3f99c));
        float y, a;
        N_F(o, 0x288) = G_F32(0x3ebdc); /* slot holds 1.0f */
        N_F(o, 0x298) = G_F32(0x3ebdc);
        N_F(o, 0x2a8) = G_F32(0x3ebdc);
        N_F(o, 0x27c) = G_F32(0x47100) + sinf(G_F32(0x3faac) * D2R) * 15.0f;
        N_F(o, 0x280) = G_F32(0x47104) + 180.0f;
        N_F(o, 0x284) = 0.0f;
        y = G_U8(0x3fabc) != 0 ? 200.0f : 160.0f;
        if (pass == 1)
            y = -y;
        N_F(o, 0x274) = y;
        a = (VIEW_YAW(player) + -90.0f) * D2R;
        N_F(o, 0x270) = sinf(a) * 180.0f;
        N_F(o, 0x278) = cosf(a) * 180.0f;
        fn_00004fb4(0xe6, 0xe6, 0xe6, 0xff);
        fn_00003d20(2, 3500.0f, 5000.0f, 10.0f, 5000.0f, G_U32(0x3eb98));
        fn_00038a98(G_S32(0x3f99c), -1, -1, 0, 0, 2);
        m12a68_scale(o, 1.0f);
    }
}

/* One octant sweep of fn_00015e4c: outer/inner counters run from 0 to +-2r in steps of +-1
 * (direction odir/idir); each starts at -r (forward) or +r (backward). */
static void m15e4c_sweep(int r, int step, int odir, int idir, int inner_is_x, int a, int b,
                         int c, int d, int e)
{
    int o, n;

    if (r * 2 == 0)
        return;
    for (o = 0; o != 2 * r * odir; o += step * odir) {
        int ov = (odir > 0 ? -r : r) + o;
        for (n = 0; n != 2 * r * idir; n += step * idir) {
            int iv = (idir > 0 ? -r : r) + n;
            if (inner_is_x)
                fn_0001621c(iv, ov, a, b, c, d, e);
            else
                fn_0001621c(ov, iv, a, b, c, d, e);
        }
    }
}

/* 0x00015e4c -- draws the track tiles within `radius` tiles of the camera tile, ordered by the
 * view yaw octant (far to near), through fn_0001621c. */
void fn_00015e4c(int player, int pass_ground, int pass_walls, int pass_objects,
                 int pass_effects, int radius, int pass_shadow)
{
    int r = radius;
    int step = r < 0 ? -1 : (r > 0 ? 1 : 0);
    float yaw;
    unsigned ang;

    G_S32(0x3f930) = 0;
    G_U8(0x470d0) = 0;
    G_S32(0x46e98) = (int)((G_F32(0x470f4) + 406.0f) / 812.0f); /* camera tile x */
    G_S32(0x46e9c) = (int)((G_F32(0x470fc) + 406.0f) / 812.0f); /* camera tile z */
    yaw = fn_00038a20(VIEW_YAW(player));
    G_F32(0x3fac8) = (G_F32(0x3ea0c) * -5.0f + 255.0f) / 255.0f;
    G_F32(0x3facc) = (G_F32(0x3ea18) * -15.0f + 255.0f) / 255.0f;
    ang = (unsigned)(int)yaw;

#define SWEEP(od, id, ix)                                                                         \
    m15e4c_sweep(r, step, od, id, ix, pass_ground, pass_walls, pass_objects, pass_effects,      \
                 pass_shadow)
    if (ang < 45)
        SWEEP(1, 1, 1);
    else if (ang - 45 < 45)
        SWEEP(1, 1, 0);
    else if (ang - 90 < 45)
        SWEEP(1, -1, 0);
    else if (ang - 135 < 45)
        SWEEP(-1, 1, 1);
    else if (ang - 180 < 45)
        SWEEP(-1, -1, 1);
    else if (ang - 225 < 45)
        SWEEP(-1, -1, 0);
    else if (ang - 270 < 45)
        SWEEP(-1, 1, 0);
    else if (ang - 315 < 46)
        SWEEP(1, -1, 1);
#undef SWEEP
}

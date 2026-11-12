/* Module 0x00020f70..0x00028a58: race HUD / pause overlay, menus, bitmap-font text, touch
 * buttons, minimap. Functional translation of the iOS armv7 build (see docs/TRANSLATION.md).
 *
 * Calling conventions seen in this range: the game's internal draw helpers use VFP registers
 * for float arguments (fn_00023808 takes x/y/scale in s0..s3), while fn_0000526c / fn_00005354
 * (sprite quads) take everything in core registers. In C both are just float parameters.
 *
 * Sprite helpers (engine module):
 *   fn_0000526c(tex, x, y, w, h, z, u, v, du, dv, rot, cx, cy, flags)
 *   fn_00005354(tex, x, y, w, h, z, flags)          -- full-texture quad (u,v = 0, du,dv = 1)
 *   fn_00004fb4(r, g, b, a)                          -- glColor4f(x / 255)
 * Bitmap font (tex 0x97, 10x10 glyph grid): fn_00023808 below. */
#include "ios.h"

/* Shared text buffer (slot 0x3f358 -> 0x47e90, 256 bytes). */
#define M20_TXT ((char *)G(0x47e90))
/* Language string pointer stored in the image (the string table entries are char *). */
#define M20_STR(a) ((const char *)G_PTR(a))

/* copies an image string into the text buffer (kept out of line: both live in the image, which
 * makes gcc's -Wrestrict see a possible overlap) */
static __attribute__((noinline)) void m20_set_txt(const char *s)
{
    snprintf(M20_TXT, 0x100, "%s", s);
}

/* the same for the 0x20-byte title buffers (0x472a0 / 0x47520) */
static __attribute__((noinline)) void m20_title(char *dst, const char *a, const char *b)
{
    if (b)
        snprintf(dst, 0x20, "%s (%s)", a, b);
    else
        snprintf(dst, 0x20, "%s", a);
}

/* Touch-arrow pulse: scale factor and alpha derived from the press timers 0x3fa98..0x3faa4. */
#define M20_K(v) ((v) / 5.0f + 0.8f)
#define M20_INV(v) ((int)(255.0f / (v)))

/* red hint line, centred at (320, y) */
static void m20_red_text(const char *s, float y)
{
    snprintf(M20_TXT, 0x100, "%s", s);
    fn_00023808(M20_TXT, 320.0f, y, 0.8f, 0.8f, 0xff, 0, 0, 0xff, 0x34);
}

/* arrow sprite (tex 0x9b) centred at (cx, cy), size w x h, v0 = 0 (up/right) or 0.64 (down/left),
 * dv = strip height, rotated by `rot` around (px, py) */
static void m20_arrow(float cx, float cy, float w, float h, float v0, float dv, float rot,
                      float px, float py)
{
    /* 3DS: these arrows are the touch buttons of the menus (indices 3-6 of the 0x40388 table);
     * the port draws no on-screen buttons -- the D-pad presses them (game_main.c
     * feed_buttons). The original draws:
     *   fn_0000526c(0x9b, cx - w * 0.5f, cy - h * 0.5f, w, h, 0.95f, 0.0f, v0, 1.0f, dv, rot,
     *               px, py, 0); */
    (void)cx; (void)cy; (void)w; (void)h; (void)v0; (void)dv; (void)rot; (void)px; (void)py;
}

/* 0x00020f70 -- in-game frame overlay: replay / split-screen HUD, the title-screen and garage
 * menus (modes 4/5/10/11/12/0x14/0x15/0x17/0x18/0x19), the pause screen (the frame is grabbed
 * into texture 0x99 on pause and drawn darkened under "Game paused") (guess: Hud_DrawOverlay). */
void fn_00020f70(void)
{
    int paused, mode, cur, fa30;

    if ((fn_00004540() & 0xff) == 0 && !(G_U8(0x3f9e0) & 1)) {
        paused = 0;
    } else {
        paused = 1;
        if (G_U8(0x3f884) & 1)
            goto pause_screen;
    }

    if (G_U8(0x47250)) {
        /* full-screen splash (tex 0xa9) */
        fn_00004254(1);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_00004f24();
        fn_00005068(1);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_00005354(0xa9, 0.0f, 0.0f, (float)G_S32(0x40368), (float)G_S32(0x40374), 0.5f, 0);
        fn_00023bd4();
        goto done;
    }

    if (G_U8(0x3fab4)) {
        /* split-screen race: one 3D view + HUD per player (0x3e9b0 players) */
        int n = G_S32(0x3e9b0);
        int t;

        if (n >= 1) {
            fn_00004254(0);
            fn_00023fb0(0);
            fn_00028a58(0, 0);
            if (n >= 2) {
                fn_00023fb0(1);
                fn_00028a58(1, 1);
                if (n >= 3) {
                    fn_00023fb0(2);
                    fn_00028a58(2, 2);
                    fn_00023fb0(3);
                    fn_00028a58(3, 3);
                }
            }
        }
        fn_00005068(1);
        fn_00004f24();
        fn_00004e68(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        fn_00003f7c(0.0f, 0.0f, 1.0f, 1.0f);
        if (G_U8(0x3f928) & 1)
            goto menus_end;
        if (n == 1)
            fn_00028058(1, 520, 96, 7, -1);
        else if (n == 2)
            fn_00028058(1, 420, 200, 5, -1);
        else
            fn_00028058(1, 320, 220, n > 1 ? 5 : 7, -1);

        /* start countdown (race timer 0x3f8e4 from 201 to 399) */
        t = G_S32(0x3f8e4);
        if ((unsigned)(t - 201) <= 198) {
            float f8;
            int a;

            if (!(G_U8(0x3f928) & 1) && !(G_U8(0x3f92c) & 1) && t >= 241) {
                G_U8(0x3f92c) = 1;
                fn_0003613c(G_S32(0x3ebbc), 15000, 15000);
                fn_000360bc(G_S32(0x3ebbc));
                t = G_S32(0x3f8e4);
            }
            f8 = (float)((t % 50) * 2);
            a = (int)(f8 * -2.4f + 255.0f);
            if (t < 350) {
                int d;

                fn_00004fb4(0xff, 0xff, 0, a);
                d = ((400 - t) / 50) % 10;
                /* digit d of the 10-digit strip (tex 0x96) */
                fn_0000526c(0x96, 320.0f - f8 * 0.5f - 24.0f, 240.0f - f8 * 0.5f - 40.0f,
                            f8 + 48.0f, f8 + 80.0f, 0.95f, (float)d * 0.1f, 0.0f, 0.1f, 1.0f,
                            0.0f, 0.0f, 0.0f, 4);
            } else {
                float s;

                snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47d3c));
                s = (f8 + 16.0f) * 0.0625f;
                fn_00023808(M20_TXT, 320.0f, 240.0f, s, s, 0xff, 0xff, 0, a, 0x14);
            }
        }
        if (G_U8(0x3f98c) && G_S32(0x3fa30) >= 0x81 && G_S32(0x3f8c8) == 0) {
            snprintf(M20_TXT, 0x100, "R");
            fn_00023808(M20_TXT, 32.0f, 32.0f, 2.0f, 2.0f, 0xff, 0xff, 0, 0xff, 4);
        }
        goto menus_end;
    }

    if (!G_U8(0x3fab8))
        goto after_menus;

    /* single view (title screen / garage / race) */
    fn_00004254(0);
    fn_00023fb0(0);
    {
        uint8_t *car0 = (uint8_t *)G(0x48ae0);

        AT_F32(car0, 0x244) = 45.0f;
        AT_S32(car0, 0x30) = -2;
    }
    fn_00004e68(0.0f, 0.0f, 0.0f, 1.0f, (float)(int)(G_F32(0x5c1a8) / G_F32(0x5c1b8)), 1.0f);

    if (G_U8(0x3fabc) && !(G_U8(0x3f928) & 1)) {
        float f, nf;

        /* track-record line: "<title>" / "- <name> -" for the record kind 0x3eb3c */
        if (G_S32(0x3fac4) != 0 && G_S32(0x3eb3c) >= 0 && G_S32(0x3fa30) >= 0x81) {
            char tmp[256];
            int m = G_S32(0x3eb3c), i = G_S32(0x3eb40);

            tmp[0] = 0; /* left uninitialised by the original for m > 2 */
            if (m == 0) {
                snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47d94));
                snprintf(tmp, 0x100, "- %s -", (const char *)G(0x47900) + i * 16);
            } else if (m == 1) {
                snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47d8c));
                snprintf(tmp, 0x100, "- %s -", (const char *)G(0x47300) + i * 16);
            } else if (m == 2) {
                snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47d90));
                snprintf(tmp, 0x100, "- %s -", (const char *)G(0x47180) + i * 4);
            }
            fn_00023808(M20_TXT, 320.0f, 230.0f, 0.8f, 0.8f, 0xff, 0, 0, 0xff, 0x34);
            fn_00023808(tmp, 320.0f, 246.0f, 0.8f, 0.8f, 0xff, 0xff, 0xff, 0xff, 0x34);
        }

        if (G_U8(0x3f90c) & 1) {
            /* title screen: four touch arrows around the screen centre */
            int big = G_U8(0x42804) != 0;
            float A = big ? 74.0f : 64.0f, B = big ? 64.0f : 70.0f;
            float wc = (float)G_S32(0x4036c);
            int wi = G_S32(0x4036c);
            float k;

            fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa0)));
            k = M20_K(G_F32(0x3faa0));
            m20_arrow(wc, 132.0f, B * k, A * k, 0.0f, 0.366f, 0.0f, 0.0f, 0.0f);
            fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa4)));
            k = M20_K(G_F32(0x3faa4));
            m20_arrow(wc, 340.0f, B * k, A * k, 0.64f, 0.366f, 0.0f, 0.0f, 0.0f);
            fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3fa98)));
            k = M20_K(G_F32(0x3fa98));
            m20_arrow((float)(wi - 132), 234.0f, B * k, A * k, 0.64f, 0.366f, 90.0f,
                      (float)(wi - 128), 234.0f);
            fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3fa9c)));
            k = M20_K(G_F32(0x3fa9c));
            m20_arrow((float)(wi + 132), 234.0f, B * k, A * k, 0.0f, 0.366f, 90.0f,
                      (float)(wi + 128), 234.0f);
            fn_00027d90(0, M20_STR(0x47d70));
            fn_00027ef4(1, M20_STR(0x47e48));
            fn_00023ad0(2, 7, M20_STR(0x47cf4));
            fn_00023ad0(3, 8, M20_STR(0x47cfc));
        } else {
            /* race standings box */
            uint32_t c = G_U32(0x3eb7c);
            int cur0 = G_S32(0x3f9dc);
            int n = G_S32(0x3e9c8), i;

            snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47ce8));
            fn_00023808(M20_TXT, 520.0f, 110.0f, 1.0f, 1.0f, 0xff, 0x1e, 0x1e, 0xff, 0x14);
            fn_00004fb4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24);
            fn_0000526c(0x9a, 459.0f, 134.0f, 146.0f, 4.0f, 0.95f, 0.05f, 0.0f, 1.0f, 1.0f, 0.0f,
                        0.0f, 0.0f, 4);
            if (!(cur0 == G_S32(0x3ebe8) || cur0 == G_S32(0x3f9d8)))
                n = 0;
            if (cur0 == G_S32(0x3ebe0) || cur0 == G_S32(0x3ebec) || cur0 == G_S32(0x3ebe4))
                n = G_S32(0x3e9ac);
            for (i = 0; i < n; i++) {
                int id = ((const int32_t *)G(0x47060))[i];
                uint8_t col[4];
                uint8_t *car;
                int dy, v, x;

                fn_00011018((int)(uintptr_t)col,
                            AT_S32((uint8_t *)G(0x47544) + id * 24, 4));
                fn_00004fb4(col[0], col[1], col[2], col[3]);
                dy = (40 - (G_S32(0x3e9c8) * 3) / 2) * i;
                fn_0000526c(0x9a, 470.0f, (float)(dy + 150), 106.0f, 14.0f, 0.95f, 0.1f, 0.0f,
                            1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
                car = (uint8_t *)G(0x48ae0) + id * 0x678;
                m20_set_txt((const char *)car + 0xc);
                fn_00023808(M20_TXT, 470.0f, (float)(dy + 142), 1.0f, 1.0f, 0xff, 0xff, 0xff,
                            0xff, 4);
                v = AT_S32(car, 0xa0);
                snprintf(M20_TXT, 0x100, "%d", v);
                x = v > 9 ? 0x225 : 0x235;
                if (v > 99)
                    x -= 16;
                if (v >= 1000)
                    x -= 16;
                fn_00023808(M20_TXT, (float)x, (float)(dy + 142), 1.0f, 1.0f, 0xff, 0xff, 0xff,
                            0xff, 4);
            }
            if (G_U8(0x3f900) & 1)
                fn_00023ad0(3, 8, M20_STR(0x47d00));
            if (G_U8(0x3f890) & 1)
                fn_00023ad0(2, 7, M20_STR(0x47e4c));
        }

        /* animated track preview on the minimap: waypoint cursor 0x5c170 cycles 1..0x3f948 */
        f = G_F32(0x5c170) + 1.5f;
        G_F32(0x5c170) = f;
        nf = (float)G_S32(0x3f948);
        if (f > nf) {
            G_F32(0x5c170) = 1.0f;
            f = 1.0f;
        } else if (f < 0.0f) {
            G_F32(0x5c170) = nf;
            f = nf;
        }
        if (!G_U8(0x3fac0) && ((const int32_t *)G(0x481f4))[G_S32(0x3f8b0)] != 0)
            fn_00028058(0, 100, 200, 7, (int)f);
    }

    mode = G_S32(0x3fa24);
    cur = G_S32(0x3f9dc);
    fa30 = G_S32(0x3fa30); /* blink counter: > 0x80 = "on" half */
    if ((unsigned)(mode - 4) >= 2) {
        /* screen title + subtitle */
        m20_set_txt((const char *)G(0x472a0));
        fn_00023808(M20_TXT, 64.0f, 45.0f, 1.0f, 1.0f, 0xff, 0x1e, 0x1e, 0xff, 4);
        m20_set_txt((const char *)G(0x47520));
        fn_00023808(M20_TXT, 64.0f, 70.0f, 0.8f, 0.8f, 0xff, 0xff, 0xff, 0xff, 4);
        if ((G_U8(0x3f90c) & 1) && ((const int32_t *)G(0x481f4))[G_S32(0x3f8b0)] == 0 &&
            fa30 >= 0x81)
            m20_red_text(M20_STR(0x47d78), 230.0f); /* locked */
    }
    if (mode == 5) {
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        if (G_S32(0x3f8c4) != 0) {
            fn_00023ad0(2, 6, M20_STR(0x47d08));
            fn_00023ad0(3, 8, M20_STR(0x47d10));
        } else {
            fn_00023ad0(3, 8, M20_STR(0x47d00));
        }
    }
    if (mode == 0x17) {
        if (((const int32_t *)G(0x481c8))[G_S32(0x3f9f8)] == 0 && fa30 >= 0x81)
            m20_red_text(M20_STR(0x47d78), 230.0f);
    } else if (mode == 0x14) {
        if (((const int32_t *)G(0x4819c))[G_S32(0x3fa00)] == 0) {
            if (fa30 >= 0x81)
                m20_red_text(M20_STR(0x47d78), 230.0f);
        } else if (cur == G_S32(0x3ebe4) && G_S32(0x3fa00) == 10 && fa30 >= 0x81) {
            m20_red_text(M20_STR(0x47da0), 230.0f);
            m20_red_text(M20_STR(0x47e5c), 250.0f);
        }
    }
    if ((unsigned)(mode - 0x14) < 2 || mode == 0x17 || mode == 0x18) {
        /* player number digit (tex 0x96 strip), blinking */
        int d = (G_S32(0x3fa08) + 1) % 10;

        fn_00004fb4(0xff, 0xff, 0xff, G_U8(0x3fa30));
        fn_0000526c(0x96, 64.0f, 95.0f, 48.0f, 80.0f, 0.95f, (float)d * 0.1f, 0.0f, 0.1f, 1.0f,
                    0.0f, 0.0f, 0.0f, 4);
    }

    if ((unsigned)(mode - 10) < 2 || mode == 0x14 || mode == 0x17 || mode == 0xc ||
        mode == 0x19) {
        /* car / track selection: left/right arrows at the bottom */
        int wi = G_S32(0x4036c), hi = G_S32(0x40378);
        float k;

        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3fa98)));
        k = M20_K(G_F32(0x3fa98));
        m20_arrow((float)(wi - 192), (float)(hi - 8), 70.0f * k, 64.0f * k, 0.64f, 0.366f, 90.0f,
                  (float)(wi - 192), (float)(hi - 8));
        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3fa9c)));
        k = M20_K(G_F32(0x3fa9c));
        m20_arrow((float)(wi + 192), (float)(hi - 8), 70.0f * k, 64.0f * k, 0.0f, 0.366f, 90.0f,
                  (float)(wi + 192), (float)(hi - 8));
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        if (mode == 0x14 && cur == G_S32(0x3ebe4)) {
            /* up/down arrows (the original pairs the alpha of one timer with the scale of the
             * other) */
            float s;

            fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa4)));
            s = 70.0f * M20_K(G_F32(0x3faa0));
            m20_arrow((float)(wi + 64), (float)(hi - 115), s, s, 0.0f, 0.364f, 0.0f, 0.0f, 0.0f);
            fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa0)));
            s = 70.0f * M20_K(G_F32(0x3faa4));
            m20_arrow((float)(wi + 64), (float)(hi + 115), s, s, 0.64f, 0.364f, 0.0f, 0.0f, 0.0f);
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
            fn_00027ef4(1, M20_STR(0x47e3c));
        }
        fn_00027d90(2, M20_STR(0x47d6c));
        fn_00023ad0(3, 8, M20_STR(0x47cfc));
        if (G_S32(0x3f8c4) != 0 && mode == 10)
            fn_00023ad0(1, 6, M20_STR(0x47d08));
        if (mode != 10) {
            fn_0002877c();
            if (mode == 0x14) {
                /* car stats: name, then 3 bars (red -> yellow) from the table at 0x3ea28 */
                int sel = G_S32(0x3fa00);
                int j, y, r, g;

                m20_set_txt((const char *)G(0x47300) + sel * 16);
                fn_00023808(M20_TXT, 222.0f, 104.0f, 0.8f, 0.8f, 0xff, 0xff, 0xff, 0xff, 4);
                fn_00004fb4(0xff, 0xff, 0xff, 0xff);
                fn_0000526c(0x9a, 221.0f, 122.0f, 192.0f, 3.0f, 0.95f, 0.1f, 0.0f, 1.0f, 1.0f,
                            0.0f, 0.0f, 0.0f, 4);
                for (j = 0, y = 128, r = 0xff, g = 0; y != 200; j++, y += 24, g += 50, r -= 50) {
                    float v = G_F32(0x3ea28 + (G_S32(0x3fa00) * 3 + j) * 4);

                    fn_00004fb4(r & 0xff, g & 0xff, 0, 0xff);
                    fn_0000526c(0x9a, 221.0f, (float)(y + 10), (v - 0.7f) * 250.0f + 50.0f, 10.0f,
                                0.95f, 0.1f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
                    if (j == 0) {
                        int s0 = G_S32(0x3fa00);

                        snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47e18));
                        fn_00023808(M20_TXT, 222.0f, (float)y, 0.5f, 0.8f, 0xff, 0xff, 0xff, 0xff,
                                    4);
                        if ((cur == G_S32(0x3ebec) && (unsigned)s0 <= 9 &&
                             ((1u << s0) & 0x310u)) ||
                            (cur == G_S32(0x3ebe4) &&
                             AT_S32((uint8_t *)G(0x47544) + G_S32(0x3fa08) * 24, 0xc) != 0)) {
                            fn_00004fb4(200, 200, 0xff, 0xff);
                            fn_0000526c(0xac, 304.0f, (float)(y + 2), 16.0f, 16.0f, 0.95f, 0.0f,
                                        0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
                        }
                    } else if (j == 1) {
                        snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47d40));
                        fn_00023808(M20_TXT, 222.0f, (float)y, 0.5f, 0.8f, 0xff, 0xff, 0xff, 0xff,
                                    4);
                    } else if (j == 2) {
                        snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47d48));
                        fn_00023808(M20_TXT, 222.0f, (float)y, 0.5f, 0.8f, 0xff, 0xff, 0xff, 0xff,
                                    4);
                    }
                }
                mode = G_S32(0x3fa24);
            }
            if (mode == 0xb && G_S32(0x3f8c4) != 0 && G_S32(0x3fa30) >= 0x81 &&
                G_S32(0x3faa8) >= 1)
                m20_red_text(M20_STR(0x47d78), 230.0f);
        }
        if (mode == 10 && G_S32(0x3f8c4) != 0 && G_S32(0x3fa30) >= 0x81 &&
            (unsigned)G_S32(0x3faa8) <= 3 && ((1u << G_S32(0x3faa8)) & 0xdu))
            m20_red_text(M20_STR(0x47d78), 230.0f);
    } else if (mode == 0x15) {
        /* colour picker: 10 swatches of the 125-colour palette, the middle one is selected */
        int w = G_S32(0x40368);
        uint8_t *ent = (uint8_t *)G(0x47544) + G_S32(0x3fa08) * 24;
        int sl, y, v;
        float k, x;

        for (sl = 0, y = 165;; y += 14) {
            uint8_t col[4];

            v = (sl - G_S32(0x3fa04)) % 125;
            if (v < 1)
                v += 125;
            if (y == 221)
                AT_S32(ent, 4) = v;
            sl++;
            fn_00011018((int)(uintptr_t)col, v);
            fn_00004fb4(col[0], col[1], col[2], col[3]);
            fn_0000526c(0x9c, (float)(w - 148), (float)y, 28.0f, 14.0f, 0.95f, 0.0f, 0.0f, 1.0f,
                        1.0f, 0.0f, 0.0f, 0.0f, 0);
            if (y == 291)
                break;
        }
        /* player car colour (car 0 RGBA at +0x394) */
        fn_00011018((int)(uintptr_t)((uint8_t *)G(0x48ae0) + 0x394), AT_S32(ent, 4));
        fn_00003f7c(0.0f, 0.0f, 1.0f, 1.0f);
        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa0)));
        k = M20_K(G_F32(0x3faa0));
        m20_arrow((float)(w - 134), 112.0f, 70.0f * k, 64.0f * k, 0.0f, 0.364f, 0.0f, 0.0f, 0.0f);
        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa4)));
        k = M20_K(G_F32(0x3faa4));
        m20_arrow((float)(w - 134), 355.0f, 70.0f * k, 64.0f * k, 0.64f, 0.364f, 0.0f, 0.0f, 0.0f);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        /* swatch frame (top, middle, bottom) and the selection marker */
        x = (float)(w - 169);
        fn_0000526c(0x9b, x, 148.0f, 70.0f, 30.0f, 0.95f, 0.0f, 0.365f, 1.0f, 0.144f, 0.0f, 0.0f,
                    0.0f, 0);
        fn_0000526c(0x9b, x, 160.0f, 70.0f, 130.0f, 0.95f, 0.0f, 0.44f, 1.0f, 0.12f, 0.0f, 0.0f,
                    0.0f, 0);
        fn_0000526c(0x9b, x, 290.0f, 70.0f, 30.0f, 0.95f, 0.0f, 0.5f, 1.0f, 0.144f, 0.0f, 0.0f,
                    0.0f, 0);
        fn_00004fb4(0xff, 200, 0, 0xff);
        x = (float)(w - 171);
        fn_0000526c(0x9b, x, 218.0f, 74.0f, 20.0f, 0.95f, 0.0f, 0.421f, 1.0f, 0.18f, 0.0f, 0.0f,
                    0.0f, 0);
        fn_0000526c(0x9b, x, 220.0f, 74.0f, 20.0f, 0.95f, 0.0f, 0.421f, 1.0f, 0.18f, 0.0f, 0.0f,
                    0.0f, 0);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_00027ef4(2, M20_STR(0x47e44));
        fn_00023ad0(3, 8, M20_STR(0x47cfc));
        fn_0002877c();
    }

    if (mode == 0x18) {
        /* 3-letter name entry: the name (car table entry +0x14) big in the background, then the
         * letters with a rolling wheel on the selected one */
        char *name = (char *)G(0x47544) + G_S32(0x3fa08) * 24 + 0x14;
        int i, big;
        float A, B, k, cx0;
        int wi;

        name[G_S32(0x3f9ec)] = (char)(G_S32(0x3f9f0) + 0x20);
        m20_set_txt(name);
        fn_00023808(M20_TXT, 320.0f, 330.0f, 6.0f, 6.0f, 200, 200, 0xff, 50, 0x34);
        snprintf(M20_TXT, 0x100, "x");
        for (i = 0; i < 3; i++) {
            int x = 256 + 32 * i;

            if (i == G_S32(0x3f9ec)) {
                int c = (signed char)((char *)G(0x47544) + G_S32(0x3fa08) * 24 + 0x14)[i];
                static const int order[5] = {-2, -1, 0, 2, 1};
                int o;

                for (o = 0; o < 5; o++) {
                    int d = order[o];
                    int ad = (int)fabsf((float)d);
                    int ch = c + d;

                    if (ch > 90)
                        ch -= 59;
                    if (ch < 32)
                        ch += 59;
                    if (ch == 0x25)
                        ch = 0x20;
                    M20_TXT[0] = (char)ch;
                    if (d != 0) {
                        float sc = 8.0f / (float)(ad + 4);
                        int y = (d * 256) / (ad + 8) + (d < 0 ? 250 : 252);

                        fn_00023808(M20_TXT, (float)(x + 32), (float)y, sc, sc, 0xff, 0xff, 0xff,
                                    (ad * -70 + 0xff) & 0xff, 0x34);
                    } else {
                        fn_00023808(M20_TXT, (float)(x + 32), 260.0f, 2.0f, 2.0f, 0xff,
                                    G_U8(0x3fa30), G_U8(0x3fa30), 0xff, 0x34);
                    }
                }
                if (G_S32(0x3fa30) % 8 <= 3) {
                    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
                    fn_0000526c(0x9b, (float)x, 225.0f, 64.0f, 40.0f, 0.95f, 0.0f, 0.421f, 1.0f,
                                0.18f, 0.0f, 0.0f, 0.0f, 0xc);
                }
            } else {
                M20_TXT[0] = name[i];
                fn_00023808(M20_TXT, (float)(x + 32), 260.0f, 2.0f, 2.0f, 0xff, 0xff, 0xff, 0xff,
                            0x34);
            }
        }
        big = G_U8(0x42804) != 0;
        A = big ? 74.0f : 64.0f;
        B = big ? 74.0f : 70.0f;
        wi = G_S32(0x4036c);
        cx0 = (float)(wi + G_S32(0x3f9ec) * 32 - 32);
        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa0)));
        k = M20_K(G_F32(0x3faa0));
        m20_arrow(cx0, 127.0f, B * k, A * k, 0.0f, 0.366f, 0.0f, 0.0f, 0.0f);
        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3faa4)));
        k = M20_K(G_F32(0x3faa4));
        m20_arrow(cx0, 365.0f, B * k, A * k, 0.64f, 0.366f, 0.0f, 0.0f, 0.0f);
        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3fa98)));
        k = M20_K(G_F32(0x3fa98));
        m20_arrow((float)(wi - 160), 244.0f, B * k, A * k, 0.64f, 0.366f, 90.0f,
                  (float)(wi - 160), 244.0f);
        fn_00004fb4(0xff, 0xff, 0xff, M20_INV(G_F32(0x3fa9c)));
        k = M20_K(G_F32(0x3fa9c));
        m20_arrow((float)(wi + 160), 244.0f, B * k, A * k, 0.0f, 0.366f, 90.0f,
                  (float)(wi + 160), 244.0f);
        fn_00027ef4(1, M20_STR(0x47e40));
        fn_00027d90(2, M20_STR(0x47d68));
        fn_00023ad0(3, 8, M20_STR(0x47cfc));
        fn_0002877c();
    }

menus_end:
    fn_00027b44();
    fn_00023eac();
    fn_00023bd4();

after_menus:
    if (paused == 1 && !(G_U8(0x3f884) & 1)) {
        /* grab the frame into the pause texture */
        int w, h;
        float a, b;

        G_U8(0x3f884) = 1;
        a = fn_00004810((float)G_S32(0x3f854));
        b = fn_00004810((float)G_S32(0x3f858));
        glBindTexture(GL_TEXTURE_2D, G_U32(0x44660));
        glEnable(GL_TEXTURE_2D);
        w = (int)a;
        if (w == 0)
            w = 1;
        h = (int)b;
        if (h == 0)
            h = 1;
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, w, h, 0);
    }
    goto done;

pause_screen:
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    /* texture-table entry of tex 0x99 (0x20 bytes each) <- grabbed-frame texture record */
    memcpy((uint8_t *)G(0x4060c) + 0x99 * 0x20, G(0x44660), 0x20);
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    fn_00005068(1);
    fn_00004f24();
    fn_00004e68(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    fn_00003f7c(0.0f, 0.0f, 1.0f, 1.0f);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    fn_00005354(0x99, 0.0f, 0.0f, (float)G_S32(0x40368), (float)G_S32(0x40374), 0.95f, 2);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    if (G_U8(0x3f9e0) & 1) {
        G_S32(0x3f8d0) = 310;
        fn_00004fb4(0, 0, 0, 150);
        fn_00005354(0xa8, 0.0f, 0.0f, (float)G_S32(0x40368), (float)G_S32(0x40374), 0.01f, 0);
        if (G_U8(0x3f810) & 1)
            snprintf(M20_TXT, 0x100, "%s", "Game paused");
        else if (!G_U8(0x3f98c))
            snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47cd4));
        else
            snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47dc8));
        fn_00023808(M20_TXT, 320.0f, 240.0f, 1.0f, 1.0f, 0xff, 0x1e, 0x1e, 0xff, 0x34);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        fn_00023ad0(3, 8, M20_STR(0x47d00));
        if (!(G_U8(0x3f810) & 1))
            fn_00023ad0(2, 7, M20_STR(0x47cfc));
        fn_00023eac();
        fn_00023bd4();
    }

done:
    fn_000045a0();
}

/* 0x00023808 -- draws a string with the bitmap font, with a 2-sample black shadow (guess:
 * Font_DrawString). flags: 0x10 = centre horizontally, 0x20 = centre vertically; the whole
 * flags word is also passed on to the sprite call (4 = virtual-screen scaling). */
void fn_00023808(const char *s, float x, float y, float sx, float sy, int r, int g, int b, int a,
                 int flags)
{
    float cw = sx * 16.0f;
    float ch = sy * 16.0f;
    float rf, gf, bf, af, adv, ybase, chl;
    int c;

    if (flags & 0x10) {
        size_t len = strlen(s);
        x = (float)((double)x + (double)((cw + 1.0f) * (float)len) * -0.5);
    }
    if (flags & 0x20)
        y = (float)((double)y + (double)(ch + 1.0f) * -0.5);
    fn_00004fb4(0, 0, 0, (int)((((uint32_t)a << 7) / 255u) & 0xff));

    c = (signed char)*s;
    if (c == 0)
        return;
    bf = (float)b / 255.0f;
    adv = cw + 1.0f;
    gf = (float)g / 255.0f;
    af = (float)a / 255.0f;
    rf = (float)r / 255.0f;
    ybase = y + 16.0f;
    chl = ch * 0.8f; /* lower-case glyphs are drawn at 80 % height */
    s++;
    do {
        int idx, row, col;
        float h = ch, u, v, yb;

        glColor4f(0.0f, 0.0f, 0.0f, af);
        idx = c - 0x20;
        if (idx > 0x40)
            idx = c - 0x40;
        row = idx / 10;
        col = idx % 10;
        if (c - 0x20 > 0x40)
            h = chl;
        v = (float)((double)(float)row * 0.1001);
        u = (float)col * 0.1001f;
        yb = ybase - h;
        fn_0000526c(0x97, x - 1.0f, yb - 1.0f, cw, h, 0.95f, u, v, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f,
                    flags);
        fn_0000526c(0x97, x + 1.0f, yb + 1.0f, cw, h, 0.95f, u, v, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f,
                    flags);
        glColor4f(rf, gf, bf, af);
        fn_0000526c(0x97, x, yb, cw, h, 0.95f, u, v, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f, flags);
        x += adv;
        c = (signed char)*s++;
    } while (c != 0);
}

/* 0x00023ad0 -- menu row: icon `icon` of the 11-icon strip (tex 0xaa) plus a label, row `row`
 * below the menu origin (guess: Menu_DrawIconRow). Hidden until the menu timer reaches 300. */
void fn_00023ad0(int row, int icon, const char *text)
{
    int base;

    if (G_S32(0x3f8d0) < 300)
        return;
    base = G_S32(0x3f868) + row * 30;
    fn_0000526c(0xaa, 42.0f, (float)(base - 160), 32.0f, 32.0f, 0.95f, (float)icon * 0.0909f, 0.0f,
                0.0909f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
    snprintf(M20_TXT, 0x100, "%s", text);
    fn_00023808(M20_TXT, 75.0f, (float)(base - 152), 0.8f, 0.8f, 0xff, 0xff, 0xff, 0xff, 4);
}

/* 0x00023bd4 -- draws the 20 on-screen touch buttons (table at 0x40388, 0x20 bytes each)
 * (guess: Buttons_Draw). Button 12 is the one with the pulsing highlight (counter 0x3fad0). */
void fn_00023bd4(void)
{
    uint8_t *tab = (uint8_t *)G(0x40388);
    int i;

    for (i = 0; i < 20; i++) {
        uint8_t *bt = tab + i * 0x20;
        float r;

        if (AT_U8(bt, 0x16) == 0 || AT_U8(bt, 0x18) == 0)
            continue;
        r = AT_F32(bt, 8);
        fn_00004fb4(0xff, 0xff, 0xff, 100);
        if (AT_U8(bt, 0x14) == 0 && AT_U8(bt, 0x15) == 0 &&
            !(i == 12 && G_S32(0x3fad0) >= 1 && G_S32(0x3f8d0) >= 300)) {
            /* idle */
            if (i == 12)
                continue;
            fn_00023e20((int)AT_F32(bt, 0), (int)AT_F32(bt, 4), AT_S32(bt, 0x1c), r + r);
            continue;
        }
        if (i != 12) {
            /* pressed */
            fn_00023e20((int)AT_F32(bt, 0), (int)AT_F32(bt, 4), AT_S32(bt, 0x1c),
                        (float)((double)r * 2.4));
            continue;
        }
        {
            int v = G_S32(0x3fad0);
            float px, py, off;

            if (v >= 1) {
                v--;
                G_S32(0x3fad0) = v;
            }
            if (G_U8(0x3f9e0) & 1)
                continue;
            fn_00004fb4(0xff, 0xff, 0xff, v & 0xff);
            fn_00005354(0x98, AT_F32(bt, 0) - r, AT_F32(bt, 4) - r, r + r, r + r, 0.01f, 0);
            fn_00004fb4(0xff, 0xff, 0xff, 100);
            if (AT_U8(bt, 0x14) == 0 && AT_U8(bt, 0x15) == 0)
                continue;
            G_S32(0x3fad0) = 100;
            px = AT_F32(bt, 0) + AT_F32(bt, 0xc) * r;
            off = (G_U8(0x3e9a0) & 1) ? -48.0f : -64.0f;
            py = AT_F32(bt, 4) + AT_F32(bt, 0x10) * r;
            fn_00005354(0x98, px + off, py + off, 96.0f, 96.0f, 0.01f, 0);
        }
    }
}

/* 0x00023e20 -- draws icon `icon` of the 11-icon strip (tex 0xaa) centred on (x + 2, y)
 * (guess: Button_DrawIcon). */
void fn_00023e20(int x, int y, int icon, float size)
{
    float half = size * 0.5f;

    fn_0000526c(0xaa, (float)(x + 2) - half, (float)y - half, size, size, 0.95f,
                (float)icon * 0.0909f, 0.0f, 0.0909f, 1.0f, 0.0f, 0.0f, 0.0f, 0);
}

/* 0x00023eac -- full-screen black fade (alpha 0x3f8f0) and the blinking "DEMO" caption
 * (guess: Overlay_DrawFadeAndDemo). */
void fn_00023eac(void)
{
    if (G_S32(0x3f8f0) >= 1) {
        fn_00004fb4(0, 0, 0, G_S32(0x3f8f0) & 0xff);
        fn_00005354(0xa8, 0.0f, 0.0f, (float)G_S32(0x40368), (float)G_S32(0x40374), 0.01f, 0);
    }
    if (G_S32(0x3f8c4) != 0 && G_U8(0x3fab8) != 0 && G_S32(0x3fa30) > 0x80) {
        snprintf(M20_TXT, 0x100, "DEMO");
        fn_00023808(M20_TXT, (float)(G_S32(0x3f864) - 64), 400.0f, 0.8f, 0.8f, 0xff, 0, 0, 0xff,
                    0x34);
    }
}

#define M20_DEG 0.0174533f
#define M20_RAD2DEG 57.2958f
#define M20_P(p) ((int)(uintptr_t)(p))

/* track tile byte from the 20-wide grid at `base` (0x44000 heights, 0x44800 tile types), row
 * clamped to [0, G_S32(0x3f8a4)], column to [0, G_S32(0x3f8a0)] */
static int m20_tile(uint32_t base, int row, int col)
{
    if (row < 0)
        row = 0;
    else if (row > G_S32(0x3f8a4))
        row = G_S32(0x3f8a4);
    if (col < 0)
        col = 0;
    else if (col > G_S32(0x3f8a0))
        col = G_S32(0x3f8a0);
    return G_U8(base + row * 20 + col);
}

/* showcase model (0x3f99c) for the selected track 0x3f9dc */
static void m20_track_icon(void)
{
    int cur = G_S32(0x3f9dc);

    if (cur == G_S32(0x3f9d8))
        G_S32(0x3f99c) = 0x79;
    else if (cur == G_S32(0x3ebe8))
        G_S32(0x3f99c) = 0x76;
    else if (cur == G_S32(0x3ebe4))
        G_S32(0x3f99c) = 0x77;
    else if (cur == G_S32(0x3ebe0))
        G_S32(0x3f99c) = 0x7b;
    else if (cur == G_S32(0x3ebec))
        G_S32(0x3f99c) = 0x74;
}

/* draws model 0x7f (`m`) as a weather sprite record `e` (pos 0x00, rot 0x18, scale 0x24, rgba
 * 0x40) */
static void m20_draw_sprite(uint8_t *m, uint8_t *e)
{
    AT_F32(m, 0x288) = AT_F32(e, 0x24);
    AT_F32(m, 0x298) = AT_F32(e, 0x28);
    AT_F32(m, 0x2a8) = AT_F32(e, 0x2c);
    memcpy(m + 0x270, e, 12);
    memcpy(m + 0x27c, e + 0x18, 12);
    fn_00004fb4(AT_U8(e, 0x40), AT_U8(e, 0x41), AT_U8(e, 0x42), AT_U8(e, 0x43));
    fn_00038a98(0x7f, -1, -1, 0, 0, 2);
}

/* 0x00023fb0 -- renders the 3D view of viewport `v` (record 0x5c160 + v * 0x80): camera
 * placement for the camera modes, sky / ground / sun, track, cars, effects and the race HUD
 * (guess: View_Render3D). */
void fn_00023fb0(int v)
{
    uint8_t *vp = (uint8_t *)G(0x5c160) + v * 0x80;
    uint8_t *vw = (uint8_t *)G(0x5c160) + v * 4; /* viewport rectangle */
    int tcar = AT_S32(vp, 0x410); /* car followed by this viewport */
    int wide, dist, sx_off, r, g, b, a, sky3d;
    float wf, tilt, skyh, scr0[2], scr1[2], p[3], q[3], dir[3], hz[3], rot[3];

    G_S32(0x3f944) = 0;
    wide = G_F32(0x3ea0c) > 5.0f && G_S32(0x3e9b0) <= 2;

    /* projection */
    if (G_U8(0x3fab8)) {
        fn_00037c78((float *)G(0x42878), AT_F32(vp, 0x418),
                    (float)G_S32(0x40368) / (float)G_S32(0x40374), 10.0f, 1000.0f);
    } else if (G_S32(0x3e9b0) == 2) {
        fn_00037c78((float *)G(0x42878), AT_F32(vp, 0x418) * 0.5f + 10.0f,
                    (float)G_S32(0x40368) / (float)G_S32(0x40378), 10.0f, G_F32(0x3e9c4));
    } else {
        fn_00037c78((float *)G(0x42878), AT_F32(vp, 0x418),
                    (float)G_S32(0x40368) / (float)G_S32(0x40374), 10.0f, G_F32(0x3e9c4));
    }

    /* camera look-at point (vp+0x3b8) for the camera mode vp+0x408 */
    dist = AT_S32(vp, 0x408) == 5 ? 60 : 65;
    if (G_U8(0x3fab8)) {
        uint8_t *car = (uint8_t *)G(0x48ae0) + tcar * 0x678;

        AT_F32(vp, 0x3b8) = AT_F32(car, 0x2bc);
        AT_F32(vp, 0x3bc) = AT_F32(car, 0x2c0) + (float)(int)((unsigned)(dist * 2) / 3u);
        AT_F32(vp, 0x3c0) = AT_F32(car, 0x2c4);
    } else if (AT_S32(vp, 0x408) == 8) {
        /* orbit camera: angle vp+0x3f4 */
        uint8_t *car = (uint8_t *)G(0x48ae0) + tcar * 0x678;
        float an = AT_F32(vp, 0x3f4) * M20_DEG;
        float c = cosf(an), s = sinf(an);

        AT_F32(vp, 0x3b8) = AT_F32(car, 0x2bc) - s * 250.0f;
        AT_F32(vp, 0x3bc) = AT_F32(car, 0x2c0) + (float)dist;
        AT_F32(vp, 0x3c0) = AT_F32(car, 0x2c4) - c * 250.0f;
    } else if (AT_S32(vp, 0x408) == 0xc) {
        /* look ahead of the car, smoothed height */
        uint8_t *car = (uint8_t *)G(0x48ae0) + AT_S32(vp, 0x410) * 0x678;
        float an = AT_F32(car, 0x244) * M20_DEG;
        float c = cosf(an), s = sinf(an);
        float ty = AT_F32(car, 0x2c0) + -12.0f + AT_F32(car, 0x298) * -10.0f;

        AT_F32(vp, 0x3b8) = AT_F32(car, 0x2bc) + s * 450.0f;
        AT_F32(vp, 0x3bc) = AT_F32(vp, 0x3bc) + (ty - AT_F32(vp, 0x3bc)) * 0.5f;
        AT_F32(vp, 0x3c0) = AT_F32(car, 0x2c4) + c * 450.0f;
    } else {
        uint8_t *car = (uint8_t *)G(0x48ae0) + tcar * 0x678;

        AT_F32(vp, 0x3b8) = AT_F32(car, 0x2bc);
        AT_F32(vp, 0x3bc) = AT_F32(car, 0x2c0) + (float)dist;
        AT_F32(vp, 0x3c0) = AT_F32(car, 0x2c4);
    }

    /* camera angles from eye (vp+0x3c4) to look-at */
    dir[0] = -AT_F32(vp, 0x3c4);
    dir[1] = -AT_F32(vp, 0x3c8);
    dir[2] = -AT_F32(vp, 0x3cc);
    fn_00037dec((float *)(vp + 0x3b8), dir, dir);
    hz[0] = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    hz[1] = 0.0f;
    hz[2] = dir[1];
    fn_000379a8(hz, hz);
    fn_000379a8(dir, dir);
    AT_F32(vp, 0x3b0) = atan2f(dir[0], dir[2]) * M20_RAD2DEG; /* yaw */
    AT_F32(vp, 0x3ac) = atan2f(hz[2], hz[0]) * M20_RAD2DEG;   /* pitch */
    AT_F32(vp, 0x3b4) = 0.0f;
    rot[0] = AT_F32(vp, 0x3ac) + -90.0f;
    rot[1] = AT_F32(vp, 0x3b0) + 180.0f;
    rot[2] = 0.0f;
    fn_00004620(M20_P(G(0x427cc)), M20_P(rot));

    /* current camera, shared with the renderers */
    G_F32(0x470f4) = AT_F32(vp, 0x3c4);
    G_S32(0x470d4) = v;
    G_F32(0x470f8) = AT_F32(vp, 0x3c8);
    G_F32(0x470fc) = AT_F32(vp, 0x3cc);
    G_F32(0x47100) = AT_F32(vp, 0x3ac);
    G_F32(0x47108) = AT_F32(vp, 0x3b4);
    G_F32(0x47104) = AT_F32(vp, 0x3b0);
    fn_00004f44(M20_P(vp + 0x3c4), M20_P(vp + 0x3b8), AT_F32(vp, 0x3a0), AT_F32(vp, 0x3a4),
                AT_F32(vp, 0x3a8));
    /* keep this viewport's view matrix (0x46fa0, 0x30 bytes each) */
    fn_00037148(G(0x4279c), (uint8_t *)G(0x46fa0) + v * 0x30);
    fn_00028864();
    fn_00004e68(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);

    /* horizon: project a far point straight ahead */
    {
        float an = AT_F32(vp, 0x3b0) * M20_DEG;
        float c = cosf(an), s = sinf(an);

        p[0] = AT_F32(vp, 0x3c4) + G_F32(0x3e9c4) * s;
        p[1] = 0.0f;
        p[2] = AT_F32(vp, 0x3cc) + G_F32(0x3e9c4) * c;
        fn_0001da58(v, p, scr0);
    }
    /* sun position: a point far away in camera space */
    p[0] = G_F32(0x3e9c4) * 40.0f;
    p[1] = G_F32(0x3ea00) * -3000.0f + 24000.0f;
    p[2] = p[0];
    fn_00037fe8((uint8_t *)G(0x46fa0) + G_S32(0x470d4) * 0x30, p, q);
    q[2] = -q[2];
    fn_0001da58(v, p, scr1);
    if (q[2] > 0.0f) {
        AT_S32(vp, 0x3fc) = (int)scr1[0];
        AT_S32(vp, 0x400) = (int)scr1[1];
    } else {
        AT_S32(vp, 0x3fc) = -1000;
        AT_S32(vp, 0x400) = -1000;
    }
    G_F32(0x48288) = scr0[1] + 10.0f; /* horizon line y */
    wf = (float)G_S32(0x4036c);

    /* sky colour 0x3eb90 (darker with rain 0x3ea00 / fog 0x3e9f4 / storm 0x3ea0c); the 2D title
     * screens and storm flashes use the clear colour 0x3eb7c */
    sky3d = G_U8(0x3fab8);
    {
        int flash = sky3d != 0;

        if (flash) {
        } else if (G_F32(0x3ea00) > 1.0f) {
            float f = G_F32(0x3ea00);

            r = (int)(f * -15.0f + 210.0f);
            b = (int)(f * -22.0f + 255.0f);
            g = (int)(f * -21.0f + 220.0f);
        } else if (G_F32(0x3e9f4) > 1.0f) {
            float f = G_F32(0x3e9f4);

            g = (int)(220.0f - (f + f));
            r = (int)(210.0f - (f + f));
            b = (int)(f * -4.0f + 255.0f);
        } else if (G_F32(0x3ea0c) > 1.0f) {
            float f = G_F32(0x3ea0c);

            if (G_S32(0x5c164) % 10 >= 6) {
                flash = 1; /* lightning */
            } else {
                g = (int)(220.0f - f * 5.0f);
                r = (int)(210.0f - f * 5.0f);
                b = (int)(f * -2.0f + 255.0f);
            }
        } else {
            r = 0xd2;
            g = 0xdc;
            b = 0xff;
        }
        if (flash) {
            uint32_t c = G_U32(0x3eb7c);

            G_U32(0x3eb90) = c;
            a = c >> 24;
            b = c >> 16;
            g = c >> 8;
            r = c;
        } else {
            G_U8(0x3eb90) = (uint8_t)r;
            G_U8(0x3eb91) = (uint8_t)g;
            G_U8(0x3eb92) = (uint8_t)b;
            G_U8(0x3eb93) = 0xff;
            a = 0xff;
        }
    }

    /* sky strip x offset follows the yaw (one texture width per 60 degrees) */
    sx_off = (int)(AT_F32(vp, 0x3f4) * wf / 60.0f) % (int)wf;
    if (sx_off >= 1)
        sx_off = (int)((float)sx_off - wf);
    skyh = (scr0[1] + 10.0f) + -350.0f > 0.0f ? (float)(int)(scr0[1] + 10.0f) : 350.0f;

    if (!sky3d) {
        int x;

        tilt = 0.0f;
        if (G_S32(0x3f8ac) == 0)
            tilt = G_F32(0x3f820); /* "Tilting Horizon": roll from the accelerometer */
        if (G_U8(0x3ebfc) & 1) {
            uint32_t c;
            int cr, cg, cb;

            /* sky (tex 0xa4) above the horizon */
            fn_00004fb4(r & 0xff, g & 0xff, b & 0xff, a);
            for (x = sx_off; x < G_S32(0x40368); x = (int)((float)x + wf))
                fn_0000526c(0xa4, (float)x, G_F32(0x48288) - skyh, wf, skyh, 0.95f, 0.0f, 0.0f,
                            1.0f, 1.0f, -tilt, (float)G_S32(0x4036c), (float)G_S32(0x40378), 0);
            /* ground below it, tinted from the sky colour */
            c = G_U32(0x3eb90);
            cr = (int)((double)(int)(c & 0xff) * 0.7);
            cg = (int)((double)(int)((c >> 8) & 0xff) * 0.9 + 25.0);
            cb = (int)((double)(int)((c >> 16) & 0xff) * 0.9 + 12.0);
            if (cr > 0xff)
                cr = -1;
            if (cg > 0xff)
                cg = -1;
            if (cb > 0xff)
                cb = -1;
            fn_00004fb4(cr & 0xff, cg & 0xff, cb & 0xff, 0xff);
            for (x = sx_off; x < G_S32(0x40368); x = (int)((float)x + wf))
                fn_0000526c(0xa4, (float)x, G_F32(0x48288), wf,
                            (float)G_S32(0x40374) - G_F32(0x48288) + 2.0f, 0.95f, 0.0f, 0.0f,
                            1.0f, 1.0f, -tilt, (float)G_S32(0x4036c), (float)G_S32(0x40378), 2);
        }
        /* sun (tex 0xa1) */
        if (G_F32(0x3ea00) > 1.0f) {
            float f = G_F32(0x3ea00);

            fn_00004fb4((int)(f * -5.0f + 255.0f) & 0xff, (int)(f * -15.0f + 235.0f),
                        (int)(215.0f - f * 25.0f) & 0xff, (int)(255.0f - f * 25.0f));
        } else {
            uint32_t c = G_U32(0x3eb7c);

            fn_00004fb4(c & 0xff, (c & 0xff00) >> 8, (c >> 16) & 0xff, c >> 24);
        }
        fn_0000526c(0xa1, (float)(AT_S32(vp, 0x3fc) - 32), (float)(AT_S32(vp, 0x400) - 32), 64.0f,
                    64.0f, 0.95f, 0.0f, 0.0f, 1.0f, 1.0f, -tilt, (float)AT_S32(vp, 0x3fc),
                    (float)AT_S32(vp, 0x400), 0);
    } else {
        /* 2D menus: plain background */
        int m;

        if (G_S32(0x3fa24) == 4)
            fn_00004fb4(0xfa, 0xfa, 0xfa, 0xff);
        else
            fn_00004fb4(0, 0, 0, 0xff);
        fn_00005354(0xa8, 0.0f, 0.0f, (float)G_S32(0x40368), (float)G_S32(0x40374), 0.01f, 0);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        m = G_S32(0x3fa24);
        if ((unsigned)(m - 10) < 2 || m == 0x14 || m == 0x15 || m == 0x17 || m == 0x18 ||
            m == 0x1e || m == 0xc || m == 0x19)
            fn_0000526c(0x9a, 64.0f, 64.0f, (float)(G_S32(0x3f864) - 128), 4.0f, 0.95f, 0.05f,
                        0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    fn_000042b0(1, 0x201, 1);
        /* back to the 3D camera, viewport rectangle (0x5c160 + v * 4: x 0x28, y 0x38, w 0x48, h 0x58) */
    fn_00004f44(M20_P(vp + 0x3c4), M20_P(vp + 0x3b8), AT_F32(vp, 0x3a0), AT_F32(vp, 0x3a4),
                AT_F32(vp, 0x3a8));
    fn_00037148(G(0x4279c), (uint8_t *)G(0x46fa0) + v * 0x30);
    fn_00003f7c(AT_F32(vw, 0x28), AT_F32(vw, 0x38), AT_F32(vw, 0x48), AT_F32(vw, 0x58));
    G_U32(0x3eb94) = G_U32(0x3eb7c);
    fn_0001e8c0(G_F32(0x3e9f4)); /* fog */
    fn_00003cdc(1);
    fn_00003cf8(2);
    glEnable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glColor4f(0.25f, 0.25f, 0.25f, 0.25f);
    {
        uint32_t c = G_U32(0x3eb74);

        fn_00004fb4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24);
    }

    /* light: position 0x4829c, direction 0x482a8, diffuse 0x3eb6c, ambient `amb`, specular-ish
     * 0x3eb98; darker with storm (0x3ea0c) or rain (0x3ea00) */
    {
        float *lpos = (float *)G(0x4829c);
        float *ldir = (float *)G(0x482a8);
        uint32_t amb;
        float lx, ly;

        if (G_U8(0x3fab8)) {
            lpos[0] = 2000.0f;
            lpos[1] = 4000.0f;
            lpos[2] = 2000.0f;
            if (G_S32(0x3fa24) == 4) {
                G_U8(0x3eb98) = 0xff;
                G_U8(0x3eb99) = 0xff;
                G_U8(0x3eb9a) = 0xff;
                G_U8(0x3eb9b) = 0xff;
                amb = 0xffd2c8c8u;
            } else {
                G_U8(0x3eb98) = 0;
                G_U8(0x3eb99) = 0;
                G_U8(0x3eb9a) = 0;
                G_U8(0x3eb9b) = 0xff;
                amb = 0xffdcdcdcu;
            }
            G_F32(0x3e9f4) = 10.0f;
            lx = 2000.0f;
            ly = 4000.0f;
        } else {
            uint32_t base = G_U32(0x3eb70);
            int R = base & 0xff, Gc = (base >> 8) & 0xff, B = (base >> 16) & 0xff;
            uint32_t c;

            amb = base;
            G_U8(0x3eb6c) = 0xff;
            G_U8(0x3eb6d) = 0xff;
            G_U8(0x3eb6e) = 0xff;
            G_U8(0x3eb6f) = 0xff;
            if (G_F32(0x3ea0c) > 1.0f) {
                float f = G_F32(0x3ea0c);
                int r1 = (int)((float)R - f * 10.0f);
                int r2 = (int)((float)Gc - f * 10.0f);
                int r3 = (int)((float)B + f * -5.0f);

                lpos[0] = 800.0f;
                lpos[1] = 1600.0f;
                lpos[2] = 800.0f;
                amb = (uint32_t)r1 | (uint32_t)r2 << 8 | (uint32_t)r3 << 16 | 0xff000000u;
                lx = 800.0f;
            } else if (G_F32(0x3ea00) > 1.0f) {
                float f = G_F32(0x3ea00);
                int r1 = (int)((float)R - f);
                int r2 = (int)((float)Gc - (f + f));
                int r3 = (int)((float)B - (f + f));

                G_F32(0x3e9f4) = f;
                lx = f * -50.0f + 800.0f;
                lpos[0] = lx;
                lpos[1] = 1600.0f;
                lpos[2] = lx;
                G_U8(0x3eb6c) = (uint8_t)r1;
                G_U8(0x3eb6d) = (uint8_t)r2;
                G_U8(0x3eb6e) = (uint8_t)r3;
                G_U8(0x3eb6f) = 0xff;
                amb = (uint32_t)r1 | (uint32_t)r2 << 8 | (uint32_t)r3 << 16 | 0xff000000u;
            } else {
                lpos[0] = 800.0f;
                lpos[1] = 1600.0f;
                lpos[2] = 800.0f;
                lx = 800.0f;
            }
            c = G_U32(0x3eb90);
            G_U8(0x3eb98) = (uint8_t)(int)((float)(int)((c & 0xff) * 2) / 3.4f);
            G_U8(0x3eb99) = (uint8_t)(int)((float)(int)(((c >> 8) & 0xff) * 2) / 3.4f);
            G_U8(0x3eb9a) = (uint8_t)(int)((float)(int)(((c >> 16) & 0xff) * 2) / 3.4f);
            ly = 1600.0f;
        }
        ldir[0] = -lx;
        ldir[1] = -ly;
        ldir[2] = -lx;
        fn_000379a8(ldir, ldir);
        fn_00004e44(M20_P(G(0x4279c)));
        fn_00003e78(lpos[0], lpos[1], lpos[2]);
        fn_00003ea0(); /* empty stub, was passed the direction */
        fn_00003ea4(G_U32(0x3eb6c));
        fn_00003e0c(amb);
        glEnable(GL_LIGHT0);
        glEnable(GL_LIGHTING);
    }

    G_S32(0x3f934) = G_S32(0x3e9c0) * -3 + 0x23;

    /* track, in four layers; detail level from the followed car's speed (0x254) and the camera
     * height */
    {
        uint8_t *me = (uint8_t *)G(0x48ae0) + tcar * 0x678;
        float lod = (float)((double)AT_F32(me, 0x254) / 22.0 +
                            (double)(G_F32(0x470f8) / 100.0f) / 10.0);

        if (!G_U8(0x3fab8)) {
            int lv = (int)lod;

            fn_00038a78(2);
            fn_00038a88(2);
            fn_00015e4c(v, 1, 0, 0, 0, lv + 4, 0);
            if (G_F32(0x3ea00) > 1.0f) {
                fn_00038a78(2);
                fn_00038a88(2);
            } else {
                fn_00038a78(1);
                fn_00038a88(1);
            }
            fn_00015e4c(v, 0, 1, 0, 0, lv + 4, 0);
            fn_00015e4c(v, 0, 0, 1, 0, lv + 3, 0);
            fn_00038a78(1);
            fn_00038a88(1);
            fn_000042b0(1, 0x201, 0);
            glDisable(GL_LIGHTING);
            fn_00015e4c(v, 0, 0, 0, 1, lv + 3, 0);
            glEnable(GL_LIGHTING);
            fn_000042b0(1, 0x201, 1);
        }
    }

    /* depth-sort the cars for this viewport: order list 0x46ea0 + v * 64 (far to near by
     * |x| + |z| in view space) */
    if (G_S32(0x3e9c8) > 0) {
        float cv[37][3];
        int32_t *order = (int32_t *)((uint8_t *)G(0x46ea0) + v * 64);
        int i, j;

        for (i = 0; i < G_S32(0x3e9c8); i++) {
            fn_00037fe8((uint8_t *)G(0x46fa0) + v * 0x30,
                        (float *)((uint8_t *)G(0x48ae0) + i * 0x678 + 0x2bc), cv[i]);
            if (i < G_S32(0x3e9b0) && AT_S32(vp, 0x408) == 0xc && i == AT_S32(vp, 0x410))
                cv[i][2] = -5.0f;
        }
        for (i = 0; i < G_S32(0x3e9c8); i++) {
            int rank = 0;

            for (j = 0; j < G_S32(0x3e9c8); j++) {
                float di, dj;

                if (i == j)
                    continue;
                di = fabsf(cv[i][2]) + fabsf(cv[i][0]);
                dj = fabsf(cv[j][2]) + fabsf(cv[j][0]);
                if (di > dj)
                    rank++;
                else if (di == dj && i > j)
                    rank++;
            }
            order[rank] = i;
        }
    }

    if (!G_U8(0x3fab8)) {
        int32_t *order = (int32_t *)((uint8_t *)G(0x46ea0) + v * 64);
        int i, k;

        /* checkpoint / finish banners (0x47f98, 0xac bytes each, G_S32(0x3f97c) of them) */
        for (k = 0; k < G_S32(0x3f97c); k++) {
            uint8_t *e = (uint8_t *)G(0x47f98) + k * 0xac;
            int row, col, sl, j;

            if (AT_S32(e, 0x24) != 1 || AT_S32(e, 0x20) == 0)
                continue;
            row = (int)((AT_F32(e, 0x10) + 406.0f) / 812.0f);
            col = (int)((AT_F32(e, 0x18) + 406.0f) / 812.0f);
            if ((unsigned)(m20_tile(0x44800, row, col) - 7) <= 1)
                row++;
            AT_F32(e, 0x14) = (float)(int)(float)m20_tile(0x44000, row, col) * 100.0f;
            sl = (int)AT_F32(e, 0x10);
            for (j = 0; j < AT_S32(e, 0x28); j++) {
                int slot;
                float *m;

                if (j >= 1) {
                    uint8_t *cc = (uint8_t *)G(0x3eb44) + AT_S32(e, 0x30 + j * 4) * 4;
                    float n = (float)AT_S32(e, 0xa8);

                    fn_00004fb4(cc[0], cc[1], cc[2], cc[3]);
                    sl = (int)((float)(int)((float)sl - AT_F32(e, 0x80 + j * 4) * 155.0f * n) -
                               AT_F32(e, 0x80 + (j - 1) * 4) * 155.0f * n);
                } else {
                    uint32_t c = G_U32(0x3eb7c);

                    fn_00004fb4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24);
                }
                slot = ((int32_t *)G(0x47454))[15 - AT_S32(e, 0x58 + j * 4)];
                m = (float *)fn_00038818(slot);
                AT_F32(m, 0x288) = AT_F32(e, 0x80 + j * 4) * 1.2f;
                AT_F32(m, 0x298) = 1.2f;
                AT_F32(m, 0x2a8) = 1.2f;
                AT_F32(m, 0x27c) = 0.0f;
                AT_F32(m, 0x280) = (float)(AT_S32(e, 0xa8) * 90 + 90);
                AT_F32(m, 0x284) = 0.0f;
                AT_F32(m, 0x270) = (float)sl;
                AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + AT_F32(e, 0x14) + 6.0f;
                AT_F32(m, 0x278) = AT_F32(e, 0x18);
                fn_00038a98(((int32_t *)G(0x47454))[15 - AT_S32(e, 0x58 + j * 4)], -1, -1, 0, 0, 2);
            }
        }

        /* blinking marker over the selected car (only reached with 0x3f9b0 == -1, so it uses
         * the record just before car 0, as the original does) */
        if (G_S32(0x3f9dc) == G_S32(0x3ebe0) && G_S32(0x3f9b0) == -1 && !G_U8(0x3fab8)) {
            uint32_t c = G_S32(0x5c16c) % 128 >= 0x41 ? G_U32(0x3eb7c) : G_U32(0x3eb80);
            uint8_t *car = (uint8_t *)G(0x48ae0) + G_S32(0x3f9b0) * 0x678;
            float *mk = (float *)G(0x47244);
            float s, o[3], res[3];
            int tmp = 0;
            uint8_t *mdl;

            fn_00004fb4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24);
            s = sinf((G_F32(0x3f9a0) + AT_F32(car, 0x268) * 20.0f) * M20_DEG);
            mk[1] = fn_0000d800(-1, mk[0], mk[1], mk[2], &tmp);
            p[0] = -(s * G_F32(0x3f9a4));
            p[1] = 25.0f;
            p[2] = 0.0f;
            fn_00037fe8(car + 0x334, p, o);
            fn_00037dec(mk, o, res);
            mdl = (uint8_t *)fn_00038818(0x7b);
            AT_F32(mdl, 0x284) = 0.0f;
            AT_F32(mdl, 0x280) = s * G_F32(0x3f9a4) + 90.0f;
            AT_F32(mdl, 0x27c) = s * G_F32(0x3f9a4);
            memcpy(mdl + 0x270, res, 12);
            AT_F32(mdl, 0x274) = AT_F32(mdl, 0x274) + AT_F32(mdl, 0x2ac);
            AT_F32(mdl, 0x288) = 1.0f;
            AT_F32(mdl, 0x298) = 1.0f;
            AT_F32(mdl, 0x2a8) = 1.0f;
            fn_00038a98(0x7b, -1, -1, 0, 0, 0);
        }

        /* heavy storm: first pass of the cars (fading with distance 0x3c), hidden when the
         * camera is under the car's bridge / tunnel tile (state 0x64 == 7 / 8) */
        if (wide) {
            fn_000042b0(0, 0x201, 1);
            G_S32(0x3f938) = 0;
            G_S32(0x3f93c) = 3;
            G_S32(0x3f940) = 7;
            for (i = 0; i < G_S32(0x3e9c8); i++) {
                int id = order[i];
                uint8_t *car = (uint8_t *)G(0x48ae0) + id * 0x678;
                float alpha = (G_F32(0x3ea0c) + -5.0f - (float)AT_S32(car, 0x3c)) / 20.0f;
                int st = AT_S32(car, 0x64), hgt = 0;

                if (alpha < 0.0f)
                    alpha = 0.0f;
                else if (alpha > 1.0f)
                    alpha = 1.0f;
                if (st == 8)
                    hgt = (int)((float)m20_tile(0x44000, AT_S32(car, 0x44) + 1, AT_S32(car, 0x48)) *
                                100.0f);
                else if (st == 7)
                    hgt = (int)((float)m20_tile(0x44000, AT_S32(car, 0x44), AT_S32(car, 0x48) + 1) *
                                100.0f);
                if (AT_F32(vp, 0x3c8) > (float)hgt && AT_S32(car, 0x6c) != 0)
                    continue;
                fn_0001ed34(id, id, v, 1, 0, 1, 1, 1, 1, alpha);
            }
            fn_000042b0(1, 0x201, 1);
        }
        G_S32(0x3f93c) = 3;
        G_S32(0x3f938) = 0;
        G_S32(0x3f940) = 7;

        /* rain: track reflections and car shadows on the wet road for cars near the followed
         * car (by race position 0x84, wrapping at G_S32(0x3f948)) */
        if (G_F32(0x3ea00) > 1.0f) {
            uint8_t *me = (uint8_t *)G(0x48ae0) + tcar * 0x678;

            fn_000042b0(0, 0x201, 1);
            fn_00015e4c(v, 0, 0, 0, 0, 3, 1);
            fn_000042b0(0, 0x201, 0);
            for (i = 0; i < G_S32(0x3e9c8); i++) {
                uint8_t *c = (uint8_t *)G(0x48ae0) + order[i] * 0x678;
                float d = (float)(AT_S32(c, 0x84) - AT_S32(me, 0x84));
                uint8_t *m;

                if (!(fabsf(d) < 4.0f) && !(fabsf(d) > (float)(G_S32(0x3f948) - 4)))
                    continue;
                m = (uint8_t *)fn_00038818(0x80);
                G_U8(0x42760) = 'Y';
                G_U8(0x42764) = 'Z';
                G_U8(0x42768) = 'X';
                memcpy(m + 0x27c, c + 0x2c8, 12);
                memcpy(m + 0x270, c + 0x2b0, 12);
                AT_F32(m, 0x274) = AT_F32(c, 0x2b4) + -32.0f;
                if (AT_S32(c, 0x100)) {
                    AT_F32(m, 0x288) = 1.2f;
                    AT_F32(m, 0x298) = 1.0f;
                    AT_F32(m, 0x2a8) = 2.0f;
                    fn_00004fb4(0xaa, 0xc8, 0xff, 0x64);
                    fn_00038a98(0x80, -1, -1, 0, 0, 2);
                }
                AT_F32(m, 0x288) = 0.7f;
                AT_F32(m, 0x298) = 1.0f;
                AT_F32(m, 0x2a8) = 0.5f;
                fn_00004fb4(0xff, 0xe1, 0xff, 0x64);
                fn_00038a98(0x80, -1, -1, 0, 0, 2);
                {
                    uint32_t cc = G_U32(0x3eb88);

                    fn_00004fb4(cc & 0xff, (cc >> 8) & 0xff, (cc >> 16) & 0xff, cc >> 24);
                }
                G_U8(0x42760) = 'Z';
                G_U8(0x42764) = 'Y';
                G_U8(0x42768) = 'X';
            }
            fn_000042b0(1, 0x201, 1);
        }

        /* the cars */
        for (i = 0; i < G_S32(0x3e9c8); i++)
            fn_0001ed34(order[i], order[i], v, 1, 1, 3, 2, 0, 0, 1.0f);

        if (G_F32(0x3ea00) > 1.0f) {
            fn_00038a78(2);
            fn_00038a88(2);
        } else {
            fn_00038a78(1);
            fn_00038a88(1);
        }
        fn_000042b0(1, 0x201, 0);
        glDisable(GL_LIGHTING);
        G_U8(0x42760) = 'Y';
        G_U8(0x42764) = 'X';
        G_U8(0x42768) = 'Z';

        /* particles (smoke / dust): 0x22 records of 0x48 bytes at 0x5c160 + 0x5a0 */
        {
            uint8_t *m = (uint8_t *)fn_00038818(0x7e);
            int off;

            for (off = 0; off != 0x990 + 0x48; off += 0x48) {
                uint8_t *e = (uint8_t *)G(0x5c160) + off;
                int al;

                if (fn_0001c7c0(0x7e, AT_F32(e, 0x5a0), AT_F32(e, 0x5a4), AT_F32(e, 0x5a8), 50.0f,
                                G_F32(0x3e9c4)) != 1)
                    continue;
                if (!(AT_F32(e, 0x5d0) > -1.0f))
                    continue;
                AT_F32(m, 0x288) = AT_F32(e, 0x5c4);
                AT_F32(m, 0x298) = AT_F32(e, 0x5c8);
                AT_F32(m, 0x2a8) = AT_F32(e, 0x5cc);
                memcpy(m + 0x270, e + 0x5a0, 12);
                AT_F32(m, 0x27c) = G_F32(0x47100);
                AT_F32(m, 0x280) = G_F32(0x47104) + 180.0f;
                AT_F32(m, 0x284) = AT_F32(e, 0x5c0);
                al = (int)(AT_S32(e, 0x5e4) * AT_U8(e, 0x5e3)) / 255;
                fn_00004fb4(AT_U8(e, 0x5e0), AT_U8(e, 0x5e1), AT_U8(e, 0x5e2), al & 0xff);
                fn_00038a98(0x7e, AT_S32(e, 0x5d8), -1, 0, 0, 2);
            }
        }
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        G_U8(0x42760) = 'Z';
        G_U8(0x42764) = 'Y';
        G_U8(0x42768) = 'X';
        fn_000042b0(1, 0x201, 0);

        /* storm: rain sheets in front of the camera (records 0..5 at 0x5c160 + 0x1f0) */
        if (G_F32(0x3ea0c) >= 1.0f) {
            float mtx[12], d[3], fwd[3], k, an;
            int lim;

            rot[0] = AT_F32(vp, 0x3ac);
            rot[1] = AT_F32(vp, 0x3b0);
            rot[2] = AT_F32(vp, 0x3b4);
            fn_00004620(M20_P(mtx), M20_P(rot));
            G_U8(0x5c160) = 1;
            d[0] = AT_F32(vp, 0x3c4) - AT_F32(vp, 0x3d0);
            d[1] = AT_F32(vp, 0x3c8) - AT_F32(vp, 0x3d4);
            d[2] = AT_F32(vp, 0x3cc) - AT_F32(vp, 0x3d8);
            an = AT_F32(vp, 0x3b0) * M20_DEG;
            fwd[0] = sinf(an);
            fwd[1] = 0.0f;
            fwd[2] = cosf(an);
            k = fmaxf(0.0f, fminf(5.0f, fn_00037e80(d, fwd))); /* camera speed */
            for (i = 0; i <= (G_U8(0x5c160) ? 5 : 0); i++) {
                uint8_t *e = (uint8_t *)G(0x5c160) + i * 0x48;
                float t = (float)i / (G_U8(0x5c160) ? 5.0f : 0.0f);
                float range = (10.0f - G_F32(0x3ea0c)) * 300.0f + 300.0f;
                float dist = t * range + k + 20.0f;
                int al;

                AT_S32(e, 0x220) = 0;
                p[0] = (float)fn_00004400((int)(dist * 4.0f)) - (dist + dist);
                p[1] = (float)fn_00004400((int)(dist * 0.125f)) + dist * -0.25f;
                p[2] = dist;
                fn_00037fe8(mtx, p, p);
                AT_F32(e, 0x208) = k * -5.0f;
                AT_F32(e, 0x20c) = AT_F32(vp, 0x3b0) + 180.0f;
                AT_F32(e, 0x210) = 0.0f;
                AT_F32(e, 0x1f0) = AT_F32(vp, 0x3c4) + p[0];
                AT_F32(e, 0x1f4) = AT_F32(vp, 0x3c8) + p[1];
                AT_F32(e, 0x1f8) = AT_F32(vp, 0x3cc) + p[2];
                AT_F32(e, 0x214) = range * 5.0f / 300.0f + 2.0f;
                AT_F32(e, 0x218) = 10.0f;
                AT_F32(e, 0x21c) = 10.0f;
                AT_U8(e, 0x230) = 0xff;
                AT_U8(e, 0x231) = 0xff;
                AT_U8(e, 0x232) = 0xff;
                AT_F32(e, 0x224) = 100.0f;
                al = (int)(((double)t + 0.2) * 240.0 * (double)(G_F32(0x3ea0c) + -5.0f) / 10.0 +
                           20.0);
                if (al > 0xff)
                    al = 0xff;
                else if (al <= -1)
                    al = 0;
                AT_U8(e, 0x233) = (uint8_t)al;
            }
            G_U8(0x42760) = 'Y';
            G_U8(0x42764) = 'X';
            G_U8(0x42768) = 'Z';
            {
                uint8_t *m = (uint8_t *)fn_00038818(0x7f);

                for (lim = G_U8(0x5c160) ? 5 : 0; lim >= 0; lim--)
                    m20_draw_sprite(m, (uint8_t *)G(0x5c160) + lim * 0x48 + 0x1f0);
            }
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
            G_U8(0x42760) = 'Z';
            G_U8(0x42764) = 'Y';
            G_U8(0x42768) = 'X';
        }

        /* snow: this viewport's share of the flake records, respawned around the camera when
         * they hit the ground, leave the depth range or go off screen */
        if (G_F32(0x3ea18) >= 1.0f) {
            float mtx[12], d[3], fwd[3], k, an;
            int n0, hi, lo, cnt, j;

            rot[0] = AT_F32(vp, 0x3ac);
            rot[1] = AT_F32(vp, 0x3b0);
            rot[2] = AT_F32(vp, 0x3b4);
            fn_00004620(M20_P(mtx), M20_P(rot));
            G_U8(0x5c160) = 1;
            d[0] = AT_F32(vp, 0x3c4) - AT_F32(vp, 0x3d0);
            d[1] = AT_F32(vp, 0x3c8) - AT_F32(vp, 0x3d4);
            d[2] = AT_F32(vp, 0x3cc) - AT_F32(vp, 0x3d8);
            n0 = G_S32(0x3e9c0);
            hi = (v * 5 + 5) / n0;
            lo = (v * 5) / n0;
            an = AT_F32(vp, 0x3b0) * M20_DEG;
            fwd[0] = sinf(an);
            fwd[1] = 0.0f;
            fwd[2] = cosf(an);
            k = fmaxf(0.0f, fminf(5.0f, fn_00037e80(d, fwd)));
            cnt = hi - lo;
            for (j = 0; j < cnt; j++) {
                uint8_t *e = (uint8_t *)G(0x5c160) + (lo + j) * 0x48;
                float *pos = (float *)(e + 0x1f0);
                float vz[3], scr[2], sc;
                int h, respawn = 0, al;

                AT_S32(e, 0x220) = 0;
                h = m20_tile(0x44000, (int)((pos[0] + 406.0f) / 812.0f),
                             (int)((pos[2] + 406.0f) / 812.0f));
                fn_00037fe8((uint8_t *)G(0x46fa0) + v * 0x30, pos, vz);
                fn_0001da58(v, pos, scr);
                if (pos[1] < (float)(int)((float)h * 100.0f) + -100.0f) {
                    respawn = 1;
                } else {
                    float tmp[3], o[3], z;

                    memcpy(tmp, pos, 12);
                    fn_00037fe8((uint8_t *)G(0x46fa0) + G_S32(0x470d4) * 0x30, tmp, o);
                    z = o[2];
                    if (z < -550.0f || z > -5.0f) {
                        respawn = 1;
                    } else {
                        uint8_t *m = (uint8_t *)fn_00038818(0x7f);
                        int mn = 10000000, mx = -10000000, c, W2;
                        float cn[3] = { 0.0f, 0.0f, 0.0f };

                        /* screen x extent of the model's bounding box */
                        for (c = 0; c < 8; c++) {
                            float s[2], sx;

                            switch (c) {
                            case 0: cn[0] = AT_F32(m, 0x2b0); cn[1] = AT_F32(m, 0x2b4); cn[2] = AT_F32(m, 0x2b8); break;
                            case 1: cn[0] = AT_F32(m, 0x2bc); cn[1] = AT_F32(m, 0x2b4); cn[2] = AT_F32(m, 0x2b8); break;
                            case 2: cn[0] = AT_F32(m, 0x2bc); cn[1] = AT_F32(m, 0x2c0); cn[2] = AT_F32(m, 0x2b8); break;
                            case 3: cn[0] = AT_F32(m, 0x2bc); cn[1] = AT_F32(m, 0x2c0); cn[2] = AT_F32(m, 0x2c4); break;
                            case 4: cn[0] = AT_F32(m, 0x2b0); cn[1] = AT_F32(m, 0x2c0); cn[2] = AT_F32(m, 0x2c4); break;
                            case 5: cn[0] = AT_F32(m, 0x2b0); cn[1] = AT_F32(m, 0x2b4); cn[2] = AT_F32(m, 0x2c4); break;
                            case 6: cn[0] = AT_F32(m, 0x2bc); cn[1] = AT_F32(m, 0x2b4); cn[2] = AT_F32(m, 0x2c4); break;
                            default: cn[0] = AT_F32(m, 0x2b0); cn[1] = AT_F32(m, 0x2c0); cn[2] = AT_F32(m, 0x2b8); break;
                            }
                            cn[0] += tmp[0];
                            cn[1] += tmp[1];
                            cn[2] += tmp[2];
                            fn_0001da58(G_S32(0x470d4), cn, s);
                            sx = s[0] * (float)G_S32(0x3f854) / (float)G_S32(0x40368);
                            if (sx < (float)mn)
                                mn = (int)sx;
                            if (sx > (float)mx)
                                mx = (int)sx;
                        }
                        W2 = G_S32(0x3f854);
                        if ((mn >= 1 && mn < W2) || (mx >= 1 && mx < W2) || (mn < 0 && mx > W2) ||
                            (mn < W2 && mx > 0))
                            pos[1] += -5.0f; /* falling */
                        else
                            respawn = 1;
                    }
                }
                if (respawn) {
                    float dist = (float)j / (float)cnt * (k + k + 500.0f) + 25.0f, y;
                    int hh;

                    p[0] = (float)fn_00004400((int)dist) + dist * -0.5f;
                    p[1] = dist / (k + k + 10.0f);
                    p[2] = dist;
                    fn_00037fe8(mtx, p, p);
                    pos[0] = AT_F32(vp, 0x3c4) + p[0];
                    pos[2] = AT_F32(vp, 0x3cc) + p[2];
                    h = m20_tile(0x44000, (int)((pos[0] + 406.0f) / 812.0f),
                                 (int)((pos[2] + 406.0f) / 812.0f));
                    y = AT_F32(vp, 0x3c8) + p[1];
                    hh = (int)((float)h * 100.0f);
                    if (y < (float)hh + -100.0f)
                        y = (float)(hh + 100);
                    pos[1] = y;
                    fn_00037fe8((uint8_t *)G(0x46fa0) + v * 0x30, pos, vz);
                }
                AT_F32(e, 0x208) = k * -3.0f;
                AT_F32(e, 0x20c) = AT_F32(vp, 0x3b0) + 180.0f;
                AT_F32(e, 0x210) = 0.0f;
                AT_U32(e, 0x230) = 0xffffffffu;
                AT_F32(e, 0x224) = 100.0f;
                sc = fmaxf(1.0f, fminf(3.0f, vz[2] / -500.0f + 1.0f));
                AT_F32(e, 0x214) = sc * 30.0f;
                AT_F32(e, 0x218) = sc * 30.0f;
                AT_F32(e, 0x21c) = sc * 30.0f;
                al = (int)((sc + -1.0f) * 300.0f * G_F32(0x3ea18) / 10.0f + 5.0f);
                if (al > 0xff)
                    al = 0xff;
                else if (al <= -1)
                    al = 0;
                AT_U8(e, 0x233) = (uint8_t)al;
            }
            G_U8(0x42760) = 'Y';
            G_U8(0x42764) = 'X';
            G_U8(0x42768) = 'Z';
            G_U8(0x5c160) = 1;
            {
                uint8_t *m = (uint8_t *)fn_00038818(0x7f);

                n0 = G_S32(0x3e9c0);
                hi = (v * 5 + 5) / n0;
                lo = (v * 5) / n0;
                for (i = hi - 1; i >= lo; i--)
                    m20_draw_sprite(m, (uint8_t *)G(0x5c160) + i * 0x48 + 0x1f0);
            }
            fn_00004fb4(0xff, 0xff, 0xff, 0xff);
            G_U8(0x42760) = 'Z';
            G_U8(0x42764) = 'Y';
            G_U8(0x42768) = 'X';
        }
        glEnable(GL_LIGHTING);
        fn_000042b0(1, 0x201, 1);
        G_U8(0x446e0 + v) = 1;
    } else {
        /* 2D menus with a 3D car / track showcase: title strings 0x472a0 / 0x47520, icon list
         * 0x474f4, then fn_000128a8 draws it */
        int32_t *q = (int32_t *)G(0x474f4);

        if (G_S32(0x3fa24) == 0x1e)
            fn_00012464(v);
        if (G_S32(0x3fa24) == 5) {
            G_S32(0x3ec00) = 1;
            G_S32(0x3f99c) = -1;
            G_F32(0x3ebdc) = 1.0f;
            snprintf((char *)G(0x472a0), 0x20, "%s", M20_STR(0x47cd0));
            q[0] = 0x78;
            fn_000128a8(v);
        }
        if (G_S32(0x3fa24) == 4) {
            G_S32(0x3ec00) = 1;
            G_S32(0x3f99c) = -1;
            G_F32(0x3ebdc) = 1.0f;
            snprintf((char *)G(0x472a0), 0x20, "%s", M20_STR(0x47cd0));
            q[0] = 0x82;
            fn_000128a8(v);
        }
        if (G_S32(0x3fa24) == 10) {
            static const uint32_t names[6] = { 0x47cf0, 0x47e10, 0x47e28, 0x47d50, 0x47e34, 0x47e0c };

            G_S32(0x3ec00) = 6;
            G_S32(0x3f99c) = 0x78;
            G_F32(0x3ebdc) = 0.24f;
            if ((unsigned)G_S32(0x3faa8) < 6)
                snprintf((char *)G(0x47520), 0x20, "%s", M20_STR(names[G_S32(0x3faa8)]));
            snprintf((char *)G(0x472a0), 0x20, "%s", M20_STR(0x47d7c));
            q[0] = 0x79;
            q[1] = 0x76;
            q[2] = 0x7b;
            q[3] = 0x77;
            q[4] = 0x74;
            q[5] = 0x7a;
            fn_000128a8(v);
        }
        if (G_S32(0x3fa24) == 0xc) {
            /* options menu */
            char *t = (char *)G(0x47520);

            G_S32(0x3ec00) = 7;
            G_S32(0x3f99c) = 0x78;
            G_F32(0x3ebdc) = 0.24f;
            switch (G_S32(0x3faa8)) {
            case 0:
                snprintf(t, 0x20, "%s: %s", M20_STR(0x47d54),
                         G_S32(0x3e9e0) ? M20_STR(0x47dac) : M20_STR(0x47da8));
                break;
            case 1:
                if (G_S32(0x3f8bc))
                    snprintf(t, 0x20, "%s:%s", M20_STR(0x47d24), "One on One");
                else
                    snprintf(t, 0x20, "%s:%s", M20_STR(0x47d24), M20_STR(0x47dc4));
                break;
            case 2:
                snprintf(t, 0x20, "%s: %d %s", M20_STR(0x47dd4), (int)G_S32(0x3e9e4),
                         M20_STR(0x47d64));
                break;
            case 3:
                if (G_F32(0x3e9b8) == 1.0f)
                    snprintf(t, 0x20, "%s: %s", M20_STR(0x47d18), M20_STR(0x47d28));
                if (G_F32(0x3e9b8) == 2.0f)
                    snprintf(t, 0x20, "%s: %s", M20_STR(0x47d18), M20_STR(0x47d84));
                if (G_F32(0x3e9b8) == 3.0f)
                    snprintf(t, 0x20, "%s: %s", M20_STR(0x47d18), M20_STR(0x47d4c));
                break;
            case 4:
                snprintf(t, 0x20, "%s: %s", M20_STR(0x47dbc),
                         G_S32(0x3ea20) ? M20_STR(0x47dac) : M20_STR(0x47da8));
                break;
            case 5:
                if (G_F32(0x3e9bc) == 0.0f)
                    snprintf(t, 0x20, "%s: %s", "Drive Assist", "Off");
                if (G_F32(0x3e9bc) == 1.0f)
                    snprintf(t, 0x20, "%s: %s", "Drive Assist", "Low");
                if (G_F32(0x3e9bc) == 2.0f)
                    snprintf(t, 0x20, "%s: %s", "Drive Assist", "Medium");
                if (G_F32(0x3e9bc) == 3.0f)
                    snprintf(t, 0x20, "%s: %s", "Drive Assist", "High");
                break;
            case 6:
                /* iOS: accelerometer horizon tilt */
                if (G_S32(0x3f8ac) == 0)
                    snprintf(t, 0x20, "%s: %s", "Tilting Horizon", "On");
                if (G_S32(0x3f8ac) == 1)
                    snprintf(t, 0x20, "%s: %s", "Tilting Horizon", "Off");
                break;
            }
            snprintf((char *)G(0x472a0), 0x20, "%s", M20_STR(0x47e0c));
            q[0] = 0x8e;
            q[1] = 0x7c;
            q[2] = 0x7b;
            q[3] = 0x79;
            q[4] = 0x88;
            q[5] = 0x7c;
            q[6] = 0x91;
            fn_000128a8(v);
        }
        if (G_S32(0x3fa24) == 0xb) {
            static const uint32_t names[4] = { 0x47db0, 0x47e38, 0x47e30, 0x47d38 };

            G_S32(0x3ec00) = 4;
            m20_track_icon();
            G_F32(0x3ebdc) = 0.5f;
            if ((unsigned)G_S32(0x3faa8) < 4)
                snprintf((char *)G(0x47520), 0x20, "%s", M20_STR(names[G_S32(0x3faa8)]));
            snprintf((char *)G(0x472a0), 0x20, "%s", M20_STR(0x47da4));
            q[0] = 0x7c;
            q[1] = 0x7c;
            q[2] = 0x7c;
            q[3] = 0x7c;
            fn_000128a8(v);
        }
        if (G_S32(0x3fa24) == 0x14)
            fn_0002889c(tcar, v);
        if (G_S32(0x3fa24) == 0x15)
            fn_0002889c(tcar, v);
        if (G_S32(0x3fa24) == 0x19) {
            G_S32(0x3ec00) = 2;
            G_S32(0x3f99c) = AT_S32(G(0x48ae0), 8);
            G_F32(0x3ebdc) = 2.5f;
            snprintf((char *)G(0x472a0), 0x20, "%s %d", M20_STR(0x47dc0), (int)G_S32(0x3fa08) + 1);
            snprintf((char *)G(0x47520), 0x20, "%s (%s)", M20_STR(0x47e04),
                     G_S32(0x3faa8) == 0 ? M20_STR(0x47d80) : M20_STR(0x47cd8));
            q[0] = 0x8d;
            q[1] = 0x8c;
            fn_000128a8(v);
        }
        if (G_S32(0x3fa24) == 0x17) {
            G_S32(0x3ec00) = 2;
            m20_track_icon();
            G_F32(0x3ebdc) = 0.5f;
            snprintf((char *)G(0x472a0), 0x20, "%s %d", M20_STR(0x47dc0), (int)G_S32(0x3fa08) + 1);
            m20_title((char *)G(0x47520), M20_STR(0x47df8), (char *)G(0x47180) + G_S32(0x3f9f8) * 4);
            q[0] = ((int32_t *)G(0x47218))[G_S32(0x3f9f8) + 1];
            fn_000128a8(v);
        }
        if (G_S32(0x3fa24) == 0x18) {
            G_S32(0x3ec00) = 1;
            m20_track_icon();
            G_F32(0x3ebdc) = 0.5f;
            if (G_S32(0x3faa8) == 0)
                m20_title((char *)G(0x47520), (char *)G(0x47544) + G_S32(0x3fa08) * 24 + 0x14, NULL);
            snprintf((char *)G(0x472a0), 0x20, "%s %d", M20_STR(0x47dc0), (int)G_S32(0x3fa08) + 1);
            snprintf((char *)G(0x47520), 0x20, "%s", M20_STR(0x47d2c));
            q[0] = ((int32_t *)G(0x47218))[G_S32(0x3f9f8) + 1];
            fn_000128a8(v);
        }
    }

    /* lens flares (0x479a0, x / y / depth, G_S32(0x3f944) of them) in clear weather */
    if (G_F32(0x3ea0c) < 1.0f && G_F32(0x3ea00) < 1.0f) {
        float vh = AT_F32(vw, 0x58), vwid = AT_F32(vw, 0x48);
        int i;

        fn_00004f24();
        fn_00005068(0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        fn_00004fb4(0xff, 0xff, 0xff, 200);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        fn_000042b0(1, 0x201, 0);
        if (G_S32(0x3f944) >= 1) {
            float asp = vwid / vh;

            for (i = 0; i < G_S32(0x3f944); i++) {
                float *f = (float *)G(0x479a0) + i * 3;
                float size, half;

                if (!(f[0] > 0.0f) || !(f[0] < (float)G_S32(0x40368)) || !(f[1] > 0.0f) ||
                    !(f[1] < (float)G_S32(0x40374)))
                    continue;
                size = 512.0f / (f[2] * 10.0f + -5.0f);
                half = size * 0.5f;
                fn_00005354(0xa0, f[0] - half, f[1] / asp - half + 2.0f, size, size,
                            (float)((double)f[2] + -0.001), 0);
            }
        }
        fn_000042b0(0, 0x207, 0);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    fn_00028864();
    fn_00004e68(0.0f, 0.0f, 0.0f, 1.0f, AT_F32(vw, 0x48) / AT_F32(vw, 0x58), 1.0f);

    /* sun glare (tex 0xa2) when the sun is on screen */
    if (G_F32(0x3ea0c) < 1.0f && G_F32(0x3ea00) < 1.0f) {
        int sx = AT_S32(vp, 0x3fc), sy = AT_S32(vp, 0x400);

        if (sx >= 1 && sx < G_S32(0x40368) && sy < G_S32(0x40374) && sy >= 1 &&
            !(G_F32(0x3ea00) >= 1.0f)) {
            int al = (int)(AT_F32(vp, 0x3f8) /
                           (G_F32(0x3e9f4) + G_F32(0x3ea00) + G_F32(0x3ea0c) + 2.0f));
            int ang = (sx * 60) / G_S32(0x40368);

            fn_00004fb4(0xff, 0xeb, 0xd7, al);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            fn_0000526c(0xa2, (float)(sx - 64), (float)(sy - 64), 256.0f, 256.0f, 0.95f, 0.0f,
                        0.0f, 1.0f, 1.0f, (float)(ang + 20), (float)sx, (float)sy, 0);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }
}

/* 0x00027b44 -- "new car unlocked" style banner: name in quotes plus description from the
 * tables at 0x44200 / 0x43600 (32 bytes per entry), fading out after menu time 250
 * (guess: Menu_DrawUnlockBanner). */
void fn_00027b44(void)
{
    char name[32], desc[32];
    float f, k;
    int base, a;

    if (G_S32(0x3f8d0) > 300 || G_S32(0x3e9e8) == G_S32(0x3e9ec))
        return;
    snprintf(name, 0x20, "%s", (const char *)G(0x44200) + G_S32(0x3e9ec) * 0x20);
    snprintf(desc, 0x20, "%s", (const char *)G(0x43600) + G_S32(0x3e9ec) * 0x20);
    base = G_S32(0x3f868);
    f = 1.0f;
    if (G_S32(0x3f8d0) >= 251)
        f = (float)(G_S32(0x3f8d0) - 250) / -50.0f + 1.0f;
    k = fminf(1.0f, fmaxf(0.0f, f));
    fn_00004fb4(0, 0, 0, (int)(k * 200.0f));
    fn_00005354(0xa8, 0.0f, (float)(base - 70), (float)G_S32(0x3f864),
                (float)(G_S32(0x3f868) + (90 - base)), 0.01f, 0xc);
    fn_00004fb4(0xff, 0xff, 0xff, (int)(k * 250.0f));
    fn_00005354(0xb0, 32.0f, (float)(base - 70), 64.0f, 64.0f, 0.01f, 4);
    snprintf(M20_TXT, 0x100, "\"%s\"", name);
    a = (int)(k * 255.0f);
    fn_00023808(M20_TXT, 100.0f, (float)(base - 65), 1.0f, 1.0f, 0xff, 0x1e, 0x1e, a, 4);
    snprintf(M20_TXT, 0x100, "%s", desc);
    fn_00023808(M20_TXT, 100.0f, (float)(base - 45), 1.0f, 1.0f, 0xff, 0xff, 0xff, a, 4);
}

/* 0x00027d90 -- menu row with a toggle icon: icon 0 at full alpha, icon 10 overlaid with the
 * blink alpha 0x3fa30 (guess: Menu_DrawBlinkRow10). */
void fn_00027d90(int row, const char *text)
{
    int base;
    float y;

    if (G_S32(0x3f8d0) < 300)
        return;
    base = G_S32(0x3f868) + row * 30;
    y = (float)(base - 160);
    fn_0000526c(0xaa, 42.0f, y, 32.0f, 32.0f, 0.95f, 0.0f, 0.0f, 0.0909f, 1.0f, 0.0f, 0.0f, 0.0f,
                4);
    fn_00004fb4(0xff, 0xff, 0xff, G_U8(0x3fa30));
    fn_0000526c(0xaa, 42.0f, y, 32.0f, 32.0f, 0.95f, 0.909f, 0.0f, 0.0909f, 1.0f, 0.0f, 0.0f, 0.0f,
                4);
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    snprintf(M20_TXT, 0x100, "%s", text);
    fn_00023808(M20_TXT, 75.0f, (float)(base - 152), 0.8f, 0.8f, 0xff, 0xff, 0xff, 0xff, 4);
}

/* 0x00027ef4 -- as fn_00027d90 with icon 9 blinking (guess: Menu_DrawBlinkRow9). */
void fn_00027ef4(int row, const char *text)
{
    int base;
    float y;

    if (G_S32(0x3f8d0) < 300)
        return;
    base = G_S32(0x3f868) + row * 30;
    y = (float)(base - 160);
    fn_0000526c(0xaa, 42.0f, y, 32.0f, 32.0f, 0.95f, 0.0f, 0.0f, 0.0909f, 1.0f, 0.0f, 0.0f, 0.0f,
                4);
    fn_00004fb4(0xff, 0xff, 0xff, G_U8(0x3fa30));
    fn_0000526c(0xaa, 42.0f, y, 32.0f, 32.0f, 0.95f, 0.8181f, 0.0f, 0.0909f, 1.0f, 0.0f, 0.0f,
                0.0f, 4);
    fn_00004fb4(0xff, 0xff, 0xff, 0xff);
    snprintf(M20_TXT, 0x100, "%s", text);
    fn_00023808(M20_TXT, 75.0f, (float)(base - 152), 0.8f, 0.8f, 0xff, 0xff, 0xff, 0xff, 4);
}

/* 0x00028058 -- minimap (guess: Hud_DrawMinimap). The track is the waypoint list at 0x45640
 * ({float row, col} per entry, 1..0x3f948) drawn with the tile ids of the grid at 0x44800
 * (20 bytes per row) from the 5x5 tile sheet 0x9d; then (draw_cars) the AI cars, the player
 * cars and the marker at 0x47244. (x, y): screen origin; cell: tile pitch; hilite: waypoint
 * index to highlight (the 6 waypoints up to it fade in red), < 0 = none. */
void fn_00028058(int draw_cars, int x, int y, int cell, int hilite)
{
    int cs = cell + 1;
    int oy = y - (G_S32(0x3f96c) - 10) * cs;
    int ox = x - (10 - G_S32(0x3f970)) * cell;
    int i;

    fn_00004fb4(0xff, 0xff, 0xff, 100);
    if (G_S32(0x3f948) >= 1) {
        float w = (float)((double)cell + 0.4);
        float h = (float)cs;
        const float *pts = (const float *)G(0x45640);
        const uint8_t *grid = (const uint8_t *)G(0x44800);
        const int32_t *done = (const int32_t *)G(0x45e38);
        unsigned fade = (unsigned)(hilite * 40 + 0xd8);

        for (i = 1; i <= G_S32(0x3f948); i++, fade -= 40) {
            int ix = (int)pts[i * 2], iy = (int)pts[i * 2 + 1];
            int cx = 0, cy = 0, sx, sy;
            unsigned tile;

            if (ix >= 0)
                cx = ix > G_S32(0x3f8a4) ? G_S32(0x3f8a4) : ix;
            if (iy >= 0)
                cy = iy > G_S32(0x3f8a0) ? G_S32(0x3f8a0) : iy;
            sy = (ix - 10) * cs + oy;
            sx = (10 - iy) * cell + ox;
            tile = grid[cx * 20 + cy];
            if (G_S32(0x3f9dc) == G_S32(0x3ebe0)) {
                if (i == 1)
                    fn_00004fb4(0xff, 0, 0, 0xfa);
                else if (i == G_S32(0x3f94c) || i == G_S32(0x3f950))
                    fn_00004fb4(0, 0xff, 0, 0xfa);
                else if (done[i] != 0)
                    fn_00004fb4(100, 0xff, 100, 0xfa);
                else
                    fn_00004fb4(0xff, 0xff, 0xff, 0xfa);
            } else if (i == 1) {
                fn_00004fb4(0xff, 0xff, 0xff, 0xfa);
                tile = 11; /* start/finish tile */
            } else if ((i == G_S32(0x3f94c) && hilite <= -1) ||
                       (i == G_S32(0x3f950) && hilite <= -1)) {
                fn_00004fb4(0, 0xff, 0, 0xfa);
            } else {
                fn_00004fb4(0xff, 0xff, 0xff, 0xfa);
            }
            if (hilite >= 1 && i >= hilite - 5 && i <= hilite)
                fn_00004fb4(0xff, fade & 0xff, fade & 0xff, 0xfa);
            fn_0000526c(0x9d, (float)sx, (float)sy, w, h, 0.95f, (float)(tile % 5) * 0.2f,
                        (float)(tile / 5) * 0.2f, 0.2f, 0.2f, 0.0f, 0.0f, 0.0f, 0xc);
        }
    }
    if (!draw_cars)
        return;

    G_S32(0x3f954) = ox;
    G_S32(0x3f958) = oy;
    {
        float fcs = (float)cs, fcell = (float)cell;
        int top = oy - 10 * cs;
        int right = ox + cell * 10;

        /* AI cars (car table 0x48ae0, 0x678 bytes per car; world pos at +0x2bc / +0x2c4) */
        for (i = G_S32(0x3e9ac); i < G_S32(0x3e9c8); i++) {
            uint8_t *car = (uint8_t *)G(0x48ae0) + i * 0x678;
            float fa = fcs * AT_F32(car, 0x2bc) / 812.0f;
            float fb = fcell * AT_F32(car, 0x2c4) / 812.0f;
            int r = 200, g = 200, b = 200;

            if (G_S32(0x3ebe4) != 0) {
                if (AT_S32(car, 0x10c) != 0) {
                    r = 200; g = 0; b = 0;
                } else if (AT_S32(car, 0x108) != 0) {
                    r = 0; g = 0; b = 200;
                }
            }
            fn_00004fb4(r, g, b, 0xff);
            fn_0000526c(0x9d, (float)(right - (int)fb), (float)(top + (int)fa), 8.0f, 8.0f, 0.95f,
                        0.01f, 0.41f, 0.18f, 0.18f, 0.0f, 0.0f, 0.0f, 4);
        }
        /* player cars: arrow (tile 11, 7 px) or dot (tile 0, 9 px), rotated by the heading */
        for (i = 0; i < G_S32(0x3e9ac); i++) {
            uint8_t *car = (uint8_t *)G(0x48ae0) + i * 0x678;
            int flag = AT_S32(car, 0x104);
            int a, b, sx, sy;
            unsigned tile = flag ? 11 : 0;
            float size = flag ? 7.0f : 9.0f;

            fn_00004fb4(AT_U8(car, 0x394), AT_U8(car, 0x395), AT_U8(car, 0x396), 0xff);
            b = (int)(fcell * AT_F32(car, 0x2c4) / 812.0f);
            a = (int)(fcs * AT_F32(car, 0x2bc) / 812.0f);
            sx = right - b;
            sy = top + a;
            fn_0000526c(0x9d, (float)sx, (float)sy, size, size, 0.95f,
                        (float)(tile % 5) * 0.2f + 0.001f, (float)(tile / 5) * 0.2f + 0.01f, 0.18f,
                        0.18f, -90.0f - AT_F32(car, 0x244), (float)(sx + 4), (float)(sy + 4), 4);
        }
        /* checkpoint marker (single-race mode) */
        if (G_S32(0x3f9dc) == G_S32(0x3ebe0) && G_S32(0x3f9b0) <= -1) {
            uint32_t c = G_U32(0x3eb7c);
            const float *m = (const float *)G(0x47244);
            int a, b, sx, sy;

            fn_00004fb4(c & 0xff, (c & 0xff00) >> 8, (c >> 16) & 0xff, 0xff);
            a = (int)(fcs * m[0] / 812.0f);
            b = (int)(fcell * m[2] / 812.0f);
            sy = top + a;
            sx = cell * 10 - b + ox;
            fn_0000526c(0x9d, (float)sx, (float)sy, 7.0f, 7.0f, 0.95f, 0.201f, 0.41f, 0.18f, 0.18f,
                        -90.0f, (float)(sx + 4), (float)(sy + 4), 4);
        }
    }
}

/* 0x0002877c -- menu row icon 7 at (197, base - 70) with the string 0x47cdc
 * (guess: Menu_DrawIcon7Label). */
void fn_0002877c(void)
{
    int base = G_S32(0x3f868);

    if (G_S32(0x3f8d0) < 300)
        return;
    fn_0000526c(0xaa, 197.0f, (float)(base - 70), 32.0f, 32.0f, 0.95f, 0.6363f, 0.0f, 0.0909f,
                1.0f, 0.0f, 0.0f, 0.0f, 4);
    snprintf(M20_TXT, 0x100, "%s", M20_STR(0x47cdc));
    fn_00023808(M20_TXT, 230.0f, (float)(base - 62), 0.8f, 0.8f, 0xff, 0xff, 0xff, 0xff, 4);
}

/* 0x00028864 -- resets the 2D draw state (white, ortho, alpha blend) (guess: Draw_Begin2D). */
void fn_00028864(void)
{
    fn_00005008(0xffffffffu);
    fn_00004f24();
    fn_00005068(1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    fn_0001e8c0(-1.0f); /* fog off */
}

/* 0x0002889c -- sets the race title strings (0x472a0 / 0x47520) for game modes 0x14 / 0x15,
 * the camera car (0x3f99c) and starts the race transition (guess: Race_SetupTitle). */
void fn_0002889c(int car, int arg)
{
    G_S32(0x3ec00) = 2;
    if (G_S32(0x3fa24) == 0x15) {
        snprintf((char *)G(0x472a0), 0x20, "%s %d", M20_STR(0x47dc0), (int)G_S32(0x3fa08) + 1);
        snprintf((char *)G(0x47520), 0x20, "%s", M20_STR(0x47dfc));
    } else if (G_S32(0x3fa24) == 0x14) {
        int p = G_S32(0x3fa08);

        if (G_S32(0x3f9dc) == G_S32(0x3ebe4)) {
            const char *tag = AT_S32((uint8_t *)G(0x47544) + p * 0x18, 0xc) != 0
                                  ? M20_STR(0x47cf8)
                                  : M20_STR(0x47e2c);
            snprintf((char *)G(0x472a0), 0x20, "%s %d (%s)", M20_STR(0x47dc0), p + 1, tag);
        } else {
            snprintf((char *)G(0x472a0), 0x20, "Player %d", p + 1);
        }
        snprintf((char *)G(0x47520), 0x20, "%s", M20_STR(0x47e08));
    }
    G_S32(0x3f99c) = AT_S32((uint8_t *)G(0x48ae0) + car * 0x678, 8);
    G_F32(0x3ebdc) = 3.0f;
    fn_000128a8(arg);
}

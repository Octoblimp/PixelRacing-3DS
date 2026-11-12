/* Module 0x0001c7c0..0x00020f70: visibility tests, terrain-grid colour / slope lookups,
 * screen projection, car wheel / suspension / driver drawing, the car renderer and the race
 * touch-control layout. Functional translation of the iOS armv7 build (docs/TRANSLATION.md).
 *
 * Calling convention note: several of these functions are internal and take some float
 * arguments in VFP registers (s0/s1) while others travel in core registers / on the stack.
 * In C they are all plain float parameters; the order chosen here is documented per function.
 *
 * Shared data used here:
 *   0x48ae0 + car * 0x678       car state (see fn_0001ed34); wheel i at + 0x3a4 + i * 0x5c
 *   G_PTR(0x3f0ac) = 0x44590    per-car wheel animation block, 0x34 bytes per car
 *   G_PTR(0x3f09c) = 0x44000    terrain height grid (u8, 20 columns per row)
 *   G_PTR(0x3f0b8) = 0x44800    terrain type grid (u8, same layout)
 *   G_PTR(0x3f098) = 0x43800    terrain colour grid A (u32 ABGR, 20 per row)
 *   G_PTR(0x3f0cc) = 0x46800    terrain colour grid B
 *   G_S32(0x3f8a4) / (0x3f8a0)  grid max x / max y index (clamp bounds)
 *   G_PTR(0x3f0ec) = 0x46fa0    per-viewport view matrices (Mtx34, 0x30 each)
 *   G_S32(0x470d4)              current viewport index (slot 0x3f0fc)
 *   0x42760/64/68               Euler rotation order chars used by fn_00004620 */
#include "ios.h"

#define M1C_P2I(p) ((int)(intptr_t)(p))

/* Clamp a grid coordinate to [0, max]. */
static inline int m1c_cx(int v)
{
    if (v < 0)
        return 0;
    return v > G_S32(0x3f8a4) ? G_S32(0x3f8a4) : v;
}

static inline int m1c_cy(int v)
{
    if (v < 0)
        return 0;
    return v > G_S32(0x3f8a0) ? G_S32(0x3f8a0) : v;
}

/* Terrain height / type at a (clamped) grid cell. */
static inline uint8_t m1c_height(int x, int y)
{
    return ((uint8_t *)G_PTR(0x3f09c))[m1c_cx(x) * 20 + m1c_cy(y)];
}

static inline uint8_t m1c_type(int x, int y)
{
    return ((uint8_t *)G_PTR(0x3f0b8))[m1c_cx(x) * 20 + m1c_cy(y)];
}

/* Current viewport's view matrix. */
static inline void *m1c_view(void)
{
    return (uint8_t *)G_PTR(0x3f0ec) + G_S32(0x470d4) * 0x30;
}

/* Model colour from the packed global 0x3eb7c (r | g << 8 | b << 16 | a << 24). */
static void m1c_model_color(void)
{
    uint32_t c = G_U32(0x3eb7c);
    fn_00004fb4(c & 0xff, (c & 0xff00) >> 8, (c >> 16) & 0xff, c >> 24);
}

/* 0x0001c7c0 -- is model `model` (its local bounding box, offset by x/y/z) on screen? Rejects
 * when the view-space depth of the origin is outside [znear, zfar], then projects the 8 box
 * corners and tests the screen-space bounding rectangle (guess: Model_IsVisible).
 * znear / zfar arrive in s0 / s1. The screen rect lives in globals 0x477f0..0x477fc; note
 * the original never resets the min-y slot (0x477fc) before the loop. */
int fn_0001c7c0(int model, float x, float y, float z, float znear, float zfar)
{
    float pos[3], v[3], c[3], scr[3];
    uint8_t *m;
    int32_t *minx = G(0x477f8), *miny = G(0x477fc), *maxx = G(0x477f0), *maxy = G(0x477f4);
    int i, W, H;

    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    fn_00037fe8(m1c_view(), pos, v);
    v[2] = -v[2];
    if (v[2] > zfar)
        return 0;
    if (v[2] < znear)
        return 0;

    m = fn_00038818(model);
    *minx = 10000000;
    *maxx = -10000000;
    *maxy = -10000000;
    for (i = 0; i < 8; i++) {
        /* box min at 0x2b0/0x2b4/0x2b8, max at 0x2bc/0x2c0/0x2c4 */
        switch (i) {
        case 0: c[0] = AT_F32(m, 0x2b0); c[1] = AT_F32(m, 0x2b4); c[2] = AT_F32(m, 0x2b8); break;
        case 1: c[0] = AT_F32(m, 0x2bc); c[1] = AT_F32(m, 0x2b4); c[2] = AT_F32(m, 0x2b8); break;
        case 2: c[0] = AT_F32(m, 0x2bc); c[1] = AT_F32(m, 0x2c0); c[2] = AT_F32(m, 0x2b8); break;
        case 3: c[0] = AT_F32(m, 0x2bc); c[1] = AT_F32(m, 0x2c0); c[2] = AT_F32(m, 0x2c4); break;
        case 4: c[0] = AT_F32(m, 0x2b0); c[1] = AT_F32(m, 0x2c0); c[2] = AT_F32(m, 0x2c4); break;
        case 5: c[0] = AT_F32(m, 0x2b0); c[1] = AT_F32(m, 0x2b4); c[2] = AT_F32(m, 0x2c4); break;
        case 6: c[0] = AT_F32(m, 0x2bc); c[1] = AT_F32(m, 0x2b4); c[2] = AT_F32(m, 0x2c4); break;
        default: c[0] = AT_F32(m, 0x2b0); c[1] = AT_F32(m, 0x2c0); c[2] = AT_F32(m, 0x2b8); break;
        }
        c[0] += x;
        c[1] += y;
        c[2] += z;
        fn_0001da58(G_S32(0x470d4), c, scr);
        if (scr[0] < (float)*minx)
            *minx = (int)scr[0];
        if (scr[1] < (float)*miny)
            *miny = (int)scr[1];
        if (scr[0] > (float)*maxx)
            *maxx = (int)scr[0];
        if (scr[1] > (float)*maxy)
            *maxy = (int)scr[1];
    }

    W = G_S32(0x40368);
    H = G_S32(0x40374);
    if (*minx >= 1) {
        if (*minx < W && *miny >= 1 && *miny < H)
            return 1;
        if (*minx < W && *maxy >= 1 && *maxy < H)
            return 1;
    }
    if (*maxx >= 1) {
        if (*maxx < W && *miny >= 1 && *miny < H)
            return 1;
        if (*maxx < W && *maxy >= 1 && *maxy < H)
            return 1;
    }
    /* box encloses the whole screen */
    if (*minx < 0 && *maxx > W && *miny < 0 && *maxy > H)
        return 1;
    if (*minx < W && *maxx >= 1 && *miny < H)
        return *maxy > 0;
    return 0;
}

/* Brightens a packed colour towards white by (G_F32(0x3ea18) - 1) / 10 when that factor is
 * above 1 (shared by the two terrain colour lookups below). */
static uint32_t m1c_brighten(uint32_t c)
{
    float k = G_F32(0x3ea18);
    uint32_t r, g, b;
    int ro, go, bo;

    if (!(k > 1.0f))
        return c;
    r = c & 0xff;
    g = (c >> 8) & 0xff;
    b = (c >> 16) & 0xff;
    k = k + -1.0f;
    ro = (int)((float)(int)r + (float)(int)(255 - r) * k / 10.0f);
    go = (int)((float)(int)g + (float)(int)(255 - g) * k / 10.0f);
    bo = (int)((float)(int)b + (float)(int)(255 - b) * k / 10.0f);
    return ((uint32_t)ro & 0xff) | ((uint32_t)bo << 16) | (((uint32_t)go << 8) & 0xffff) |
           0xff000000u;
}

/* 0x0001cae8 -- terrain colour of grid cell (x, y) from grid A, brightened by the global
 * light factor; a constant colour (0x44190) when the flag 0x3fab8 is set
 * (guess: Terrain_GetColorA). */
void fn_0001cae8(uint32_t *out, int x, int y)
{
    if (G_U8(0x3fab8) != 0) {
        *out = G_U32(0x44190);
        return;
    }
    *out = m1c_brighten(((uint32_t *)G_PTR(0x3f098))[m1c_cx(x) * 20 + m1c_cy(y)]);
}

/* 0x0001cc18 -- is world point (x, y, z) far outside the camera heading `ang` (degrees, in s0)?
 * Returns 1 when the point is beyond 500 units in front, or when the heading difference
 * (with a +-35 degree bias by screen side) exceeds 50 degrees (guess: Cam_IsOffAngle). */
int fn_0001cc18(float x, float y, float z, float ang)
{
    float pos[3], v[3], bias, cam;

    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    fn_00037fe8(m1c_view(), pos, v);
    if (v[0] < 0.0f)
        bias = -35.0f;
    else
        bias = v[0] > 0.0f ? 35.0f : 0.0f;
    if (v[2] > -500.0f)
        return 1;
    ang = bias + ang;
    cam = G_F32(0x47104);
    if (cam > ang + 180.0f)
        cam = cam + -360.0f;
    else if (cam < ang + -180.0f)
        cam = cam + 360.0f;
    return fabsf(cam - ang) > 50.0f;
}

/* 0x0001cd24 -- terrain colour of grid cell (x, y) from grid B, brightened like fn_0001cae8
 * (guess: Terrain_GetColorB). */
void fn_0001cd24(uint32_t *out, int x, int y)
{
    *out = m1c_brighten(((uint32_t *)G_PTR(0x3f0cc))[m1c_cx(x) * 20 + m1c_cy(y)]);
}

/* A neighbour cell counts as a slope when it is higher than h and its type is not 7 or 8. */
static int m1c_is_slope(int x, int y, float h)
{
    if (!((float)m1c_height(x, y) > h))
        return 0;
    if (m1c_type(x, y) == 7)
        return 0;
    if (m1c_type(x, y) == 8)
        return 0;
    return 1;
}

/* 0x0001ce34 -- slope of the terrain next to cell (gx, gy) at height h. axis 1 looks at the
 * y neighbours (writes *outY), axis 2 at the x neighbours (writes *outX); the ramp height
 * difference * 100 goes to *rise, the tilt (rise / +-812) to the axis output
 * (guess: Terrain_GetSlope). */
void fn_0001ce34(int axis, int gx, int gy, int h, float *rise, float *outX, float *outY)
{
    float hf, v = 0.0f;

    if (axis == 1) {
        hf = (float)h;
        if (m1c_is_slope(gx, gy + 1, hf)) {
            v = ((float)m1c_height(gx, gy + 1) - hf) * 100.0f;
            *outY = v / 812.0f;
        } else if (m1c_is_slope(gx, gy - 1, hf)) {
            v = ((float)m1c_height(gx, gy - 1) - hf) * 100.0f;
            *outY = v / -812.0f;
        } else {
            v = 0.0f;
        }
    }
    if (axis == 2) {
        hf = (float)h;
        if (m1c_is_slope(gx + 1, gy, hf)) {
            v = ((float)m1c_height(gx + 1, gy) - hf) * 100.0f;
            *outX = v / 812.0f;
        } else if (m1c_is_slope(gx - 1, gy, hf)) {
            v = ((float)m1c_height(gx - 1, gy) - hf) * 100.0f;
            *outX = v / -812.0f;
        }
    }
    *rise = v;
}

#define M1C_D2R 0.0174533f /* 0x3c8efa39 */

/* 0x0001d388 -- draws a car's four wheels (fn_0001db80) and the animated body part (model
 * 0x89) for car `car` at (x, y, z) with Euler rotation (rx, ry, rz) in degrees. The per-car
 * block G_PTR(0x3f0ac) + car * 0x34 holds the animation angle (+0), the four wheel angles
 * (+4..+0x10) and four sound-state words (+0x24..+0x30). side: 1 = upside-down (mirror the
 * ground offset) (guess: Car_DrawWheelsAndBody). */
void fn_0001d388(int car, float x, float y, float z, float rx, float ry, float rz, int side)
{
    float pos[3], rot[3], off[3], t[3], wp[3], body[3], e[3], t2[3];
    float mtxA[12], mtxB[12];
    uint8_t *w = (uint8_t *)G_PTR(0x3f0ac) + car * 0x34;
    uint8_t *m;
    float a, R, h, d;
    int flags, i;

    G_U8(0x42760) = 'Y';
    G_U8(0x42764) = 'Z';
    G_U8(0x42768) = 'X';
    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    rot[0] = rx;
    rot[1] = ry;
    rot[2] = rz;
    fn_00004620(M1C_P2I(mtxA), M1C_P2I(rot));
    fn_00004620(M1C_P2I(mtxB), M1C_P2I(rot));

    /* front wheels */
    off[0] = -70.0f;
    off[1] = 0.0f;
    off[2] = 30.0f;
    fn_00037fe8(mtxB, off, t);
    fn_00037dec(pos, t, wp);
    fn_0001db80(car, wp[0], wp[1], wp[2], rot[0], rot[1], rot[2], mtxA, 0, side,
                (int *)(w + 0x24), AT_F32(w, 4));
    off[0] = -70.0f;
    off[1] = 0.0f;
    off[2] = -20.0f;
    fn_00037fe8(mtxB, off, t);
    fn_00037dec(pos, t, wp);
    fn_0001db80(car, wp[0], wp[1], wp[2], rot[0], rot[1], rot[2], mtxA, 0, side,
                (int *)(w + 0x28), AT_F32(w, 8));

    /* body part (model 0x89), turned around */
    rot[1] = rot[1] + 180.0f;
    fn_00004620(M1C_P2I(mtxA), M1C_P2I(rot));
    off[0] = 45.0f;
    off[1] = 0.0f;
    off[2] = 60.0f;
    fn_00037fe8(mtxB, off, t);
    fn_00037dec(pos, t, body);
    a = AT_F32(w, 0);
    R = rot[1];
    flags = 0;
    m = fn_00038818(0x89);
    AT_F32(m, 0x288) = 1.0f;
    AT_F32(m, 0x298) = 1.0f;
    AT_F32(m, 0x2a8) = 1.0f;
    AT_F32(m, 0x294) = 0.0f;
    AT_F32(m, 0x29c) = 0.0f;
    AT_F32(m, 0x2a0) = 0.0f;
    AT_F32(m, 0x2a4) = 0.0f;
    AT_F32(m, 0x28c) = 0.0f;
    AT_F32(m, 0x290) = 0.0f;
    AT_F32(m, 0x27c) = rot[0];
    AT_F32(m, 0x280) = R;
    AT_F32(m, 0x284) = rot[2];
    e[0] = 0.0f;
    e[1] = 0.0f;
    e[2] = 0.0f;
    if (a >= 0.0f && a < 90.0f) {
        float s;
        i = (int)(a * 5.0f) % 180;
        s = sinf((float)i * M1C_D2R);
        e[0] = a / 3.0f;
        e[2] = -(90.0f - a) / 3.0f;
        AT_F32(m, 0x280) = R + 100.0f + a * -0.5f;
        e[1] = s * 8.0f;
    }
    if (a >= 90.0f && a < 135.0f) {
        float s = sinf((a + -90.0f + (a + -90.0f)) * M1C_D2R);
        float p = s * 0.3f;
        AT_F32(m, 0x280) = R + 100.0f + -45.0f;
        AT_F32(m, 0x29c) = p;
        e[0] = p * 15.0f + 30.0f;
        e[1] = 0.0f;
        e[2] = AT_F32(m, 0x29c) * 15.0f;
    }
    if (a >= 135.0f && a < 270.0f) {
        AT_F32(m, 0x280) = R + 100.0f + -45.0f;
        AT_F32(m, 0x29c) = 0.3f;
        e[0] = 34.5f;
        e[1] = 0.0f;
        e[2] = AT_F32(m, 0x29c) * 15.0f;
    }
    if (a >= 270.0f && a < 315.0f) {
        float c = cosf((a + -270.0f + (a + -270.0f)) * M1C_D2R);
        float p = c * 0.3f;
        AT_F32(m, 0x280) = R + 100.0f + -45.0f;
        AT_F32(m, 0x29c) = p;
        e[0] = p * 15.0f + 30.0f;
        e[1] = 0.0f;
        e[2] = AT_F32(m, 0x29c) * 15.0f;
    }
    if (a >= 315.0f && a < 360.0f) {
        float dd = a + -315.0f, s;
        i = ((int)dd * 6) % 180;
        s = sinf((float)i * M1C_D2R);
        e[0] = (360.0f - a + (360.0f - a)) / 3.0f;
        e[2] = dd * -2.0f / 3.0f;
        AT_F32(m, 0x280) = R + 100.0f - (360.0f - a);
        e[1] = s * 8.0f;
    }
    if (a >= 360.0f) {
        AT_F32(m, 0x280) = R + 100.0f;
        e[0] = 0.0f;
        e[1] = 0.0f;
        e[2] = -30.0f;
    }
    body[1] = 0.0f;
    AT_F32(m, 0x270) = body[0];
    AT_F32(m, 0x274) = 0.0f;
    AT_F32(m, 0x278) = body[2];
    fn_00037fe8(mtxA, e, t2);
    fn_00037dec(body, t2, (float *)(m + 0x270));
    if (G_S32(0x3fac4) != 0)
        h = 0.0f;
    else
        h = (float)(int)fn_0000d800(-1, AT_F32(m, 0x270), AT_F32(m, 0x274), AT_F32(m, 0x278),
                                    &flags);
    if (side == 1)
        d = h - AT_F32(m, 0x2ac) + -1.0f;
    else
        d = h + AT_F32(m, 0x2ac) + 1.0f;
    AT_F32(m, 0x274) = AT_F32(m, 0x274) + d;
    m1c_model_color();
    fn_00038a98(0x89, -1, -1, 0, 0, 2);

    /* rear wheels */
    off[0] = 70.0f;
    off[1] = 0.0f;
    off[2] = 30.0f;
    fn_00037fe8(mtxB, off, t);
    fn_00037dec(pos, t, wp);
    fn_0001db80(car, wp[0], wp[1], wp[2], rot[0], rot[1], rot[2], mtxA, 0, side,
                (int *)(w + 0x2c), AT_F32(w, 0xc));
    off[0] = 70.0f;
    off[1] = 0.0f;
    off[2] = -20.0f;
    fn_00037fe8(mtxB, off, t);
    fn_00037dec(pos, t, wp);
    fn_0001db80(car, wp[0], wp[1], wp[2], rot[0], rot[1], rot[2], mtxA, 0, side,
                (int *)(w + 0x30), AT_F32(w, 0x10));

    G_U8(0x42760) = 'Z';
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';
}

/* 0x0001da58 -- projects world point pos through the current modelview (0x4279c) and
 * projection (0x42878) into viewport `vp` (fractions at 0x5c188/98/a8/b8 of the GL surface
 * size 0x42938) and returns virtual-screen coordinates in out[0..1], window depth in out[2]
 * (guess: Cam_ProjectToScreen). */
void fn_0001da58(int vp, float *pos, float *out)
{
    uint16_t *dims = G(0x42938);
    float fx = ((float *)G(0x5c188))[vp], fy = ((float *)G(0x5c198))[vp];
    float fw = ((float *)G(0x5c1a8))[vp], fh = ((float *)G(0x5c1b8))[vp];
    float d1 = (float)(int)dims[1], d0;
    float mv[16], win[3];
    int viewport[4];

    viewport[0] = (int)(fx * d1);
    d0 = (float)(int)dims[0];
    viewport[1] = (int)(fy * d0);
    viewport[2] = (int)(fw * d1);
    viewport[3] = (int)(fh * d0);
    fn_000045d0(M1C_P2I(G(0x4279c)), M1C_P2I(mv));
    fn_000058f0(M1C_P2I(pos), M1C_P2I(mv), M1C_P2I(G(0x42878)), M1C_P2I(viewport), win);
    out[0] = win[0] * (float)G_S32(0x40368) / (float)(int)dims[1] / fw;
    out[1] = win[1] * (float)G_S32(0x40374) / (float)(int)dims[0] / fh;
    out[2] = win[2];
}

/* Places the driver/suspension model `drv` at pos + mtx * (ox, oy, oz) and returns the
 * truncated ground height there (fn_0000d800). */
static int m1c_place(uint8_t *drv, void *mtx, float *pos, float ox, float oy, float oz,
                     int *flags)
{
    float off[3], t[3];

    off[0] = ox;
    off[1] = oy;
    off[2] = oz;
    fn_00037fe8(mtx, off, t);
    fn_00037dec(pos, t, (float *)(drv + 0x270));
    return (int)fn_0000d800(-1, AT_F32(drv, 0x270), AT_F32(drv, 0x274), AT_F32(drv, 0x278),
                            flags);
}

/* Common tail of the two driver poses in fn_0001db80: height, then draw model 0x73 twice
 * (second copy mirrored) with the texture of the car slot idx. */
static void m1c_draw_driver(uint8_t *drv, int ground, int dy, int side, int nocolor, float a,
                            int idx)
{
    int v, tex;

    if (nocolor == 0)
        m1c_model_color();
    if (G_S32(0x3fac4) != 0)
        ground = 0;
    if (side == 1)
        v = ground + ~dy;
    else
        v = dy + ground + 1;
    AT_F32(drv, 0x274) = (float)v;
    if (a > 0.0f) {
        tex = AT_S32(G(0x48ae0), idx * 0x678);
        fn_00038a98(0x73, tex + 6, -1, 0, 0, 2);
        AT_F32(drv, 0x280) = AT_F32(drv, 0x280) + 180.0f;
        AT_F32(drv, 0x284) = -AT_F32(drv, 0x284);
        AT_F32(drv, 0x27c) = -AT_F32(drv, 0x27c);
        tex = AT_S32(G(0x48ae0), idx * 0x678);
        fn_00038a98(0x73, tex + 6, -1, 0, 0, 2);
    }
}

/* 0x0001db80 -- draws one wheel (model 0x88) at (x, y, z) with car rotation (rx, ry, rz) and
 * the car rotation matrix mtx, animated by the wheel angle `a` (passed in s0; 0..365, the
 * wheel swings over while the car tumbles). Plays the bump sound (id G_S32(0x3ebd0)) once per
 * swing through *state, then poses / draws the driver model 0x73 when the car state allows
 * (guess: Car_DrawWheel). nocolor: skip the colour resets. side: 1 = mirrored ground offset. */
void fn_0001db80(int car, float x, float y, float z, float rx, float ry, float rz, void *mtx,
                 int nocolor, int side, int *state, float a)
{
    float pos[3], off[3], t[3];
    uint8_t *drv, *wh, *w;
    int flags = 0, ground, dy, idx, inB;
    float hf;

    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    drv = fn_00038818(0x73);
    AT_F32(drv, 0x288) = 1.0f;
    AT_F32(drv, 0x298) = 1.0f;
    AT_F32(drv, 0x2a8) = 1.0f;
    wh = fn_00038818(0x88);
    AT_F32(wh, 0x288) = 1.0f;
    AT_F32(wh, 0x298) = 1.0f;
    AT_F32(wh, 0x2a8) = 1.0f;
    AT_F32(wh, 0x294) = 0.0f;
    AT_F32(wh, 0x29c) = 0.0f;
    AT_F32(wh, 0x2a0) = 0.0f;
    AT_F32(wh, 0x2a4) = 0.0f;
    AT_F32(wh, 0x28c) = 0.0f;
    AT_F32(wh, 0x290) = 0.0f;
    AT_F32(wh, 0x27c) = rx;
    AT_F32(wh, 0x280) = ry;
    AT_F32(wh, 0x284) = rz;
    off[0] = 0.0f;
    off[1] = 0.0f;
    off[2] = 0.0f;

    if (a >= 0.0f && a < 45.0f) {
        float p = sinf(a * 4.0f * M1C_D2R) * 0.5f;
        AT_F32(wh, 0x280) = ry + 90.0f;
        AT_F32(wh, 0x29c) = p;
        off[0] = p * 20.0f;
    }
    inB = (a >= 45.0f) && (a < 90.0f);
    if (inB)
        AT_F32(wh, 0x280) = ry + 90.0f + (a + -45.0f) * -2.0f;
    if (a >= 90.0f && a < 135.0f)
        AT_F32(wh, 0x280) = ry + (a + -90.0f) * 4.0f;
    if (a >= 135.0f && a < 180.0f) {
        float s = sinf((a + -135.0f) * 4.0f * M1C_D2R);
        AT_F32(wh, 0x280) = ry + 180.0f;
        AT_F32(wh, 0x2a4) = s * -0.4f;
        AT_F32(wh, 0x29c) = s * 0.3f;
    }
    if (a >= 180.0f && a < 270.0f)
        AT_F32(wh, 0x280) = ry + 180.0f - (a + -180.0f);
    if (a >= 270.0f && a < 365.0f) {
        float p = sinf((a + -270.0f + (a + -270.0f)) * M1C_D2R) * 0.5f;
        AT_F32(wh, 0x280) = ry + 90.0f;
        AT_F32(wh, 0x29c) = p;
        off[0] = p * 20.0f;
    }

    ground = 0;
    pos[1] = 0.0f;
    AT_F32(wh, 0x270) = pos[0];
    AT_F32(wh, 0x274) = 0.0f;
    AT_F32(wh, 0x278) = pos[2];
    fn_00037fe8(mtx, off, t);
    fn_00037dec(pos, t, (float *)(wh + 0x270));
    if (G_S32(0x3fac4) == 0)
        ground = (int)fn_0000d800(-1, AT_F32(wh, 0x270), AT_F32(wh, 0x274), AT_F32(wh, 0x278),
                                  &flags);
    hf = (float)ground;
    if (side == 1)
        hf = hf - AT_F32(wh, 0x2ac) + -1.0f;
    else
        hf = hf + AT_F32(wh, 0x2ac) + 1.0f;
    AT_F32(wh, 0x274) = AT_F32(wh, 0x274) + hf;
    if (inB)
        *state = 0;

    /* bump sound while the wheel swings through 22..45 or 320..340 */
    if ((a > 22.0f && a < 45.0f) || (a > 320.0f && a < 340.0f)) {
        if (*state == 0) {
            float l = 0.0f, r = 0.0f;
            *state = 1;
            fn_0001e804(AT_F32(wh, 0x270), AT_F32(wh, 0x274), AT_F32(wh, 0x278), G_S32(0x470d4),
                        &l, &r);
            fn_0003613c(G_S32(0x3ebd0), (int)(l * 10000.0f), (int)(r * 10000.0f));
            fn_000360bc(G_S32(0x3ebd0));
        }
    }
    if (nocolor == 0)
        m1c_model_color();
    dy = 0;
    fn_00038a98(0x88, -1, -1, 0, 0, 2);

    w = (uint8_t *)G_PTR(0x3f0ac) + car * 0x34;
    idx = AT_S32(w, 0x14);
    if (idx < 0)
        idx = 0;
    if (AT_S32(w, 0x18) == idx) {
        /* first pose */
        dy = 0;
        if (a >= 0.0f && a < 22.0f) {
            float xo = side != 0 ? -30.0f : 30.0f;
            AT_F32(drv, 0x27c) = 0.0f;
            AT_F32(drv, 0x280) = ry;
            AT_F32(drv, 0x284) = 0.0f;
            ground = m1c_place(drv, mtx, pos, xo, 0.0f, 0.0f, &flags);
            dy = (int)(AT_F32(drv, 0x2ac) + 25.0f);
        }
        if (a >= 22.0f && a < 90.0f) {
            float ox, oy, oz;
            ground = 0;
            AT_F32(drv, 0x27c) = 0.0f;
            AT_F32(drv, 0x280) = AT_F32(wh, 0x280) - ry + 90.0f;
            AT_F32(drv, 0x284) = (a + -22.0f) * 1.3f + 0.0f;
            ox = AT_F32(wh, 0x270) + sinf(AT_F32(wh, 0x280) * M1C_D2R) * 25.0f;
            oy = AT_F32(wh, 0x274);
            oz = AT_F32(wh, 0x278) + cosf(AT_F32(wh, 0x280) * M1C_D2R) * 25.0f;
            dy = (int)oy;
            AT_F32(drv, 0x270) = ox;
            AT_F32(drv, 0x274) = oy;
            AT_F32(drv, 0x278) = oz;
        }
        if (a >= 90.0f && a < 110.0f) {
            float k;
            AT_F32(drv, 0x27c) = 0.0f;
            AT_F32(drv, 0x280) = ry + -90.0f;
            AT_F32(drv, 0x284) = (a + -90.0f) * 35.0f / 20.0f + 90.0f;
            k = (110.0f - a) * 0.5f;
            ground = m1c_place(drv, mtx, pos, 0.0f, 0.0f, 25.0f, &flags);
            dy = (int)(AT_F32(drv, 0x2ac) + k + 15.0f);
        }
        if (a >= 110.0f && a < 180.0f) {
            float k = 180.0f - a, d = a + -110.0f;
            float ang = d * 10.28f * M1C_D2R;
            float c = cosf(ang), s = sinf(ang);
            AT_F32(drv, 0x27c) = d * 90.0f / 70.0f + 0.0f;
            AT_F32(drv, 0x280) = s * k * 0.5f + -90.0f + ry;
            AT_F32(drv, 0x284) = c * k * 0.5f + 90.0f;
            ground = m1c_place(drv, mtx, pos, 0.0f, 0.0f, 25.0f, &flags);
            dy = (int)(AT_F32(drv, 0x2ac) + k / 14.0f + 10.0f);
        }
        if (a >= 180.0f && a < 365.0f) {
            AT_F32(drv, 0x27c) = 90.0f;
            AT_F32(drv, 0x280) = ry + -90.0f;
            AT_F32(drv, 0x284) = 90.0f;
            ground = m1c_place(drv, mtx, pos, 0.0f, 0.0f, 25.0f, &flags);
            dy = (int)(AT_F32(drv, 0x2ac) + 10.0f);
        }
        m1c_draw_driver(drv, ground, dy, side, nocolor, a, idx);
    }

    if (AT_S32(G(0x48ae0), idx * 0x678 + 0x70) != 4) {
        /* second pose */
        if (a >= 0.0f && a < 160.0f) {
            AT_F32(drv, 0x27c) = 0.0f;
            AT_F32(drv, 0x280) = ry + 90.0f;
            AT_F32(drv, 0x284) = -90.0f;
            ground = m1c_place(drv, mtx, pos, 0.0f, 0.0f, -25.0f, &flags);
            dy = (int)(AT_F32(drv, 0x2ac) + 12.0f);
        }
        if (a >= 160.0f && a < 180.0f) {
            float d = a + -160.0f;
            AT_F32(drv, 0x27c) = 0.0f;
            AT_F32(drv, 0x280) = ry + 90.0f;
            AT_F32(drv, 0x284) = -90.0f;
            ground = m1c_place(drv, mtx, pos, 0.0f, 0.0f, d * 0.25f + -25.0f, &flags);
            dy = (int)(AT_F32(drv, 0x2ac) + d / 2.5f + 12.0f);
        }
        if (a >= 180.0f && a < 315.0f) {
            float d = a + -180.0f, q = d / -3.0f;
            float ox, oy, oz;
            ground = 0;
            AT_F32(drv, 0x27c) = 0.0f;
            AT_F32(drv, 0x280) = q + q + 90.0f + ry;
            AT_F32(drv, 0x284) = 90.0f - (AT_F32(wh, 0x280) - ry);
            ox = AT_F32(wh, 0x270) + sinf(AT_F32(wh, 0x280) * M1C_D2R) * 20.0f;
            oy = AT_F32(wh, 0x274) + d / 20.0f + -6.0f;
            oz = AT_F32(wh, 0x278) + cosf(AT_F32(wh, 0x280) * M1C_D2R) * 20.0f;
            dy = (int)(AT_F32(drv, 0x274) + 12.0f); /* previous height, read before the store */
            AT_F32(drv, 0x270) = ox;
            AT_F32(drv, 0x274) = oy;
            AT_F32(drv, 0x278) = oz;
        }
        if (a >= 315.0f && a < 365.0f) {
            AT_F32(drv, 0x27c) = 0.0f;
            AT_F32(drv, 0x280) = ry + 0.0f;
            AT_F32(drv, 0x284) = 0.0f;
            ground = m1c_place(drv, mtx, pos, 30.0f, 0.0f, 0.0f, &flags);
            dy = (int)(AT_F32(drv, 0x2ac) + 25.0f);
        }
        m1c_draw_driver(drv, ground, dy, side, nocolor, a, idx);
    }
    if (nocolor == 0)
        m1c_model_color();
}

/* 0x0001e804 -- stereo gains for a sound at world point (x, y, z) heard from viewport vp:
 * pan = clamp((view x + 50) / 100, 0, 1), both sides attenuated by distance / 100 + 0.8
 * (guess: Sound_PanForPoint). */
void fn_0001e804(float x, float y, float z, int vp, float *left, float *right)
{
    float pos[3], v[3], dist, pan, att;

    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    dist = fn_00037fa4(pos, (float *)((uint8_t *)G(0x5c160) + vp * 0x80 + 0x3c4));
    fn_00037fe8((uint8_t *)G_PTR(0x3f0ec) + vp * 0x30, pos, v);
    pan = (v[0] + 50.0f) / 100.0f;
    pan = fminf(1.0f, pan);
    pan = fmaxf(0.0f, pan);
    att = dist / 100.0f + 0.8f;
    *left = pan;
    *right = 1.0f - pan;
    *left = *left / att;
    *right = *right / att;
}

/* 0x0001e8c0 -- fog for depth offset v (s0): v < 0 disables GL_FOG, otherwise linear fog
 * from 600 - 50v to 2500 - 50v in colour G_U32(0x3eb98) (guess: Render_SetFog). */
void fn_0001e8c0(float v)
{
    if (v < 0.0f) {
        glDisable(GL_FOG);
        return;
    }
    glEnable(GL_FOG);
    fn_00003d20(2, 600.0f - v * 50.0f, 2500.0f - v * 50.0f, 10.0f, G_F32(0x3e9c4),
                G_U32(0x3eb98));
}

/* 0x0001e94c -- draws a lone wheel (car 0's wheel 0 animation) at (x, y, z) with rotation
 * (rx, ry, rz) (guess: Car_DrawLooseWheel). */
void fn_0001e94c(float x, float y, float z, float rx, float ry, float rz, int nocolor,
                 int side)
{
    float pos[3], rot[3], off[3], t[3], p[3];
    float mtxA[12], mtxB[12];
    uint8_t *w = G_PTR(0x3f0ac);

    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    G_U8(0x42760) = 'Y';
    G_U8(0x42764) = 'Z';
    G_U8(0x42768) = 'X';
    rot[0] = rx;
    rot[1] = ry;
    rot[2] = rz;
    fn_00004620(M1C_P2I(mtxA), M1C_P2I(rot));
    fn_00004620(M1C_P2I(mtxB), M1C_P2I(rot));
    off[0] = 0.0f;
    off[1] = 0.0f;
    off[2] = 0.0f;
    fn_00037fe8(mtxB, off, t);
    fn_00037dec(pos, t, p);
    fn_0001db80(0, p[0], p[1], p[2], rot[0], rot[1], rot[2], mtxA, nocolor, side,
                (int *)(w + 0x24), AT_F32(w, 4));
    G_U8(0x42760) = 'Z';
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';
}

/* 0x0001ea38 -- updates wheel `wheel` of car `car` (car rotation matrix mtx): suspension
 * position (travel clamped to -15..-20), ground clearance, spin angle and steering for the
 * four road wheels (0..3; 0 and 3 are turned 180 degrees) (guess: Car_UpdateWheel). */
void fn_0001ea38(int car, int wheel, void *mtx)
{
    uint8_t *C = (uint8_t *)G(0x48ae0) + car * 0x678;
    uint8_t *W = C + wheel * 0x5c;
    float v[3], t[3], u[3], r[3], s, spd;

    v[0] = AT_F32(W, 0x3b0);
    v[1] = AT_F32(W, 0x3b4);
    v[2] = AT_F32(W, 0x3b8);
    fn_00037fe8(mtx, v, t);
    u[0] = 0.0f;
    u[1] = AT_F32(W, 0x3d4) - (AT_F32(C, 0x2b4) + t[1]) + 17.0f;
    u[2] = 0.0f;
    fn_00037fe8(C + 0x364, u, r);
    if (r[1] > -15.0f) {
        r[1] = -15.0f;
        s = -15.0f;
    } else {
        s = r[1];
        if (r[1] < -20.0f) {
            r[1] = -20.0f;
            s = -20.0f;
        }
    }
    v[1] = v[1] + s;
    fn_00037fe8(mtx, v, t);
    fn_00037dec((float *)(C + 0x2b0), t, (float *)(W + 0x398));
    if (AT_F32(W, 0x39c) < AT_F32(W, 0x3ec) + 12.0f)
        AT_F32(W, 0x39c) = AT_F32(W, 0x3ec) + 12.0f;

    spd = AT_F32(C, 0x40) * 100.0f;
    if (G_U8(0x3fab8) != 0)
        AT_F32(C, 0x264) = 0.0f;
    if ((unsigned)wheel > 3) {
        s = AT_F32(W, 0x3a4);
    } else {
        float div = spd + 1.0f;
        switch (wheel) {
        case 0:
            s = AT_F32(W, 0x3a4) - AT_F32(C, 0x264) * 5.0f;
            AT_F32(W, 0x3a4) = s;
            AT_F32(W, 0x3a8) = AT_F32(C, 0x244) + AT_F32(C, 0x250) * 5.0f + 180.0f;
            AT_F32(W, 0x3ac) = -AT_F32(C, 0x2d0);
            break;
        case 1:
            s = AT_F32(W, 0x3a4) + AT_F32(C, 0x264) * 5.0f;
            AT_F32(W, 0x3a4) = s;
            AT_F32(W, 0x3a8) = AT_F32(C, 0x244) + AT_F32(C, 0x250) * 5.0f;
            AT_U32(W, 0x3ac) = AT_U32(C, 0x2d0);
            break;
        case 2:
            s = AT_F32(W, 0x3a4) + AT_F32(C, 0x264) * 5.0f / div;
            AT_F32(W, 0x3a4) = s;
            AT_U32(W, 0x3a8) = AT_U32(C, 0x244);
            AT_U32(W, 0x3ac) = AT_U32(C, 0x2d0);
            break;
        default:
            s = AT_F32(W, 0x3a4) + AT_F32(C, 0x264) * -5.0f / div;
            AT_F32(W, 0x3a4) = s;
            AT_F32(W, 0x3a8) = AT_F32(C, 0x244) + 180.0f;
            AT_F32(W, 0x3ac) = -AT_F32(C, 0x2d0);
            break;
        }
    }
    if (fabsf(s) > 360.0f)
        AT_F32(W, 0x3a4) = (float)((int)AT_F32(W, 0x3a4) % 360);
}

/* Wheel k of car state C (k = 0..3; block at C + 0x398 + k * 0x5c: position +0, rotation
 * +0xc, ground height +0x54, rest height +0x3c) copied into model m and drawn with model 0x73.
 * In the mirrored (reflection) pass the height is reflected about the ground plus yoff. */
static void m1c_draw_wheel(uint8_t *m, uint8_t *C, int k, int mirror, float yoff, int tex)
{
    uint8_t *W = C + 0x398 + k * 0x5c;
    AT_F32(m, 0x270) = AT_F32(W, 0);
    if (mirror)
        AT_F32(m, 0x274) = AT_F32(W, 0x54) - (AT_F32(W, 0x3c) - AT_F32(W, 0x54)) + yoff;
    else
        AT_F32(m, 0x274) = AT_F32(W, 4);
    AT_F32(m, 0x278) = AT_F32(W, 8);
    memcpy(m + 0x27c, W + 0xc, 12);
    fn_00038a98(0x73, tex, -1, 0, 0, 2);
}

/* Helper for the light / glow sprites: car-local offset v -> world p, then pulled 20 units
 * towards the camera (G_F32(0x470f4..0x470fc)) and stored as model m's position. */
static void m1c_sprite_pos(uint8_t *m, uint8_t *C, float *v, int mirror)
{
    float t[3], p[3], d[3];
    fn_00037fe8(C + 0x334, v, t);
    fn_00037dec((float *)(C + 0x2b0), t, p);
    if (mirror)
        p[1] = (AT_F32(C, 0x274) + AT_F32(C, 0x270)) - p[1];
    d[0] = G_F32(0x470f4) - p[0];
    d[1] = G_F32(0x470f8) - p[1];
    d[2] = G_F32(0x470fc) - p[2];
    fn_000379a8(d, d);
    AT_F32(m, 0x270) = p[0] + d[0] * 20.0f;
    AT_F32(m, 0x274) = p[1] + d[1] * 20.0f;
    AT_F32(m, 0x278) = p[2] + d[2] * 20.0f;
}

/* Attachment model on the roof / bumper (0x7c for some race kinds, otherwise `model`). */
static void m1c_draw_attach(uint8_t *C, uint8_t *main, float *v, int model)
{
    float t[3], p[3];
    uint8_t *m;
    fn_00037fe8(C + 0x334, v, t);
    fn_00037dec((float *)(C + 0x2b0), t, p);
    m = fn_00038818(model);
    AT_F32(m, 0x288) = 0.45f;
    AT_F32(m, 0x298) = 0.45f;
    AT_F32(m, 0x2a8) = 0.45f;
    memcpy(m + 0x270, p, 12);
    memcpy(m + 0x27c, main + 0x27c, 12);
    AT_F32(m, 0x280) = AT_F32(m, 0x280) + AT_F32(C, 0x250) * 10.0f;
    AT_F32(m, 0x284) = AT_F32(m, 0x284) + (AT_F32(C, 0x268) + AT_F32(C, 0x268));
    fn_00038a98(model, -1, -1, 0, 0, 2);
}

/* 0x0001ed34 -- draws car `car` (state block 0x48ae0 + car * 0x678) as seen from viewport vp
 * (guess: Car_Draw). kind indexes the race-kind table G_PTR(0x3f168) (0x18 bytes, +0 type;
 * type 0xb doubles the front wheels); wheels == 1 draws the wheels, extras == 1 the roof
 * attachments; lodA / lodB go to fn_00038a78/88 (clamped by distance); mirror != 0 is the
 * reflection pass (height mirrored about C+0x270); nofx skips shadow, sirens, lights and
 * smoke; fade (s0) scales every alpha.
 * Car state fields used: +0 base model id (body = +0, +1 glass, +2 glow, +3 shadow/under,
 * +4 windows, +5 low-detail, +6 wheel texture), +4 attachment model, +0x30, +0x58 smoke
 * amount, +0x5c brake lights, +0x6c damaged, +0x70/+0x78 camera owner, +0xe8 blink timer,
 * +0xec flash timer, +0xfc boost, +0x104 spoiler, +0x108 siren, +0x1e4 glow strength,
 * +0x248 yaw, +0x250/+0x268 attachment sway, +0x270 ground y, +0x274 car y, +0x2b0 position,
 * +0x2bc centre, +0x2c8 rotation, +0x334 Mtx34, +0x394 RGBA bytes, +0x398 wheels. */
void fn_0001ed34(int car, int kind, int vp, int wheels, int extras, unsigned lodA,
                 unsigned lodB, int mirror, int nofx, float fade)
{
    uint8_t *C = (uint8_t *)G(0x48ae0) + car * 0x678;
    uint8_t *vpb = (uint8_t *)G(0x5c160) + vp * 0x80;
    uint8_t *S, *main, *m;
    uint32_t col, fc;
    int bright, boost, inCar, flash, lowDetail, near, ownCam, type, model, base, i, a;
    int r, g, b, alpha, glowR, glowG, glowA, a155, a200, aCol, aBody, mirrorY;
    float f, gmax, z, znear, sc, t9;
    float tv[3], v[3], rot[3], mtx[12], q[3], o[3];

    if (G_S32(0x3f938) > G_S32(0x3f940))
        return;
    bright = (G_U8(0x3f928) & 1) || G_U8(0x3f98c) != 0;
    col = G_U32(0x3eb7c);
    r = AT_U8(C, 0x394);
    g = AT_U8(C, 0x395);
    b = AT_U8(C, 0x396);
    alpha = AT_U8(C, 0x397);
    f = AT_F32(C, 0x1e4);
    boost = AT_S32(C, 0xfc) > 1 && mirror == 0;
    /* underbody glow colour */
    if (boost) {
        if (f > 1.0f || G_F32(0x3e9f4) > 1.0f) {
            glowG = (int)(f * 25.0f / 10.0f + 10.0f);
            glowR = (int)(f * 105.0f / 10.0f + 150.0f);
        } else {
            glowR = 150;
            glowG = 10;
        }
        gmax = 255.0f;
    } else if (f > 1.0f) {
        glowG = (int)(f * 15.0f / 10.0f + 10.0f);
        glowR = (int)(f * 55.0f / 10.0f + 180.0f);
        gmax = 255.0f;
    } else {
        glowR = 180;
        glowG = 10;
        gmax = 200.0f;
    }
    /* blinking (e.g. after a respawn) */
    if (AT_S32(C, 0xe8) > 0 && AT_S32(C, 0xe8) % 10 > 5)
        return;
    glowA = (int)(gmax * fade);
    a155 = (int)(155.0f * fade);
    a200 = (int)(200.0f * fade);
    aCol = (int)((float)(col >> 24) * fade);
    aBody = (int)((float)alpha * fade);

    fn_00037fe8((uint8_t *)G_PTR(0x3f0ec) + vp * 0x30, (float *)(C + 0x2bc), tv);
    mirrorY = (int)(AT_F32(C, 0x270) - (AT_F32(C, 0x274) - AT_F32(C, 0x270)));
    inCar = 0;
    znear = 25.0f;
    if (AT_S32(vpb, 0x408) == 0xc && AT_S32(vpb, 0x410) == car) {
        /* this viewport is the car's own in-car camera */
        znear = -25.0f;
        tv[2] = -5.0f;
        inCar = 1;
    }
    z = tv[2];
    if (z > 0.0f || z < -1500.0f)
        return;
    if (mirror != 0 && z < -700.0f)
        return;
    if (fn_0001c7c0(AT_S32(C, 0) + 5, AT_F32(C, 0x2bc), AT_F32(C, 0x2c0), AT_F32(C, 0x2c4),
                    znear, G_F32(0x3e9c4)) != 1)
        return;

    G_U8(0x42760) = 'Y';
    G_U8(0x42764) = 'Z';
    G_U8(0x42768) = 'X';
    if (z < -500.0f) {
        if (lodB > 2)
            lodB = 2;
        if (lodA > 2)
            lodA = 2;
        if (z < -1000.0f && !(G_F32(0x3ea00) > 1.0f)) {
            if (lodA > 1)
                lodA = 1;
            if (lodB >= 2)
                lodB = 1;
        }
    }
    fn_00038a78(lodA);
    fn_00038a88(lodA);
    if (AT_S32(C, 0x6c) == 1 && !(G_F32(0x3ea00) > 1.0f) && !(G_F32(0x3ea0c) > 1.0f))
        fn_00004fb4((int)((float)r * 0.5f), (int)((float)g * 0.5f), (int)((float)b * 0.5f),
                    aBody);
    else
        fn_00004fb4(r, g, b, aBody);
    flash = 0;
    if (AT_S32(C, 0xec) >= 1 && mirror == 0) {
        fc = AT_S32(C, 0xec) % 10 >= 6 ? G_U32(0x3eb80) : G_U32(0x3eb7c);
        fn_00004fb4(fc & 0xff, (fc & 0xff00) >> 8, (fc >> 16) & 0xff, fc >> 24);
        flash = 1;
    }

    /* body: low-detail model C[0]+5 beyond 600 - 100 * G_S32(0x3e9b0) units */
    lowDetail = !bright && (float)(G_S32(0x3e9b0) * -100 + 600) < -z;
    base = AT_S32(C, 0);
    main = fn_00038818(base + (lowDetail ? 5 : 0));
    if (mirror != 0) {
        AT_F32(main, 0x27c) = AT_F32(C, 0x2c8);
        AT_F32(main, 0x280) = AT_F32(C, 0x2cc);
        AT_F32(main, 0x284) = 180.0f - AT_F32(C, 0x2d0);
        AT_F32(main, 0x270) = AT_F32(C, 0x2b0);
        AT_F32(main, 0x274) = (float)(mirrorY - 42);
        AT_F32(main, 0x278) = AT_F32(C, 0x2b8);
    } else {
        memcpy(main + 0x27c, C + 0x2c8, 12);
        memcpy(main + 0x270, C + 0x2b0, 12);
    }
    for (i = 1; i < 5; i++) {
        m = fn_00038818(base + i);
        memcpy(m + 0x27c, main + 0x27c, 12);
        memcpy(m + 0x270, main + 0x270, 12);
        memcpy(m + 0x288, main + 0x288, 36); /* +0x288 / +0x294 / +0x2a0 */
    }
    if (flash == 1)
        glDisable(GL_LIGHTING);
    fn_00038a98(base + (lowDetail ? 5 : 0), -1, -1, 0, 0, 2);
    if (flash == 1)
        glEnable(GL_LIGHTING);

    if (!lowDetail) {
        if (nofx == 0 && (z > -500.0f || boost || bright) && !inCar) {
            if (G_S32(0x3fac4) == 0 && mirror == 0) {
                /* drop shadow */
                m = fn_00038818(0x90);
                AT_F32(m, 0x27c) = AT_F32(C, 0x2c8) + -90.0f;
                AT_F32(m, 0x280) = AT_F32(C, 0x2cc);
                AT_F32(m, 0x284) = AT_F32(C, 0x2d0);
                AT_F32(m, 0x270) = AT_F32(C, 0x2b0) + -4.0f;
                AT_F32(m, 0x274) = AT_F32(C, 0x2b4) + -35.0f;
                AT_F32(m, 0x278) = AT_F32(C, 0x2b8) + -4.0f;
                AT_F32(m, 0x288) = 8.4f;
                AT_F32(m, 0x298) = 9.4f;
                AT_F32(m, 0x2a8) = 9.4f;
                fn_00004fb4(0, 0, 0, 0x9b);
                fn_00038a98(0x90, -1, -1, 0, 0, 2);
            }
            if (boost) {
                glDisable(GL_LIGHTING);
                m1c_model_color();
                fn_00038a78(1);
                fn_00038a88(1);
                fn_0001e8c0(0.0f);
                fn_00038a98(base + 3, 0xae, -1, 0, 0, 2);
                glEnable(GL_LIGHTING);
                fn_0001e8c0(G_F32(0x3e9f4));
            } else {
                fn_00004fb4(0xff, 0xff, 0xff, a200);
                fn_00038a98(base + 3, 0xae, -1, 0, 0, 2);
            }
        }
        fn_00038a78(lodB);
        fn_00038a88(lodB);
        near = z > -500.0f;
        if (bright || near) {
            fn_00004fb4(col & 0xff, (col & 0xff00) >> 8, (col >> 16) & 0xff, aCol);
            fn_00038a98(base + 1, -1, -1, 0, 0, 2);
            if (mirror == 0 && extras == 1 && !inCar) {
                S = (uint8_t *)G(0x47544) + kind * 0x18;
                type = AT_S32(S, 0);
                if (type == 0xb) {
                    v[0] = 0.0f;
                    v[1] = 8.0f;
                    v[2] = 10.0f;
                } else {
                    v[0] = 15.0f;
                    v[1] = 12.0f;
                    v[2] = 5.0f;
                }
                /* 0x7d if neither bright nor near (unreachable here) */
                model = 0x7c;
                if (type != 0xb &&
                    !(G_S32(0x3f9dc) == G_S32(0x3ebec) && (unsigned)type <= 8 &&
                      ((1u << type) & 0x190)))
                    model = AT_S32(C, 4);
                m1c_draw_attach(C, main, v, model);
                type = AT_S32(S, 0);
                if (G_S32(0x3f9dc) == G_S32(0x3ebec) && (unsigned)type <= 8 &&
                    ((1u << type) & 0x132) && AT_S32(C, 0x30) != -1) {
                    v[0] = -15.0f;
                    v[1] = 12.0f;
                    v[2] = 5.0f;
                    model = ((1u << type) & 0x190) ? 0x7c : AT_S32(G(0x47218), 0x10);
                    m1c_draw_attach(C, main, v, model);
                }
            }
            if (mirror == 0 && !inCar && (near || G_S32(0x3e9b0) < 2 || bright)) {
                fn_00004fb4(0x9b, 0x9b, 0xff, a155);
                fn_00038a98(base + 4, 0xae, -1, 0, 0, 2);
            }
        }
    }

    /* wheels (not when this car carries the active camera, mode 3) */
    ownCam = 0;
    if (AT_S32(C, 0x70) == 3) {
        uint8_t *w = (uint8_t *)G_PTR(0x3f0ac) + AT_S32(C, 0x78) * 0x34;
        ownCam = AT_S32(w, 0x18) == car;
    }
    if (!ownCam && wheels == 1 && !inCar) {
        fn_00004fb4(col & 0xff, (col & 0xff00) >> 8, (col >> 16) & 0xff, aCol);
        m = fn_00038818(0x73);
        if (z > -700.0f || bright) {
            AT_F32(m, 0x288) = AT_F32(C, 0x294) * AT_F32(main, 0x288);
            AT_F32(m, 0x298) = AT_F32(main, 0x298) * AT_F32(C, 0x294);
            AT_F32(m, 0x2a8) = AT_F32(main, 0x2a8) * AT_F32(C, 0x294);
            m1c_draw_wheel(m, C, 2, mirror, -16.0f, base + 6);
            m1c_draw_wheel(m, C, 3, mirror, -16.0f, base + 6);
            if (z > -500.0f || bright) {
                AT_F32(m, 0x288) = AT_F32(C, 0x290) * AT_F32(main, 0x288);
                AT_F32(m, 0x298) = AT_F32(main, 0x298) * AT_F32(C, 0x290);
                AT_F32(m, 0x2a8) = AT_F32(main, 0x2a8) * AT_F32(C, 0x290);
                S = (uint8_t *)G(0x47544) + kind * 0x18;
                m1c_draw_wheel(m, C, 0, mirror, -15.0f, base + 6);
                if (AT_S32(S, 0) == 0xb) {
                    AT_F32(m, 0x280) = AT_F32(C, 0x3a8) + 180.0f;
                    fn_00038a98(0x73, base + 6, -1, 0, 0, 2);
                }
                m1c_draw_wheel(m, C, 1, mirror, -15.0f, base + 6);
                if (AT_S32(S, 0) == 0xb) {
                    AT_F32(m, 0x280) = AT_F32(C, 0x404) + 180.0f;
                    fn_00038a98(0x73, base + 6, -1, 0, 0, 2);
                }
            }
        }
    }

    /* underbody glow */
    if ((z > -1500.0f || boost || bright) && !inCar) {
        fn_00004fb4(glowR, glowG, 0, glowA);
        if (AT_F32(C, 0x1e4) > 7.0f || boost) {
            fn_00038a78(1);
            fn_00038a88(1);
            glDisable(GL_LIGHTING);
            fn_0001e8c0(0.0f);
        }
        fn_00038a98(base + 2, 0xae, -1, 0, 0, 2);
        if (AT_F32(C, 0x1e4) > 7.0f || boost) {
            glEnable(GL_LIGHTING);
            fc = G_U32(0x3eb94);
            fn_00004fb4(fc & 0xff, (fc & 0xff00) >> 8, (fc >> 16) & 0xff, fc >> 24);
            fn_0001e8c0(G_F32(0x3e9f4));
        }
    }
    fn_00038a78(lodB);
    fn_00038a88(lodB);

    /* swinging rear spoiler / flag (model 0x7b, position kept in 0x47244) */
    if (AT_S32(C, 0x104) != 0 && G_S32(0x3f9dc) == G_S32(0x3ebe0) && G_U8(0x3fab8) == 0) {
        float s, t[3];
        fn_00004fb4(col & 0xff, (col & 0xff00) >> 8, (col >> 16) & 0xff, aCol);
        s = sinf((G_F32(0x3f9a0) + AT_F32(C, 0x268) * 20.0f) * M1C_D2R);
        v[0] = -(s * G_F32(0x3f9a4));
        v[1] = 25.0f;
        v[2] = -60.0f;
        fn_00037fe8(C + 0x334, v, t);
        fn_00037dec((float *)(C + 0x2b0), t, (float *)G(0x47244));
        m = fn_00038818(0x7b);
        AT_F32(m, 0x284) = AT_F32(main, 0x27c);
        AT_F32(m, 0x280) = AT_F32(main, 0x280) + s * G_F32(0x3f9a4) + 90.0f;
        AT_F32(m, 0x27c) = AT_F32(main, 0x284) - s * G_F32(0x3f9a4);
        memcpy(m + 0x270, G(0x47244), 12);
        AT_F32(m, 0x288) = 0.5f;
        AT_F32(m, 0x298) = 0.5f;
        AT_F32(m, 0x2a8) = 0.5f;
        fn_00038a98(0x7b, -1, -1, 0, 0, 2);
    }
    if (nofx != 0)
        goto done;

    /* police siren bar + flashing light (race mode G_S32(0x3ebe4)) */
    if (G_S32(0x3f9dc) == G_S32(0x3ebe4) && AT_S32(C, 0x108) != 0) {
        float t[3], p[3];
        fn_00004fb4(col & 0xff, (col & 0xff00) >> 8, (col >> 16) & 0xff, aCol);
        v[0] = 0.0f;
        v[1] = -3.0f;
        v[2] = 0.0f;
        fn_00037fe8(C + 0x334, v, t);
        fn_00037dec((float *)(C + 0x2b0), t, p);
        m = fn_00038818(0x75);
        memcpy(m + 0x27c, main + 0x27c, 12);
        memcpy(m + 0x270, p, 12);
        AT_F32(m, 0x288) = 1.01f;
        AT_F32(m, 0x298) = 1.0f;
        AT_F32(m, 0x2a8) = 1.0f;
        if (mirror != 0)
            AT_F32(m, 0x274) = (float)(mirrorY - 42);
        fn_00038a98(0x75, -1, -1, 0, 0, 2);
        v[0] = 0.0f;
        v[1] = 30.0f;
        v[2] = 3.0f;
        fn_00037fe8(C + 0x334, v, t);
        fn_00037dec((float *)(C + 0x2b0), t, p);
        m = fn_00038818(0x77);
        memcpy(m + 0x27c, main + 0x27c, 12);
        memcpy(m + 0x270, p, 12);
        AT_F32(m, 0x288) = 0.5f;
        AT_F32(m, 0x298) = 0.5f;
        AT_F32(m, 0x2a8) = 0.5f;
        if (mirror != 0) {
            AT_F32(m, 0x274) = (AT_F32(C, 0x274) + AT_F32(C, 0x270)) - p[1];
            AT_F32(m, 0x280) = AT_F32(main, 0x280) + 180.0f;
            fn_00038a98(0x77, -1, -1, 0, 0, 2);
        } else {
            fn_00038a98(0x77, -1, -1, 0, 0, 2);
            fn_000042b0(1, 0x201 /* GL_LESS */, 0);
        }
        /* camera-facing light sprite, red / blue alternating with timer G_F32(0x3f9a8) */
        m = fn_00038818(0x84);
        AT_F32(m, 0x27c) = G_F32(0x47100);
        AT_F32(m, 0x280) = G_F32(0x47104) + 180.0f;
        AT_F32(m, 0x284) = 0.0f;
        t9 = G_F32(0x3f9a8);
        if (t9 > 180.0f) {
            sc = sinf(((t9 + -180.0f) + (t9 + -180.0f)) * M1C_D2R) + 1.5f;
            a = (int)(sc * 150.0f);
            if (a > 255)
                a = 255;
            v[0] = -15.0f;
            fn_00004fb4(0x32, 0x32, 0xff, a & 0xff);
        } else {
            sc = sinf((t9 + t9) * M1C_D2R) + 1.5f;
            a = (int)(sc * 150.0f);
            if (a > 255)
                a = 255;
            v[0] = 15.0f;
            fn_00004fb4(0xff, 0x32, 0x32, a & 0xff);
        }
        v[1] = 36.0f;
        v[2] = 3.0f;
        AT_F32(m, 0x288) = sc;
        AT_F32(m, 0x298) = sc;
        AT_F32(m, 0x2a8) = sc;
        m1c_sprite_pos(m, C, v, mirror);
        glDisable(GL_LIGHTING);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        fn_00038a98(0x84, -1, -1, 0, 0, 2);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LIGHTING);
    }
    if (mirror != 0)
        goto done;

    /* brake lights: two additive camera-facing sprites */
    if (AT_S32(C, 0x5c) >= 1) {
        fn_000042b0(1, 0x201 /* GL_LESS */, 0);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        m = fn_00038818(0x84);
        AT_F32(m, 0x27c) = G_F32(0x47100);
        AT_F32(m, 0x280) = G_F32(0x47104) + 180.0f;
        AT_F32(m, 0x284) = 0.0f;
        sc = sinf(G_F32(0x3f9a8) * 4.0f * M1C_D2R) * 0.5f + 0.5f;
        fn_00004fb4(0xff, (int)(sc * 150.0f + 100.0f), 0x32, (int)(sc * 100.0f + 150.0f));
        AT_F32(m, 0x288) = sc;
        AT_F32(m, 0x298) = sc;
        AT_F32(m, 0x2a8) = sc;
        S = (uint8_t *)G(0x47544) + kind * 0x18;
        v[0] = (unsigned)(AT_S32(S, 0) - 7) <= 2 ? -13.0f : -19.0f;
        v[1] = -26.0f;
        v[2] = -49.0f;
        m1c_sprite_pos(m, C, v, 0);
        glDisable(GL_LIGHTING);
        fn_00038a98(0x84, -1, -1, 0, 0, 2);
        v[0] = (unsigned)(AT_S32(S, 0) - 7) <= 2 ? 13.0f : 19.0f;
        m1c_sprite_pos(m, C, v, 0);
        fn_00038a98(0x84, -1, -1, 0, 0, 2);
        glEnable(GL_LIGHTING);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    /* exhaust smoke puffs (model 0x7f), amount C+0x58 */
    if (AT_F32(C, 0x58) > 0.0f && G_U8(0x3fab8) == 0) {
        m = fn_00038818(0x7f);
        AT_F32(m, 0x288) = AT_F32(C, 0x58) * 6.0f;
        AT_F32(m, 0x298) = 0.125f;
        AT_F32(m, 0x2a8) = 0.125f;
        memcpy(rot, C + 0x2c8, 12);
        rot[1] = AT_F32(C, 0x248);
        fn_00004620(M1C_P2I(mtx), M1C_P2I(rot));
        for (i = 0; i < (int)(AT_F32(C, 0x58) * 10.0f); i++) {
            q[1] = (float)(fn_00004400(40) + 20);
            q[0] = (float)(fn_00004400(60) - 30);
            q[2] = q[1] / 3.0f + -100.0f;
            fn_00037fe8(mtx, q, o);
            memcpy(m + 0x270, C + 0x2bc, 12);
            fn_00037dec((float *)(m + 0x270), o, (float *)(m + 0x270));
            memcpy(m + 0x27c, rot, 12);
            AT_F32(m, 0x280) = AT_F32(m, 0x280) + 90.0f;
            AT_F32(m, 0x27c) = AT_F32(m, 0x27c) + -90.0f;
            a = (int)((200.0f - fabsf(q[1]) - fabsf(q[0])) * AT_F32(C, 0x58));
            if (a != 0) {
                fc = G_U32(0x3eb7c);
                fn_00004fb4(fc & 0xff, (fc & 0xff00) >> 8, (fc >> 16) & 0xff, a);
                fn_00038a98(0x7f, 0x9a, -1, 0, 0, 0);
            }
        }
    }
    fn_000042b0(1, 0x201 /* GL_LESS */, 1);

done:
    G_U8(0x42760) = 'Z';
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';
    G_S32(0x3f938) = G_S32(0x3f938) + 1;
}

/* 0x000209e8 -- registers the race touch-control areas with fn_00004500 (x, y, radius, id,
 * enabled, icon, flag) for the current screen size W = G_S32(0x40368), H = G_S32(0x40374)
 * (guess: Race_SetupTouchControls). Mode G_S32(0x3f848) == 9 uses a reduced layout;
 * G_U8(0x3e9a0) & 1 selects the small control size. */
void fn_000209e8(void)
{
    int en, small;
    float r;

    if (G_S32(0x3f848) == 9) {
        fn_00004500((float)(G_S32(0x40368) - 0x80), (float)(G_S32(0x40374) - 0x20), 100.0f, 2,
                    1, 8, 0);
        fn_00004500(128.0f, (float)(G_S32(0x40374) - 0x20), 100.0f, 1, 1, 7, 0);
        fn_00004500((float)(G_S32(0x40368) - 0x80), 32.0f, 100.0f, 0xb, 1, 4, 0);
        fn_00004500(32.0f, 32.0f, 32.0f, 0xe, 0, 4, 0);
        return;
    }
    /* steering / accelerate cluster in the bottom-right corner */
    if (G_U8(0x3e9a0) & 1) {
        fn_00004500((float)(G_S32(0x40368) - 0x20), (float)(G_S32(0x40374) - 0x60), 32.0f, 7,
                    1, 8, 0);
        fn_00004500((float)(G_S32(0x40368) - 0x60), (float)(G_S32(0x40374) - 0x20), 32.0f, 1,
                    1, 7, 1);
        fn_00004500((float)(G_S32(0x40368) - 0x20), (float)(G_S32(0x40374) - 0x20), 32.0f, 2,
                    1, 8, 1);
    } else {
        fn_00004500((float)(G_S32(0x40368) - 0x30), (float)(G_S32(0x40374) - 0x94), 48.0f, 7,
                    1, 8, 0);
        fn_00004500((float)(G_S32(0x40368) - 0x94), (float)(G_S32(0x40374) - 0x30), 48.0f, 1,
                    1, 7, 1);
        fn_00004500((float)(G_S32(0x40368) - 0x30), (float)(G_S32(0x40374) - 0x30), 48.0f, 2,
                    1, 8, 1);
    }
    fn_00004500((float)(G_S32(0x4036c) / 2), (float)G_S32(0x40378), 64.0f, 3, 1, 2, 0);
    fn_00004500((float)(G_S32(0x40368) - G_S32(0x4036c) / 2), (float)G_S32(0x40378), 64.0f, 4,
                1, 3, 0);
    if ((G_S32(0x3f848) == 4 && G_S32(0x3fa24) == 0x18) || (G_U8(0x3f90c) & 1) ||
        (G_S32(0x3fa24) == 0x14 && G_S32(0x3f9dc) == G_S32(0x3ebe4)) || G_S32(0x3f848) == 5 ||
        G_S32(0x3f848) == 8) {
        fn_00004500((float)G_S32(0x4036c), (float)(G_S32(0x40374) - 0x6e), 96.0f, 5, 1, 5, 0);
        fn_00004500((float)G_S32(0x4036c), 110.0f, 96.0f, 6, 1, 4, 0);
        small = G_U8(0x3e9a0) & 1;
        r = small ? 80.0f : 128.0f;
        en = G_S32(0x3f848) == 5 && G_U8(0x47250) == 0;
        fn_00004500(96.0f, (float)(G_S32(0x40374) - 0x40), r, 0xc, en, 4, 1);
    } else {
        fn_00004500((float)(G_S32(0x40368) - 0x86), (float)(G_S32(0x40374) - 0x6e), 44.0f, 5, 1,
                    5, 0);
        fn_00004500((float)(G_S32(0x40368) - 0x86), 110.0f, 44.0f, 6, 1, 4, 0);
        fn_00004500((float)G_S32(0x4036c), (float)G_S32(0x40378), 80.0f, 0xd, 1, 4, 0);
    }
    fn_00004500((float)G_S32(0x4036c), (float)(G_S32(0x40374) - 0x60), 128.0f, 10, 1, 6, 0);
    fn_00004500((float)(G_S32(0x40368) - 0x20), 32.0f, 32.0f, 0xb, 1, 4, 1);
    fn_00004500(32.0f, 32.0f, 32.0f, 0xe, 1, 6, 1);
}

/* 0x00020f08 -- pause handling after a frame: when fn_00004540() reports a pause request
 * (or the paused flag G_U8(0x3e914) is already set) draws the pause screen (fn_00020f70);
 * then fn_0000436c(0) / fn_0000442c() (guess: Race_HandlePause). */
void fn_00020f08(void)
{
    if ((uint8_t)fn_00004540() == 0 && G_U8(0x5c1e2) != 0)
        fn_00004570();
    if ((uint8_t)fn_00004540() != 0) {
        G_U8(0x3e914) = 1;
        fn_00020f70();
    } else if (G_U8(0x3e914) != 0) {
        fn_00020f70();
    }
    fn_0000436c(0);
    fn_0000442c();
}

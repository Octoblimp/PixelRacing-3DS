/* Track cell renderer: draws one 812 x 812 cell of the track grid. [0x1621c, 0x1c7c0)
 *
 * Track maps (byte grids, row stride 20, index x * 20 + y):
 *   0x44000  height (units of 100)       0x44800  tile type (0 flat, 1..6 slopes, 7/8 ramps,
 *   0x44400  object id (0 none)                    9/10 water, 11 = override from 0x3fab8)
 *   0x46400  level / section number
 *   0x45000  u32 terrain colour (row stride 0x50)
 *   0x42968, 0x42fa8  per-cell f32 (row stride 0x50): object height / velocity
 * Height and tile reads clamp the cell to [0, max] (max x 0x3f8a4, max y 0x3f8a0); object and
 * level reads return 0 outside the map.
 *
 * Tables: 0x47770 int[] ground model slots, 0x47454 int[] object/wall model slots (+20 = far LOD),
 * 0x473b4 int[] object id -> object kind. Current model pointers: 0x47f90 (ground), 0x473b0
 * (objects and walls).
 */
#include "ios.h"

#define P2I(p) ((int)(intptr_t)(p))

#define M1621C_TILE 812.0f
#define M1621C_HALF 406.0f
#define M1621C_DEG 0.0174533f

/* Per-call cell state shared by the passes. */
typedef struct {
    int dx, dy;     /* cell offset from the camera cell (the parameters) */
    int cx, cy;     /* absolute cell */
    float px, pz;   /* cell origin, world units */
    float dist;     /* LOD bias - view-space z of the cell origin */
    float hf;       /* cell height */
    int type;       /* tile type */
    int lv;         /* level-map value of the cell */
    int last;       /* 0x3f8e0 set and the cell is in the last section(s) */
} m1621c_cell;

/* ---- map access ---------------------------------------------------------------------------- */

static int m1621c_cx(int x)
{
    if (x < 0)
        return 0;
    return G_S32(0x3f8a4) < x ? G_S32(0x3f8a4) : x;
}

static int m1621c_cy(int y)
{
    if (y < 0)
        return 0;
    return G_S32(0x3f8a0) < y ? G_S32(0x3f8a0) : y;
}

static int m1621c_inside(int x, int y)
{
    return (x | y) >= 0 && x <= G_S32(0x3f8a4) && y <= G_S32(0x3f8a0);
}

/* Height map (clamped), as float. */
static float m1621c_h(int x, int y)
{
    return (float)G_U8(0x44000u + m1621c_cx(x) * 20 + m1621c_cy(y));
}

/* Tile-type map (clamped). */
static int m1621c_tile(int x, int y)
{
    return G_U8(0x44800u + m1621c_cx(x) * 20 + m1621c_cy(y));
}

/* Terrain colour (clamped). */
static uint32_t m1621c_tcol(int x, int y)
{
    return G_U32(0x45000u + m1621c_cx(x) * 0x50 + m1621c_cy(y) * 4);
}

/* Level map, 0 outside. */
static int m1621c_level(int x, int y)
{
    if (!m1621c_inside(x, y))
        return 0;
    return G_U8(0x46400u + x * 20 + y);
}

/* Object map, 0 outside. */
static int m1621c_obj(int x, int y)
{
    if (!m1621c_inside(x, y))
        return 0;
    return G_U8(0x44400u + x * 20 + y);
}

static int m1621c_objkind(int id)
{
    return G_S32(0x473b4u + id * 4);
}

/* ---- model helpers ------------------------------------------------------------------------- */

static void m1621c_ident(uint8_t *m)
{
    AT_F32(m, 0x288) = 1.0f;
    AT_F32(m, 0x28c) = 0.0f;
    AT_F32(m, 0x290) = 0.0f;
    AT_F32(m, 0x294) = 0.0f;
    AT_F32(m, 0x298) = 1.0f;
    AT_F32(m, 0x29c) = 0.0f;
    AT_F32(m, 0x2a0) = 0.0f;
    AT_F32(m, 0x2a4) = 0.0f;
    AT_F32(m, 0x2a8) = 1.0f;
}

static void m1621c_rot(uint8_t *m, float x, float y, float z)
{
    AT_F32(m, 0x27c) = x;
    AT_F32(m, 0x280) = y;
    AT_F32(m, 0x284) = z;
}

static void m1621c_draw(int slot, int tex, int mode)
{
    fn_00038a98(slot, tex, -1, 0, 0, mode);
}

static void m1621c_color(uint32_t c)
{
    fn_00004fb4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24);
}

/* Terrain colour scaled by the light factor 0x3fac8, alpha from 0x3facc. */
static void m1621c_color_lit(uint32_t c)
{
    float k = G_F32(0x3fac8);
    fn_00004fb4((int)((float)(c & 0xff) * k), (int)((float)((c >> 8) & 0xff) * k),
                (int)((float)((c >> 16) & 0xff) * k), (int)(G_F32(0x3facc) * 255.0f));
}

/* ---- thin wrappers for engine calls ---------------------------------------------------------- */

/* fn_0001c7c0: frustum test of a model at (x, y, z) within the z range (near, far). */
static int m1621c_visible(int slot, float x, float y, float z, float zmin, float zmax)
{
    return fn_0001c7c0(slot, x, y, z, zmin, zmax);
}

/* fn_0001cc18: facing test of the current model (position, yaw) against the camera. */
static int m1621c_facing(const uint8_t *m)
{
    return fn_0001cc18(AT_F32(m, 0x270), AT_F32(m, 0x274), AT_F32(m, 0x278), AT_F32(m, 0x280));
}

/* fn_0001e8c0: fog on/off; -1 disables, else the fog setting. */
static void m1621c_fog(float v)
{
    fn_0001e8c0(v);
}

/* fn_0001ce34: slope of a tile: writes the height offset and the two shear terms. */
static void m1621c_slope(int type, int x, int y, int h, float *dh, float *s1, float *s2)
{
    fn_0001ce34(type, x, y, h, dh, s1, s2);
}

/* fn_0001da58: spawns a particle at world position pos for camera cam into slot out. */
static void m1621c_spawn(int cam, float *pos, float *out)
{
    fn_0001da58(cam, pos, out);
}

/* ---- pass 1: ground -------------------------------------------------------------------------- */

static void m1621c_ground(const m1621c_cell *c)
{
    int *tab = (int *)G(0x47770);
    int cx = c->cx, cy = c->cy, type = c->type;
    uint8_t *m;
    uint32_t col, tc, r, g, b;
    float y;
    int hh, saved, lit;

    switch (type) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 11: {
        float dh = 0.0f, s1 = 0.0f, s2 = 0.0f;
        int hi = 0, model2;
        int slope = (unsigned)(type - 1) < 6;

        m = fn_00038818(tab[0]);
        G_PTR(0x47f90) = m;
        AT_F32(m, 0x280) = 0.0f;
        if (type == 2 || type == 4)
            AT_F32(m, 0x280) = -90.0f;
        else if (type == 6)
            AT_F32(m, 0x280) = -180.0f;
        else if (type == 5)
            AT_F32(m, 0x280) = -270.0f;
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        if (!G_U8(0x3fab8))
            hi = (int)m1621c_h(cx, cy);
        m1621c_slope(type, cx, cy, hi, &dh, &s1, &s2);
        y = AT_F32(m, 0x2ac) + (float)hi * 100.0f + dh * 0.5f;
        if (type == 2) {
            AT_F32(m, 0x2a4) = -s1;
            AT_F32(m, 0x28c) = -s2;
        } else {
            AT_F32(m, 0x2a4) = s2;
            AT_F32(m, 0x28c) = s1;
        }
        /* flat and entirely above the camera: nothing visible */
        if (AT_F32(m, 0x2a4) == 0.0f && AT_F32(m, 0x28c) == 0.0f &&
            G_F32(0x470f8) + 1.0f <= y && !(G_U8(0x3fab0) & 1))
            return;
        fn_0001cae8(&col, cx, cy);
        if (slope)
            fn_000042b0(1, 0x201, 0);
        m1621c_color(col);
        AT_F32(m, 0x274) = y;
        m1621c_draw(tab[0], type == 11 ? tab[11] : tab[0], G_S32(0x3ea24));
        if (slope) {
            /* slope overlay */
            fn_000042b0(1, 0x201, 1);
            m1621c_color_lit(m1621c_tcol(cx, cy));
            model2 = tab[type];
            if (!G_U8(0x3fab8) && G_S32(0x3f8e0) != 0 && (unsigned)(type - 1) < 2 &&
                c->lv >= G_S32(0x3f948) - 1) {
                /* finish-section arrows, turned by the race direction */
                model2 = tab[9];
                AT_F32(m, 0x280) += (float)(G_S32(0x3e9d0) * 90 - 90);
                if (G_S32(0x3e9d0) == -1) {
                    if (type == 2) {
                        AT_F32(m, 0x2a4) = s1;
                        AT_F32(m, 0x28c) = s2;
                    } else {
                        AT_F32(m, 0x2a4) = -s2;
                        AT_F32(m, 0x28c) = -s1;
                    }
                }
            }
            AT_F32(m, 0x274) = y;
            m1621c_draw(tab[0], model2, G_S32(0x3ea24));
        }
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        break;
    }

    case 7: /* ramp along x */
        m = fn_00038818(tab[0]);
        G_PTR(0x47f90) = m;
        AT_F32(m, 0x280) = 0.0f;
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        hh = (int)m1621c_h(cx, cy + 1);
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hh * 100.0f;
        fn_0001cae8(&col, cx, cy);
        tc = m1621c_tcol(cx, cy);
        m1621c_color(col);
        fn_000042b0(1, 0x201, 0);
        m1621c_draw(tab[0], tab[0], G_S32(0x3ea24));
        m1621c_color_lit(tc);
        fn_000042b0(1, 0x201, 1);
        m1621c_draw(tab[0], tab[1], G_S32(0x3ea24));

        saved = G_U8(0x48294);
        fn_00038a78(1);
        fn_00038a88(1);
        m1621c_rot(m, 180.0f, 90.0f, 0.0f);
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hh * 100.0f;
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        glDisable(GL_LIGHTING);
        m1621c_draw(tab[0], tab[7], G_S32(0x3ea24));
        glEnable(GL_LIGHTING);

        m1621c_rot(m, 0.0f, 0.0f, 0.0f);
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        hh = (int)m1621c_h(cx + 1, cy);
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hh * 100.0f;
        fn_00038a78(saved);
        fn_00038a88(saved);

        r = col & 0xff;
        g = (col >> 8) & 0xff;
        b = (col >> 16) & 0xff;
        fn_00004fb4(r, g, b, col >> 24);
        if (G_F32(0x3ea00) <= 0.0f) {
            r >>= 1;
            g >>= 1;
            b >>= 1;
        }
        fn_00004fb4(r, g, b, 0xff);
        fn_000042b0(1, 0x201, 0);
        m1621c_draw(tab[0], tab[0], G_S32(0x3ea24));
        r = tc & 0xff;
        g = (tc >> 8) & 0xff;
        b = (tc >> 16) & 0xff;
        if (G_F32(0x3ea00) <= 0.0f) {
            r >>= 1;
            g >>= 1;
            b >>= 1;
        }
        fn_00004fb4(r, g, b, 0xff);
        fn_000042b0(1, 0x201, 1);
        AT_F32(m, 0x280) = -90.0f;
        m1621c_draw(tab[0], tab[2], G_S32(0x3ea24));
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        break;

    case 8: /* ramp along y */
        m = fn_00038818(tab[0]);
        G_PTR(0x47f90) = m;
        m1621c_rot(m, 0.0f, 0.0f, 0.0f);
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        hh = (int)m1621c_h(cx + 1, cy);
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hh * 100.0f;
        fn_0001cae8(&col, cx, cy);
        tc = m1621c_tcol(cx, cy);
        m1621c_color(col);
        fn_000042b0(1, 0x201, 0);
        m1621c_draw(tab[0], tab[0], 2);
        m1621c_color_lit(tc);
        fn_000042b0(1, 0x201, 1);
        AT_F32(m, 0x280) = -90.0f;
        m1621c_draw(tab[0], tab[2], 2);

        saved = G_U8(0x48294);
        fn_00038a78(1);
        fn_00038a88(1);
        AT_F32(m, 0x280) = 0.0f;
        AT_F32(m, 0x27c) = 180.0f;
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hh * 100.0f;
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        fn_00004fb4(0xff, 0xff, 0xff, 0xff);
        glDisable(GL_LIGHTING);
        m1621c_draw(tab[0], tab[7], 2);
        glEnable(GL_LIGHTING);

        m1621c_rot(m, 0.0f, 0.0f, 0.0f);
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        hh = (int)m1621c_h(cx, cy + 1);
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hh * 100.0f;
        fn_00038a78(saved);
        fn_00038a88(saved);

        lit = G_F32(0x3ea00) > 0.0f;
        r = col & 0xff;
        g = (col >> 8) & 0xff;
        b = (col >> 16) & 0xff;
        if (!lit) {
            r >>= 1;
            g >>= 1;
            b >>= 1;
        }
        fn_00004fb4(r, g, b, 0xff);
        fn_000042b0(1, 0x201, 0);
        m1621c_draw(tab[0], tab[0], 2);
        /* original quirk: lit uses the cell colour again, unlit the raw terrain colour */
        if (!lit) {
            r = tc & 0xff;
            g = (tc >> 8) & 0xff;
            b = (tc >> 16) & 0xff;
        }
        fn_00004fb4(r, g, b, 0xff);
        fn_000042b0(1, 0x201, 1);
        m1621c_draw(tab[0], tab[1], 2);
        m1621c_color(G_U32(0x3eb94));
        break;

    default: /* 9, 10: water, drawn by the object pass */
        break;
    }
}

/* ---- pass 2: walls and rails ----------------------------------------------------------------- */

/* Wall model: near LOD when close, outside the last sections and the neighbour (lx, ly) is not
 * in the last sections either; else the far LOD (+20). */
static int m1621c_wall_model(const m1621c_cell *c, int lx, int ly)
{
    int *t454 = (int *)G(0x47454);

    if (c->dist < 2000.0f && !c->last && m1621c_level(lx, ly) < G_S32(0x3f948) - 1)
        return t454[0];
    return t454[20];
}

/* Sheared rail along a slope tile that rises from this cell's height to hi, facing a lower
 * neighbour of height side. Returns 1 when drawn. */
static int m1621c_rail(const m1621c_cell *c, float hi, float side, float shear, int lx, int ly,
                       float x, float z, float yaw)
{
    float base = m1621c_h(c->cx, c->cy);
    float d = hi - base;
    float mid = d * 100.0f * 0.5f;
    float bh = base * 100.0f;
    int slot = m1621c_wall_model(c, lx, ly);
    uint8_t *m = fn_00038818(slot);

    G_PTR(0x473b0) = m;
    AT_F32(m, 0x28c) = d * shear / M1621C_TILE;
    AT_F32(m, 0x298) = hi - side + 0.005f;
    AT_F32(m, 0x288) = 1.005f;
    AT_F32(m, 0x270) = x;
    AT_F32(m, 0x274) = (mid + bh) - AT_F32(m, 0x2ac) * AT_F32(m, 0x298);
    AT_F32(m, 0x278) = z;
    AT_F32(m, 0x280) = yaw;
    if (m1621c_facing(m) == 1) {
        m1621c_draw(slot, -1, 2);
        return 1;
    }
    return 0;
}

/* Vertical side wall toward the neighbour (nx, ny). On a ramp (edge) the wall uses model
 * [1] scaled to half the height difference ha - hb; otherwise it fills from the neighbour's
 * height up to this cell's. */
static void m1621c_side(const m1621c_cell *c, int nx, int ny, int edge, float ha, float hb,
                        float x, float z, float yaw)
{
    int *t454 = (int *)G(0x47454);
    int slot;
    uint8_t *m;
    float y;

    if (edge) {
        slot = t454[1];
        m = fn_00038818(slot);
        G_PTR(0x473b0) = m;
        AT_F32(m, 0x28c) = 0.0f;
        AT_F32(m, 0x298) = (ha - hb) * 0.5f;
        AT_F32(m, 0x288) = 1.005f;
        float s = AT_F32(m, 0x298) * AT_F32(m, 0x2ac);
        y = m1621c_h(c->cx, c->cy) * 100.0f + (s + s);
    } else {
        slot = m1621c_wall_model(c, nx, ny);
        m = fn_00038818(slot);
        G_PTR(0x473b0) = m;
        AT_F32(m, 0x28c) = 0.0f;
        y = c->hf * 100.0f;
        AT_F32(m, 0x298) = c->hf - m1621c_h(nx, ny) + 0.005f;
        AT_F32(m, 0x288) = 1.005f;
    }
    AT_F32(m, 0x270) = x;
    AT_F32(m, 0x274) = y - AT_F32(m, 0x2ac) * AT_F32(m, 0x298);
    AT_F32(m, 0x278) = z;
    AT_F32(m, 0x280) = yaw;
    if (m1621c_facing(m) != 0 || edge)
        m1621c_draw(slot, -1, 2);
}

static void m1621c_walls(const m1621c_cell *c)
{
    int cx = c->cx, cy = c->cy, type = c->type, dx = c->dx, dy = c->dy;
    float px = c->px, pz = c->pz, hf = c->hf, h0;
    int skip_xn = 0, skip_xp = 0, skip_yn = 0, skip_yp = 0;
    uint32_t col;

    fn_0001cae8(&col, 0x13, 0);
    m1621c_color(col);

    /* rails along the raised edge of a slope, on the side facing the camera */
    h0 = m1621c_h(cx, cy);
    if (type == 2) {
        float hx = m1621c_h(cx + 1, cy);
        if (h0 < hx) {
            if (dy < 0 && m1621c_h(cx, cy + 1) < hx) {
                m1621c_rail(c, hx, m1621c_h(cx, cy + 1), 100.0f, cx, cy + 1, px,
                            pz + M1621C_HALF + 11.0f, 0.0f);
                skip_yp = 1;
            }
            if (dy > 0 && m1621c_h(cx, cy - 1) < hx) {
                m1621c_rail(c, hx, m1621c_h(cx, cy - 1), -100.0f, cx, cy - 1, px,
                            pz - M1621C_HALF - 11.0f, 180.0f);
                skip_yn = 1;
            }
        }
        hx = m1621c_h(cx - 1, cy);
        if (h0 < hx) {
            if (dy < 0 && m1621c_h(cx, cy + 1) < hx) {
                m1621c_rail(c, hx, m1621c_h(cx, cy + 1), -100.0f, cx, cy + 1, px,
                            pz + M1621C_HALF + 11.0f, 0.0f);
                skip_yp = 1;
            }
            if (dy > 0 && m1621c_h(cx, cy - 1) < hx) {
                m1621c_rail(c, hx, m1621c_h(cx, cy - 1), 100.0f, cx, cy - 1, px,
                            pz - M1621C_HALF - 11.0f, 180.0f);
                skip_yn = 1;
            }
        }
    } else if (type == 1) {
        float hy = m1621c_h(cx, cy + 1);
        if (h0 < hy) {
            if (dx < 0 && m1621c_h(cx + 1, cy) < hy) {
                if (m1621c_rail(c, hy, m1621c_h(cx + 1, cy), -100.0f, cx + 1, cy,
                                px + M1621C_HALF + 11.0f, pz, 90.0f))
                    skip_xp = 1;
            }
            if (dx > 0 && m1621c_h(cx - 1, cy) < hy) {
                m1621c_rail(c, hy, m1621c_h(cx - 1, cy), 100.0f, cx - 1, cy,
                            px - M1621C_HALF - 11.0f, pz, -90.0f);
                skip_xn = 1;
            }
        }
        hy = m1621c_h(cx, cy - 1);
        if (h0 < hy) {
            if (dx < 0 && m1621c_h(cx + 1, cy) < hy) {
                if (m1621c_rail(c, hy, m1621c_h(cx + 1, cy), 100.0f, cx + 1, cy,
                                px + M1621C_HALF + 11.0f, pz, 90.0f))
                    skip_xp = 1;
            }
            if (dx > 0 && m1621c_h(cx - 1, cy) < hy) {
                m1621c_rail(c, hy, m1621c_h(cx - 1, cy), -100.0f, cx - 1, cy,
                            px - M1621C_HALF - 11.0f, pz, -90.0f);
                skip_xn = 1;
            }
        }
    }

    /* side walls down to lower neighbours; slopes only next to a matching ramp */
    if (!skip_xn && !(type < 7 && ((1 << type) & 0x64) && m1621c_tile(cx - 1, cy) != 8) &&
        (dx > 0 || (type == 7 && dx >= 0)) && (m1621c_h(cx - 1, cy) < hf || type == 7))
        m1621c_side(c, cx - 1, cy, type == 7, m1621c_h(cx, cy + 1), m1621c_h(cx - 1, cy),
                    px - M1621C_HALF - 11.0f, pz, -90.0f);
    if (!skip_xp && !(((unsigned)(type - 2) < 2 || type == 4) && m1621c_tile(cx + 1, cy) != 8) &&
        (dx < 0 || (type == 7 && dx < 1)) && (m1621c_h(cx + 1, cy) < hf || type == 7))
        m1621c_side(c, cx + 1, cy, type == 7, m1621c_h(cx, cy - 1), m1621c_h(cx + 1, cy),
                    px + M1621C_HALF + 11.0f, pz, 90.0f);
    if (!skip_yn && !(type < 6 && ((1 << type) & 0x2a) && m1621c_tile(cx, cy - 1) != 7) &&
        (dy > 0 || (type == 8 && dy >= 0)) && (m1621c_h(cx, cy - 1) < hf || type == 8))
        m1621c_side(c, cx, cy - 1, type == 8, m1621c_h(cx - 1, cy), m1621c_h(cx, cy + 1), px,
                    pz - M1621C_HALF - 11.0f, 180.0f);
    if (!skip_yp && !(type < 7 && ((1 << type) & 0x52) && m1621c_tile(cx, cy + 1) != 7) &&
        (dy < 0 || (type == 8 && dy < 1)) && (m1621c_h(cx, cy + 1) < hf || type == 8))
        m1621c_side(c, cx, cy + 1, type == 8, m1621c_h(cx + 1, cy), m1621c_h(cx, cy - 1), px,
                    pz + M1621C_HALF + 11.0f, 0.0f);

    m1621c_color(G_U32(0x3eb94));
}

/* ---- pass 3: objects, finish arrows, water --------------------------------------------------- */

/* fn_0000d800: ground height under (x, y, z) (no car). */
static float m1621c_groundh(float x, float y, float z, int *out)
{
    return fn_0000d800(-1, x, y, z, out);
}

static void *m1621c_cam_mtx(void)
{
    return (uint8_t *)G(0x46fa0) + G_S32(0x470d4) * 0x30;
}

/* Object model at the cell origin, standing on the cell height. */
static uint8_t *m1621c_place(const m1621c_cell *c, int slot)
{
    uint8_t *m = fn_00038818(slot);

    G_PTR(0x473b0) = m;
    AT_F32(m, 0x270) = c->px;
    AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + c->hf * 100.0f;
    AT_F32(m, 0x278) = c->pz;
    return m;
}

/* Kinds 4 and 12: a model swinging on a circle (sheared, height scaled by 3 / (k + 2)),
 * tinted with the cell's object colour. */
static void m1621c_swing(const m1621c_cell *c, int slot, int k, int lift)
{
    int a = (int)(G_F32(0x5c168) * 20.0f + (float)(k * 45));
    uint8_t *m = fn_00038818(slot);
    float co, si, sc, y;
    uint32_t col;

    G_PTR(0x473b0) = m;
    co = cosf((float)a * M1621C_DEG);
    si = sinf((float)(a + 45) * M1621C_DEG);
    sc = 3.0f / (float)(k + 2);
    AT_F32(m, 0x270) = c->px + si * AT_F32(m, 0x2ac) / (20.0f - G_F32(0x3ea0c));
    y = sc * AT_F32(m, 0x2ac) + c->hf * 100.0f;
    if (lift)
        y += 3.0f;
    AT_F32(m, 0x274) = y;
    AT_F32(m, 0x278) = c->pz + co * AT_F32(m, 0x2ac) / (20.0f - G_F32(0x3ea0c));
    AT_F32(m, 0x280) = (float)(k * -90); /* overwritten at once in the original */
    AT_F32(m, 0x280) = 0.0f;
    AT_F32(m, 0x294) = si / (20.0f - G_F32(0x3ea0c));
    AT_F32(m, 0x29c) = co / (20.0f - G_F32(0x3ea0c));
    AT_F32(m, 0x298) = sc;
    fn_0001cd24(&col, c->cx, c->cy);
    fn_00004fb4(col & 0xff, (col >> 8) & 0xff, (col >> 16) & 0xff, 0xff);
}

/* Kind 7: fence pieces along the low edges of a slope (sheared by the slope terms). */
static void m1621c_fence(const m1621c_cell *c, int slot)
{
    int cx = c->cx, cy = c->cy;
    float px = c->px, pz = c->pz, hf = c->hf;
    float dh = 0.0f, s1 = 0.0f, s2 = 0.0f;
    uint8_t *m = fn_00038818(slot);

    G_PTR(0x473b0) = m;
    m1621c_ident(m);
    m1621c_slope(c->type, cx, cy, (int)hf, &dh, &s1, &s2);
    AT_F32(m, 0x270) = px;
    AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + hf * 100.0f + dh * 0.5f;
    AT_F32(m, 0x278) = pz;
    m1621c_rot(m, 0.0f, 0.0f, 0.0f);

    switch (c->type) {
    case 1:
        if (m1621c_h(cx - 1, cy) <= hf) {
            AT_F32(m, 0x280) = 90.0f;
            AT_F32(m, 0x28c) = -s2 - s1;
            AT_F32(m, 0x270) = px - M1621C_HALF;
            m1621c_draw(slot, -1, 2);
        }
        if (m1621c_h(cx + 1, cy) <= hf) {
            AT_F32(m, 0x280) = 270.0f;
            AT_F32(m, 0x28c) = s2 + s1;
            AT_F32(m, 0x270) = px + M1621C_HALF;
            m1621c_draw(slot, -1, 2);
        }
        break;
    case 2:
        if (m1621c_h(cx, cy - 1) <= hf) {
            AT_F32(m, 0x280) = 0.0f;
            AT_F32(m, 0x28c) = s2 + s1;
            AT_F32(m, 0x278) = pz - M1621C_HALF;
            m1621c_draw(slot, -1, 2);
        }
        if (m1621c_h(cx, cy + 1) <= hf) {
            AT_F32(m, 0x280) = 180.0f;
            AT_F32(m, 0x28c) = -s2 - s1;
            AT_F32(m, 0x278) = pz + M1621C_HALF;
            m1621c_draw(slot, -1, 2);
        }
        break;
    case 3:
        if (m1621c_h(cx, cy + 1) <= hf) {
            AT_F32(m, 0x280) = 0.0f;
            AT_F32(m, 0x270) = px;
            AT_F32(m, 0x278) = pz + M1621C_HALF + -24.0f;
            m1621c_draw(slot, -1, 2);
        }
        if (m1621c_h(cx - 1, cy) <= hf) {
            AT_F32(m, 0x280) = 270.0f;
            AT_F32(m, 0x270) = px - M1621C_HALF + 24.0f;
            AT_F32(m, 0x278) = pz;
            m1621c_draw(slot, -1, 2);
        }
        break;
    case 4:
        if (m1621c_h(cx - 1, cy) <= hf) {
            AT_F32(m, 0x280) = 270.0f;
            AT_F32(m, 0x270) = px - M1621C_HALF + 24.0f;
            AT_F32(m, 0x278) = pz;
            m1621c_draw(slot, -1, 2);
        }
        if (m1621c_h(cx, cy - 1) <= hf) {
            AT_F32(m, 0x280) = 180.0f;
            AT_F32(m, 0x270) = px;
            AT_F32(m, 0x278) = pz - M1621C_HALF + 24.0f;
            m1621c_draw(slot, -1, 2);
        }
        break;
    case 5:
        if (m1621c_h(cx + 1, cy) <= hf) {
            AT_F32(m, 0x280) = 90.0f;
            AT_F32(m, 0x270) = px + M1621C_HALF + -24.0f;
            AT_F32(m, 0x278) = pz;
            m1621c_draw(slot, -1, 2);
        }
        if (m1621c_h(cx, cy + 1) <= hf) {
            AT_F32(m, 0x280) = 0.0f;
            AT_F32(m, 0x270) = px;
            AT_F32(m, 0x278) = pz + M1621C_HALF + -24.0f;
            m1621c_draw(slot, -1, 2);
        }
        break;
    case 6:
        if (m1621c_h(cx, cy - 1) <= hf) {
            AT_F32(m, 0x280) = 180.0f;
            AT_F32(m, 0x270) = px;
            AT_F32(m, 0x278) = pz - M1621C_HALF + 24.0f;
            m1621c_draw(slot, -1, 2);
        }
        if (m1621c_h(cx + 1, cy) <= hf) {
            AT_F32(m, 0x280) = 90.0f;
            AT_F32(m, 0x270) = px + M1621C_HALF + -24.0f;
            AT_F32(m, 0x278) = pz;
            m1621c_draw(slot, -1, 2);
        }
        break;
    case 7:
        AT_F32(m, 0x274) = m1621c_h(cx, cy - 1) * 100.0f + 24.0f;
        AT_F32(m, 0x280) = 90.0f;
        AT_F32(m, 0x28c) = -s2 - s1;
        AT_F32(m, 0x270) = px - M1621C_HALF;
        m1621c_draw(slot, -1, 2);
        AT_F32(m, 0x280) = 270.0f;
        AT_F32(m, 0x28c) = s2 + s1;
        AT_F32(m, 0x270) = px + M1621C_HALF;
        m1621c_draw(slot, -1, 2);
        break;
    case 8:
        AT_F32(m, 0x274) = m1621c_h(cx - 1, cy) * 100.0f + 24.0f;
        AT_F32(m, 0x280) = 0.0f;
        AT_F32(m, 0x28c) = s2 + s1;
        AT_F32(m, 0x278) = pz - M1621C_HALF;
        m1621c_draw(slot, -1, 2);
        AT_F32(m, 0x280) = 180.0f;
        AT_F32(m, 0x28c) = -s2 - s1;
        AT_F32(m, 0x278) = pz + M1621C_HALF;
        m1621c_draw(slot, -1, 2);
        break;
    default:
        break;
    }
}

/* Kinds 2 and 3: a model circling over the cell whose height (0x42968) follows the ground with
 * a velocity (0x42fa8). Returns the LOD index it picked from its own view depth. */
static int m1621c_hopper(const m1621c_cell *c, int v, int k, int kind)
{
    int *t454 = (int *)G(0x47454);
    float *hp = (float *)G(0x42968u + c->cx * 0x50 + c->cy * 4);
    float *vp = (float *)G(0x42fa8u + c->cx * 0x50 + c->cy * 4);
    float p[3], out[3], co, si, r, g, h, tgt;
    int a, idx, scratch = 0;
    uint8_t *m;

    if (kind == 2) {
        a = (int)(G_F32(0x5c168) * (float)(k + 6) + (float)(k * 30));
        co = cosf((float)a * M1621C_DEG);
        si = sinf((float)(a + 30) * M1621C_DEG);
    } else {
        a = (int)(G_F32(0x5c168) * 5.0f);
        co = cosf((float)a * M1621C_DEG);
        si = sinf((float)(a + 45) * M1621C_DEG);
    }
    p[0] = c->px + si * M1621C_TILE * -2.0f;
    p[1] = *hp;
    p[2] = c->pz - (co * M1621C_TILE + co * M1621C_TILE);
    fn_00037fe8(m1621c_cam_mtx(), p, out);
    idx = out[2] < -2000.0f ? v + 20 : v;
    m = fn_00038818(t454[idx]);
    G_PTR(0x473b0) = m;
    AT_F32(m, 0x270) = p[0];
    AT_F32(m, 0x274) = p[1];
    AT_F32(m, 0x278) = p[2];
    r = AT_F32(m, 0x280) * M1621C_DEG;

    if (kind == 2) {
        /* bouncing: falls back, kicked up while below the ground + clearance */
        float vel, off;
        g = m1621c_groundh(p[0] + sinf(r) * M1621C_TILE, p[1], p[2] + cosf(r) * M1621C_TILE,
                           &scratch);
        tgt = (float)(int)(AT_F32(m, 0x2ac) + g + (float)(k + 6) * 100.0f);
        vel = *vp;
        h = *hp + vel;
        *hp = h;
        if (h < tgt) {
            if (vel < 10.0f)
                *vp = vel + 1.5f;
            if (a % 10 >= 6) {
                AT_F32(m, 0x284) = 180.0f;
                AT_F32(m, 0x280) = (float)(180 - a);
                off = -7.0f;
            } else {
                AT_F32(m, 0x284) = 0.0f;
                AT_F32(m, 0x280) = (float)a;
                off = 7.0f;
            }
        } else {
            *vp = vel + -0.75f;
            AT_F32(m, 0x284) = 0.0f;
            AT_F32(m, 0x280) = (float)a;
            off = 7.0f;
        }
        AT_F32(m, 0x274) = *hp + off;
        AT_F32(m, 0x27c) = 5.0f;
    } else {
        /* hovering: eases up toward the ground ahead + clearance, sinks slowly when above */
        float sn = sinf(r), cs = cosf(r), lift, ahead;
        g = m1621c_groundh(p[0] + (sn * M1621C_TILE + sn * M1621C_TILE), p[1],
                           p[2] + (cs * M1621C_TILE + cs * M1621C_TILE), &scratch);
        lift = (float)(k + 10) * 100.0f;
        ahead = AT_F32(m, 0x2ac) + g + lift;
        g = m1621c_groundh(AT_F32(m, 0x270), AT_F32(m, 0x274), AT_F32(m, 0x278), &scratch);
        h = *hp;
        tgt = (float)(int)ahead;
        if (h < tgt) {
            h = h + (tgt - h) / 50.0f;
            *hp = h;
        } else if ((float)(int)(AT_F32(m, 0x2ac) + g + lift) < h) {
            h = h + -0.5f;
            *hp = h;
        }
        AT_F32(m, 0x274) = h;
        AT_F32(m, 0x280) = (float)a;
        AT_F32(m, 0x27c) = 20.0f;
    }
    return idx;
}

/* Kind 10: a model plus two copies of model 0x8a spinning above it (the rotation-order bytes
 * at 0x42760/0x42764/0x42768 are switched from Z, Y, X to Y, X, Z for them). */
static void m1621c_spinner(const m1621c_cell *c, int slot)
{
    uint8_t *m = m1621c_place(c, slot);
    uint8_t *m2;
    float hy = c->hf * 100.0f;

    m1621c_rot(m, 0.0f, -90.0f, 0.0f);
    m1621c_draw(slot, -1, 2);
    G_U8(0x42760) = 'Y';
    G_U8(0x42764) = 'X';
    G_U8(0x42768) = 'Z';
    m2 = fn_00038818(0x8a);
    AT_F32(m2, 0x270) = c->px;
    AT_F32(m2, 0x274) = AT_F32(m, 0x2ac) + hy + 100.0f;
    AT_F32(m2, 0x278) = c->pz + -150.0f;
    AT_F32(m2, 0x27c) = 0.0f;
    AT_F32(m2, 0x280) = 180.0f;
    AT_F32(m2, 0x284) = G_F32(0x5c168) * (float)((c->cx + c->cy) % 2 + 2);
    AT_F32(m2, 0x288) = 60.0f;
    AT_F32(m2, 0x298) = 60.0f;
    AT_F32(m2, 0x2a8) = 60.0f;
    m1621c_draw(0x8a, -1, 2);
    AT_F32(m2, 0x280) = AT_F32(m2, 0x280) + 180.0f;
    m1621c_draw(0x8a, -1, 2);
    G_U8(0x42760) = 'Z';
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';
}

/* Kinds 6 and 11: face the race direction (0x3e9d0 = +1 / -1) on straight slopes. */
static void m1621c_face_dir(const m1621c_cell *c, uint8_t *m, int shift)
{
    int dir = G_S32(0x3e9d0);

    if (c->type == 2) {
        if (shift)
            AT_F32(m, 0x270) = AT_F32(m, 0x270) + (float)dir * M1621C_HALF;
        AT_F32(m, 0x280) = (float)(dir * 90);
    } else if (c->type == 1) {
        if (shift)
            AT_F32(m, 0x278) = AT_F32(m, 0x278) + (float)dir * -M1621C_HALF;
        AT_F32(m, 0x280) = (float)(dir * 90 + 90);
    }
}

/* Object of the cell (object map 0x44400, id v != 0). */
static void m1621c_object(m1621c_cell *c, int v)
{
    int *t454 = (int *)G(0x47454);
    int cx = c->cx, cy = c->cy, type = c->type;
    int idx = v, kind, k, a, saved;
    uint8_t *m;

    if (!(G_U8(0x3f928) & 1) && c->dist > 2000.0f)
        idx = v + 20;
    kind = m1621c_objkind(idx);
    if ((unsigned)kind <= 13) {
        k = (cx + cy) % 4;
        switch (kind) {
        case 0: /* static */
            m1621c_place(c, t454[idx]);
            break;
        case 1: /* wobbling */
            a = (int)(G_F32(0x5c168) * 10.0f + (float)(k * 30));
            m = m1621c_place(c, t454[idx]);
            AT_F32(m, 0x27c) =
                (G_F32(0x3ea0c) / 3.0f + 3.0f) * sinf((float)(a * k) * M1621C_DEG);
            AT_F32(m, 0x284) = (G_F32(0x3ea0c) / 3.0f + 3.0f) * cosf((float)a * M1621C_DEG);
            break;
        case 2: /* bouncing */
        case 3: /* hovering */
            idx = m1621c_hopper(c, v, k, kind);
            break;
        case 4: /* swinging */
            m1621c_swing(c, t454[idx], k, 0);
            break;
        case 5: /* turned by k, every other one tinted yellow */
            m = m1621c_place(c, t454[idx]);
            AT_F32(m, 0x280) = (float)(k * 90);
            switch (k) {
            case 0:
            case 2:
                m1621c_color(G_U32(0x3eb94));
                break;
            case 1:
            case 3:
                fn_00004fb4(0xff, 0xff, 0x96, 0xff);
                break;
            }
            break;
        case 6: /* at the slope edge, facing the race direction */
            m = m1621c_place(c, t454[idx]);
            m1621c_face_dir(c, m, 1);
            break;
        case 7: /* fences */
            m1621c_fence(c, t454[idx]);
            break;
        case 8: /* light source: drawn unlit and without fog (glow in the effects pass) */
            saved = G_U8(0x48294);
            fn_00038a78(1);
            fn_00038a88(1);
            m = m1621c_place(c, t454[idx]);
            if (type == 2)
                AT_F32(m, 0x280) = 180.0f;
            else if (type == 1)
                AT_F32(m, 0x280) = 90.0f;
            m1621c_fog(-1.0f);
            m1621c_draw(t454[idx], -1, 2);
            m1621c_fog(G_F32(0x3e9f4));
            fn_00038a78(saved);
            fn_00038a88(saved);
            break;
        case 9: /* pair of posts on both x edges */
            m = fn_00038818(t454[idx]);
            G_PTR(0x473b0) = m;
            if ((unsigned)(type - 7) < 2)
                c->hf = m1621c_h(cx + 1, cy); /* kept for the rest of the call */
            AT_F32(m, 0x274) = c->hf * 100.0f + -6.0f;
            AT_F32(m, 0x278) = c->pz;
            AT_F32(m, 0x270) = c->px + -M1621C_HALF;
            m1621c_draw(t454[idx], -1, 2);
            AT_F32(m, 0x270) = c->px + M1621C_HALF;
            m1621c_draw(t454[idx], -1, 2);
            break;
        case 10:
            m1621c_spinner(c, t454[idx]);
            break;
        case 11: /* faces the race direction, no shift */
            m = m1621c_place(c, t454[idx]);
            m1621c_face_dir(c, m, 0);
            break;
        case 12: /* swinging, lifted, drawn here */
            m1621c_swing(c, t454[idx], k, 1);
            m1621c_draw(t454[idx], -1, 2);
            break;
        case 13: /* every other one tinted orange */
            m1621c_place(c, t454[idx]);
            switch (k) {
            case 0:
            case 2:
                fn_00004fb4(200, 100, 0, 0xff);
                break;
            case 1:
            case 3:
                m1621c_color(G_U32(0x3eb94));
                break;
            }
            break;
        }
    }

    /* common draw (kinds 7..10 and 12 drew themselves; kind 6 is hidden in one mode) */
    if ((unsigned)(kind - 7) >= 2 && kind != 9 && kind != 10 && kind != 12 &&
        (kind != 6 || G_S32(0x3f9dc) != G_S32(0x3ebe4)))
        m1621c_draw(t454[idx], -1, 2);
    if ((unsigned)(kind - 4) < 2 || kind == 12 || kind == 13)
        m1621c_color(G_U32(0x3eb94));
    G_S32(0x3f930)++;
}

/* Finish-section markers on straight slopes (fn_0000fb78 gives two points per marker,
 * fn_0001d388 draws it). */
static void m1621c_arrows(const m1621c_cell *c)
{
    int cnt = G_S32(0x3f948);
    int i, end, stop;

    if (G_U8(0x3fab8) || !c->last || (unsigned)(c->type - 1) >= 2)
        return;
    if (c->lv == cnt) {
        i = 0;
        end = 2;
    } else if (c->lv == cnt - 1) {
        i = 2;
        end = 4;
    } else {
        i = 0;
        end = 4;
    }
    stop = end > i + 1 ? end : i + 1;
    do {
        float pos[3] = {c->px, c->hf * 100.0f, c->pz};
        float a[3], b[3];

        fn_0000fb78(i, c->type, P2I(a), P2I(b));
        fn_00037dec(pos, a, a);
        fn_0001d388(i, a[0], a[1], a[2], b[0], b[1], b[2], 0);
        i++;
    } while (i != stop);
}

/* Water tiles (9, 10): rippling surface, faded with distance, with random splashes. */
static void m1621c_water(const m1621c_cell *c)
{
    int slot = ((int *)G(0x47770))[8];
    int cx = c->cx, cy = c->cy, alpha, hi;
    uint8_t *m;
    float s, p[3], out[3];
    uint32_t col;

    if ((unsigned)(c->type - 9) >= 2)
        return;
    m = fn_00038818(slot);
    m1621c_rot(m, 0.0f, 0.0f, 0.0f);
    AT_F32(m, 0x270) = c->px;
    AT_F32(m, 0x278) = c->pz;
    m1621c_ident(m);
    s = sinf((float)G_S32(0x5c16c) * M1621C_DEG);
    if (cx % 2 != 1)
        s = -s;
    AT_F32(m, 0x28c) = s / (G_F32(0x3ea0c) + 40.0f);
    s = sinf((float)(G_S32(0x5c16c) + 90) * M1621C_DEG);
    if (cy % 2 != 1)
        s = -s;
    AT_F32(m, 0x2a4) = s / (G_F32(0x3ea0c) + 40.0f);
    hi = (int)m1621c_h(cx, cy);
    if (c->type == 10)
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hi * 100.0f + -10.0f;
    else
        AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + (float)hi * 100.0f + 50.0f;
    if (AT_F32(m, 0x2a4) == 0.0f && AT_F32(m, 0x28c) == 0.0f &&
        G_F32(0x470f8) + 1.0f <= AT_F32(m, 0x274))
        return;

    fn_0001cae8(&col, cx, cy);
    p[0] = AT_F32(m, 0x270);
    p[1] = AT_F32(m, 0x274);
    p[2] = AT_F32(m, 0x278);
    fn_00037fe8(m1621c_cam_mtx(), p, out);
    alpha = (int)((out[2] + 2000.0f) * 100.0f / 2000.0f + 103.0f);
    if (alpha < 0)
        alpha = 0;
    else if (alpha > 0xff)
        alpha = 0xff;
    fn_00004fb4(col & 0xff, (col >> 8) & 0xff, (col >> 16) & 0xff, alpha);
    m1621c_draw(slot, -1, 2);
    m1621c_color(G_U32(0x3eb94));

    if (fn_00004400(10) > 7) {
        /* splash particle somewhere on the tile, if at mid distance */
        p[0] = AT_F32(m, 0x270) + ((float)fn_00004400(0x32c) + -M1621C_HALF);
        p[1] = AT_F32(m, 0x274);
        p[2] = AT_F32(m, 0x278) + ((float)fn_00004400(0x32c) + -M1621C_HALF);
        fn_00037fe8(m1621c_cam_mtx(), p, out);
        if (out[2] < -1000.0f && out[2] > -3500.0f) {
            m1621c_spawn(G_S32(0x470d4), p,
                         (float *)((uint8_t *)G(0x479a0) + G_S32(0x3f944) * 0xc));
            if (G_S32(0x3f944) < 0x19)
                G_S32(0x3f944)++;
        }
    }
}

static void m1621c_objects(m1621c_cell *c)
{
    int v;

    /* object budget (0x3f930 drawn this frame, 0x3f934 limit) */
    if (G_S32(0x3f930) < G_S32(0x3f934)) {
        v = m1621c_obj(c->cx, c->cy);
        if (v != 0)
            m1621c_object(c, v);
        m1621c_arrows(c);
    }
    m1621c_water(c);
}

/* ---- pass 4: effects ------------------------------------------------------------------------ */

/* One glow sprite (model 0x84) pulled 20 units from (x, y, z) toward the camera. */
static void m1621c_glow(uint8_t *g, float x, float y, float z)
{
    float d[3];

    d[0] = G_F32(0x470f4) - x;
    d[1] = G_F32(0x470f8) - y;
    d[2] = G_F32(0x470fc) - z;
    fn_000379a8(d, d);
    AT_F32(g, 0x270) = x + d[0] * 20.0f;
    AT_F32(g, 0x274) = y + d[1] * 20.0f;
    AT_F32(g, 0x278) = z + d[2] * 20.0f;
    m1621c_draw(0x84, -1, 2);
}

static void m1621c_effects(const m1621c_cell *c)
{
    int cx = c->cx, cy = c->cy, s = cx + cy, v;

    /* every 5th diagonal: a slowly turning model 0x83 high above the track, pulsing in width,
     * faded in with distance (clouds?) */
    if (s % 5 == 0) {
        uint8_t *m = fn_00038818(0x83);
        float ang = (float)((s % 4) * 90) + G_F32(0x5c168);
        float pulse, af;
        int a, grey;

        AT_F32(m, 0x280) = ang;
        pulse = sinf(ang * M1621C_DEG) * 0.5f + 0.5f;
        AT_F32(m, 0x27c) = 90.0f;
        AT_F32(m, 0x284) = 0.0f;
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz;
        m1621c_ident(m);
        AT_F32(m, 0x288) = pulse + 1.0f;
        AT_F32(m, 0x274) = (float)(int)(m1621c_h(cx, cy) + 6.0f) * 100.0f;
        a = (int)(c->dist * 255.0f / -3000.0f + 355.0f);
        grey = (int)((G_F32(0x3ea0c) + G_F32(0x3ea00)) * -30.0f + 255.0f);
        if (a < 0)
            a = 0;
        if (grey < 0)
            grey = 0;
        else if (grey > 0xff)
            grey = 0xff;
        af = a > 0xff ? 255.0f : (float)a;
        fn_00004fb4(grey, grey, grey, (int)(af * pulse));
        m1621c_draw(0x83, -1, 2);
        m1621c_color(G_U32(0x3eb94));
    }

    /* light level above 4: glow sprites on both sides of a kind-8 object (lamp) */
    if (G_F32(0x3ea00) > 4.0f) {
        v = m1621c_obj(cx, cy);
        if (v != 0 && m1621c_objkind(v) == 8) {
            uint8_t *g, *mo;
            float ox = 0.0f, oz = 0.0f, y;

            m1621c_fog(-1.0f);
            g = fn_00038818(0x84);
            mo = fn_00038818(((int *)G(0x47454))[v]);
            AT_F32(g, 0x27c) = G_F32(0x47100);
            AT_F32(g, 0x280) = G_F32(0x47104) + 180.0f;
            AT_F32(g, 0x284) = 0.0f;
            AT_F32(g, 0x288) = 8.0f;
            AT_F32(g, 0x298) = 8.0f;
            AT_F32(g, 0x2a8) = 8.0f;
            fn_00004fb4(0xff, 0xff, 0xff, 0x96);
            if (c->type == 1)
                ox = 346.0f;
            else if (c->type == 2)
                oz = 346.0f;
            y = c->hf * 100.0f + (AT_F32(mo, 0x2ac) + AT_F32(mo, 0x2ac));
            m1621c_glow(g, c->px + ox, y, c->pz - oz);
            m1621c_glow(g, c->px - ox, y, c->pz + oz);
            m1621c_fog(G_F32(0x3e9f4));
        }
    }
}

/* ---- pass 5: lamp light on the ground ------------------------------------------------------- */

static void m1621c_lamplight(const m1621c_cell *c, int pass_ground, int pass_shadow)
{
    int ramp = (unsigned)(c->type - 7) < 2;
    int v, i;
    uint8_t *m;

    if (!pass_shadow && (pass_ground != 1 || !ramp))
        return;
    if (G_F32(0x3ea00) <= 1.0f)
        return;
    v = m1621c_obj(c->cx, c->cy);
    if (v == 0 && !ramp)
        return;
    if (m1621c_objkind(v) != 8 && !ramp)
        return;

    m = fn_00038818(0x81);
    m1621c_fog(-1.0f);
    AT_F32(m, 0x274) = AT_F32(m, 0x2ac) + c->hf * 100.0f + 2.0f;
    fn_000042b0(0, 0x201, 1);
    switch (c->type) {
    case 1:
        AT_F32(m, 0x288) = 1.0f;
        AT_F32(m, 0x298) = 1.0f;
        AT_F32(m, 0x2a8) = 1.0f;
        fn_00004fb4(0xff, 0xff, 0xff, 0x32);
        AT_F32(m, 0x278) = c->pz;
        AT_F32(m, 0x270) = c->px + 200.0f / 3.0f;
        AT_F32(m, 0x280) = 90.0f;
        m1621c_draw(0x81, -1, 2);
        AT_F32(m, 0x270) = c->px + -200.0f / 3.0f;
        AT_F32(m, 0x280) = 270.0f;
        m1621c_draw(0x81, -1, 2);
        break;
    case 2:
        AT_F32(m, 0x288) = 1.0f;
        AT_F32(m, 0x298) = 1.0f;
        AT_F32(m, 0x2a8) = 1.0f;
        fn_00004fb4(0xff, 0xff, 0xff, 0x32);
        AT_F32(m, 0x270) = c->px;
        AT_F32(m, 0x278) = c->pz + 200.0f / 3.0f;
        AT_F32(m, 0x280) = 0.0f;
        m1621c_draw(0x81, -1, 2);
        AT_F32(m, 0x278) = c->pz + -200.0f / 3.0f;
        AT_F32(m, 0x280) = 180.0f;
        m1621c_draw(0x81, -1, 2);
        break;
    case 7:
        if (G_F32(0x470f8) < AT_F32(m, 0x274) + 200.0f) {
            AT_F32(m, 0x288) = 2.0f;
            AT_F32(m, 0x298) = 1.0f;
            AT_F32(m, 0x2a8) = 0.5f;
            fn_00004fb4(0xff, 0xff, 0xff, 100);
            AT_F32(m, 0x278) = c->pz;
            AT_F32(m, 0x280) = 90.0f;
            for (i = 0; i < 3; i++) {
                AT_F32(m, 0x270) = c->px + ((float)i * M1621C_TILE / 3.0f + -M1621C_HALF);
                m1621c_draw(0x81, -1, 2);
            }
        }
        break;
    case 8:
        if (G_F32(0x470f8) < AT_F32(m, 0x274) + 200.0f) {
            AT_F32(m, 0x288) = 2.0f;
            AT_F32(m, 0x298) = 1.0f;
            AT_F32(m, 0x2a8) = 0.5f;
            fn_00004fb4(0xff, 0xff, 0xff, 100);
            AT_F32(m, 0x270) = c->px;
            AT_F32(m, 0x280) = 0.0f;
            for (i = 0; i < 3; i++) {
                AT_F32(m, 0x278) = c->pz + ((float)i * M1621C_TILE / 3.0f + -M1621C_HALF);
                m1621c_draw(0x81, -1, 2);
            }
        }
        break;
    default:
        break;
    }
    fn_000042b0(1, 0x201, 1);
    m1621c_fog(G_F32(0x3e9f4));
}

/* ---- entry ---------------------------------------------------------------------------------- */

/* 0x0001621c -- Track_DrawCell: draws the cell (camera cell + dx, dy): ground, walls, objects,
 * effects and lamp light, each pass enabled by its flag (1) */
void fn_0001621c(int dx, int dy, int pass_ground, int pass_walls, int pass_objects,
                 int pass_effects, int pass_shadow)
{
    m1621c_cell c;
    float pos[3], out[3], zmin, lod;
    int v;

    c.dx = dx;
    c.dy = dy;
    c.cx = G_S32(0x46e98) + dx;
    c.cy = G_S32(0x46e9c) + dy;
    c.px = (float)c.cx * M1621C_TILE;
    c.pz = (float)c.cy * M1621C_TILE;
    pos[0] = c.px;
    pos[1] = 0.0f;
    pos[2] = c.pz;

    /* cull; tall objects (kinds 2, 3) widen the near limit */
    zmin = -M1621C_HALF;
    v = m1621c_obj(c.cx, c.cy);
    if (v != 0 && (unsigned)(m1621c_objkind(v) - 2) <= 1)
        zmin = -1624.0f;
    if (m1621c_visible(((int *)G(0x47770))[0], c.px, 0.0f, c.pz, zmin,
                       G_F32(0x3e9c4) + M1621C_HALF) != 1)
        return;

    fn_00037fe8(m1621c_cam_mtx(), pos, out);
    lod = G_S32(0x3e9c0) < 2 ? 0.0f : (float)(G_S32(0x3e9c0) * 250 - 500);
    c.hf = m1621c_h(c.cx, c.cy);
    c.type = G_U8(0x3fab8) ? 11 : m1621c_tile(c.cx, c.cy);
    c.lv = m1621c_level(c.cx, c.cy);
    c.last = G_S32(0x3f8e0) != 0 && c.lv >= G_S32(0x3f948) - 1;
    c.dist = lod - out[2];

    if (pass_ground == 1)
        m1621c_ground(&c);
    if (pass_walls == 1)
        m1621c_walls(&c);
    if (pass_objects == 1)
        m1621c_objects(&c);
    if (pass_effects == 1)
        m1621c_effects(&c);
    m1621c_lamplight(&c, pass_ground, pass_shadow);
}

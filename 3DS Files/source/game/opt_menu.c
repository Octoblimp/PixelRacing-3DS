/* opt_menu.c -- the port's Controls and 3D pages, inside the game's own Options screen.
 *
 * The Options screen (state 12) is a carousel of voxel props, one per setting: left / right
 * turn it, OK changes the setting in front, and the label under it says the setting and its
 * value (m_20f70.c draws it, m_2d550.c fn_00030318 reads the input, m_12a68.c draws the
 * props). The port adds two props after the game's seven, a game pad (Controls) and a "3D"
 * (3D), and each opens a page of its own that works the same way: the same carousel with this
 * page's props, the same label, its own title. OK changes the setting (up / down too, in both
 * directions); back returns to the Options page, and the settings are saved then.
 *
 * The props are voxel models in the game's style made by tools/make_menu_props.py, loaded on
 * the free model slots 245.. (the game uses up to 244). */
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "controls.h"
#include "ios.h"
#include "opt_menu.h"

#define D2R 0.0174533f

enum {
    SLOT_3D = 245,
    SLOT_PAD,
    SLOT_BAR,
    SLOT_BTN, /* 8 button caps: A B X Y L R ZL ZR, in controls.c's button order */
    SLOT_END = SLOT_BTN + 8,
};
#define SLOT_KEY 0x7a   /* the game's key prop */
#define SLOT_CLOCK 0x74 /* the game's clock prop */

#define GAME_ROWS 7 /* the game's own Options rows */
enum { ROW_CONTROLS = GAME_ROWS, ROW_3D, OPTIONS_ROWS };

enum { PAGE_OPTIONS, PAGE_CONTROLS, PAGE_3D };
enum { C_STEER, C_SENS, C_DZ, C_TILT, C_GAS, C_BRAKE, C_NITRO, C_RESET, C_ROWS };
enum { D_DEPTH, D_POP, D_TILT, D_HUD, D_ROWS };

static int s_page;
static bool s_reset_done; /* "Reset Controls: Done" until the cursor moves */

void optm_load(void) {
    static const char *const k_names[SLOT_END - SLOT_3D] = {
        "opt_3d", "opt_pad", "opt_bar", "opt_btn_a", "opt_btn_b", "opt_btn_x",
        "opt_btn_y", "opt_btn_l", "opt_btn_r", "opt_btn_zl", "opt_btn_zr",
    };
    for (int i = 0; i < SLOT_END - SLOT_3D; i++) {
        char smd[64], png[64];
        snprintf(smd, sizeof smd, "/models/others/%s.smd", k_names[i]);
        snprintf(png, sizeof png, "/models/others/%s.png", k_names[i]);
        float scale = SLOT_3D + i == SLOT_PAD || SLOT_3D + i == SLOT_BAR ? 1.0f : 1.27f;
        fn_0003836c(SLOT_3D + i, smd, scale, 0, 0, G_U8(0x3e99c) & 1);
        fn_0000ff60(SLOT_3D + i, png, 0x10);
    }
}

/* ---------------------------------------------------------------- labels */

static const int k_depths[] = {50, 75, 100, 125, 150};
static const char *const k_depth_names[] = {"Very Low", "Low", "Normal", "High", "Very High"};
static const char *const k_pop_names[] = {"Low", "Normal", "High"};
#define NDEPTHS (int)(sizeof k_depths / sizeof k_depths[0])

static int depth_index(int pct) {
    for (int i = 0; i < NDEPTHS; i++)
        if (pct <= k_depths[i]) return i;
    return NDEPTHS - 1;
}

static void label_controls(int row, char *t) {
    const Controls *c = controls_get();
    switch (row) {
    case C_STEER: snprintf(t, 0x20, "Steering: %s", c->steer == STEER_TILT ? "Tilt" : "Circle Pad"); break;
    case C_SENS: snprintf(t, 0x20, "Stick Sensitivity: %d", c->stick_sens); break;
    case C_DZ: snprintf(t, 0x20, "Stick Dead Zone: %d", c->stick_dz); break;
    case C_TILT: snprintf(t, 0x20, "Tilt Range: %d", c->tilt_range); break;
    case C_GAS: snprintf(t, 0x20, "Gas: %s", controls_button_name(c->gas)); break;
    case C_BRAKE: snprintf(t, 0x20, "Brake: %s", controls_button_name(c->brake)); break;
    case C_NITRO: snprintf(t, 0x20, "Nitro: %s", controls_button_name(c->nitro)); break;
    default: snprintf(t, 0x20, s_reset_done ? "Reset Controls: Done" : "Reset Controls"); break;
    }
}

static void label_3d(int row, char *t) {
    const Controls *c = controls_get();
    switch (row) {
    case D_DEPTH: snprintf(t, 0x20, "3D Depth: %s", k_depth_names[depth_index(c->stereo_depth)]); break;
    case D_POP: snprintf(t, 0x20, "Pop-out: %s", k_pop_names[c->stereo_pop]); break;
    case D_TILT: snprintf(t, 0x20, "3D While Tilting: %s", c->stereo_tilt ? "On" : "Off"); break;
    default: snprintf(t, 0x20, "HUD Depth: %s", c->hud_depth ? "Pop-out" : "Screen"); break;
    }
}

int optm_options_rows(int32_t *q, char *label) {
    q[ROW_CONTROLS] = SLOT_PAD;
    q[ROW_3D] = SLOT_3D;
    int sel = G_S32(0x3faa8);
    if (sel == ROW_CONTROLS) snprintf(label, 0x20, "Controls");
    if (sel == ROW_3D) snprintf(label, 0x20, "3D");
    return OPTIONS_ROWS;
}

bool optm_fill(int32_t *q, char *label, char *title) {
    if (s_page == PAGE_OPTIONS) return false;
    int sel = G_S32(0x3faa8);
    G_S32(0x3f99c) = 0x78; /* the floating 3D Pixel Racing logo, as on the Options page */
    G_F32(0x3ebdc) = 0.24f;
    if (s_page == PAGE_CONTROLS) {
        static const int32_t k_q[C_ROWS] = {SLOT_PAD, SLOT_BAR, SLOT_BAR, SLOT_BAR,
                                            SLOT_BTN, SLOT_BTN, SLOT_BTN, SLOT_KEY};
        const Controls *c = controls_get();
        G_S32(0x3ec00) = C_ROWS;
        memcpy(q, k_q, sizeof k_q);
        q[C_GAS] = SLOT_BTN + c->gas;
        q[C_BRAKE] = SLOT_BTN + c->brake;
        q[C_NITRO] = SLOT_BTN + c->nitro;
        label_controls(sel, label);
        snprintf(title, 0x20, "Controls");
    } else {
        static const int32_t k_q[D_ROWS] = {SLOT_3D, SLOT_BAR, SLOT_PAD, SLOT_CLOCK};
        G_S32(0x3ec00) = D_ROWS;
        memcpy(q, k_q, sizeof k_q);
        label_3d(sel, label);
        snprintf(title, 0x20, "3D");
    }
    return true;
}

/* ---------------------------------------------------------------- input */

static int step(int v, int lo, int hi, int by, int dir) {
    v += by * dir;
    return v > hi ? lo : v < lo ? hi : v; /* round and round, like the game's lap count */
}

static void change(int row, int dir) {
    Controls *c = controls_edit();
    if (s_page == PAGE_CONTROLS) {
        switch (row) {
        case C_STEER: c->steer = c->steer == STEER_TILT ? STEER_PAD : STEER_TILT; break;
        case C_SENS: c->stick_sens = step(c->stick_sens, 50, 200, 25, dir); break;
        case C_DZ: c->stick_dz = step(c->stick_dz, 0, 40, 5, dir); break;
        case C_TILT: c->tilt_range = step(c->tilt_range, 10, 60, 10, dir); break;
        case C_GAS: controls_cycle_button(0, dir); break;
        case C_BRAKE: controls_cycle_button(1, dir); break;
        case C_NITRO: controls_cycle_button(2, dir); break;
        case C_RESET:
            controls_reset();
            s_reset_done = true;
            break;
        }
    } else {
        switch (row) {
        case D_DEPTH: c->stereo_depth = k_depths[(depth_index(c->stereo_depth) + NDEPTHS + dir) % NDEPTHS]; break;
        case D_POP: c->stereo_pop = (c->stereo_pop + 3 + dir) % 3; break;
        case D_TILT: c->stereo_tilt = !c->stereo_tilt; break;
        case D_HUD: c->hud_depth = !c->hud_depth; break;
        }
    }
}

static void open_page(int page, int row) {
    s_page = page;
    s_reset_done = false;
    G_S32(0x3faa8) = row;
    G_F32(0x3faac) = 0.0f; /* the selected prop's spin */
}

bool optm_input(void) {
    static int s_last_sel = -1;
    bool ok = G_U8(0x3fa74) & 1, back = G_U8(0x3fa70) & 1;
    int sel = G_S32(0x3faa8);
    if (sel != s_last_sel) s_reset_done = false;
    s_last_sel = sel;
    if (s_page == PAGE_OPTIONS) {
        if (!ok || (sel != ROW_CONTROLS && sel != ROW_3D)) return false;
        sys_log("options: %s page", sel == ROW_CONTROLS ? "controls" : "3d");
        open_page(sel == ROW_CONTROLS ? PAGE_CONTROLS : PAGE_3D, 0);
        s_last_sel = 0;
        return true;
    }
    /* up / down: the game's menu flags, with its key repeat (fn_00030bf4) */
    bool up = G_U8(0x3fa60) & 1, dn = G_U8(0x3fa64) & 1;
    if (back) {
        int row = s_page == PAGE_CONTROLS ? ROW_CONTROLS : ROW_3D;
        controls_commit();
        open_page(PAGE_OPTIONS, row);
        s_last_sel = row;
    } else if (ok || up) {
        change(sel, 1);
    } else if (dn) {
        change(sel, -1);
    }
    return true;
}

bool optm_meter(void) { return s_page == PAGE_CONTROLS && G_S32(0x3fa24) == 0xc; }

/* ---------------------------------------------------------------- props */

static void tint(int r, int g, int b, int a) { fn_00004fb4(r, g, b, a); }
static void tint_on(void) { tint(0xe6, 0xe6, 0xe6, 0xff); }
static void tint_off(void) { tint(0x96, 0x96, 0x96, 0x64); }

static void scale(uint8_t *o, float s) {
    AT_F32(o, 0x288) = s;
    AT_F32(o, 0x298) = s;
    AT_F32(o, 0x2a8) = s;
}

static void draw(int slot) { fn_00038a98(slot, -1, -1, 0, 0, 0); }

/* A row of n small blocks across the prop's place on the carousel, the first `lit` coloured
 * (green to red), the rest grey, like the game's lap-count meter. */
static void meter(uint8_t *o, int pass, int lit, int n, bool dim) {
    uint8_t *b = fn_00038818(SLOT_BAR);
    /* The prop sits at (R sin q, R cos q) and the carousel turns it to q + 180 degrees, plus a
     * spin on the selected one; the row lies along the circle and doesn't spin (a spinning row
     * of blocks reads as a mess). Pass 1, the floor reflection, mirrors the turn to 180 - it. */
    float a = atan2f(AT_F32(o, 0x270), AT_F32(o, 0x278));
    float tx = cosf(a), tz = -sinf(a), ry = a / D2R + 180.0f;
    memcpy(b + 0x270, o + 0x270, 24); /* position and rotation */
    AT_F32(b, 0x280) = pass == 1 ? 180.0f - ry : ry;
    scale(b, 0.85f);
    for (int k = 0; k < n; k++) {
        float d = ((float)k - (float)(n - 1) * 0.5f) * 10.0f;
        AT_F32(b, 0x270) = AT_F32(o, 0x270) + tx * d;
        AT_F32(b, 0x278) = AT_F32(o, 0x278) + tz * d;
        float f = n > 1 ? (float)k / (float)(n - 1) : 1.0f; /* green .. yellow .. red */
        if (k < lit && !dim)
            tint((int)(70.0f + 185.0f * fminf(1.0f, f * 2.0f)),
                 (int)(220.0f - 170.0f * fmaxf(0.0f, f * 2.0f - 1.0f)), 40, 0xff);
        else if (k < lit)
            tint(0x96, 0x96, 0x96, 0xb4);
        else
            tint(0x50, 0x50, 0x50, 0x64);
        draw(SLOT_BAR);
    }
}

/* the prop rocks from side to side, like a console being tilted */
static void rock(uint8_t *o) {
    float t = (float)(osGetTime() % 100000u) * 0.001f;
    AT_F32(o, 0x284) = sinf(t * 3.0f) * 20.0f;
}

bool optm_prop(uint8_t *o, int i, int pass, int slot) {
    const Controls *c = controls_get();
    if (s_page == PAGE_OPTIONS && i < GAME_ROWS) return false;
    /* the game's props have about this size on the carousel (it scales each of its own) */
    scale(o, slot == SLOT_PAD ? 0.6f : slot == SLOT_3D ? 0.75f : 0.8f);
    if (s_page == PAGE_OPTIONS) {
        tint_on();
        draw(slot);
        return true;
    }
    if (s_page == PAGE_CONTROLS) {
        switch (i) {
        case C_STEER:
            if (c->steer == STEER_TILT) rock(o);
            tint_on();
            draw(slot);
            break;
        case C_SENS: meter(o, pass, (c->stick_sens - 50) / 25 + 1, 7, false); break;
        case C_DZ: meter(o, pass, c->stick_dz / 5, 8, false); break;
        case C_TILT: meter(o, pass, c->tilt_range / 10, 6, c->steer != STEER_TILT); break;
        default:
            tint_on();
            draw(slot);
            break;
        }
        return true;
    }
    switch (i) {
    case D_DEPTH:
        scale(o, 0.75f * (0.5f + 0.5f * (float)c->stereo_depth / 100.0f));
        tint_on();
        draw(slot);
        break;
    case D_POP: meter(o, pass, c->stereo_pop + 1, 3, false); break;
    case D_TILT:
        if (c->stereo_tilt) {
            rock(o);
            tint_on();
        } else {
            tint_off();
        }
        draw(slot);
        break;
    default:
        if (c->hud_depth)
            tint_on();
        else
            tint_off();
        draw(slot);
        break;
    }
    return true;
}

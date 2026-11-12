/* Controls settings, their save file and the bottom-screen menu (see controls.h).
 *
 * Steering reaches the game through its own tilt input (game_main.c feed_steering), so the
 * Circle Pad, the D-pad and the tilt all go through the game's steering code and its per-car
 * handling, exactly as the iPhone's accelerometer did.
 *
 * The menu is drawn with the menus' bitmap font (texture 0x97) through the GL
 * overlay, so it looks like part of the game. SELECT opens and closes it; while it is open the
 * game gets no input, and opening it mid-race pauses the race first. */
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "controls.h"
#include "input.h"
#include "ios.h"

/* ---------------------------------------------------------------- settings */

static const struct {
    const char *name;
    uint32_t key;
    bool n3ds_only;
} k_buttons[] = {
    {"A", KEY_A, false},  {"B", KEY_B, false}, {"X", KEY_X, false},   {"Y", KEY_Y, false},
    {"L", KEY_L, false},  {"R", KEY_R, false}, {"ZL", KEY_ZL, true}, {"ZR", KEY_ZR, true},
};
#define NBUTTONS (int)(sizeof k_buttons / sizeof k_buttons[0])
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR };

static const Controls k_default = {
    .steer = STEER_PAD,
    .stick_sens = 100,
    .stick_dz = 10,
    .tilt_range = 30,
    .gas = BTN_A,
    .brake = BTN_B,
    .nitro = BTN_R,
};

static Controls s_c;
static bool s_dirty;

const Controls *controls_get(void) { return &s_c; }

uint32_t controls_key(int choice) {
    return choice >= 0 && choice < NBUTTONS ? k_buttons[choice].key : 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Usable on this console (ZL/ZR only on a New 3DS). */
static bool button_ok(int b) {
    return b >= 0 && b < NBUTTONS && (!k_buttons[b].n3ds_only || sys_is_new3ds());
}

/* Returns false if gas, brake or nitro had to go back to the defaults. Each action needs a
 * button of its own: one button on both gas and brake presses both, and the game reads the
 * brake at a standstill as reverse (fn_0002bf68), so the car could only reverse (build 5). */
static bool sanitize(Controls *c) {
    c->steer = c->steer == STEER_TILT ? STEER_TILT : STEER_PAD;
    c->stick_sens = clampi(c->stick_sens, 50, 200);
    c->stick_dz = clampi(c->stick_dz, 0, 40);
    c->tilt_range = clampi(c->tilt_range, 10, 60);
    if (!button_ok(c->gas) || !button_ok(c->brake) || !button_ok(c->nitro) ||
        c->gas == c->brake || c->gas == c->nitro || c->brake == c->nitro) {
        c->gas = k_default.gas;
        c->brake = k_default.brake;
        c->nitro = k_default.nitro;
        return false;
    }
    return true;
}

static void cfg_path(char *buf, size_t n) { snprintf(buf, n, "%s/controls.cfg", ios_doc_dir()); }

static void save(void);

static void load(void) {
    s_c = k_default;
    char path[96];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char key[32];
    int v;
    while (fscanf(f, "%31s %d", key, &v) == 2) {
        if (!strcmp(key, "steer")) s_c.steer = v;
        else if (!strcmp(key, "stick_sensitivity")) s_c.stick_sens = v;
        else if (!strcmp(key, "stick_dead_zone")) s_c.stick_dz = v;
        else if (!strcmp(key, "tilt_range")) s_c.tilt_range = v;
        else if (!strcmp(key, "gas")) s_c.gas = v;
        else if (!strcmp(key, "brake")) s_c.brake = v;
        else if (!strcmp(key, "nitro")) s_c.nitro = v;
    }
    fclose(f);
    if (!sanitize(&s_c)) {
        sys_log("controls: gas/brake/nitro shared a button or used a missing one: defaults");
        save();
    }
}

static void save(void) {
    char path[96];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) {
        sys_log("controls: could not save %s", path);
        return;
    }
    fprintf(f, "steer %d\nstick_sensitivity %d\nstick_dead_zone %d\ntilt_range %d\n"
               "gas %d\nbrake %d\nnitro %d\n",
            s_c.steer, s_c.stick_sens, s_c.stick_dz, s_c.tilt_range, s_c.gas, s_c.brake,
            s_c.nitro);
    fclose(f);
    sys_log("controls: saved (steer %s, stick %d%% dz %d%%, tilt %d deg, gas %s brake %s nitro %s)",
            s_c.steer == STEER_TILT ? "tilt" : "pad", s_c.stick_sens, s_c.stick_dz,
            s_c.tilt_range, k_buttons[s_c.gas].name, k_buttons[s_c.brake].name,
            k_buttons[s_c.nitro].name);
}

void controls_init(void) {
    load();
    sys_log("controls: steer %s, stick %d%% dz %d%%, tilt %d deg, gas %s brake %s nitro %s",
            s_c.steer == STEER_TILT ? "tilt" : "pad", s_c.stick_sens, s_c.stick_dz,
            s_c.tilt_range, k_buttons[s_c.gas].name, k_buttons[s_c.brake].name,
            k_buttons[s_c.nitro].name);
}

/* ---------------------------------------------------------------- steering */

static float s_dpad, s_tilt_last;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Tilt: the console's roll about the axis through the screen, from the gravity direction in
 * the screen plane, so it reads the same however far back the console is tipped. Full lock at
 * tilt_range degrees, with 2 degrees of dead zone in the middle. */
static float tilt_steer(void) {
    float g[3];
    input_accel_g(g);
    if (hypotf(g[0], g[1]) < 0.25f) return s_tilt_last; /* lying flat: roll is meaningless */
    float roll = atan2f(g[0], -g[1]) * (180.0f / (float)M_PI);
    float a = fabsf(roll) - 2.0f;
    float m = a <= 0.0f ? 0.0f : clampf(a / ((float)s_c.tilt_range - 2.0f), 0.0f, 1.0f);
    s_tilt_last = roll < 0.0f ? -m : m;
    return s_tilt_last;
}

static float steer_now(void) {
    uint32_t held = input_held();
    int d = ((held & KEY_DRIGHT) ? 1 : 0) - ((held & KEY_DLEFT) ? 1 : 0);
    if (d) {
        /* the D-pad turns in over about a sixth of a second instead of snapping to full lock */
        if (s_dpad * (float)d < 0.0f) s_dpad = 0.0f;
        s_dpad = clampf(s_dpad + 0.1f * (float)d, -1.0f, 1.0f);
        return s_dpad;
    }
    s_dpad = 0.0f;

    float x, y;
    input_circle(&x, &y);
    float dz = (float)s_c.stick_dz / 100.0f, outer = 0.95f; /* the pad rarely reads past ~0.95 */
    float a = fabsf(x), stick = 0.0f;
    if (a > dz) {
        stick = clampf((a - dz) / (outer - dz) * (float)s_c.stick_sens / 100.0f, 0.0f, 1.0f);
        if (x < 0.0f) stick = -stick;
    }
    if (s_c.steer == STEER_PAD || stick != 0.0f) return stick;
    return tilt_steer();
}

static float s_steer; /* this tick's value, for the menu's meter */

float controls_steer(void) {
    s_steer = steer_now();
    return s_steer;
}

/* ---------------------------------------------------------------- menu */

enum { ROW_STEER, ROW_SENS, ROW_DZ, ROW_TILT, ROW_GAS, ROW_BRAKE, ROW_NITRO, ROW_RESET, NROWS };
static const char *const k_row_name[NROWS] = {
    "STEERING", "STICK SENSITIVITY", "STICK DEAD ZONE", "TILT RANGE",
    "GAS",      "BRAKE",             "NITRO",           "RESET TO DEFAULTS",
};

static bool s_open;
static int s_row;

bool controls_menu_open(void) { return s_open; }

static int next_button(int cur, int dir) {
    for (int i = 0; i < NBUTTONS; i++) {
        cur = (cur + dir + NBUTTONS) % NBUTTONS;
        if (!k_buttons[cur].n3ds_only || sys_is_new3ds()) return cur;
    }
    return cur;
}

/* Moves one action to the next button; an action that already had that button takes this
 * one's old button, so no two actions ever share one. */
static void set_button(int *which, int dir) {
    int old = *which, nb = next_button(old, dir);
    int *acts[] = {&s_c.gas, &s_c.brake, &s_c.nitro};
    for (int i = 0; i < 3; i++)
        if (acts[i] != which && *acts[i] == nb) *acts[i] = old;
    *which = nb;
}

static void change(int row, int dir) {
    Controls before = s_c;
    switch (row) {
    case ROW_STEER: s_c.steer = s_c.steer == STEER_PAD ? STEER_TILT : STEER_PAD; break;
    case ROW_SENS: s_c.stick_sens = clampi(s_c.stick_sens + 10 * dir, 50, 200); break;
    case ROW_DZ: s_c.stick_dz = clampi(s_c.stick_dz + 5 * dir, 0, 40); break;
    case ROW_TILT: s_c.tilt_range = clampi(s_c.tilt_range + 5 * dir, 10, 60); break;
    case ROW_GAS: set_button(&s_c.gas, dir); break;
    case ROW_BRAKE: set_button(&s_c.brake, dir); break;
    case ROW_NITRO: set_button(&s_c.nitro, dir); break;
    case ROW_RESET: s_c = k_default; break;
    }
    if (memcmp(&before, &s_c, sizeof s_c)) s_dirty = true;
}

/* Bottom-screen layout, in pixels */
#define ROW_Y0 32.0f
#define ROW_H 19.0f

bool controls_menu_update(void) {
    uint32_t down = input_down();
    if (!s_open) {
        if (down & KEY_SELECT) {
            s_open = true;
            s_dirty = false;
            sys_log("controls: menu opened");
            return true;
        }
        return false;
    }
    if (down & (KEY_SELECT | KEY_B | KEY_START)) {
        s_open = false;
        if (s_dirty) save();
        return true; /* this tick's buttons belonged to the menu */
    }
    if (down & (KEY_DUP | KEY_CPAD_UP)) s_row = (s_row + NROWS - 1) % NROWS;
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) s_row = (s_row + 1) % NROWS;
    if (down & (KEY_DLEFT | KEY_CPAD_LEFT)) change(s_row, -1);
    if (down & (KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_A)) change(s_row, 1);
    if (down & KEY_TOUCH) {
        /* tap a row to pick it, tap the picked row again to change it */
        touchPosition tp;
        hidTouchRead(&tp);
        int r = (int)(((float)tp.py - ROW_Y0) / ROW_H);
        if (tp.py >= ROW_Y0 && r >= 0 && r < NROWS) {
            if (r == s_row) change(r, tp.px < 160 && r != ROW_RESET ? -1 : 1);
            s_row = r;
        }
    }
    return true;
}

/* --- drawing
 *
 * Text uses the menus' bitmap font, texture 0x97 (/pics/font_3_b.png: a 10 x 10 grid of ASCII
 * from 32, each cell 0.1 of the texture), monospaced with a one-pixel black shadow on two
 * sides, as fn_00023808 draws it. One draw per string. */

#define FONT_TEX 0x97
#define MAX_CHARS 48

/* texture table entry (engine.c): GL name at +0, the image's share of its power-of-two
 * texture at +0x14 / +0x18 */
static uint8_t *font_entry(void) { return (uint8_t *)G(0x4060c) + FONT_TEX * 0x20; }

/* width in pixels of `s` with glyphs `sz` pixels square */
static float text_w(const char *s, float sz) { return (float)strlen(s) * (sz + 1.0f) - 1.0f; }

static void text_pass(float x, float y, const char *s, float sz, float r, float g, float b) {
    float su = AT_F32(font_entry(), 0x14), sv = AT_F32(font_entry(), 0x18);
    static float pos[MAX_CHARS * 12], tc[MAX_CHARS * 12];
    int n = 0;
    for (; *s && n < MAX_CHARS; s++, x += sz + 1.0f) {
        int c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c -= 32;
        int idx = c - 0x20;
        if (idx <= 0 || idx >= 100) continue; /* space and anything outside the grid */
        float u = (float)(idx % 10) * 0.1001f, v = (float)(idx / 10) * 0.1001f;
        float x1 = x + sz, y1 = y + sz, u1 = (u + 0.1f) * su, v1 = (v + 0.1f) * sv;
        u *= su;
        v *= sv;
        float *p = pos + n * 12, *t = tc + n * 12;
        p[0] = x, p[1] = y, p[2] = x1, p[3] = y, p[4] = x, p[5] = y1;
        p[6] = x1, p[7] = y, p[8] = x1, p[9] = y1, p[10] = x, p[11] = y1;
        t[0] = u, t[1] = v, t[2] = u1, t[3] = v, t[4] = u, t[5] = v1;
        t[6] = u1, t[7] = v, t[8] = u1, t[9] = v1, t[10] = u, t[11] = v1;
        n++;
    }
    if (!n) return;
    glColor4f(r, g, b, 1.0f);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    glDrawArrays(GL_TRIANGLES, 0, n * 6);
}

static void text(float x, float y, const char *s, float sz, float r, float g, float b) {
    GLuint tex = AT_U32(font_entry(), 0);
    if (!tex) return; /* the font isn't loaded yet (only very early at boot) */
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    text_pass(x + 1.0f, y + 1.0f, s, sz, 0.0f, 0.0f, 0.0f);
    text_pass(x, y, s, sz, r, g, b);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

/* Right-aligned at x, shrunk if it would run left of `min_x`. */
static void text_right(float x, float min_x, float y, const char *s, float sz, float r, float g,
                       float b) {
    float w = text_w(s, sz);
    if (x - w < min_x && w > 0.0f) {
        float k = (x - min_x) / w;
        y += sz * (1.0f - k) * 0.5f;
        sz *= k;
        w = text_w(s, sz);
    }
    text(x - w, y, s, sz, r, g, b);
}

static void text_center(float y, const char *s, float sz, float r, float g, float b) {
    text((320.0f - text_w(s, sz)) * 0.5f, y, s, sz, r, g, b);
}

static void row_value(int row, char *buf, size_t n) {
    switch (row) {
    case ROW_STEER: snprintf(buf, n, "%s", s_c.steer == STEER_TILT ? "TILT" : "CIRCLE PAD"); break;
    case ROW_SENS: snprintf(buf, n, "%d", s_c.stick_sens); break; /* the font has no % */
    case ROW_DZ: snprintf(buf, n, "%d", s_c.stick_dz); break;
    case ROW_TILT: snprintf(buf, n, "%d DEG", s_c.tilt_range); break;
    case ROW_GAS: snprintf(buf, n, "%s", k_buttons[s_c.gas].name); break;
    case ROW_BRAKE: snprintf(buf, n, "%s", k_buttons[s_c.brake].name); break;
    case ROW_NITRO: snprintf(buf, n, "%s", k_buttons[s_c.nitro].name); break;
    default: buf[0] = 0; break;
    }
}

static void draw_menu(float steer) {
    gfxGL_OverlayRect(0, 0, 320, 240, 0.04f, 0.04f, 0.08f, 1.0f);
    gfxGL_OverlayRect(0, 0, 320, 28, 0.55f, 0.05f, 0.05f, 1.0f);
    text_center(7.0f, "CONTROLS", 14.0f, 1.0f, 1.0f, 1.0f);

    for (int r = 0; r < NROWS; r++) {
        float y = ROW_Y0 + (float)r * ROW_H;
        bool sel = r == s_row;
        bool dim = (r == ROW_TILT && s_c.steer != STEER_TILT);
        if (sel) gfxGL_OverlayRect(4, y, 312, ROW_H - 1, 0.75f, 0.12f, 0.08f, 0.9f);
        float lc = dim ? 0.5f : 1.0f;
        text(10.0f, y + 4.0f, k_row_name[r], 10.0f, lc, lc, lc);
        char v[24];
        row_value(r, v, sizeof v);
        if (v[0]) {
            char shown[32];
            snprintf(shown, sizeof shown, sel ? "< %s >" : "%s", v);
            float min_x = 16.0f + text_w(k_row_name[r], 10.0f);
            text_right(310.0f, min_x, y + 4.0f, shown, 10.0f, 1.0f, dim ? 0.55f : 0.85f,
                       dim ? 0.35f : 0.2f);
        }
    }

    /* live steering meter, to try the settings before racing */
    float my = ROW_Y0 + NROWS * ROW_H + 6.0f;
    text(10.0f, my, "STEER", 8.0f, 0.8f, 0.8f, 0.8f);
    gfxGL_OverlayRect(70, my, 240, 8, 0.2f, 0.2f, 0.25f, 1.0f);
    float cx = 190.0f, w = steer * 120.0f;
    gfxGL_OverlayRect(w < 0 ? cx + w : cx, my, fabsf(w), 8, 1.0f, 0.8f, 0.15f, 1.0f);
    gfxGL_OverlayRect(cx - 0.5f, my - 2, 1, 12, 1, 1, 1, 1);

    text_center(224.0f, "SELECT OR B: CLOSE AND SAVE", 8.0f, 0.7f, 0.7f, 0.75f);
}

void controls_menu_draw(void) {
    if (!s_open) gfxGL_DrawBottomIdle();
    gfxGL_OverlayBegin(GFX_SCREEN_BOTTOM);
    if (s_open) {
        draw_menu(s_steer);
    } else {
        gfxGL_OverlayRect(0, 224, 320, 16, 0.0f, 0.0f, 0.0f, 0.55f);
        text_center(228.0f, "SELECT: CONTROLS", 8.0f, 0.85f, 0.85f, 0.85f);
    }
    gfxGL_OverlayEnd();
    G_S32(0x40608) = -1; /* the game's bound-texture cache: the font was bound behind its back */
}

/* Controls and 3D settings and their save file (see controls.h). The player changes them on
 * the Options screen's Controls and 3D pages (opt_menu.c).
 *
 * Steering reaches the game through its own tilt input (game_main.c feed_steering), so the
 * Circle Pad, the D-pad and the tilt all go through the game's steering code and its per-car
 * handling, exactly as the iPhone's accelerometer did.
 *
 * Build 10 and earlier had a bottom-screen menu on SELECT for these; build 11 moved them into
 * the game's own Options screen. */
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "controls.h"
#include "input.h"
#include "ios.h"
#include "net_game.h"

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
    .stereo_depth = 100,
    .stereo_pop = 1,
    .stereo_tilt = 0,
    .hud_depth = 0,
};

static Controls s_c, s_saved;

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
    c->stereo_depth = clampi(c->stereo_depth, 50, 150);
    c->stereo_pop = clampi(c->stereo_pop, 0, 2);
    c->stereo_tilt = c->stereo_tilt != 0;
    c->hud_depth = c->hud_depth != 0;
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
        else if (!strcmp(key, "3d_depth")) s_c.stereo_depth = v;
        else if (!strcmp(key, "3d_popout")) s_c.stereo_pop = v;
        else if (!strcmp(key, "3d_while_tilting")) s_c.stereo_tilt = v;
        else if (!strcmp(key, "hud_depth")) s_c.hud_depth = v;
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
               "gas %d\nbrake %d\nnitro %d\n3d_depth %d\n3d_popout %d\n3d_while_tilting %d\n"
               "hud_depth %d\n",
            s_c.steer, s_c.stick_sens, s_c.stick_dz, s_c.tilt_range, s_c.gas, s_c.brake,
            s_c.nitro, s_c.stereo_depth, s_c.stereo_pop, s_c.stereo_tilt, s_c.hud_depth);
    fclose(f);
    sys_log("controls: saved (steer %s, stick %d%% dz %d%%, tilt %d deg, gas %s brake %s nitro %s)",
            s_c.steer == STEER_TILT ? "tilt" : "pad", s_c.stick_sens, s_c.stick_dz,
            s_c.tilt_range, k_buttons[s_c.gas].name, k_buttons[s_c.brake].name,
            k_buttons[s_c.nitro].name);
    sys_log("controls: 3d depth %d, pop-out %d, while tilting %d, hud depth %d", s_c.stereo_depth,
            s_c.stereo_pop, s_c.stereo_tilt, s_c.hud_depth);
    s_saved = s_c;
}

void controls_init(void) {
    load();
    s_saved = s_c;
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
/* Tilt = how far the console's left-right axis dips below level, asin(g.x / |g|), the same
 * measure as the iPhone version (its accelerometer's long axis). It means the same however far
 * back the console is held. (Build 5 used the turn on the screen plane, atan2(x, -y), which
 * grows without bound as the console leans back: at 60 degrees back a 20 degree tilt read as
 * 30. Recorded on hardware, 2026-11-12.) */
static float tilt_steer(void) {
    float g[3];
    input_accel_g(g);
    float mag = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (mag < 0.5f) return s_tilt_last; /* falling or shaken: no usable gravity */
    float roll = asinf(clampf(g[0] / mag, -1.0f, 1.0f)) * (180.0f / (float)M_PI);
    float a = fabsf(roll) - 2.0f;
    float m = a <= 0.0f ? 0.0f : clampf(a / ((float)s_c.tilt_range - 2.0f), 0.0f, 1.0f);
    s_tilt_last = roll < 0.0f ? -m : m;
    return s_tilt_last;
}

static bool s_tilting; /* this tick's steering came from the tilt of the console */

bool controls_tilting(void) { return s_tilting; }

static float steer_now(void) {
    s_tilting = false;
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
    s_tilting = true;
    return tilt_steer();
}

static float s_steer; /* this tick's value, for the Controls page's meter */

float controls_steer(void) {
    s_steer = steer_now();
    return s_steer;
}

/* ---------------------------------------------------------------- Options pages */

Controls *controls_edit(void) { return &s_c; }

const char *controls_button_name(int choice) {
    return choice >= 0 && choice < NBUTTONS ? k_buttons[choice].name : "?";
}

static int next_button(int cur, int dir) {
    for (int i = 0; i < NBUTTONS; i++) {
        cur = (cur + dir + NBUTTONS) % NBUTTONS;
        if (!k_buttons[cur].n3ds_only || sys_is_new3ds()) return cur;
    }
    return cur;
}

/* Moves one action (0 gas, 1 brake, 2 nitro) to the next button; an action that already had
 * that button takes this one's old button, so no two actions ever share one. */
void controls_cycle_button(int action, int dir) {
    int *acts[] = {&s_c.gas, &s_c.brake, &s_c.nitro};
    int *which = acts[action < 0 ? 0 : action > 2 ? 2 : action];
    int old = *which, nb = next_button(old, dir);
    for (int i = 0; i < 3; i++)
        if (acts[i] != which && *acts[i] == nb) *acts[i] = old;
    *which = nb;
}

void controls_reset(void) {
    Controls c = k_default;
    c.stereo_depth = s_c.stereo_depth;
    c.stereo_pop = s_c.stereo_pop;
    c.stereo_tilt = s_c.stereo_tilt;
    c.hud_depth = s_c.hud_depth;
    s_c = c;
}

void controls_commit(void) {
    sanitize(&s_c);
    if (memcmp(&s_c, &s_saved, sizeof s_c)) save();
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

static void text_center(float y, const char *s, float sz, float r, float g, float b) {
    text((320.0f - text_w(s, sz)) * 0.5f, y, s, sz, r, g, b);
}

void controls_text_center(float y, const char *s, float sz, float r, float g, float b) {
    text_center(y, s, sz, r, g, b);
}

static bool s_hud_bottom; /* this frame drew the race gauges on the bottom screen */

void hud_bottom_begin(float gx, float gy, float px, float py, float scale) {
    /* The HUD's sprites (flag 4, fn_0000526c) widen the 640 x 480 layout to the virtual
     * screen, 720 units wide (x 1.125), which the top screen stretches further to 5:3. The
     * bottom screen is 4:3 like the layout, so the ortho undoes both and the gauges keep
     * their shape. The modelview is the game's 2D one (a plain scale, fn_00004e68). */
    float mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    float kx = mv[0] * (float)G_S32(0x40368) / 640.0f, ky = mv[5] * (float)G_S32(0x40374) / 480.0f;
    float x = kx * gx + mv[12], y = ky * gy + mv[13];
    float sx = scale / kx, sy = scale / ky;
    float l = x - px / sx, t = y - py / sy;
    gfxGL_ScreenPush(GFX_SCREEN_BOTTOM);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(l, l + 320.0f / sx, t + 240.0f / sy, t, 0.0f, 1.0f);
    s_hud_bottom = true;
}

void hud_bottom_screen(void) {
    gfxGL_ScreenPush(GFX_SCREEN_BOTTOM);
    s_hud_bottom = true;
}

void hud_bottom_end(void) {
    gfxGL_ScreenPop();
}

void controls_bottom_draw(bool meter) {
    bool hud = s_hud_bottom;
    s_hud_bottom = false;
    if (!hud) gfxGL_DrawBottomIdle();
    /* the Wireless panel or a status line (net_lobby.c) */
    if (ng_bottom_draw()) G_S32(0x40608) = -1;
    if (!meter) return;
    /* the Controls page: a live steering meter, to try the settings before racing */
    gfxGL_OverlayBegin(GFX_SCREEN_BOTTOM);
    gfxGL_OverlayRect(0, 200, 320, 40, 0.0f, 0.0f, 0.0f, 0.6f);
    text_center(206.0f, "STEERING", 8.0f, 0.85f, 0.85f, 0.85f);
    float my = 222.0f, cx = 160.0f, w = s_steer * 120.0f;
    gfxGL_OverlayRect(40, my, 240, 8, 0.2f, 0.2f, 0.25f, 1.0f);
    gfxGL_OverlayRect(w < 0 ? cx + w : cx, my, fabsf(w), 8, 1.0f, 0.8f, 0.15f, 1.0f);
    gfxGL_OverlayRect(cx - 0.5f, my - 2, 1, 12, 1, 1, 1, 1);
    gfxGL_OverlayEnd();
    G_S32(0x40608) = -1; /* the game's bound-texture cache: the font was bound behind its back */
}

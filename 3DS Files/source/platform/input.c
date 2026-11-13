/* 3DS input (see input.h). */
#include <3ds.h>
#include <string.h>
#include "input.h"
#include "sys.h"

#define BIND_SLOTS 12
#define ANALOG_SLOTS 2

typedef struct {
    uint32_t keys;
    float x, y;
    bool was;
} Bind;

typedef struct {
    bool active, was;
    float x, y, px, py;
} Analog;

static uint32_t s_held, s_down;
static float s_cx, s_cy;
static bool s_touch_was;
static float s_tx, s_ty, s_tpx, s_tpy;
static Bind s_bind[BIND_SLOTS];
static Analog s_analog[ANALOG_SLOTS];
static float s_g[3] = {0.0f, -1.0f, 0.0f};
static bool s_accel_on;

/* 3DS accelerometer: about 512 units per g. Raw axes -> body frame (x right, y up along the
 * screen, z out of the screen). Raw x grows when the right side goes down, raw y is -512 held
 * upright facing the player... these signs are checked in Azahar/hardware (TASKS #29). */
#define ACCEL_UNITS_PER_G 512.0f
#define ACCEL_SX (-1.0f)
#define ACCEL_SY (1.0f)
#define ACCEL_SZ (-1.0f)

/* The motion sensor is on only while the player steers by tilt (controls.c calls
 * input_motion every tick). With the Circle Pad chosen it is switched off and the gravity
 * reads level, so no movement of the console reaches the game. The gyroscope is never used. */
void input_init(void) {}

void input_motion(bool on) {
    if (on == s_accel_on) return;
    if (on) {
        s_accel_on = R_SUCCEEDED(HIDUSER_EnableAccelerometer());
    } else {
        HIDUSER_DisableAccelerometer();
        s_accel_on = false;
        s_g[0] = 0.0f, s_g[1] = -1.0f, s_g[2] = 0.0f;
    }
    sys_log("input: motion sensor %s", s_accel_on ? "on (tilt steering)" : "off");
}

void input_exit(void) { input_motion(false); }

void input_update(void) {
    hidScanInput();
    s_held = hidKeysHeld();
    s_down = hidKeysDown();

    circlePosition cp;
    hidCircleRead(&cp);
    s_cx = cp.dx / 156.0f;
    s_cy = cp.dy / 156.0f;
    if (s_cx > 1) s_cx = 1;
    if (s_cx < -1) s_cx = -1;
    if (s_cy > 1) s_cy = 1;
    if (s_cy < -1) s_cy = -1;

    bool t = (s_held & KEY_TOUCH) != 0;
    if (t) {
        touchPosition tp;
        hidTouchRead(&tp);
        s_tpx = s_touch_was ? s_tx : tp.px / 320.0f;
        s_tpy = s_touch_was ? s_ty : tp.py / 240.0f;
        s_tx = tp.px / 320.0f;
        s_ty = tp.py / 240.0f;
    }
    s_touch_was = t;

    if (s_accel_on) {
        accelVector a;
        hidAccelRead(&a);
        float g[3] = {ACCEL_SX * a.x / ACCEL_UNITS_PER_G, ACCEL_SY * a.y / ACCEL_UNITS_PER_G,
                      ACCEL_SZ * a.z / ACCEL_UNITS_PER_G};
        for (int i = 0; i < 3; i++) s_g[i] += (g[i] - s_g[i]) * 0.3f;
    }
}

void input_end_tick(void) {
    for (int i = 0; i < BIND_SLOTS; i++) s_bind[i].was = s_bind[i].keys && (s_held & s_bind[i].keys);
    for (int i = 0; i < ANALOG_SLOTS; i++) {
        s_analog[i].was = s_analog[i].active;
        s_analog[i].px = s_analog[i].x;
        s_analog[i].py = s_analog[i].y;
    }
}

int input_touches(InputTouch *out, int max) {
    int n = 0;
    if (s_touch_was && n < max) {
        out[n++] = (InputTouch){true, true, s_tx, s_ty, s_tpx, s_tpy};
    }
    for (int i = 0; i < BIND_SLOTS && n < max; i++) {
        Bind *b = &s_bind[i];
        if (b->keys && (s_held & b->keys))
            out[n++] = (InputTouch){true, b->was, b->x, b->y, b->x, b->y, true};
    }
    for (int i = 0; i < ANALOG_SLOTS && n < max; i++) {
        Analog *a = &s_analog[i];
        if (a->active)
            out[n++] = (InputTouch){true, a->was, a->x, a->y, a->was ? a->px : a->x,
                                    a->was ? a->py : a->y, true};
    }
    return n;
}

void input_accel_g(float g[3]) { memcpy(g, s_g, sizeof s_g); }
uint32_t input_held(void) { return s_held; }
uint32_t input_down(void) { return s_down; }

void input_circle(float *x, float *y) {
    *x = s_cx;
    *y = s_cy;
}

void input_bind_button(int slot, uint32_t keys, float x, float y) {
    if (slot < 0 || slot >= BIND_SLOTS) return;
    s_bind[slot].keys = keys;
    s_bind[slot].x = x;
    s_bind[slot].y = y;
}

void input_set_analog_touch(int slot, bool active, float x, float y) {
    if (slot < 0 || slot >= ANALOG_SLOTS) return;
    s_analog[slot].active = active;
    s_analog[slot].x = x;
    s_analog[slot].y = y;
}

/* 3DS input: touch screen, buttons, circle pad, accelerometer/gyro.
 *
 * The iOS game is driven by up to 10 touches on its virtual screen and the accelerometer.
 * input.c turns the bottom screen and the physical controls into that shape: real touches
 * plus "virtual touches" that hold down on-screen buttons of the game while a physical button
 * is pressed (see input_bind_button). Coordinates are normalized 0..1 over the game's
 * virtual screen. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define INPUT_MAX_TOUCHES 10

typedef struct {
    bool down;      /* active this tick */
    bool moved;     /* active last tick too (px/py valid) */
    float x, y;     /* 0..1 over the virtual screen */
    float px, py;   /* previous position */
} InputTouch;

void input_init(void);
void input_exit(void);
/* Once per 60 Hz tick, before the game reads anything. */
void input_update(void);
/* Called after the game tick (clears per-tick edges). */
void input_end_tick(void);

/* Touches for the game's touch table, real ones first. Returns the count. */
int input_touches(InputTouch *out, int max);

/* Gravity in g in the 3DS body frame: x right along the long side, y up along the screen,
 * z out of the screen towards the player (held upright: (0,-1,0)). Low-pass filtered. */
void input_accel_g(float g[3]);

/* Raw state for the platform layer. */
uint32_t input_held(void);
uint32_t input_down(void);
/* Circle pad, -1..1. */
void input_circle(float *x, float *y);

/* While any key in `keys` is held, keep a virtual touch at (x, y) (normalized). Slot 0..11.
 * keys = 0 clears the slot. */
void input_bind_button(int slot, uint32_t keys, float x, float y);
/* An analog virtual touch that follows a value: while `active`, touch at (x, y). Slot 0..1. */
void input_set_analog_touch(int slot, bool active, float x, float y);

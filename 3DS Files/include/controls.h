/* The port's controls and 3D settings. The player sets them on the Options screen's Controls
 * and 3D pages (opt_menu.c).
 *
 * The iOS game only knew touch buttons and the accelerometer; the 3DS adds the Circle Pad,
 * the D-pad and buttons the player can choose. The settings are saved to
 * sdmc:/3ds/PixelRacing3D/controls.cfg. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { STEER_PAD = 0, STEER_TILT = 1 };

typedef struct {
    int steer;      /* STEER_PAD (Circle Pad) or STEER_TILT; the D-pad steers in both */
    int stick_sens; /* percent, 50..200 */
    int stick_dz;   /* Circle Pad dead zone, percent 0..40 */
    int tilt_range; /* degrees of roll for full lock, 10..60 */
    int gas, brake, nitro; /* index into the button list (controls_key) */
    int stereo_depth; /* 3D: percent of the normal eye separation, 50..150 */
    int stereo_pop;   /* 3D: how far things come out of the screen, 0..2 (screen plane) */
    int stereo_tilt;  /* 3D stays on while steering by tilt (off by default) */
    int hud_depth;    /* the HUD floats in front of the screen (0: at screen depth) */
} Controls;

void controls_init(void);
const Controls *controls_get(void);
/* 3DS key mask of a button choice. */
uint32_t controls_key(int choice);

/* Steering this tick, -1 (left) .. 1 (right), from the D-pad, the Circle Pad and (when chosen)
 * the tilt of the console. */
float controls_steer(void);
/* True when this tick's steering came from tilting the console (not the pads): the game's
 * "Tilting Horizon" rolls the view only then. */
bool controls_tilting(void);

/* For the Options pages: change the settings in place, then controls_commit checks and saves
 * them (if they changed). */
Controls *controls_edit(void);
const char *controls_button_name(int choice);
/* Moves gas (0), brake (1) or nitro (2) to the next (dir 1) or previous button. */
void controls_cycle_button(int action, int dir);
/* The controls back to their defaults (the 3D settings stay). */
void controls_reset(void);
void controls_commit(void);
/* The race gauges and the track map on the bottom screen (single-player races). The game's
 * HUD code lays them out in 640 x 480 units (y down); between begin and end its point
 * (gx, gy) lands on bottom-screen pixel (px, py) at `scale` pixels per unit. Call it after
 * the game has set its 2D modelview. A frame that draws them gets no idle logo. */
void hud_bottom_begin(float gx, float gy, float px, float py, float scale);
void hud_bottom_end(void);
/* The bottom screen after the game's frame: the logo unless the race gauges are there, and
 * with `meter` a live steering meter (the Controls page). */
void controls_bottom_draw(bool meter);
/* Text on the bottom screen between gfxGL_OverlayBegin/End: centred at y, glyphs sz pixels
 * square (the game's font, upper case). */
void controls_text_center(float y, const char *s, float sz, float r, float g, float b);

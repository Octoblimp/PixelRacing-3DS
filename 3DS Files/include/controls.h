/* The port's controls settings and their bottom-screen menu (SELECT opens it).
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

/* The menu. controls_menu_update runs once per tick and returns true while the menu is open
 * (the game must not see any input then); controls_menu_draw draws the bottom screen. */
bool controls_menu_update(void);
bool controls_menu_open(void);
void controls_menu_draw(void);

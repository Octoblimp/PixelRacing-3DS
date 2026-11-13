/* Multi-card races: the glue between the game's race tick and the lockstep (net_race.h).
 *
 * The game's race code already handles 2-4 human cars (the iOS split-screen mode). A
 * multi-card race raises the player count for the race only, fills every player's input
 * block from the network, steps the race only when every player's input for the tick is here,
 * and draws this console's player alone, full screen, as in a one-player race. The game's
 * split-screen (several views on one screen) is never shown. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Player p's input block. The game addresses player p's at 0x5c160 + p*0x24 (analog at
 * +0x8c, race buttons at +0xa0..+0xae), but from player 1 on that overlaps the car controls
 * at 0x5c210 (the iOS split-screen was never finished): players 1-3 get blocks of their own
 * here, with the same offsets. Player 0's is the game's. */
uint8_t *ng_pad(int p);

/* tick.c (split out of fn_00009dc0) */
int tick_steer(void);
void tick_fill_pad(int pl, unsigned k, unsigned b, int v);

/* net_game.c: the session the lobby set up (players' UDS nodes, player 0 = the host). */
void ng_set_session(int players, int local, uint32_t seed, const uint16_t *nodes);
void ng_end_session(void);
uint16_t ng_node(int p);
/* The next random seed of the session (the race setup, m_0a184.c). */
uint32_t ng_seed_next(void);

/* net_lobby.c. Every tick (app_tick): the Wireless panel's buttons, the lobby messages. */
void ng_lobby_tick(void);
/* The panel is open: the game's menus get no buttons. */
bool ng_panel_open(void);
/* The bottom screen after the game's frame: the panel or a status line. True when it drew. */
bool ng_bottom_draw(void);
/* tick.c, the track browser: `pressed` = the player starts. True when the race may start
 * (in a session: once every console has started and the race setup is shared). */
bool ng_menu_start(bool pressed);

/* A multi-card session is set up (or the debug loopback test is on). */
bool ng_session(void);
/* Players in the session, and this console's player. */
int ng_players(void);
int ng_local(void);
/* The game state changes (fn_0000a484): sets the race's player count. */
void ng_state(int state);
/* In place of fn_00009dc0(0) in the race tick. Returns true when the race steps this tick. */
bool ng_race_input(void);
/* After a race tick that stepped. */
void ng_race_stepped(void);
/* The car whose view is drawn: this console's player in a multi-card race, else -1. */
int ng_view_car(void);
/* Around the race overlay (fn_00020f70): one player, this console's view full screen.
 * ng_draw_begin returns that view (-1 outside a multi-card race); ng_drawn_view tells it
 * while drawing. */
int ng_draw_begin(void);
void ng_draw_end(void);
int ng_drawn_view(void);

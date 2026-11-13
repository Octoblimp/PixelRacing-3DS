/* Multi-card races in lockstep (net_race.c), on the UDS transport (net_uds.h).
 *
 * Every console runs the same race, with the same setup and random seed, and steps it with
 * every player's input for the same game tick, so the simulations stay identical without
 * sending any car state. Each console draws only its own player, full screen (no split
 * screen: the game's own split-screen views are never shown).
 *
 * Inputs are scheduled NR_DELAY ticks ahead (local input read at tick t drives tick
 * t + NR_DELAY) and every packet repeats the last NR_RESEND ticks, so a lost packet is covered
 * by the next one. A console that lacks someone's input for the next tick waits (draws the
 * same frame) until it arrives. Every NR_CHECK_TICKS ticks the consoles compare a checksum of
 * the race state and log a mismatch.
 *
 * Loopback (a debug test on one console): the other players are stand-ins that only hold the
 * gas, and nothing is sent. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NR_MAX_PLAYERS 4
#define NR_DELAY 3
#define NR_RESEND 12
#define NR_CHECK_TICKS 60

/* One player's controls for one game tick: what the race reads from the player's input block
 * (tick.c fn_00009dc0). */
typedef struct {
    int16_t steer;  /* -100..100 */
    uint16_t bits;  /* held race buttons (fn_0002bf68: 0x100 gas, 0x200 brake, 0x800 reverse..) */
    uint16_t keys;  /* pressed buttons (fn_0000a184) */
    uint16_t pad;
} NrInput;

/* Starts lockstep for a race of `players` (2..4), this console being player `local`. */
void nr_start(int players, int local, bool loopback);
void nr_stop(void);
bool nr_active(void);
int nr_players(void);
int nr_local(void);
uint32_t nr_tick(void);

/* Reads the network (call once per game tick). */
void nr_poll(void);
/* This console's input for the tick NR_DELAY ahead (ignored while the race is waiting for
 * others: the delay stays NR_DELAY). Always resends the window. Call once per game tick. */
void nr_submit(const NrInput *in);
/* True when every player's input for the next tick is here. */
bool nr_ready(void);
/* The next tick's input of player p (valid after nr_ready). */
const NrInput *nr_input(int p);
/* The tick has been simulated: hash = a checksum of the race state after it. */
void nr_advance(uint32_t hash);

/* Players whose input hasn't come for a while (bit p), for the waiting message. */
uint32_t nr_waiting(void);

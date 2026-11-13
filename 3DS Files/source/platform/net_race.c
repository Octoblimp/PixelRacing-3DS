/* Lockstep input exchange for multi-card races (see net_race.h). */
#include <string.h>
#include "net_race.h"
#include "net_uds.h"
#include "sys.h"

enum { MSG_INPUT = 0x31, MSG_CHECK = 0x32 };

#define RING 256 /* ticks of input kept per player (power of two) */

typedef struct {
    uint32_t tick[RING]; /* which tick each slot holds (+1; 0 = empty) */
    NrInput in[RING];
    uint32_t last_ms;    /* when this player's input last arrived */
} PlayerRing;

static bool s_on, s_loop;
static int s_players, s_local;
static uint32_t s_tick;   /* the next tick to simulate */
static uint32_t s_sent;   /* the next tick this console submits input for */
static PlayerRing s_ring[NR_MAX_PLAYERS];
static uint32_t s_check[RING]; /* own state checksum per tick */
static uint32_t s_desyncs, s_stalls;

/* Packet: type, player, count, pad, first tick (LE u32), then count x NrInput. */
typedef struct {
    uint8_t type, player, count, pad;
    uint32_t first;
    NrInput in[NR_RESEND];
} InputMsg;

typedef struct {
    uint8_t type, player, pad[2];
    uint32_t tick, hash;
} CheckMsg;

static void store(int p, uint32_t tick, const NrInput *in) {
    PlayerRing *r = &s_ring[p];
    r->tick[tick & (RING - 1)] = tick + 1;
    r->in[tick & (RING - 1)] = *in;
}

static bool have(int p, uint32_t tick) {
    return s_ring[p].tick[tick & (RING - 1)] == tick + 1;
}

void nr_start(int players, int local, bool loopback) {
    memset(s_ring, 0, sizeof s_ring);
    memset(s_check, 0, sizeof s_check);
    s_players = players < 1 ? 1 : players > NR_MAX_PLAYERS ? NR_MAX_PLAYERS : players;
    s_local = local;
    s_loop = loopback;
    s_tick = 0;
    s_desyncs = 0;
    s_stalls = 0;
    /* the first NR_DELAY ticks run with no input from anyone */
    NrInput none = {0, 0, 0, 0};
    for (int p = 0; p < s_players; p++) {
        for (uint32_t t = 0; t < NR_DELAY; t++) store(p, t, &none);
        s_ring[p].last_ms = sys_ms();
    }
    s_sent = NR_DELAY;
    s_on = true;
    sys_log("net race: lockstep, %d players, this console is player %d%s", s_players, s_local,
            s_loop ? " (loopback test)" : "");
}

void nr_stop(void) {
    if (s_on)
        sys_log("net race: stopped at tick %lu, %lu desyncs, %lu ticks waited", (unsigned long)s_tick,
                (unsigned long)s_desyncs, (unsigned long)s_stalls);
    s_on = false;
}

bool nr_active(void) { return s_on; }
int nr_players(void) { return s_players; }
int nr_local(void) { return s_local; }
uint32_t nr_tick(void) { return s_tick; }

void nr_poll(void) {
    if (!s_on || s_loop || !net_active()) return;
    uint8_t buf[NET_MAX_MSG];
    uint16_t from;
    int n;
    while ((n = net_recv(buf, sizeof buf, &from)) > 0) {
        if (buf[0] == MSG_INPUT && n >= 8) {
            const InputMsg *m = (const InputMsg *)buf;
            int p = m->player, cnt = m->count;
            if (p >= s_players || p == s_local || cnt > NR_RESEND || n < 8 + cnt * (int)sizeof(NrInput))
                continue;
            for (int i = 0; i < cnt; i++) {
                uint32_t t = m->first + i;
                /* keep only ticks not yet simulated and within the ring */
                if (t >= s_tick && t < s_tick + RING) store(p, t, &m->in[i]);
            }
            s_ring[p].last_ms = sys_ms();
        } else if (buf[0] == MSG_CHECK && n >= (int)sizeof(CheckMsg)) {
            const CheckMsg *m = (const CheckMsg *)buf;
            uint32_t mine = s_check[m->tick & (RING - 1)];
            if (m->tick < s_tick && s_tick - m->tick < RING && mine && mine != m->hash) {
                if (s_desyncs++ < 8)
                    sys_log("net race: DESYNC at tick %lu: player %d has %08lx, this console %08lx",
                            (unsigned long)m->tick, m->player, (unsigned long)m->hash,
                            (unsigned long)mine);
            }
        }
    }
}

void nr_submit(const NrInput *in) {
    if (!s_on) return;
    if (s_sent <= s_tick + NR_DELAY) {
        store(s_local, s_sent, in);
        if (s_loop) {
            /* stand-ins: hold the gas, wheel straight */
            NrInput gas = {0, 0x100, 0, 0};
            for (int p = 0; p < s_players; p++)
                if (p != s_local) store(p, s_sent, &gas);
        }
        s_sent++;
    }
    if (s_loop || !net_active()) return;
    InputMsg m;
    memset(&m, 0, sizeof m);
    m.type = MSG_INPUT;
    m.player = (uint8_t)s_local;
    uint32_t first = s_sent > NR_RESEND ? s_sent - NR_RESEND : 0;
    m.count = (uint8_t)(s_sent - first);
    m.first = first;
    for (uint32_t i = 0; i < m.count; i++) m.in[i] = s_ring[s_local].in[(first + i) & (RING - 1)];
    net_send(NET_BROADCAST, &m, 8 + m.count * (int)sizeof(NrInput));
}

bool nr_ready(void) {
    if (!s_on) return false;
    for (int p = 0; p < s_players; p++)
        if (!have(p, s_tick)) {
            s_stalls++;
            return false;
        }
    return true;
}

const NrInput *nr_input(int p) {
    static const NrInput none = {0, 0, 0, 0};
    if (!s_on || p < 0 || p >= s_players || !have(p, s_tick)) return &none;
    return &s_ring[p].in[s_tick & (RING - 1)];
}

void nr_advance(uint32_t hash) {
    if (!s_on) return;
    s_check[s_tick & (RING - 1)] = hash;
    if (s_tick % NR_CHECK_TICKS == 0 && !s_loop && net_active()) {
        CheckMsg m = {MSG_CHECK, (uint8_t)s_local, {0, 0}, s_tick, hash};
        net_send(NET_BROADCAST, &m, sizeof m);
    }
#ifdef DEBUG_LOG
    if (s_tick % 600 == 0)
        sys_log("net race: tick %lu hash %08lx", (unsigned long)s_tick, (unsigned long)hash);
#endif
    s_tick++;
}

uint32_t nr_waiting(void) {
    uint32_t w = 0, now = sys_ms();
    for (int p = 0; p < s_players; p++)
        if (p != s_local && !have(p, s_tick) && now - s_ring[p].last_ms > 500) w |= 1u << p;
    return w;
}

/* Multi-card races: the glue between the race tick and the lockstep (see net_game.h). */
#include "ios.h"
#include "net_game.h"
#include "net_race.h"
#include "net_uds.h"
#include "sys.h"

#define NPLAYERS G_S32(0x3e9b0)
#define CAR(i) ((uint8_t *)G(0x48ae0) + (i) * 0x678)

static int s_players, s_local; /* the session (0 players = none) */
static uint8_t s_pads[NR_MAX_PLAYERS - 1][0xb0];

uint8_t *ng_pad(int p) {
    if (p <= 0 || p >= NR_MAX_PLAYERS) return G(0x5c160);
    return s_pads[p - 1];
}
static bool s_loopback, s_raised;
static uint16_t s_nodes[NR_MAX_PLAYERS]; /* each player's console (UDS node) */
static uint32_t s_seed;                   /* the session's random seed, stepped per race */
static uint32_t s_stall_ms;               /* the race has waited since (0 = not waiting) */

void ng_set_session(int players, int local, uint32_t seed, const uint16_t *nodes) {
    s_players = players;
    s_local = local;
    s_seed = seed;
    s_loopback = false;
    memcpy(s_nodes, nodes, sizeof s_nodes);
}

void ng_end_session(void) {
    if (nr_active()) nr_stop();
    if (s_loopback) return;
    s_players = 0; /* s_local stays: a race whose others left goes on with this car */
}

uint16_t ng_node(int p) { return (p >= 0 && p < NR_MAX_PLAYERS) ? s_nodes[p] : 0; }

uint32_t ng_seed_next(void) {
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed;
}

/* Debug: a file sdmc:/3ds/PixelRacing_netloop turns every race into a 2-player lockstep
 * race on this console, player 2 being a stand-in that holds the gas. */
static void check_loopback(void) {
#ifdef DEBUG_LOG
    static bool s_checked;
    if (s_checked) return;
    s_checked = true;
    FILE *f = fopen("sdmc:/3ds/PixelRacing_netloop", "rb");
    if (f) {
        fclose(f);
        s_loopback = true;
        s_players = 2;
        s_local = 0;
        sys_log("net race: loopback test on (sdmc:/3ds/PixelRacing_netloop)");
    }
#endif
}

bool ng_session(void) {
    check_loopback();
    return s_players > 1;
}
int ng_players(void) { return s_players; }
int ng_local(void) { return s_local; }

static bool s_raised_alone; /* the others left mid-race: the race goes on with no lockstep */

/* A race of the session: the race state, not a replay or the attract mode. */
static bool net_race(void) {
    return ng_session() && G_S32(0x3f848) == 5 && G_S32(0x3f984) != 1 && !G_U8(0x3f98c) &&
           NPLAYERS == s_players;
}

void ng_state(int state) {
    if (state != 5 && nr_active()) nr_stop();
    if (state == 5 && ng_session()) {
        /* the race's setup draws random numbers: the same ones on every console */
        srand(ng_seed_next());
        s_stall_ms = 0;
    }
    if (state == 6 && ng_session()) {
        /* the race's human cars: one per console */
        NPLAYERS = s_players;
        s_raised = true;
        sys_log("net race: %d players, this console drives car %d", s_players, s_local);
    } else if (state == 4 && (s_raised || s_raised_alone)) {
        s_raised_alone = false;
        if (!ng_session()) s_local = 0;
        /* back in the menus: one player, as everywhere outside a multi-card race */
        NPLAYERS = 1;
        s_raised = false;
    }
}

bool ng_race_input(void) {
    if (!net_race()) {
        if (nr_active()) nr_stop();
        if (s_raised_alone && NPLAYERS > 1) {
            /* the others left: this console's car on its own input, theirs stand still */
            for (int p = 0; p < NPLAYERS && p < NR_MAX_PLAYERS; p++)
                if (p != s_local) memset(ng_pad(p) + 0x8c, 0, 0x24);
            fn_00009dc0(s_local);
        } else {
            fn_00009dc0(0);
        }
        return true;
    }
    if (!nr_active()) nr_start(s_players, s_local, s_loopback);

    fn_0000442c();
    if (fn_00004540() & 0xff)
        return false; /* paused (HOME Menu): the others wait */

    /* Pause in a multi-card race: the pause menu (fn_0002bf68 bit 0x1010) goes to every
     * console through the input, as in the split-screen original, where any player resumes or
     * quits. Before the countdown ends the button would open the help pages on this console
     * alone instead: ignore it then. */
    uint8_t *btn = G(0x40388);
    if (G_U8(0x3fab4) == 0) {
        btn[0x174] = 0;
        btn[0x175] = 0;
    }
    NrInput mine;
    mine.keys = (uint16_t)fn_0000a184(s_local);
    mine.bits = (uint16_t)fn_0002bf68(s_local);
    mine.steer = (int16_t)tick_steer();
    mine.pad = 0;
    nr_submit(&mine);
    nr_poll();
    if (!nr_ready()) {
        /* a console that left: after 3 s the race goes on here without lockstep, its car
         * standing still */
        uint32_t now = sys_ms();
        if (!s_stall_ms) s_stall_ms = now;
        uint32_t w = nr_waiting(), gone = 0;
        for (int p = 0; p < s_players; p++)
            if ((w & (1u << p)) && !(net_nodes() & (1u << s_nodes[p]))) gone |= 1u << p;
        if (!s_loopback && gone && now - s_stall_ms > 3000) {
            sys_log("net race: player(s) %lx left, the race goes on alone", (unsigned long)gone);
            if (net_active()) net_leave();
            ng_end_session();
            s_raised_alone = true;
        }
        return false;
    }
    s_stall_ms = 0;
    for (int p = 0; p < s_players; p++) {
        const NrInput *in = nr_input(p);
        tick_fill_pad(p, in->keys, in->bits, in->steer);
    }
    return true;
}

static uint32_t race_hash(void) {
    uint32_t h = 2166136261u;
    int n = G_S32(0x3e9c8);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        const uint8_t *c = CAR(i);
        const uint8_t *parts[2] = {c + 0x2bc, c + 0x264};
        const int len[2] = {12, 4};
        for (int k = 0; k < 2; k++)
            for (int j = 0; j < len[k]; j++) h = (h ^ parts[k][j]) * 16777619u;
    }
    h = (h ^ (uint32_t)G_S32(0x3f988)) * 16777619u;
    return h;
}

void ng_race_stepped(void) {
    if (!nr_active()) return;
#ifdef DEBUG_LOG
    if (nr_tick() % 300 == 0) {
        char buf[128];
        int o = 0;
        for (int p = 0; p < s_players && o < 100; p++)
            o += snprintf(buf + o, sizeof buf - o, " p%d %.0f,%.0f v%.1f", p,
                          AT_F32(CAR(p), 0x2bc), AT_F32(CAR(p), 0x2c4), AT_F32(CAR(p), 0x264));
        sys_log("net race: tick %lu cars%s", (unsigned long)nr_tick(), buf);
    }
#endif
    nr_advance(race_hash());
}

int ng_view_car(void) {
    /* from the race setup until back in the menus: the race, the results, the podium */
    return ((s_raised || s_raised_alone) && NPLAYERS > 1) ? s_local : -1;
}

/* Drawing a multi-card race: the overlay (fn_00020f70) draws as in a one-player race, this
 * console's view alone, full screen with the one-player HUD. */
static int s_draw = -1, s_draw_np;
static float s_draw_rect[4];

int ng_draw_begin(void) {
    int k = ng_view_car();
    s_draw = k;
    if (k < 0) return -1;
    uint8_t *vw = G(0x5c160) + k * 4;
    for (int i = 0; i < 4; i++) s_draw_rect[i] = AT_F32(vw, 0x28 + i * 0x10);
    AT_F32(vw, 0x28) = 0.0f;
    AT_F32(vw, 0x38) = 0.0f;
    AT_F32(vw, 0x48) = 1.0f;
    AT_F32(vw, 0x58) = 1.0f;
    s_draw_np = NPLAYERS;
    NPLAYERS = 1;
    return k;
}

void ng_draw_end(void) {
    if (s_draw < 0) return;
    uint8_t *vw = G(0x5c160) + s_draw * 4;
    for (int i = 0; i < 4; i++) AT_F32(vw, 0x28 + i * 0x10) = s_draw_rect[i];
    NPLAYERS = s_draw_np;
    s_draw = -1;
}

int ng_drawn_view(void) { return s_draw; }

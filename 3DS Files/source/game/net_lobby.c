/* Multi-card races: the lobby (see net_game.h).
 *
 * In the menus SELECT opens the Wireless panel on the bottom screen (physical buttons only):
 * host a race, or search for one and join it. Then everyone picks a mode, car, colour, face
 * and name in the game's own menus as usual, and starts. A client that starts sends its car
 * (READY) and waits. When the host starts and every client is ready, the host sends the race
 * (START): the players' consoles in order (player 0 = the host), the host's mode, options,
 * unlocked tracks and cars, every player's car, and a random seed. Every console then builds
 * the same race from that: the same computer drivers (fn_000117b0 on the shared seed), the
 * same track, weather and grid, and races it in lockstep (net_race.c). */
#include <3ds.h>
#include "ios.h"
#include "controls.h"
#include "input.h"
#include "net_game.h"
#include "net_race.h"
#include "net_uds.h"
#include "sys.h"

#define NPLAYERS G_S32(0x3e9b0)
#define NCARS G_S32(0x3e9c8)
#define MODE_CUR G_S32(0x3f9dc)
#define SETUP(i) ((uint8_t *)G(0x47544) + (i) * 0x18)
#define CAR(i) ((uint8_t *)G(0x48ae0) + (i) * 0x678)

enum { MSG_HELLO = 0x21, MSG_READY = 0x22, MSG_START = 0x23, MSG_ACK = 0x24 };

/* options the host's race uses: difficulty, drive assist, laps, players only, pit stops */
static const uint32_t k_opts[5] = {0x3e9b8, 0x3e9bc, 0x3e9e4, 0x3f8bc, 0x3ea20};

typedef struct {
    uint8_t type, pad[3];
    uint8_t setup[0x18];
} ReadyMsg;

typedef struct {
    uint8_t type, players, race, pad;
    uint16_t nodes[NR_MAX_PLAYERS];
    uint32_t seed;
    int32_t mode, ncars, opts[5], track, weather;
    int32_t trk[10], cars_a[11], cars_b[11];
    uint8_t setup[NR_MAX_PLAYERS][0x18];
} StartMsg;

typedef struct {
    uint8_t type, race, pad[2];
} AckMsg;

enum { PAGE_MAIN, PAGE_SCAN };

static bool s_open;
static int s_page, s_sel, s_scan_wait; /* s_scan_wait: frames until the (blocking) scan */
static NetHostInfo s_hosts[4];
static int s_nhosts;
static char s_host_name[32]; /* the race joined */

static bool s_want, s_go;    /* this console started in the track browser / may race */
static uint32_t s_ready_ms;  /* client: READY last sent */
static uint8_t s_ready_setup[NET_MAX_PLAYERS + 1][0x18];
static bool s_ready[NET_MAX_PLAYERS + 1];
static StartMsg s_start;      /* host: the race sent, resent until every client has it */
static uint32_t s_acked, s_start_ms;
static bool s_start_live;
static uint8_t s_race, s_race_seen = 0xff;
static uint8_t s_own[0x18];   /* client: its own car, put back after the race */
static bool s_own_saved;
static int s_prev_state = -1;
static char s_note[48];
static uint32_t s_note_ms;

static void note(const char *s) {
    snprintf(s_note, sizeof s_note, "%s", s);
    s_note_ms = sys_ms();
    sys_log("wireless: %s", s);
}

static int popcount(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

/* connected consoles other than this one (bit = node id) */
static uint32_t others(void) {
    return net_nodes() & ~(1u << net_my_node());
}

static void leave(void) {
    if (net_active()) net_leave();
    ng_end_session();
    s_want = s_go = s_start_live = false;
    memset(s_ready, 0, sizeof s_ready);
}

/* ---------------------------------------------------------------- the race */

static void apply_start(const StartMsg *m) {
    uint16_t me = net_my_node();
    int local = -1;
    for (int p = 0; p < m->players; p++)
        if (m->nodes[p] == me) local = p;
    if (local < 0) return;
    if (local != 0 && !s_own_saved) {
        memcpy(s_own, SETUP(0), 0x18);
        s_own_saved = true;
    }
    MODE_CUR = m->mode;
    NCARS = m->ncars;
    for (int i = 0; i < 5; i++) G_S32(k_opts[i]) = m->opts[i];
    G_S32(0x3f8b0) = m->track;
    G_S32(0x3f8b8) = m->weather;
    memcpy(G(0x481f4), m->trk, sizeof m->trk);
    memcpy(G(0x4819c), m->cars_a, sizeof m->cars_a);
    memcpy(G(0x481c8), m->cars_b, sizeof m->cars_b);
    for (int p = 0; p < m->players; p++) memcpy(SETUP(p), m->setup[p], 0x18);
    /* the computer drivers, from the shared seed, as the name screen built them (m_312cc.c) */
    srand(m->seed);
    NPLAYERS = m->players;
    fn_000117b0();
    for (int i = 0; i < NCARS; i++) sprintf((char *)CAR(i) + 0xc, "%s", (char *)SETUP(i) + 0x14);
    NPLAYERS = 1; /* raised again for the race (ng_state) */
    ng_set_session(m->players, local, m->seed, m->nodes);
    s_go = true;
    sys_log("wireless: race %d, %d players, this console is player %d, seed %08lx", m->race,
            m->players, local, (unsigned long)m->seed);
}

/* host: everyone is ready -> the race */
static void host_start(void) {
    StartMsg *m = &s_start;
    memset(m, 0, sizeof *m);
    m->type = MSG_START;
    m->race = ++s_race;
    m->nodes[0] = net_my_node();
    memcpy(m->setup[0], SETUP(0), 0x18);
    int n = 1;
    uint32_t o = others();
    for (int node = 1; node <= NET_MAX_PLAYERS && n < NR_MAX_PLAYERS; node++) {
        if (!(o & (1u << node))) continue;
        m->nodes[n] = (uint16_t)node;
        memcpy(m->setup[n], s_ready_setup[node], 0x18);
        n++;
    }
    m->players = (uint8_t)n;
    m->seed = (uint32_t)svcGetSystemTick() ^ 0x9e3779b9u;
    m->mode = MODE_CUR;
    m->ncars = NCARS;
    for (int i = 0; i < 5; i++) m->opts[i] = G_S32(k_opts[i]);
    m->track = G_S32(0x3f8b0);
    m->weather = G_S32(0x3f8b8);
    memcpy(m->trk, G(0x481f4), sizeof m->trk);
    memcpy(m->cars_a, G(0x4819c), sizeof m->cars_a);
    memcpy(m->cars_b, G(0x481c8), sizeof m->cars_b);
    s_acked = 0;
    s_start_live = true;
    s_start_ms = 0;
    memset(s_ready, 0, sizeof s_ready);
    apply_start(m);
}

bool ng_menu_start(bool pressed) {
    if (!net_active()) return pressed;
    if (pressed && !s_want) {
        s_want = true;
        s_go = false;
        s_ready_ms = 0;
    }
    if (s_want && !s_go && net_is_host()) {
        uint32_t o = others();
        bool all = true;
        for (int node = 1; node <= NET_MAX_PLAYERS; node++)
            if ((o & (1u << node)) && !s_ready[node]) all = false;
        if (all) {
            if (o) {
                host_start();
            } else {
                ng_end_session(); /* nobody joined: a one-player race */
                s_go = true;
            }
        }
    }
    return s_go;
}

/* ---------------------------------------------------------------- the network in the menus */

static void poll_lobby(void) {
    uint8_t buf[NET_MAX_MSG];
    uint16_t from;
    int n;
    while ((n = net_recv(buf, sizeof buf, &from)) > 0) {
        if (from < 1 || from > NET_MAX_PLAYERS) continue;
        if (net_is_host()) {
            if (buf[0] == MSG_READY && n >= (int)sizeof(ReadyMsg)) {
                const ReadyMsg *m = (const ReadyMsg *)buf;
                memcpy(s_ready_setup[from], m->setup, 0x18);
                if (!s_ready[from]) sys_log("wireless: %s is ready", net_node_name(from));
                s_ready[from] = true;
            } else if (buf[0] == MSG_ACK && n >= (int)sizeof(AckMsg)) {
                if (((const AckMsg *)buf)->race == s_start.race) s_acked |= 1u << from;
            } else if (buf[0] == MSG_HELLO) {
                char t[48];
                snprintf(t, sizeof t, "%s JOINED", net_node_name(from));
                note(t);
            }
        } else if (buf[0] == MSG_START && n >= (int)sizeof(StartMsg)) {
            const StartMsg *m = (const StartMsg *)buf;
            AckMsg a = {MSG_ACK, m->race, {0, 0}};
            net_send(NET_HOST_NODE, &a, sizeof a);
            /* a race this console started on: apply once */
            if (m->race != s_race_seen && s_want && G_S32(0x3f848) == 4) {
                s_race_seen = m->race;
                apply_start(m);
            }
        }
    }
    uint32_t now = sys_ms();
    if (net_is_host()) {
        /* resend the race until every client has it */
        if (s_start_live) {
            uint32_t need = 0;
            for (int p = 1; p < s_start.players; p++) need |= 1u << s_start.nodes[p];
            need &= net_nodes();
            if ((s_acked & need) == need) {
                s_start_live = false;
            } else if (now - s_start_ms > 150) {
                net_send(NET_BROADCAST, &s_start, sizeof s_start);
                s_start_ms = now;
            }
        }
    } else {
        if (!(net_nodes() & (1u << NET_HOST_NODE))) {
            leave();
            note("THE HOST LEFT");
            return;
        }
        if (s_want && !s_go && now - s_ready_ms > 300) {
            ReadyMsg m;
            memset(&m, 0, sizeof m);
            m.type = MSG_READY;
            memcpy(m.setup, SETUP(0), 0x18);
            net_send(NET_HOST_NODE, &m, sizeof m);
            s_ready_ms = now;
        }
    }
}

/* ---------------------------------------------------------------- the panel */

bool ng_panel_open(void) { return s_open && G_S32(0x3f848) == 4; }

static void panel_keys(uint32_t down) {
    if (down & KEY_SELECT) {
        s_open = !s_open;
        s_sel = 0;
        if (s_open) net_init(); /* UDS up early: a search then finds the hosts heard since */
        return;
    }
    if (!s_open) return;
    if (net_active()) {
        if (down & KEY_B) {
            leave();
            note(net_is_host() ? "STOPPED HOSTING" : "LEFT THE RACE");
            s_page = PAGE_MAIN;
        }
        return;
    }
    if (s_page == PAGE_MAIN) {
        if (down & (KEY_DUP | KEY_DDOWN | KEY_CPAD_UP | KEY_CPAD_DOWN)) s_sel ^= 1;
        if (down & KEY_B) s_open = false;
        if (down & KEY_A) {
            if (s_sel == 0) {
                const uint8_t app[4] = {'P', 'X', 'R', 1};
                if (net_host(app, sizeof app)) {
                    ng_end_session();
                    memset(s_ready, 0, sizeof s_ready);
                    note("HOSTING");
                } else {
                    note("COULD NOT START WIRELESS");
                }
            } else {
                s_page = PAGE_SCAN;
                s_sel = 0;
                s_nhosts = 0;
                s_scan_wait = 2;
            }
        }
    } else {
        if (down & KEY_B) {
            s_page = PAGE_MAIN;
            s_sel = 1;
        }
        if (down & KEY_Y) s_scan_wait = 2;
        if (s_nhosts > 0) {
            if (down & (KEY_DUP | KEY_CPAD_UP)) s_sel = (s_sel + s_nhosts - 1) % s_nhosts;
            if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) s_sel = (s_sel + 1) % s_nhosts;
            if ((down & KEY_A) && s_scan_wait == 0) {
                if (net_join(s_sel)) {
                    snprintf(s_host_name, sizeof s_host_name, "%s", s_hosts[s_sel].host_name);
                    uint8_t hello = MSG_HELLO;
                    net_send(NET_HOST_NODE, &hello, 1);
                    note("JOINED");
                    s_page = PAGE_MAIN;
                } else {
                    note("COULD NOT JOIN");
                }
            }
        }
    }
}

void ng_lobby_tick(void) {
    int state = G_S32(0x3f848);
    if (state != s_prev_state) {
        if (state == 4 && s_own_saved) {
            /* back in the menus: this console's own car again */
            memcpy(SETUP(0), s_own, 0x18);
            s_own_saved = false;
        }
        if (state != 4) s_want = false;
        if (state == 4) s_go = false;
        s_prev_state = state;
    }
    if (state == 4 && G_S32(0x3fa24) != 0x1e && s_want && !s_go) s_want = false;

    if (state == 4) panel_keys(input_down());
    if (s_scan_wait > 0 && --s_scan_wait == 0) {
        s_nhosts = net_scan(s_hosts, 4);
        if (s_sel >= s_nhosts) s_sel = 0;
    }
    if (net_active() && state != 5) poll_lobby();
}

/* ---------------------------------------------------------------- the bottom screen */

static void line(float y, const char *s, float sz, float r, float g, float b) {
    controls_text_center(y, s, sz, r, g, b);
}

static void strip(const char *s) {
    gfxGL_OverlayRect(0, 222, 320, 18, 0.0f, 0.0f, 0.0f, 0.7f);
    line(227.0f, s, 8.0f, 1.0f, 0.85f, 0.2f);
}

bool ng_bottom_draw(void) {
    int state = G_S32(0x3f848);
    char t[64];
    bool panel = ng_panel_open();
    bool notice = s_note_ms && sys_ms() - s_note_ms < 3000;
    bool waiting = state == 4 && s_want && !s_go && net_active();
    uint32_t stalled = (state == 5 && nr_active()) ? nr_waiting() : 0;
    bool status = state == 4 && net_active() && !panel;
    if (!panel && !notice && !waiting && !stalled && !status) return false;

    gfxGL_OverlayBegin(GFX_SCREEN_BOTTOM);
    if (panel) {
        gfxGL_OverlayRect(0, 0, 320, 240, 0.0f, 0.0f, 0.05f, 0.85f);
        line(16.0f, "WIRELESS RACE", 12.0f, 1.0f, 0.85f, 0.2f);
        if (net_active()) {
            uint32_t nodes = net_nodes();
            if (net_is_host())
                snprintf(t, sizeof t, "HOSTING - %d OF 4 PLAYERS", popcount(nodes));
            else
                snprintf(t, sizeof t, "IN %s'S RACE", s_host_name);
            line(48.0f, t, 8.0f, 1, 1, 1);
            float y = 72.0f;
            for (int node = 1; node <= NET_MAX_PLAYERS; node++) {
                if (!(nodes & (1u << node))) continue;
                snprintf(t, sizeof t, "%s%s", net_node_name((uint16_t)node),
                         node == net_my_node() ? " (YOU)" : "");
                line(y, t, 8.0f, 0.8f, 0.9f, 1.0f);
                y += 14.0f;
            }
            line(144.0f, "PICK A MODE AND A CAR, THEN START.", 8.0f, 0.85f, 0.85f, 0.85f);
            line(158.0f, net_is_host() ? "THE RACE BEGINS WHEN EVERYONE" : "THE RACE BEGINS WHEN THE HOST",
                 8.0f, 0.85f, 0.85f, 0.85f);
            line(172.0f, net_is_host() ? "HAS STARTED TOO." : "STARTS IT.", 8.0f, 0.85f, 0.85f, 0.85f);
            line(214.0f, net_is_host() ? "B STOP HOSTING   SELECT HIDE" : "B LEAVE   SELECT HIDE", 8.0f,
                 0.6f, 0.6f, 0.6f);
        } else if (s_page == PAGE_MAIN) {
            const char *opt[2] = {"HOST A RACE", "JOIN A RACE"};
            for (int i = 0; i < 2; i++) {
                snprintf(t, sizeof t, "%s%s%s", s_sel == i ? "> " : "  ", opt[i], s_sel == i ? " <" : "  ");
                line(80.0f + i * 24.0f, t, 10.0f, s_sel == i ? 1.0f : 0.6f, s_sel == i ? 1.0f : 0.6f,
                     s_sel == i ? 1.0f : 0.6f);
            }
            line(150.0f, "UP TO 4 CONSOLES, EACH WITH", 8.0f, 0.85f, 0.85f, 0.85f);
            line(164.0f, "ITS OWN COPY OF THE GAME.", 8.0f, 0.85f, 0.85f, 0.85f);
            line(214.0f, "A CHOOSE   B CLOSE", 8.0f, 0.6f, 0.6f, 0.6f);
        } else {
            if (s_scan_wait > 0) {
                line(90.0f, "SEARCHING...", 10.0f, 1, 1, 1);
            } else if (s_nhosts == 0) {
                line(90.0f, "NO RACES FOUND", 10.0f, 1, 1, 1);
            } else {
                for (int i = 0; i < s_nhosts; i++) {
                    snprintf(t, sizeof t, "%s%s  %d/%d", s_sel == i ? "> " : "  ", s_hosts[i].host_name,
                             s_hosts[i].players, s_hosts[i].max_players);
                    line(60.0f + i * 20.0f, t, 10.0f, s_sel == i ? 1.0f : 0.6f, s_sel == i ? 1.0f : 0.6f,
                         s_sel == i ? 1.0f : 0.6f);
                }
            }
            line(214.0f, "A JOIN   Y SEARCH AGAIN   B BACK", 8.0f, 0.6f, 0.6f, 0.6f);
        }
    } else if (stalled) {
        int p = 0;
        while (p < NR_MAX_PLAYERS && !(stalled & (1u << p))) p++;
        snprintf(t, sizeof t, "WAITING FOR %s...", net_node_name(ng_node(p)));
        strip(t);
    } else if (waiting) {
        strip(net_is_host() ? "WAITING FOR THE OTHERS TO START" : "WAITING FOR THE HOST TO START");
    } else if (notice) {
        strip(s_note);
    } else if (status) {
        snprintf(t, sizeof t, "WIRELESS: %d PLAYERS   SELECT", popcount(net_nodes()));
        strip(t);
    }
    gfxGL_OverlayEnd();
    return true;
}

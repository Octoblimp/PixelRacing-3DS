/* Local wireless (UDS) transport for multi-card races: up to 4 consoles, each with the game
 * installed. One console hosts; the others scan and join. Messages are small datagrams
 * (<= NET_MAX_MSG bytes) broadcast or sent to one node; delivery is unreliable, so the
 * race protocol (net_race.c) sends state every frame and repeats important events. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NET_MAX_PLAYERS 4
#define NET_MAX_MSG 512
#define NET_BROADCAST 0xFFFF
#define NET_HOST_NODE 1

typedef struct {
    char host_name[32];     /* host console's user name (UTF-8) */
    int players, max_players;
    uint8_t app_data[32];   /* lobby info the host publishes (track, mode, laps...) */
    int app_data_size;
} NetHostInfo;

bool net_init(void);
void net_exit(void);

/* Host: create the network. app_data (<= 32 bytes) is shown to scanning consoles. */
bool net_host(const void *app_data, int size);
void net_set_app_data(const void *app_data, int size);
/* Client: scan for hosts (blocking ~1 s). Returns the number found (<= max). */
int net_scan(NetHostInfo *out, int max);
/* Client: join the host found by the last net_scan. */
bool net_join(int index);
void net_leave(void);

bool net_active(void);
bool net_is_host(void);
uint16_t net_my_node(void);
/* Bitmask of connected node ids (bit n = node n, 1..4). */
uint32_t net_nodes(void);
const char *net_node_name(uint16_t node);

bool net_send(uint16_t dest, const void *msg, int size);
/* Next received message, or 0 when none. *from = sender node. */
int net_recv(void *buf, int bufsize, uint16_t *from);

/* Local wireless transport on libctru UDS (see net_uds.h). */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_uds.h"
#include "sys.h"

#define NET_COMM_ID 0x0B3D9000u  /* derived from the title's unique id 0xB3D90 */
#define NET_DATA_CHANNEL 1
static const char k_pass[] = "PixelRacing3D-uds";

static bool s_init, s_active, s_host;
static udsBindContext s_bind;
static udsNetworkScanInfo *s_scan;
static size_t s_scan_count;
static char s_names[NET_MAX_PLAYERS + 1][32];

bool net_init(void) {
    if (s_init) return true;
    s_init = R_SUCCEEDED(udsInit(0x3000, NULL));
    if (!s_init) sys_log("udsInit failed");
    return s_init;
}

void net_exit(void) {
    net_leave();
    if (s_scan) free(s_scan);
    s_scan = NULL;
    if (s_init) udsExit();
    s_init = false;
}

bool net_host(const void *app_data, int size) {
    if (!net_init()) return false;
    udsNetworkStruct net;
    udsGenerateDefaultNetworkStruct(&net, NET_COMM_ID, 0, NET_MAX_PLAYERS);
    Result r = udsCreateNetwork(&net, k_pass, sizeof k_pass, &s_bind, NET_DATA_CHANNEL,
                                UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(r)) {
        sys_log("udsCreateNetwork %08lx", (unsigned long)r);
        return false;
    }
    s_active = s_host = true;
    net_set_app_data(app_data, size);
    return true;
}

void net_set_app_data(const void *app_data, int size) {
    if (!s_active || !s_host || !app_data || size <= 0) return;
    if (size > 32) size = 32;
    udsSetApplicationData(app_data, size);
}

int net_scan(NetHostInfo *out, int max) {
    if (!net_init()) return 0;
    const size_t bufsize = 0x4000;
    void *buf = malloc(bufsize);
    if (!buf) return 0;
    Result r = 0;
    /* Emulators collect the hosts' beacons from the moment UDS starts and hand back what came
     * since: a scan right after udsInit finds nothing. Up to 4 tries, 250 ms apart. */
    for (int tries = 0; tries < 4; tries++) {
        if (s_scan) free(s_scan);
        s_scan = NULL;
        s_scan_count = 0;
        if (tries) svcSleepThread(250000000LL);
        r = udsScanBeacons(buf, bufsize, &s_scan, &s_scan_count, NET_COMM_ID, 0, NULL, false);
        if (R_FAILED(r) || s_scan_count > 0) break;
    }
    free(buf);
    if (R_FAILED(r)) {
        sys_log("udsScanBeacons %08lx", (unsigned long)r);
        return 0;
    }
    int n = 0;
    for (size_t i = 0; i < s_scan_count && n < max; i++) {
        udsNetworkScanInfo *si = &s_scan[i];
        NetHostInfo *h = &out[n++];
        memset(h, 0, sizeof *h);
        udsGetNodeInfoUsername(&si->nodes[0], h->host_name);
        h->players = si->network.total_nodes;
        h->max_players = si->network.max_nodes;
        size_t ads = 0;
        udsGetNetworkStructApplicationData(&si->network, h->app_data, sizeof h->app_data, &ads);
        h->app_data_size = (int)ads;
    }
    return n;
}

bool net_join(int index) {
    if (!s_scan || index < 0 || (size_t)index >= s_scan_count) return false;
    Result r = 0;
    for (int tries = 0; tries < 10; tries++) {
        r = udsConnectNetwork(&s_scan[index].network, k_pass, sizeof k_pass, &s_bind,
                              UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, NET_DATA_CHANNEL,
                              UDS_DEFAULT_RECVBUFSIZE);
        if (R_SUCCEEDED(r)) break;
    }
    if (R_FAILED(r)) {
        sys_log("udsConnectNetwork %08lx", (unsigned long)r);
        return false;
    }
    s_active = true;
    s_host = false;
    return true;
}

void net_leave(void) {
    if (!s_active) return;
    udsUnbind(&s_bind);
    if (s_host) udsDestroyNetwork();
    else udsDisconnectNetwork();
    s_active = s_host = false;
}

bool net_active(void) { return s_active; }
bool net_is_host(void) { return s_host; }

uint16_t net_my_node(void) {
    udsConnectionStatus st;
    if (!s_active || R_FAILED(udsGetConnectionStatus(&st))) return 0;
    return st.cur_NetworkNodeID;
}

uint32_t net_nodes(void) {
    udsConnectionStatus st;
    if (!s_active || R_FAILED(udsGetConnectionStatus(&st))) return 0;
    return (uint32_t)st.node_bitmask << 1; /* UDS: bit 0 = node 1 */
}

const char *net_node_name(uint16_t node) {
    if (node < 1 || node > NET_MAX_PLAYERS) return "";
    udsNodeInfo ni;
    if (!s_active || R_FAILED(udsGetNodeInformation(node, &ni))) return "";
    udsGetNodeInfoUsername(&ni, s_names[node]);
    return s_names[node];
}

bool net_send(uint16_t dest, const void *msg, int size) {
    if (!s_active || size <= 0 || size > NET_MAX_MSG) return false;
    u16 to = dest == NET_BROADCAST ? UDS_BROADCAST_NETWORKNODEID : dest;
    Result r = udsSendTo(to, NET_DATA_CHANNEL, UDS_SENDFLAG_Default, msg, size);
    if (UDS_CHECK_SENDTO_FATALERROR(r)) {
        sys_log("udsSendTo %08lx", (unsigned long)r);
        return false;
    }
    return true;
}

int net_recv(void *buf, int bufsize, uint16_t *from) {
    if (!s_active) return 0;
    size_t got = 0;
    u16 src = 0;
    if (R_FAILED(udsPullPacket(&s_bind, buf, bufsize, &got, &src))) return 0;
    if (from) *from = src;
    return (int)got;
}

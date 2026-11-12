/* m_0a184.c -- game flow: menu-state transitions, save load, race/track setup, car spawn,
 * track map + racing-line builder. iOS range [0x0000a184, 0x0000d800).
 *
 * Shared state used here (iOS addresses, see the report in the module notes):
 *   0x3f848 menu/game state (set by fn_0000a484), 0x3f840 "screen" state of the tick
 *   0x48ae0 car array, 8 (G_S32(0x3e9c8)) cars of 0x678 bytes; 0x4f260 backup copy
 *   0x44000/0x44400/0x44800/0x46400 20x20 byte grids (map layers / racing-line order)
 *   0x45640 racing line: 255 x {float row, float col}, G_S32(0x3f948) points
 */
#include "ios.h"

#define CAR_SIZE 0x678
#define CAR(i) ((uint8_t *)G(0x48ae0) + (i) * CAR_SIZE)
#define CAR_BAK(i) ((uint8_t *)G(0x4f260) + (i) * CAR_SIZE)
#define NCARS G_S32(0x3e9c8)

/* Game-mode ids live in globals (compared against the current mode G_S32(0x3f9dc)). */
#define MODE_CUR G_S32(0x3f9dc)
#define MODE_A G_S32(0x3ebe0) /* 1 */
#define MODE_B G_S32(0x3ebe4) /* 2 */
#define MODE_C G_S32(0x3ebe8) /* 3 */
#define MODE_D G_S32(0x3ebec) /* 4 */
#define MODE_E G_S32(0x3f9d8)

/* 20x20 byte grids, row stride 20. */
#define GRID_STRIDE 0x14
#define GRID_OBJ ((uint8_t *)G(0x44000))   /* slot 0x3f09c */
#define GRID_HGT ((uint8_t *)G(0x44400))   /* slot 0x3f0a8 */
#define GRID_TILE ((uint8_t *)G(0x44800))  /* slot 0x3f0b8: road tile type */
#define GRID_ORDER ((uint8_t *)G(0x46400)) /* slot 0x3f0c8: racing-line order */
#define LINE_PTS ((float *)G(0x45640))     /* slot 0x3f0c0 */

/* memset_pattern16 (Darwin libc). */
static void m0a184_memset_pattern16(void *dst, const void *pat, size_t len)
{
    uint8_t *d = dst;
    const uint8_t *p = pat;
    for (size_t i = 0; i < len; i++)
        d[i] = p[i & 15];
}

/* Clamp to [0, hi] the way the game does (negative -> 0, then min). */
static int m0a184_clamp(int v, int hi)
{
    if (v < 0)
        return 0;
    return v > hi ? hi : v;
}

/* 0x0000a184 -- in-race HUD buttons: set visibility, read presses into a control bitmask */
int fn_0000a184(int car)
{
    int state = G_S32(0x3f848);
    uint8_t *b = G(0x40388); /* HUD button table, 0x20 bytes per button */
    unsigned flags, r;
    int screen, pause_mode;
    uint8_t fab8;

    if (!((unsigned)(state - 1) < 2 || state == 4 || state == 6 || state == 7 || state == 5 ||
          state == 8 || state == 9 || (G_U8(0x3f9e0) & 1)))
        return 0;

    b[0x36] = 1;
    b[0x56] = 1;
    b[0x76] = 1;
    b[0x96] = 1;
    b[0xb6] = 1;
    b[0xd6] = 1;
    b[0x156] = 0;
    b[0x176] = 1;
    screen = G_S32(0x3f840);
    b[0x196] = screen == 7;
    fab8 = G_U8(0x3fab8);
    b[0x1b6] = fab8;
    b[0x1d6] = fab8;
    if (G_U8(0x3fab4) != 0 && (state == 5 || state == 8))
        pause_mode = 3;
    else
        pause_mode = 4;
    AT_S32(b, 0x17c) = pause_mode;
    b[0xf6] = state == 5;

    if (state == 5 && !(G_U8(0x3f9e0) & 1)) {
        flags = 0;
        if (b[0x35] != 0) {
            flags = 0x800;
            if (AT_F32(CAR(car), 0x264) > 1.0f)
                flags = 0x200;
        }
    } else {
        flags = b[0x35] != 0 ? 0x200 : 0;
    }
    if (b[0x55] != 0 || (b[0x1b5] != 0 && fab8 != 0))
        flags |= 0x100;
    if (b[0x75] != 0)
        flags |= 8;
    if (b[0x95] != 0)
        flags |= 4;
    flags |= b[0xb5] != 0;
    if (b[0xd5] != 0)
        flags |= 2;
    if (b[0xf5] != 0)
        flags |= 0x10;

    r = flags;
    if (b[0x175] != 0) {
        r = flags | 0x1010;
        if (pause_mode != 3 && state != 9) {
            G_S32(0x3f84c) = state;
            G_S32(0x3f850) = screen;
            fn_0000a370(0);
        }
    }
    if (b[0x1d5] != 0)
        fn_00003ccc();
    return (int)r;
}

/* 0x0000a370 -- open help/info page N (loads "page_NN.png" into texture 0xa9, state 9) */
void fn_0000a370(int page)
{
    char buf[256];

    if (page < 0)
        page = 0;
    if (page > 11)
        page = 11;
    G_S32(0x3f844) = page;
    snprintf(buf, sizeof(buf), "%s_%02d%s", "page", page, page == 6 ? "b.png" : ".png");
    fn_0000ff60(0xa9, buf, 0);
    fn_0000a484(9);
    G_U8(0x47250) = 1; /* slot 0x3f13c */
    fn_000209e8();
}

/* 0x0000a40c -- Game_NextState: advance the menu/game state machine (not a Ghidra function; called by the tick).
 * Not in decomp/symbols.csv; its prototype comes from decomp/protos/m_0a184.tsv. */
void fn_0000a40c(void)
{
    switch (G_S32(0x3f848)) {
    case 0:
        fn_0000a484(1);
        break;
    case 1:
        fn_0000a484(2);
        break;
    case 2:
        fn_0000a484(4);
        break;
    case 4:
        fn_0000a484(6);
        break;
    case 5:
    case 8:
        fn_0000a484(7);
        break;
    case 6:
        fn_0000a484(5);
        break;
    case 7:
        if (G_U8(0x3f88c) & 1)
            fn_0000a484(8);
        else if (G_S32(0x3f8b4) < G_S32(0x3e9cc))
            fn_0000a484(6);
        else
            fn_0000a484(4);
        break;
    default: /* 3 and > 8 */
        break;
    }
    fn_00004254(1);
}

/* fn_0000a484 helper: reset the unlock tables when there is no usable save. */
static void m0a184_save_defaults(int *tracks, int *cars_a, int *cars_b)
{
    int u;

    G_F32(0x3e9bc) = 3.0f;
    G_F32(0x3e9b8) = 1.0f;
    G_S32(0x3e9e0) = 1;
    G_S32(0x3ea20) = G_S32(0x3f8c4) == 0;
    G_S32(0x3e9e4) = 3;
    G_S32(0x3f8bc) = 0;
    G_S32(0x3f8ac) = 0;
    if (G_S32(0x3f8c0) == 0) {
        for (int i = 0; i < 10; i++)
            tracks[i] = i < 5;
        u = 0;
    } else {
        m0a184_memset_pattern16(tracks, G(0x3cd70), 0x28);
        u = 1;
    }
    cars_a[0] = 1;
    cars_a[1] = 1;
    cars_a[2] = u;
    cars_a[3] = u;
    cars_a[4] = 1;
    cars_a[5] = 1;
    cars_a[6] = u;
    cars_a[7] = 1;
    cars_a[8] = u;
    cars_a[9] = 1;
    cars_b[0] = 1;
    cars_b[1] = 1;
    cars_b[2] = u;
    cars_b[3] = u;
    cars_b[4] = u;
    cars_b[5] = 1;
    cars_b[6] = u;
    cars_b[7] = 1;
    cars_b[8] = u;
    cars_b[9] = 1;
    cars_a[10] = u;
    cars_b[10] = u;
    fn_0000fe34();
}

/* fn_0000a484 helper: unlock the first locked entry of `tab` (n entries) as reward `kind`. */
static void m0a184_unlock_next(int *tab, int n, int kind)
{
    for (int i = 0; i < n; i++) {
        if (tab[i] == 0) {
            G_S32(0x3eb40) = i;
            tab[i] = 1;
            G_S32(0x3eb3c) = kind;
            return;
        }
    }
    G_S32(0x3eb3c) = -1;
}

/* 0x0000a484 -- Game_SetState: enter menu/game state N (1 title, 2 attract race, 4 load save,
 * 5/8 start race, 6 random quick race, 7 race finished, 9 help page) */
void fn_0000a484(int state)
{
    char buf[256];
    int *tracks = (int *)G(0x481f4); /* slot 0x3f370: track unlocked[10] */
    int *cars_a = (int *)G(0x4819c); /* slot 0x3f368: unlocked[11] */
    int *cars_b = (int *)G(0x481c8); /* slot 0x3f36c: unlocked[11] */

    G_S32(0x3f848) = state;

    if (state == 1) {
        int a;

        fn_00010160();
        G_S32(0x3f874) = 0;
        G_S32(0x3f878) = 0;
        G_U8(0x3fab8) = 1;
        G_U8(0x3f900) = 0;
        G_U8(0x3f910) = 0;
        G_U8(0x3fabc) = 1;
        G_S32(0x47540) = 0; /* slot 0x3f164 */
        G_U32(0x3fa3c) = G_U32(0x3fa34);
        G_S32(0x3fa44) = G_S32(0x3f8b0);
        a = (int)G_F32(0x3fa38) % 360;
        G_F32(0x3fa40) = (float)a;
        G_F32(0x3fa38) = (float)a;
        G_U8(0x3f908) = MODE_CUR != MODE_C;
        G_S32(0x3f8f4) = 0;
        G_S32(0x3f8f0) = 0xff;
        G_F32(0x5c554) = 150.0f;
        fn_00012024();
        G_S32(0x3f840) = 6;
    }

    if (G_S32(0x3f848) == 2) {
        int n, tr, wx, coin, dir;
        float f;

        fn_00011dbc();
        G_S32(0x3fa28) = 4;
        G_S32(0x3fa24) = 4;
        G_U8(0x3f928) = 1;
        G_U8(0x3f920) = 1;
        G_S32(0x3e9b0) = 0;
        fn_000117b0();
        G_S32(0x3e9b0) = 1;
        n = NCARS;
        for (int i = 0; i < n; i++) {
            uint8_t *c = CAR(i);
            ((int *)G(0x476c4))[i] = i; /* slot 0x3f16c */
            AT_S32(c, 0xa0) = 0;
            AT_S32(c, 0x104) = 0;
            AT_F32(c, 0x3f0) = 1.0f;
            AT_F32(c, 0x44c) = 1.0f;
            AT_F32(c, 0x4a8) = 1.0f;
            AT_F32(c, 0x504) = 1.0f;
            AT_F32(c, 0x20) = 1.0f;
        }
        srand(fn_00004208());
        tr = fn_00004400(10);
        wx = fn_00004400(5);
        coin = fn_00004400(2);
        snprintf(buf, sizeof(buf), "map_%02d", ((int *)G(0x3eafc))[G_S32(0x3f8b0)]);
        fn_0000cd00(buf);
        f = (float)coin + -0.5f;
        if (f < 0.0f)
            dir = -1;
        else
            dir = f > 0.0f;
        fn_0000b054(tr, wx, dir, 1);
        fn_0001026c();
        G_S32(0x3f840) = 7;
    }

    if (G_S32(0x3f848) == 5) {
        fn_0000b054(G_S32(0x3f8b0), G_S32(0x3f8b8), G_S32(0x3e9d0), 0);
        fn_0001026c();
        G_S32(0x3f840) = 7;
    }

    if (G_S32(0x3f848) == 8) {
        fn_0000b054(G_S32(0x3f8b0), G_S32(0x3f8b8), G_S32(0x3e9d0), 0);
        fn_0001026c();
        G_S32(0x3f840) = 7;
    }

    if (G_S32(0x3f848) == 4) {
        void **save = (void **)G(0x477ec); /* slot 0x3f184: save buffer */
        int r;

        G_U8(0x3fab8) = 1;
        G_U8(0x3f910) = 0;
        G_S32(0x3fa28) = 5;
        G_U8(0x3f888) = 1;
        fn_00004550();
        G_S32(0x3fa24) = 5;
        G_S32(0x3f8f0) = 0xff;
        G_U8(0x3e9a4) = 20;
        G_U8(0x3e9a8) = 20;
        G_S32(0x3f8f4) = 0;
        G_S32(0x3f8a0) = 19;
        G_S32(0x3f8a4) = 19;
        memset(LINE_PTS, 0, 0x7f8);
        for (int y = 0; y < 20; y++) {
            for (int x = 0; x < 20; x++) {
                GRID_ORDER[y * 20 + x] = 0;
                GRID_TILE[y * 20 + x] = 0;
                GRID_OBJ[y * 20 + x] = 0;
                GRID_HGT[y * 20 + x] = 0;
            }
        }
        G_S32(0x3f874) = 0;
        if (*save != NULL)
            fn_00005654(*save);
        *save = NULL;
        *save = fn_000055f8(0x80);

        if (G_S32(0x3f8c4) != 0) {
            puts("Savefile not found");
            memset(*save, 0, 0x80);
            m0a184_save_defaults(tracks, cars_a, cars_b);
        } else if ((r = fn_00036274("RaceData", *save, 0x80)) < 0) {
            if (r == -3)
                puts("Savefile was corrupted, it have been deleted");
            else if (r == -1)
                puts("Savefile not found");
            else
                puts("RMS_LoadSavedData unknown error");
            memset(*save, 0, 0x80);
            m0a184_save_defaults(tracks, cars_a, cars_b);
        } else {
            const uint8_t *s = *save;

            G_F32(0x3e9b8) = (float)s[0];
            G_S32(0x3e9e0) = s[1];
            G_S32(0x3ea20) = s[2];
            G_S32(0x3e9e4) = s[3];
            G_S32(0x3f8bc) = s[4];
            G_F32(0x3e9bc) = (float)s[5];
            G_S32(0x3f8ac) = s[6];
            for (int i = 0; i < 10; i++)
                tracks[i] = s[10 + i];
            for (int i = 0; i < 11; i++)
                cars_a[i] = s[0x1e + i];
            for (int i = 0; i < 10; i++)
                cars_b[i] = s[0x32 + i];
            if (G_S32(0x3f8c0) != 0) {
                m0a184_memset_pattern16(tracks, G(0x3cd70), 0x28);
                for (int i = 0; i < 11; i++) {
                    cars_a[i] = 1;
                    cars_b[i] = 1;
                }
            }
            if (G_S32(0x3f8c4) != 0) {
                memset(tracks, 0, 0x28);
                tracks[3] = 1;
                for (int i = 0; i < 11; i++) {
                    cars_a[i] = i == 0;
                    cars_b[i] = i == 0;
                }
            }
        }
        fn_00012024();
        G_S32(0x3f840) = 6;
    } else if (G_S32(0x3f848) == 9) {
        G_S32(0x3f840) = 9;
        return;
    }

    if (G_S32(0x3f848) == 6) {
        int *used = (int *)G(0x47744);    /* slot 0x3f174: tracks already played[10] */
        int *wx_used = (int *)G(0x48274); /* slot 0x3f37c: weather already used[5] */
        int coin, dir, tr, wx, any, i;

        srand(fn_00004208());
        G_U8(0x3f920) = 1;
        coin = fn_00004400(2);
        for (i = 0; i < 10; i++)
            if (used[i] == 0 && tracks[i] != 0)
                break;
        if (i == 10)
            memset(used, 0, 0x28);
        dir = coin == 0 ? -1 : 1;
        any = 0;
        for (i = 1; i < 5; i++) {
            if (wx_used[i] == 0) {
                any = 1;
                break;
            }
        }
        if (!any)
            memset(wx_used, 0, 0x14);

        tr = fn_00004400(10);
        for (;;) {
            tr %= 10;
            if (used[tr] == 0 && tracks[tr] != 0)
                break;
            tr += fn_00004400(10);
        }
        if (fn_00004400(100) <= 50) {
            used[tr] = 1;
            wx = 0;
        } else {
            int k = fn_00004400(4) + 1;
            while (wx_used[k] != 0)
                k = (fn_00004400(5) + k) % 5;
            used[tr] = 1;
            wx = 0;
            if (k != 0) {
                wx_used[k] = 1;
                wx = k;
            }
        }
        if (NCARS > 0)
            memcpy(G(0x471d8), G(0x476c4), NCARS > 1 ? NCARS * 4 : 4); /* 0x3f130 <- 0x3f16c */
        if (MODE_CUR == MODE_C) {
            wx = 0;
            dir = 1;
            tr = G_S32(0x3f8c4) != 0 ? 3 : 0;
        }
        G_U8(0x3f98c) = 0;
        G_U8(0x3f88c) = 0;
        fn_0000bed0();
        G_S32(0x3e9d0) = dir;
        G_S32(0x3f8b0) = tr;
        G_S32(0x3f8b8) = wx;
        fn_0000b910();
        G_U8(0x3f928) = 0;
        G_S32(0x3f840) = 8;
    }

    if (G_S32(0x3f848) != 7)
        return;

    /* Race finished: a human winner unlocks the next item of the current mode. */
    fn_0000bed0();
    G_S32(0x3eb3c) = -1;
    G_S32(0x3eb40) = -1;
    if (G_S32(0x3fac4) != 0) {
        int winner = G_S32(0x47060); /* slot 0x3f0f0: race order[0] */
        int mode = MODE_CUR;

        if (AT_S32(CAR(winner), 0x30) < 0 || G_S32(0x3f8bc) != 0)
            G_S32(0x3eb3c) = -1;
        else if (mode == MODE_B)
            m0a184_unlock_next(cars_a, 11, 1);
        else if (mode == MODE_D)
            m0a184_unlock_next(tracks, 10, 0);
        else if (mode == MODE_C)
            m0a184_unlock_next(cars_b, 10, 2);
        else if (mode == MODE_E)
            m0a184_unlock_next(tracks, 10, 0);
        else if (mode == MODE_A)
            m0a184_unlock_next(cars_a, 11, 1);
        else
            G_S32(0x3eb3c) = -1;
    }
    G_U8(0x3f900) = 1;
    G_U8(0x3fab8) = 1;
    G_U8(0x3fab4) = 0;
    G_U8(0x3f898) = 1;
    fn_0000b910();
    G_U8(0x3f928) = 0;
    G_S32(0x3f840) = 8;
}

/* 0x0000b054 -- Track_Load(track, weather, dir, reset): load the map's object models, weather
 * texture, height/road maps and road tiles, then set up the mode's race rules and AI speed. */
void fn_0000b054(int track, int weather, int dir, int reset)
{
    char name[64], tiles[64], maps[64], objs[64], nm[256];
    char path[512], tmp[512], png[512], smd[512];
    const char *txt = NULL;
    int size, pos, i;
    int a = 0, b = 0; /* sscanf targets: object kind, scale */

    if (G_U8(0x3f88c) & 1) {
        /* Replay: keep the live grid order and car state, restore the saved start order. */
        int n = NCARS;
        int *order = (int *)G(0x476c4);   /* slot 0x3f16c */
        int *keep = (int *)G(0x477ac);    /* slot 0x3f180 */
        int *saved = (int *)G(0x471d8);   /* slot 0x3f130 */
        for (i = 0; i < n; i++) {
            keep[i] = order[i];
            order[i] = saved[i];
            memcpy(CAR_BAK(i), CAR(i), CAR_SIZE);
        }
        G_U8(0x3f98c) = 1;
    }
    G_S32(0x3e9d0) = dir;
    G_S32(0x3f8b0) = track;
    G_S32(0x3f8b8) = weather;
    fn_00010160();
    G_S32(0x3f874) = 0;
    G_S32(0x3f878) = 0;
    if (reset == 1)
        G_S32(0x3f8e0) = 0;

    snprintf(name, sizeof(name), "map_%02d", ((int *)G(0x3eafc))[G_S32(0x3f8b0)]);
    snprintf(tiles, sizeof(tiles), "models/map_tiles");
    snprintf(maps, sizeof(maps), "models/maps");
    snprintf(path, sizeof(path), "models/maps/%s_objs.txt", name);
    snprintf(objs, sizeof(objs), "models/objs");

    size = fn_000058b8(&txt, 0, path);
    for (pos = 0, i = 0; pos < size; i++) {
        sscanf(txt + pos, "%s %d %d", nm, &a, &b);
        if (nm[0] != '-') {
            if (i < 40) {
                int *model = (int *)G(0x47454); /* slot 0x3f158: object model slot per index */
                int *kind = (int *)G(0x473b4);  /* slot 0x3f154: object kind per index */
                int bounds = G_U8(0x3e99c) & 1;

                if (strstr(nm, "31") != NULL) { /* windmill: extra blades model */
                    snprintf(smd, sizeof(smd), "%s/blades.smd", objs);
                    fn_0003836c(0x8a, smd, 2.54f, 1, 0, bounds);
                    snprintf(png, sizeof(png), "%s/blades.png", objs);
                    fn_0000ff60(0x8a, png, 0);
                }
                snprintf(smd, sizeof(smd), "%s/%s.smd", objs, nm);
                fn_0003836c(model[i], smd, (float)b * 2.54f, 1, 0, G_U8(0x3e99c) & 1);
                snprintf(png, sizeof(png), "%s/%s.png", objs, nm);
                fn_0000ff60(model[i], png, 0x10);
                kind[i] = a;
                /* (the original formats the .smd path once more here; unused) */
            } else {
                snprintf(png, sizeof(png), "%s/%s.png", objs, nm);
                switch (i) {
                case 40: fn_0000ff60(0xa4, png, 0); break;
                case 41: fn_0000ff60(0xa0, png, 0); break;
                case 42: fn_0000ff60(0xa1, png, 0); break;
                case 44: fn_0000ff60(0x9e, png, 0); break;
                default: break;
                }
            }
        }
        pos += (int)strlen(nm) + 7;
    }

    if (G_S32(0x3f8b8) == 4)
        fn_0000ff60(0x7f, "/models/others/snow.png", 0);
    if (G_S32(0x3f8b8) == 3)
        fn_0000ff60(0x7f, "/models/others/drop.png", 0);

    snprintf(path, sizeof(path), "%s/%s", maps, name);
    fn_0000cd64(path);
    snprintf(path, sizeof(path), "%s/%s_grnd.pbm", maps, name);
    fn_0000fff0(path, 0);
    snprintf(path, sizeof(path), "%s/%s_tree.pbm", maps, name);
    fn_0000fff0(path, 1);
    snprintf(path, sizeof(path), "%s/%s_road.pbm", maps, name);
    fn_0000fff0(path, 2);

    if (!(G_U8(0x3f8fc) & 1)) {
        int *road = (int *)G(0x47770); /* slot 0x3f17c: road tile texture/model slots[10] */

        G_U8(0x3f8fc) = 1;
        snprintf(tmp, sizeof(tmp), "%s/road_00.png", tiles);
        fn_0000ff60(road[0], tmp, 0);
        snprintf(tmp, sizeof(tmp), "%s/road_01.png", tiles);
        fn_0000ff60(road[1], tmp, 0);
        road[2] = road[1];
        snprintf(tmp, sizeof(tmp), "%s/road_03.png", tiles);
        fn_0000ff60(road[3], tmp, 0);
        road[4] = road[3];
        road[5] = road[3];
        road[6] = road[3];
        snprintf(tmp, sizeof(tmp), "%s/road_07.png", tiles);
        fn_0000ff60(road[7], tmp, 0);
        snprintf(path, sizeof(path), "%s/road_00.smd", tiles);
        fn_0003836c(road[8], path, 2.54f, 1, 1, G_U8(0x3e99c) & 1);
        snprintf(tmp, sizeof(tmp), "%s/road_08.png", tiles);
        fn_0000ff60(road[8], tmp, 0);
        snprintf(tmp, sizeof(tmp), "%s/road_09.png", tiles);
        fn_0000ff60(road[9], tmp, 0);
    }

    fn_0000cfc8();
    if (G_S32(0x3f878) != 0)
        return;

    {
        int mode = MODE_CUR;
        float base;

        if (mode == MODE_A) {
            int p = G_S32(0x3e9ac);
            G_S32(0x3f9d0) = p;
            G_S32(0x3f9ac) = 0;
            AT_S32(CAR(p - 1), 0xf4) = 0;
            G_S32(0x3e9d4) = 100;
        } else if (mode == MODE_B) {
            int n = NCARS;
            int *grid = (int *)G(0x47544); /* slot 0x3f168: per-car setup, 6 ints each */
            for (i = 0; i < n; i++) {
                uint8_t *c = CAR(i);
                if (grid[i * 6 + 3] == 1) {
                    AT_S32(c, 0x108) = 1;
                    AT_S32(c, 0x10c) = 0;
                } else if (i < G_S32(0x3e9ac)) {
                    AT_S32(c, 0x10c) = 1;
                    AT_S32(c, 0x108) = 0;
                }
            }
            G_S32(0x3e9d4) = 30;
        } else if (mode == MODE_D || mode == MODE_E || mode == MODE_C) {
            G_S32(0x3e9d4) = G_S32(0x3e9e4);
        }

        G_U8(0x3fab4) = 1;
        G_U8(0x3fab8) = 0;
        G_S32(0x3f8f0) = 0xff;
        G_S32(0x3f8f4) = 0;

        base = 5.0f / ((G_F32(0x3e9b8) - 1.0f) * 20.0f + 1.0f) + 7.0f +
               ((float *)G(0x3eaac))[G_S32(0x3f8b0)];
        if (mode == MODE_E || mode == MODE_C || mode == MODE_D)
            G_F32(0x3e9b4) = base;
        else
            G_F32(0x3e9b4) = base + (float)G_S32(0x3f8b4) * -0.5f;
    }
    G_U8(0x3f888) = 1;
    fn_00004550();
}

/* 0x0000b910 -- Race_Reset: restore replay state, reset camera/lights/projection, place car 0
 * and the chase camera, reset per-race flags and reload the current map. */
void fn_0000b910(void)
{
    uint8_t *cam = G(0x5c160); /* camera/light block */
    char name[256];
    float a, ang, s, c;
    int state, deg;

    if (G_U8(0x3f88c) & 1) {
        int n = NCARS;

        G_U8(0x3f88c) = 0;
        G_U8(0x3f9e4) = 0;
        G_U8(0x3f9e0) = 0;
        G_U8(0x3f98c) = 0;
        if (n > 0) {
            memcpy(G(0x476c4), G(0x477ac), (n > 1 ? n : 1) * 4); /* 0x3f16c <- 0x3f180 */
            for (int i = 0; i < n; i++)
                memcpy(CAR(i), CAR_BAK(i), CAR_SIZE);
        }
    }

    G_U8(0x3fee4) = 0;
    G_U32(0x482bc) = 0;  /* PTR 0x3d284 */
    G_U8(0x4776c) = 0;   /* slot 0x3f178 */
    G_F32(0x482b8) = 2.2f; /* PTR 0x3d280 */
    G_U8(0x402f4) = 1;   /* PTR 0x3d294 */
    G_U8(0x3fee0) = 0;
    G_U8(0x3fab0) = 1;
    G_S32(0x3fa28) = 30;
    G_F32(0x482b4) = -0.1f; /* PTR 0x3d27c */
    fn_0000fc98();
    fn_0000fc28();

    G_U16(0x3eb9c) = 0x100;
    G_U16(0x3eba0) = 0x100;
    AT_S32(cam, 0x28) = 0;
    AT_S32(cam, 0x38) = 0;
    AT_F32(cam, 0x48) = 1.0f;
    AT_F32(cam, 0x58) = 1.0f;
    G_S32(0x3eba4) = 0x80;
    G_S32(0x3eba8) = 0x80;
    G_U16(0x3ebac) = 0x100;
    G_U16(0x3ebb0) = 0x100;

    /* Shadow projection: orthographic box around the track. */
    G_F32(0x47f94) = -1500.0f; /* slot 0x3f360 */
    G_F32(0x47254) = -1500.0f; /* slot 0x3f140 */
    fn_00037d0c((float *)G(0x428f8), -1500.0f, 1500.0f, -1500.0f, 1500.0f, 10.0f, 7000.0f);
    fn_00037d5c(G(0x428b8), -G_F32(0x47f94), G_F32(0x47f94), G_F32(0x47254), -G_F32(0x47254),
                0.5f, 0.5f, 0.5f, 0.5f);

    /* Chase camera behind car 0. */
    a = G_F32(0x3fa38) + 180.0f;
    AT_F32(cam, 0x3f4) = a;
    ang = a * 0.0174533f;
    c = cosf(ang);
    s = sinf(ang);
    AT_F32(cam, 0x3c4) = AT_F32(CAR(0), 0x2bc) - s * 235.0f;
    AT_F32(cam, 0x3c8) = AT_F32(CAR(0), 0x2c0) + 150.0f;
    AT_F32(cam, 0x3cc) = AT_F32(CAR(0), 0x2c4) - c * 235.0f;
    G_U8(0x3f924) = 0;
    fn_000042e0();

    G_S32(0x3e9f0) = 1;
    G_F32(0x3e9f4) = 0.5f;
    G_F32(0x3e9f8) = 0.1f;
    G_S32(0x3e9fc) = 1;
    G_F32(0x3ea00) = 0.5f;
    G_F32(0x3ea04) = 0.1f;
    G_S32(0x3ea08) = 1;
    G_F32(0x3ea0c) = 0.5f;
    G_F32(0x3ea10) = 0.1f;
    G_S32(0x3ea14) = 1;
    G_F32(0x3ea18) = 0.5f;
    G_F32(0x3ea1c) = 0.1f;
    AT_S32(cam, 4) = 0;
    G_U8(0x3f990) = 0;
    G_S32(0x3f984) = 0;
    G_S32(0x3f988) = 0;
    fn_0000bf74(0, ((int *)G(0x47544))[0], ((int *)G(0x47544))[2]); /* slot 0x3f168 */
    fn_0000cc3c();

    AT_S32(CAR(0), 0x30) = -2;
    AT_S32(cam, 0x410) = 0;
    G_S32(0x3f8f0) = 0xff;
    G_U8(0x3fab8) = 1;
    G_U8(0x3fabc) = 1;
    G_S32(0x3f8f4) = 0;
    G_U8(0x3f900) = 0;
    G_U8(0x3f910) = 0;
    state = G_S32(0x3f848);
    if (state == 6) {
        G_U8(0x3f920) = 1;
        G_U8(0x3f880) = 1;
        G_U8(0x3f890) = 0;
    } else {
        G_U8(0x3f880) = 0;
        G_U8(0x3f920) = 0;
        G_U8(0x3f890) = (G_U8(0x3f9e4) & 1) ^ 1;
    }
    G_U32(0x3fa3c) = G_U32(0x3fa34);
    G_S32(0x3fa44) = G_S32(0x3f8b0);
    G_S32(0x47540) = 0; /* slot 0x3f164 */
    deg = (int)G_F32(0x3fa38) % 360;
    G_F32(0x3fa40) = (float)deg;
    G_F32(0x3fa38) = (float)deg;
    if (MODE_CUR == MODE_C) {
        G_U8(0x3f908) = 0;
        G_U8(0x3f90c) = state == 6;
    } else {
        G_U8(0x3f908) = state == 6;
        G_U8(0x3f90c) = 0;
    }
    G_U8(0x3f900) = state == 7;
    snprintf(name, sizeof(name), "map_%02d", ((int *)G(0x3eafc))[G_S32(0x3f8b0)]);
    fn_0000cd00(name);
    fn_000209e8();
}

/* 0x0000bed0 -- Race_SortOrder: race order (0x47060) = car indices sorted by score (+0xa0), best first */
void fn_0000bed0(void)
{
    int *order = (int *)G(0x47060); /* slot 0x3f0f0 */
    int n = NCARS;

    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (AT_S32(CAR(order[i]), 0xa0) < AT_S32(CAR(order[j]), 0xa0)) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
}

/* fn_0000bf74 helper: grid-start position of slot `row` next to the start tile. */
static void m0a184_grid_slot(uint8_t *c, int row, int tile)
{
    float d = (float)G_S32(0x3e9d0);
    int di = G_S32(0x3e9d0);
    int m = row % 2;
    float sx = (float)G_S32(0x3f974) * 812.0f;
    float sz = (float)G_S32(0x3f978) * 812.0f;

    if (tile == 1) {
        AT_F32(c, 0x2b0) = sx + ((float)m * 203.0f) * d + d * 101.5f;
        AT_F32(c, 0x2b8) = sz + (float)(row * di * 100) + d * -406.0f;
        AT_F32(c, 0x244) = (float)(di * -90 + 90);
    } else {
        AT_F32(c, 0x2b0) = sx - (float)(row * di * 100) + d * 406.0f;
        AT_F32(c, 0x2b8) = sz + ((float)m * -203.0f) * d + d * -101.5f;
        AT_F32(c, 0x244) = (float)(di * -90);
    }
}

/* 0x0000bf74 -- Car_Spawn(car, model, color): reset the car struct, pick its controller, place it
 * on the starting grid (or along the racing line) and drop it onto the ground. */
void fn_0000bf74(int car, int model, int color)
{
    uint8_t *c = CAR(car);
    int *grid = (int *)G(0x47544); /* slot 0x3f168: per-car setup, 6 ints each */
    int ctl, v, fab8, tile, mode, gx, gz, ord;
    int w1, h1;
    float h;

    AT_S32(c, 0x10) = 0;
    AT_S32(c, 0x14) = 0;
    AT_S32(c, 0x18) = 0;
    AT_S32(c, 0x1c) = 0;
    AT_S32(c, 0x24) = 0;
    AT_S32(c, 0x28) = 0;
    AT_S32(c, 0x2c) = 0;
    AT_S32(c, 0xac) = 0;
    AT_S32(c, 0xb0) = 0;
    AT_S32(c, 0xb4) = 0;
    AT_S32(c, 0xb8) = 0;
    AT_S32(c, 0xbc) = 0;
    AT_S32(c, 0xc0) = 0;
    AT_S32(c, 0xc4) = 0;
    AT_S32(c, 0xf8) = 0;
    AT_S32(c, 0xfc) = 0;
    AT_S32(c, 0x100) = 0;
    AT_S32(c, 0x110) = 0;
    AT_S32(c, 0x114) = 0;
    AT_S32(c, 0x118) = 0;
    AT_S32(c, 0x11c) = 0;
    AT_S32(c, 0xa4) = 0;
    AT_S32(c, 0x120) = 0;
    AT_S32(c, 0x124) = 0;
    AT_S32(c, 0x128) = 0;
    AT_S32(c, 0x1e0) = 0;
    AT_S32(c, 0x1e4) = 0;
    memset(c + 0x34, 0, 0x6c);
    memset(c + 0xcc, 0, 0x28);
    memset(c + 0x238, 0, 0x40);
    memset(c + 0x298, 0, 0x24);
    for (int k = 0; k < 11; k++)
        memcpy(c + 0x2bc + k * 12, c + 0x2b0, 12); /* position history */
    strcpy((char *)c + 0x12c, " ");
    strcpy((char *)c + 0x14c, " ");
    strcpy((char *)c + 0x16c, " ");
    strcpy((char *)c + 0x18c, " ");
    strcpy((char *)c + 0x1ac, " ");
    for (int k = 0; k < 5; k++)
        AT_S32(c, 0x1cc + k * 4) = G_S32(0x3eb7c);

    AT_S32(c, 0) = ((int *)G(0x470a0))[model]; /* slot 0x3f0f4 */
    AT_S32(c, 4) = ((int *)G(0x47218))[color]; /* slot 0x3f134 */
    AT_S32(c, 8) = ((int *)G(0x47258))[model]; /* slot 0x3f144 */

    /* Controller: player index for human cars, -1 for AI, -2 while in the menus. */
    ctl = G_S32(0x3e9b0) <= car ? -1 : car;
    AT_S32(c, 0x30) = ctl;
    if (G_U8(0x3f928) & 1) {
        AT_S32(c, 0x30) = -1;
        ctl = -1;
    }
    if (G_S32(0x3f8c8) != 0) {
        AT_S32(c, 0x30) = -1;
        ctl = -1;
    }
    fab8 = G_U8(0x3fab8);
    if (fab8) {
        AT_S32(c, 0x30) = -2;
        v = grid[car * 6 + 4];
    } else if (ctl == -1) {
        v = 1;
    } else {
        v = grid[car * 6 + 4];
    }
    AT_S32(c, 0x2c) = v;
    AT_S32(c, 0x28) = 2;

    AT_F32(c, 0x1e8) = -5.0f;
    AT_F32(c, 0x1ec) = 0.0f;
    AT_F32(c, 0x1f0) = 5.0f;
    AT_F32(c, 0x1f4) = 10.0f;
    AT_F32(c, 0x1f8) = 15.0f;
    AT_F32(c, 0x1fc) = 20.0f;
    AT_F32(c, 0x200) = 25.0f;
    AT_F32(c, 0x204) = 30.0f;
    AT_F32(c, 0x208) = 35.0f;
    AT_F32(c, 0x230) = 30.0f;
    AT_F32(c, 0x210) = -5.0f;
    AT_F32(c, 0x214) = 0.0f;
    AT_F32(c, 0x218) = 5.0f;
    AT_F32(c, 0x21c) = 10.0f;
    AT_F32(c, 0x220) = 15.0f;
    AT_F32(c, 0x224) = 20.0f;
    AT_F32(c, 0x228) = 25.0f;
    AT_F32(c, 0x22c) = 27.0f;
    memset(c + 0x2f8, 0, 0x3c);

    w1 = G_S32(0x3f8a4);
    h1 = G_S32(0x3f8a0);
    tile = GRID_TILE[m0a184_clamp(G_S32(0x3f974), w1) * GRID_STRIDE + m0a184_clamp(G_S32(0x3f978), h1)];

    /* Wheel / collision points (4 corners). */
    AT_F32(c, 0x278) = 37.0f;
    AT_F32(c, 0x280) = -10.0f;
    AT_F32(c, 0x288) = 28.0f;
    AT_F32(c, 0x290) = 1.0f;
    AT_F32(c, 0x27c) = 37.0f;
    AT_F32(c, 0x284) = -9.0f;
    AT_F32(c, 0x28c) = 19.0f;
    AT_F32(c, 0x294) = 1.1f;
    AT_F32(c, 0x3dc) = 0.0f;
    AT_F32(c, 0x3e4) = 0.0f;
    AT_F32(c, 0x3b0) = 37.0f;
    AT_F32(c, 0x3b4) = -10.0f;
    AT_F32(c, 0x3b8) = 28.0f;
    AT_F32(c, 0x438) = 0.0f;
    AT_F32(c, 0x440) = 0.0f;
    AT_F32(c, 0x40c) = -37.0f;
    AT_F32(c, 0x410) = -10.0f;
    AT_F32(c, 0x414) = 28.0f;
    AT_F32(c, 0x494) = 0.0f;
    AT_F32(c, 0x49c) = 0.0f;
    AT_F32(c, 0x468) = -37.0f;
    AT_F32(c, 0x46c) = -9.0f;
    AT_F32(c, 0x470) = -19.0f;
    AT_F32(c, 0x4f0) = 0.0f;
    AT_F32(c, 0x4f8) = 0.0f;
    AT_F32(c, 0x4c4) = 37.0f;
    AT_F32(c, 0x4c8) = -9.0f;
    AT_F32(c, 0x4cc) = -19.0f;
    AT_S32(c, 0x120) = 1;
    AT_S32(c, 0x118) = -1;
    AT_S32(c, 0x114) = 0;
    AT_S32(c, 0x70) = 0;
    AT_S32(c, 0x78) = -1;
    AT_S32(c, 0x74) = -1;
    AT_S32(c, 0x54) = 0;
    mode = MODE_CUR;
    if (mode == MODE_B && AT_S32(c, 0x108) != 0)
        AT_S32(c, 0x54) = 3;
    if (mode == MODE_D && (unsigned)grid[car * 6] <= 10 && ((1u << grid[car * 6]) & 0x620))
        AT_S32(c, 0x54) = 3;
    AT_S32(c, 0x5c) = 0;
    AT_S32(c, 0x60) = 0;
    AT_S32(c, 0xf8) = 0;

    if (fab8) {
        AT_F32(c, 0x2b0) = 0.0f;
        AT_F32(c, 0x2b4) = 0.0f;
        AT_F32(c, 0x2b8) = 0.0f;
        AT_F32(c, 0x244) = 45.0f;
        h = 0.0f;
    } else {
        int tmp;

        if (mode == MODE_E || mode == MODE_C) {
            m0a184_grid_slot(c, AT_S32(c, 0xf4) - NCARS - 1, tile);
        } else if (mode == MODE_B || mode == MODE_A || mode == MODE_D) {
            int p = G_S32(0x3e9ac);

            if (AT_S32(c, 0x108) == 0 && AT_S32(c, 0x10c) == 0 && p <= car) {
                /* AI cars behind the players start spread along the racing line. */
                int idx = ((car - p) * G_S32(0x3f948)) / (NCARS - p);
                const float *pt = LINE_PTS + (idx + 1) * 2;
                int xi, yi, t;

                do {
                    xi = (int)pt[0];
                    yi = (int)pt[1];
                    pt += 2;
                    t = GRID_TILE[m0a184_clamp(xi, w1) * GRID_STRIDE + m0a184_clamp(yi, h1)];
                } while ((unsigned)(t - 1) > 1);
                AT_F32(c, 0x2b0) = (float)xi * 812.0f;
                AT_F32(c, 0x2b8) = (float)yi * 812.0f;
                AT_F32(c, 0x244) = (float)(G_S32(0x3e9d0) * -90 + (t == 1 ? 90 : 0));
            } else {
                int row;

                if (AT_S32(c, 0x10c) != 0)
                    row = -car - 1;
                else if (AT_S32(c, 0x108) != 0)
                    row = -8 - car;
                else
                    row = car - 2 - p;
                m0a184_grid_slot(c, row, tile);
            }
        }
        AT_F32(c, 0x2b4) = 2000.0f;
        h = fn_0000d800(-1, AT_F32(c, 0x2b0), AT_F32(c, 0x2b4), AT_F32(c, 0x2b8), &tmp) + 5.0f;
        AT_F32(c, 0x2b4) = h;
    }

    AT_S32(c, 0xf4) = 0;
    AT_S32(c, 0x24c) = 0;
    memcpy(c + 0x2bc, c + 0x2b0, 12);
    AT_F32(c, 0x274) = h;
    AT_S32(c, 0x64) = GRID_TILE[m0a184_clamp(AT_S32(c, 0x44), w1) * GRID_STRIDE +
                                m0a184_clamp(AT_S32(c, 0x48), h1)];
    AT_F32(c, 0x3d4) = h;
    AT_F32(c, 0x430) = h;
    AT_F32(c, 0x48c) = h;
    AT_F32(c, 0x4e8) = h;
    AT_S32(c, 0x3e0) = 0;
    AT_S32(c, 0x43c) = 0;
    AT_S32(c, 0x498) = 0;
    AT_S32(c, 0x4f4) = 0;
    memcpy(c + 0x398, c + 0x2bc, 12);
    memcpy(c + 0x3f4, c + 0x2bc, 12);
    memcpy(c + 0x450, c + 0x2bc, 12);
    memcpy(c + 0x4ac, c + 0x2bc, 12);
    AT_S32(c, 0x94) = 2;
    AT_S32(c, 0x98) = -1;
    AT_S32(c, 0x8c) = -1;
    AT_S32(c, 0x90) = -1;
    AT_S32(c, 0xbc) = 0;
    AT_S32(c, 0xc0) = 0;
    AT_S32(c, 0xc4) = 0;
    AT_S32(c, 0xac) = 0;
    AT_S32(c, 0xf4) = 0;
    AT_S32(c, 0x298) = 0;
    AT_S32(c, 0x29c) = 0;
    AT_S32(c, 0xb8) = MODE_CUR == MODE_D ? G_S32(0x3f8d8) : 3599999; /* time limit (ms) */
    AT_S32(c, 0xe8) = 0;
    AT_S32(c, 0xec) = 0;
    AT_S32(c, 0x11c) = 0;
    AT_S32(c, 0xa4) = 0;
    gx = (int)((AT_F32(c, 0x2b0) + 406.0f) / 812.0f);
    AT_S32(c, 0x44) = gx;
    gz = (int)((AT_F32(c, 0x2b8) + 406.0f) / 812.0f);
    AT_S32(c, 0x48) = gz;
    ord = 0;
    if ((gx | gz) >= 0 && gx <= G_S32(0x3f8a4) && gz <= h1)
        ord = GRID_ORDER[gx * GRID_STRIDE + gz];
    AT_S32(c, 0x84) = ord;
    AT_S32(c, 0x88) = NCARS - car;
    fn_0000f9a8(car, ord);
}

/* 0x0000cc3c -- Car0_ResetPose: put car 0 at the origin, 37 units up, at rest */
void fn_0000cc3c(void)
{
    uint8_t *c = CAR(0);

    AT_F32(c, 0x274) = 0.0f;
    AT_F32(c, 0x270) = 0.0f;
    AT_F32(c, 0x3ec) = 0.0f;
    AT_F32(c, 0x448) = 0.0f;
    AT_F32(c, 0x4a4) = 0.0f;
    AT_F32(c, 0x500) = 0.0f;
    AT_F32(c, 0x3d4) = 0.0f;
    AT_F32(c, 0x430) = 0.0f;
    AT_F32(c, 0x48c) = 0.0f;
    AT_F32(c, 0x4e8) = 0.0f;
    AT_F32(c, 0x2bc) = 0.0f;
    AT_F32(c, 0x2c0) = 37.0f;
    AT_F32(c, 0x2c4) = 0.0f;
    AT_F32(c, 0x2b8) = 0.0f;
    AT_F32(c, 0x2b4) = AT_F32(c, 0x2c0);
    AT_F32(c, 0x2b0) = AT_F32(c, 0x2bc);
    AT_F32(c, 0x2f8) = 0.0f;
    AT_F32(c, 0x2fc) = 0.0f;
    AT_F32(c, 0x300) = 0.0f;
    AT_F32(c, 0x328) = 0.0f;
    AT_F32(c, 0x32c) = 0.0f;
    AT_F32(c, 0x330) = 0.0f;
    memcpy(c + 0x31c, c + 0x328, 12);
    memcpy(c + 0x304, c + 0x328, 12);
    memcpy(c + 0x310, c + 0x328, 12);
    AT_F32(c, 0x264) = 0.0f;
    AT_F32(c, 0x268) = 0.0f;
    AT_F32(c, 0x254) = 0.0f;
    AT_F32(c, 0x238) = 0.0f;
}

/* 0x0000cd00 -- Map_Load(name): load models/maps/<name>.map and rebuild the racing line */
void fn_0000cd00(const char *name)
{
    char tiles[64], maps[64], path[512];

    snprintf(tiles, sizeof(tiles), "models/map_tiles"); /* unused */
    snprintf(maps, sizeof(maps), "models/maps");
    snprintf(path, sizeof(path), "%s/%s", maps, name);
    fn_0000cd64(path);
    fn_0000cfc8();
}

/* 0x0000cd64 -- Map_Read(path): parse <path>.map (w, h, then 4 w*h byte layers, columns mirrored):
 * racing-line order (cleared), road tiles, objects, heights (with checkpoint and start markers). */
void fn_0000cd64(const char *path)
{
    char buf[512];
    const uint8_t *d = NULL;
    const uint8_t *p;
    int w, h, x, y, n;
    uint8_t *cp = G(0x47f98); /* slot 0x3f364: checkpoints, 0xac bytes each */

    snprintf(buf, sizeof(buf), "%s.map", path);
    fn_00005784(&d, 0, buf);
    w = d[0];
    h = d[1];
    G_U8(0x3e9a4) = (uint8_t)w;
    G_U8(0x3e9a8) = (uint8_t)h;
    G_S32(0x3f8a0) = h - 1;
    G_S32(0x3f8a4) = w - 1;
    memset(LINE_PTS, 0, 0x7f8);
    p = d + 2;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            GRID_ORDER[y * GRID_STRIDE + (w - 1 - x)] = 0;
    for (y = 0; y < h; y++, p += w)
        for (x = 0; x < w; x++)
            GRID_TILE[y * GRID_STRIDE + (w - 1 - x)] = p[x];
    for (y = 0; y < h; y++, p += w)
        for (x = 0; x < w; x++)
            GRID_OBJ[y * GRID_STRIDE + (w - 1 - x)] = p[x] ? (uint8_t)(p[x] - 0x27) : 0;

    for (int k = 0; k < 3; k++)
        AT_S32(cp + k * 0xac, 0x24) = 0;
    G_S32(0x3f97c) = 0;
    n = 0;
    for (y = 0; y < h; y++, p += w) {
        for (x = 0; x < w; x++) {
            int cc = w - 1 - x;
            int v = p[x];
            uint8_t hv;

            if (v == 0) {
                GRID_HGT[y * GRID_STRIDE + cc] = 0;
                continue;
            }
            hv = (uint8_t)(v - 15);
            GRID_HGT[y * GRID_STRIDE + cc] = hv;
            if (hv == 0x0e) { /* checkpoint marker */
                uint8_t *e = cp + n * 0xac;
                AT_S32(e, 0) = y;
                AT_S32(e, 4) = cc;
                AT_S32(e, 0x24) = 1;
                AT_S32(e, 0x1c) = 1;
                AT_S32(e, 0xa8) = 1;
                n++;
                G_S32(0x3f97c) = n;
                GRID_HGT[y * GRID_STRIDE + cc] = 0x0d;
            } else if (hv == 0x13) { /* start tile */
                GRID_ORDER[y * GRID_STRIDE + cc] = 1;
                G_S32(0x3f974) = y;
                G_S32(0x3f978) = cc;
            }
        }
    }
}

/* fn_0000cfc8 helper: racing-line order at (r, c), 0 when off the map. */
static int m0a184_order_at(int r, int c)
{
    if ((r | c) >= 0 && r <= G_S32(0x3f8a4) && c <= G_S32(0x3f8a0))
        return GRID_ORDER[r * GRID_STRIDE + c];
    return 0;
}

/* 0x0000cfc8 -- RacingLine_Build: walk the road from the start tile, numbering each tile in
 * GRID_ORDER and recording its centre in LINE_PTS; sets lap length, thirds and track bounds. */
void fn_0000cfc8(void)
{
    float *pts = LINE_PTS;
    int r = G_S32(0x3f974);
    int c = G_S32(0x3f978);
    int d = G_S32(0x3e9d0);
    int w1 = G_S32(0x3f8a4);
    int h1 = G_S32(0x3f8a0);
    int min_r = 1000, min_c = 1000, max_r = 0, max_c = 0;

    G_S32(0x3f948) = 0;
    memset(pts, 0, 0x7f8);
    for (int y = 0; y < G_U8(0x3e9a8); y++)
        for (int x = 0; x < G_U8(0x3e9a4); x++)
            GRID_ORDER[y * GRID_STRIDE + x] = 0;
    G_S32(0x3f95c) = 1000;
    G_S32(0x3f960) = 1000;
    G_S32(0x3f964) = 0;
    G_S32(0x3f968) = 0;
    pts[2] = (float)r;
    pts[3] = (float)c;

    for (;;) {
        int t = GRID_TILE[m0a184_clamp(r, w1) * GRID_STRIDE + m0a184_clamp(c, h1)];
        int cur = m0a184_order_at(r, c);
        int prev = cur - 1;
        int nv;

        /* Step to the neighbour that is not the previous tile, following the tile's road shape. */
        switch (t) {
        case 1: /* straight along c */
            if (m0a184_order_at(r, c - d) == prev)
                c += d;
            else if (m0a184_order_at(r, c + d) == prev)
                c -= d;
            break;
        case 2: /* straight along r */
            if (m0a184_order_at(r + d, c) == prev)
                r -= d;
            else if (m0a184_order_at(r - d, c) == prev)
                r += d;
            break;
        case 3:
            if (m0a184_order_at(r, c - 1) == prev)
                r++;
            else if (m0a184_order_at(r + 1, c) == prev)
                c--;
            break;
        case 4:
            if (m0a184_order_at(r, c + 1) == prev)
                r++;
            else if (m0a184_order_at(r + 1, c) == prev)
                c++;
            break;
        case 5:
            if (m0a184_order_at(r, c - 1) == prev)
                r--;
            else if (m0a184_order_at(r - 1, c) == prev)
                c--;
            break;
        case 6:
            if (m0a184_order_at(r, c + 1) == prev)
                r--;
            else if (m0a184_order_at(r - 1, c) == prev)
                c++;
            break;
        case 7: /* crossings */
        case 8:
            if (m0a184_order_at(r + 1, c) == prev)
                r--;
            else if (m0a184_order_at(r - 1, c) == prev)
                r++;
            else if (m0a184_order_at(r, c + 1) == prev)
                c--;
            else if (m0a184_order_at(r, c - 1) == prev)
                c++;
            break;
        default:
            break;
        }

        nv = cur + 1;
        if ((r | c) >= 0 && r <= w1 && c <= h1 && GRID_ORDER[r * GRID_STRIDE + c] == 1) {
            /* Back at the start tile: the lap is closed. */
            int n = (uint8_t)nv - 1;
            int k;

            G_S32(0x3f95c) = min_r;
            G_S32(0x3f960) = min_c;
            G_S32(0x3f964) = max_r;
            G_S32(0x3f968) = max_c;
            G_S32(0x3f948) = n;
            G_S32(0x3f94c) = n / 3;
            G_S32(0x3f950) = (2 * n) / 3;
            k = n / 3;
            t = GRID_TILE[m0a184_clamp((int)pts[k * 2], w1) * GRID_STRIDE + m0a184_clamp((int)pts[k * 2 + 1], h1)];
            if ((unsigned)(t - 7) <= 1)
                G_S32(0x3f94c) = k + 1;
            k = (2 * n) / 3;
            t = GRID_TILE[m0a184_clamp((int)pts[k * 2], w1) * GRID_STRIDE + m0a184_clamp((int)pts[k * 2 + 1], h1)];
            if ((unsigned)(t - 7) <= 1)
                G_S32(0x3f950) = k + 1;
            G_S32(0x3f96c) = min_r + (max_r - min_r) / 2;
            G_S32(0x3f970) = min_c + (max_c - min_c) / 2;
            return;
        }
        /* (the original writes here without a bounds check, like this) */
        GRID_ORDER[r * GRID_STRIDE + c] = (uint8_t)nv;
        pts[(uint8_t)nv * 2] = (float)r;
        pts[(uint8_t)nv * 2 + 1] = (float)c;
        if ((float)r < (float)min_r)
            min_r = r;
        if ((float)c < (float)min_c)
            min_c = c;
        if ((float)r > (float)max_r)
            max_r = r;
        if ((float)c > (float)max_c)
            max_c = c;
    }
}

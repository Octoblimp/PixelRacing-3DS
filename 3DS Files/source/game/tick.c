/* The per-frame game tick: FUN_00005ac4 of the iOS build (armv7, 0x5ac4..0x9dbc), plus the
 * input/recording helper that follows it in the binary (FUN_00009dc0, 0x9dc0..0xa184).
 *
 * fn_00005ac4 is called once per frame by drawFrame. It filters the accelerometer into the
 * steering value, then dispatches on the top-level game state G_S32(0x3f840):
 *   0      boot: engine/sound init, language strings, tables, big buffers, first menu
 *   1..5   nothing (states not used by the iOS build)
 *   6      menus (front end; sub-screen in G_S32(0x3fa24))
 *   7      race (pause menu, race-over detection, collisions, ranking, weather)
 *   8      race results / podium
 *   9      a stand-alone screen G_S32(0x3f844), back to the state in G_S32(0x3f850)
 * State 0 always ends with fn_0000a40c() (music selection); states 6, 7 and 8 only on the frame
 * they hand over to another state; state 9 calls it itself when leaving. */
#include "ios.h"
#include "controls.h"

/* Race recording (fn_00009dc0). Original: 200000 frames per player, snapshot stride 0x277830
 * (1562 * 0x678) per car, 16 cars. 3DS: 36864 frames (10.24 min at 60 ticks/s, a multiple of
 * the 128-frame snapshot period), 8 cars (G_S32(0x3e9c8)); recording stops and replay holds
 * its last frame past the window. */
#define REC_FRAMES 36864
#define REC_SNAPS (REC_FRAMES / 128)
#define REC_CAR_STRIDE (REC_SNAPS * 0x678)
#define REC_MAX_CARS 8
#define REC_INPUT_BYTES (4 * REC_FRAMES * 16)
#define REC_SNAP_BYTES (REC_MAX_CARS * REC_CAR_STRIDE)

/* fn_0002d550: per-car physics step (m_2d550.c). */
#define TICK_CAR_PHYSICS(car) fn_0002d550(car)

#define CAR(i) ((uint8_t *)G(0x48ae0) + (i) * 0x678)   /* car structs, 0x678 bytes each */

/* __strcpy_chk(dst, src, size): strcpy that never writes past the destination object. */
static void tick_strcpy(char *dst, const char *s, int size)
{
    int n = (int)strlen(s);
    if (n >= size) {
        sys_log("tick: string too long for its buffer (%d >= %d)", n, size);
        n = size - 1;
    }
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* Reset of the per-car fields done when a race is (re)started. Cars are 0x678 bytes at
 * 0x48ae0, G_S32(0x3e9c8) of them; 0x476c4 is the car order table. */
static void tick_reset_cars(void)
{
    int n = G_S32(0x3e9c8);
    uint8_t *car = G(0x48ae0);
    for (int i = 0; i < n; i++, car += 0x678) {
        ((int *)G(0x476c4))[i] = i;
        AT_S32(car, 0xa0) = 0;
        AT_S32(car, 0x104) = 0;
        AT_F32(car, 0x3f0) = 1.0f;
        AT_F32(car, 0x44c) = 1.0f;
        AT_F32(car, 0x4a8) = 1.0f;
        AT_F32(car, 0x504) = 1.0f;
        AT_F32(car, 0x20) = 1.0f;
    }
}

/* State 0: one-time boot. */
static void tick_boot(void)
{
    fn_00004028();
    fn_00038158();
    fn_00035fdc();
    G_U8(0x3e9a0) = G_U8(0x3f83c) & 1;
    G_U8(0x3f888) = 0;
    fn_00004560();
    /* screen size (u16 pair at 0x42938) and the 640x480 / 320x240 reference sizes */
    G_S32(0x3f854) = G_U16(0x42938 + 2);
    G_S32(0x3f858) = G_U16(0x42938);
    G_S32(0x3f85c) = G_U16(0x42938 + 2) >> 1;
    G_S32(0x3f860) = G_U16(0x42938) >> 1;
    G_S32(0x3f864) = 0x280;
    G_S32(0x3f868) = 0x1e0;
    G_S32(0x3f86c) = 0x140;
    G_S32(0x3f870) = 0xf0;
    /* race recording buffers, see fn_00009dc0. The original reserves 12.8 MB of inputs and
     * 41.4 MB of car snapshots (200000 frames, ~55 min); the 3DS port keeps a 10-minute
     * window (2.4 MB + 3.8 MB): REC_FRAMES / REC_CAR_STRIDE / REC_MAX_CARS below. */
    G_S32(0x3f994) = REC_INPUT_BYTES;
    G_PTR(0x5c174) = fn_000055f8(REC_INPUT_BYTES);
    G_S32(0x3f998) = REC_SNAP_BYTES;
    G_PTR(0x5c178) = fn_000055f8(REC_SNAP_BYTES);
    G_U8(0x3f990) = 0;
    G_S32(0x446e0) = 0;
    for (int i = 0; i < 10; i++)
        ((int *)G(0x4821c))[i] = i + 0xb4;
    for (int i = 0; i < 15; i++)
        ((int *)G(0x47770))[i] = i + 0xbe;
    for (int i = 0; i < 40; i++)
        ((int *)G(0x47454))[i] = i + 0xcd;
    memset(G(0x470a0), 0xff, 0x30);
    memset(G(0x47218), 0xff, 0x2c);
    memset(G(0x47258), 0xff, 0x30);
    memset(G(0x46e50), 0xff, 0x2c);
    memset(G(0x46e40), 0xff, 0x14);
    memset(G(0x46e88), 0xff, 0x14);
    memset(G(0x46e78), 0xff, 0x14);
    fn_00035df8();
    /* language strings (char * globals) */
    G_PTR(0x47df0) = (void *)fn_00035f88(1);
    G_PTR(0x47df4) = (void *)fn_00035f88(2);
    G_PTR(0x47d60) = (void *)fn_00035f88(3);
    G_PTR(0x47d74) = (void *)fn_00035f88(4);
    G_PTR(0x47d30) = (void *)fn_00035f88(5);
    G_PTR(0x47dd0) = (void *)fn_00035f88(6);
    G_PTR(0x47ddc) = (void *)fn_00035f88(7);
    G_PTR(0x47e00) = (void *)fn_00035f88(8);
    G_PTR(0x47e14) = (void *)fn_00035f88(9);
    G_PTR(0x47dd8) = (void *)fn_00035f88(10);
    G_PTR(0x47d34) = (void *)fn_00035f88(11);
    G_PTR(0x47d98) = (void *)fn_00035f88(12);
    G_PTR(0x47e20) = (void *)fn_00035f88(13);
    G_PTR(0x47d9c) = (void *)fn_00035f88(14);
    G_PTR(0x47de0) = (void *)fn_00035f88(15);
    G_PTR(0x47cd0) = (void *)fn_00035f88(16);
    G_PTR(0x47cf0) = (void *)fn_00035f88(17);
    G_PTR(0x47e10) = (void *)fn_00035f88(18);
    G_PTR(0x47e28) = (void *)fn_00035f88(19);
    G_PTR(0x47d50) = (void *)fn_00035f88(20);
    G_PTR(0x47e34) = (void *)fn_00035f88(21);
    G_PTR(0x47d7c) = (void *)fn_00035f88(22);
    G_PTR(0x47d54) = (void *)fn_00035f88(23);
    G_PTR(0x47dac) = (void *)fn_00035f88(24);
    G_PTR(0x47da8) = (void *)fn_00035f88(25);
    G_PTR(0x47d24) = (void *)fn_00035f88(26);
    G_PTR(0x47db4) = (void *)fn_00035f88(27);
    G_PTR(0x47dc4) = (void *)fn_00035f88(28);
    G_PTR(0x47dd4) = (void *)fn_00035f88(29);
    G_PTR(0x47d64) = (void *)fn_00035f88(30);
    G_PTR(0x47d18) = (void *)fn_00035f88(31);
    G_PTR(0x47d28) = (void *)fn_00035f88(32);
    G_PTR(0x47d84) = (void *)fn_00035f88(33);
    G_PTR(0x47d4c) = (void *)fn_00035f88(34);
    G_PTR(0x47dbc) = (void *)fn_00035f88(35);
    G_PTR(0x47e0c) = (void *)fn_00035f88(36);
    G_PTR(0x47db0) = (void *)fn_00035f88(37);
    G_PTR(0x47e38) = (void *)fn_00035f88(38);
    G_PTR(0x47e30) = (void *)fn_00035f88(39);
    G_PTR(0x47d38) = (void *)fn_00035f88(40);
    G_PTR(0x47da4) = (void *)fn_00035f88(41);
    G_PTR(0x47dc0) = (void *)fn_00035f88(42);
    G_PTR(0x47df8) = (void *)fn_00035f88(43);
    G_PTR(0x47e04) = (void *)fn_00035f88(44);
    G_PTR(0x47d80) = (void *)fn_00035f88(45);
    G_PTR(0x47cd8) = (void *)fn_00035f88(46);
    G_PTR(0x47d2c) = (void *)fn_00035f88(47);
    G_PTR(0x47cf8) = (void *)fn_00035f88(48);
    G_PTR(0x47e2c) = (void *)fn_00035f88(49);
    G_PTR(0x47e08) = (void *)fn_00035f88(50);
    G_PTR(0x47dfc) = (void *)fn_00035f88(51);
    G_PTR(0x47cdc) = (void *)fn_00035f88(52);
    G_PTR(0x47dec) = (void *)fn_00035f88(53);
    G_PTR(0x47d5c) = (void *)fn_00035f88(54);
    G_PTR(0x47e50) = (void *)fn_00035f88(55);
    G_PTR(0x47d1c) = (void *)fn_00035f88(56);
    G_PTR(0x47ce4) = (void *)fn_00035f88(57);
    G_PTR(0x47db8) = (void *)fn_00035f88(58);
    G_PTR(0x47de4) = (void *)fn_00035f88(59);
    G_PTR(0x47d88) = (void *)fn_00035f88(60);
    G_PTR(0x47dcc) = (void *)fn_00035f88(61);
    G_PTR(0x47e24) = (void *)fn_00035f88(62);
    G_PTR(0x47e78) = (void *)fn_00035f88(63);
    G_PTR(0x47d04) = (void *)fn_00035f88(64);
    G_PTR(0x47d44) = (void *)fn_00035f88(65);
    G_PTR(0x47e68) = (void *)fn_00035f88(66);
    G_PTR(0x47e58) = (void *)fn_00035f88(67);
    G_PTR(0x47e70) = (void *)fn_00035f88(68);
    G_PTR(0x47e6c) = (void *)fn_00035f88(69);
    G_PTR(0x47e60) = (void *)fn_00035f88(70);
    G_PTR(0x47e64) = (void *)fn_00035f88(71);
    G_PTR(0x47e74) = (void *)fn_00035f88(72);
    G_PTR(0x47d58) = (void *)fn_00035f88(73);
    G_PTR(0x47ce8) = (void *)fn_00035f88(74);
    G_PTR(0x47d94) = (void *)fn_00035f88(75);
    G_PTR(0x47d8c) = (void *)fn_00035f88(76);
    G_PTR(0x47d90) = (void *)fn_00035f88(77);
    G_PTR(0x47cfc) = (void *)fn_00035f88(78);
    G_PTR(0x47cf4) = (void *)fn_00035f88(79);
    G_PTR(0x47e48) = (void *)fn_00035f88(80);
    G_PTR(0x47d70) = (void *)fn_00035f88(81);
    G_PTR(0x47d78) = (void *)fn_00035f88(82);
    G_PTR(0x47da0) = (void *)fn_00035f88(83);
    G_PTR(0x47e5c) = (void *)fn_00035f88(84);
    G_PTR(0x47e18) = (void *)fn_00035f88(85);
    G_PTR(0x47d40) = (void *)fn_00035f88(86);
    G_PTR(0x47d48) = (void *)fn_00035f88(87);
    G_PTR(0x47cd4) = (void *)fn_00035f88(88);
    G_PTR(0x47d00) = (void *)fn_00035f88(89);
    G_PTR(0x47e4c) = (void *)fn_00035f88(90);
    G_PTR(0x47d3c) = (void *)fn_00035f88(91);
    G_PTR(0x47ce0) = (void *)fn_00035f88(92);
    G_PTR(0x47d20) = (void *)fn_00035f88(93);
    G_PTR(0x47e54) = (void *)fn_00035f88(94);
    G_PTR(0x47cec) = (void *)fn_00035f88(95);
    G_PTR(0x47e1c) = (void *)fn_00035f88(96);
    G_PTR(0x47d6c) = (void *)fn_00035f88(97);
    G_PTR(0x47e3c) = (void *)fn_00035f88(98);
    G_PTR(0x47e40) = (void *)fn_00035f88(99);
    G_PTR(0x47e44) = (void *)fn_00035f88(100);
    G_PTR(0x47d68) = (void *)fn_00035f88(101);
    G_PTR(0x47dc8) = (void *)fn_00035f88(102);
    G_PTR(0x47de8) = (void *)fn_00035f88(103);
    G_PTR(0x47d08) = (void *)"Download Full Version";
    G_PTR(0x47d10) = (void *)"Start Demo";
    G_PTR(0x47d14) = (void *)"Wii Menu";
    G_PTR(0x47d0c) = (void *)"Play Again";
    /* fixed-size string tables (strcpy targets overlap as in the original) */
    tick_strcpy((char *)G(0x47180), fn_00035f88(119), 0x50);
    tick_strcpy((char *)G(0x47180) + 0x4, fn_00035f88(120), 0x4c);
    tick_strcpy((char *)G(0x47180) + 0x8, fn_00035f88(121), 0x48);
    tick_strcpy((char *)G(0x47180) + 0xc, fn_00035f88(122), 0x44);
    tick_strcpy((char *)G(0x47180) + 0x10, fn_00035f88(123), 0x40);
    tick_strcpy((char *)G(0x47180) + 0x14, fn_00035f88(124), 0x3c);
    tick_strcpy((char *)G(0x47180) + 0x18, fn_00035f88(125), 0x38);
    tick_strcpy((char *)G(0x47180) + 0x1c, fn_00035f88(126), 0x34);
    tick_strcpy((char *)G(0x47180) + 0x20, fn_00035f88(127), 0x30);
    tick_strcpy((char *)G(0x47180) + 0x24, fn_00035f88(128), 0x2c);
    tick_strcpy((char *)G(0x47180) + 0x28, fn_00035f88(129), 0x28);
    tick_strcpy((char *)G(0x47180) + 0x2c, fn_00035f88(130), 0x24);
    tick_strcpy((char *)G(0x47180) + 0x30, fn_00035f88(131), 0x20);
    tick_strcpy((char *)G(0x47180) + 0x34, fn_00035f88(132), 0x1c);
    tick_strcpy((char *)G(0x47180) + 0x38, fn_00035f88(133), 0x18);
    tick_strcpy((char *)G(0x47180) + 0x3c, fn_00035f88(134), 0x14);
    tick_strcpy((char *)G(0x47180) + 0x40, fn_00035f88(135), 0x10);
    tick_strcpy((char *)G(0x47180) + 0x44, fn_00035f88(136), 0xc);
    tick_strcpy((char *)G(0x47180) + 0x48, fn_00035f88(137), 0x8);
    tick_strcpy((char *)G(0x47180) + 0x4c, fn_00035f88(138), 0x4);
    tick_strcpy((char *)G(0x43600), fn_00035f88(149), 0xe0);
    tick_strcpy((char *)G(0x43600) + 0x20, fn_00035f88(150), 0xc0);
    tick_strcpy((char *)G(0x43600) + 0x40, fn_00035f88(151), 0xa0);
    tick_strcpy((char *)G(0x43600) + 0x60, fn_00035f88(152), 0x80);
    tick_strcpy((char *)G(0x43600) + 0x80, fn_00035f88(153), 0x60);
    tick_strcpy((char *)G(0x43600) + 0xa0, fn_00035f88(154), 0x40);
    tick_strcpy((char *)G(0x43600) + 0xc0, fn_00035f88(155), 0x20);
    tick_strcpy((char *)G(0x44200), fn_00035f88(159), 0xe0);
    tick_strcpy((char *)G(0x44200) + 0x20, fn_00035f88(160), 0xc0);
    tick_strcpy((char *)G(0x44200) + 0x40, fn_00035f88(161), 0xa0);
    tick_strcpy((char *)G(0x44200) + 0x60, fn_00035f88(162), 0x80);
    tick_strcpy((char *)G(0x44200) + 0x80, fn_00035f88(163), 0x60);
    tick_strcpy((char *)G(0x44200) + 0xa0, fn_00035f88(164), 0x40);
    tick_strcpy((char *)G(0x44200) + 0xc0, fn_00035f88(165), 0x20);
    tick_strcpy((char *)G(0x47900), fn_00035f88(169), 0xa0);
    tick_strcpy((char *)G(0x47900) + 0x10, fn_00035f88(170), 0x90);
    tick_strcpy((char *)G(0x47900) + 0x20, fn_00035f88(171), 0x80);
    tick_strcpy((char *)G(0x47900) + 0x30, fn_00035f88(172), 0x70);
    tick_strcpy((char *)G(0x47900) + 0x40, fn_00035f88(173), 0x60);
    tick_strcpy((char *)G(0x47900) + 0x50, fn_00035f88(174), 0x50);
    tick_strcpy((char *)G(0x47900) + 0x60, fn_00035f88(175), 0x40);
    tick_strcpy((char *)G(0x47900) + 0x70, fn_00035f88(176), 0x30);
    tick_strcpy((char *)G(0x47900) + 0x80, fn_00035f88(177), 0x20);
    tick_strcpy((char *)G(0x47900) + 0x90, fn_00035f88(178), 0x10);
    tick_strcpy((char *)G(0x47300), fn_00035f88(189), 0xb0);
    tick_strcpy((char *)G(0x47300) + 0x10, fn_00035f88(190), 0xa0);
    tick_strcpy((char *)G(0x47300) + 0x20, fn_00035f88(191), 0x90);
    tick_strcpy((char *)G(0x47300) + 0x30, fn_00035f88(192), 0x80);
    tick_strcpy((char *)G(0x47300) + 0x40, fn_00035f88(193), 0x70);
    tick_strcpy((char *)G(0x47300) + 0x50, fn_00035f88(194), 0x60);
    tick_strcpy((char *)G(0x47300) + 0x60, fn_00035f88(195), 0x50);
    tick_strcpy((char *)G(0x47300) + 0x70, fn_00035f88(196), 0x40);
    tick_strcpy((char *)G(0x47300) + 0x80, fn_00035f88(197), 0x30);
    tick_strcpy((char *)G(0x47300) + 0x90, fn_00035f88(198), 0x20);
    tick_strcpy((char *)G(0x47300) + 0xa0, fn_00035f88(199), 0x10);
    fn_000055bc();
    G_U8(0x3f918) = 0;
    G_U8(0x3fab8) = 1;
    G_U8(0x3f91c) = 0;
    fn_00011dbc();
    G_S32(0x3fa28) = 4;
    G_S32(0x3fa24) = 4;
    G_U8(0x3f928) = 1;
    G_U8(0x3f920) = 1;
    G_S32(0x3e9b0) = 0;
    fn_000117b0();
    G_S32(0x3e9b0) = 1;
    tick_reset_cars();
    srand(fn_00004208());
    fn_000209e8();
}

/* Music handle of the current track (index G_S32(0x3e9ec), -1 when none). */
#define TICK_MUSIC() (((int *)G(0x46e50))[G_S32(0x3e9ec)])

/* State 6: front-end menus. Returns false when the tick must return without the music
 * update (fn_0000a40c). */
static bool tick_menus(void)
{
    int sub = G_S32(0x3fa24);
    if (G_S32(0x3f8e8) < 400 && (sub == 5 || sub == 0x1e)) {
        fn_000302d0(TICK_MUSIC(), 12000);
    } else {
        if (G_S32(0x3f8e8) < 400)
            G_S32(0x3f8e8)++;
        if (sub != 4)
            fn_000302d0(TICK_MUSIC(), 12000);
    }
    fn_00009dc0(0);
    if ((fn_00004540() & 0xff) == 0) {
        if (G_S32(0x3f9dc) == G_S32(0x3ebe4)) {
            float a = G_F32(0x3f9a8) + 10.0f;
            if (a > 360.0f)
                a += -360.0f;
            G_F32(0x3f9a8) = a;
        }
        fn_0003485c();
        fn_00034a98();
        if (G_S32(0x3fa24) == 4) {
            if (G_S32(0x3f088) > 0x157b)
                G_S32(0x3f874) = 1;
            else
                G_S32(0x3f088) += 5;
        }
        G_U8(0x3f8f8) = 1;
        G_F32(0x3f8ec) = (float)fn_00004208();
        fn_00030bf4();
        sub = G_S32(0x3fa24);
        if ((unsigned)(sub - 10) < 2)
            G_U8(0x402f4) = 1;
        else if ((unsigned)(sub - 0x14) < 2)
            G_U8(0x402f4) = 0;

        /* screen fade: 0x3fa28 = requested screen, 0x3fa14 = fade alpha (0..255) */
        if (G_S32(0x3fa28) == sub) {
            if (G_F32(0x3fa14) < 255.0f) {
                G_F32(0x3fa14) += 10.0f;
                if (G_F32(0x3fa14) >= 255.0f) {
                    G_F32(0x3fa14) = 255.0f;
                    G_U8(0x3f924) = 1;
                }
            }
        } else if (G_S32(0x3fa2c) >= 1) {
            G_S32(0x3fa2c)--;
        } else {
            G_U8(0x3f924) = 0;
            G_F32(0x3fa14) += -10.0f;
            if (!(G_F32(0x3fa14) > 0.0f)) {
                int next = G_S32(0x3fa28);
                G_F32(0x3fa14) = 0.0f;
                if (next != 0x17 && next != 0x19) {
                    if (next == 10 && G_S32(0x3f8c4) != 0) {
                        G_S32(0x3faa8) = 1;
                        G_S32(0x3ec00) = 2;
                    } else {
                        G_S32(0x3faa8) = 0;
                    }
                }
                G_S32(0x3fa24) = next;
                G_S32(0x3fa08) = G_S32(0x3fa0c);
                fn_000209e8();
                sub = G_S32(0x3fa24);
                if ((unsigned)(sub - 0x14) < 2) {
                    uint8_t *e = (uint8_t *)G(0x47544) + G_S32(0x3fa08) * 0x18;
                    G_S32(0x3f9fc) = AT_S32(e, 0) - 1;
                    int r = fn_00011cf8(G_S32(0x3f9fc), AT_S32(e, 0xc), 0);
                    AT_S32(e, 4) = r;
                    G_S32(0x3fa04) = (4 - r) % 125;
                }
                G_F32(0x3fa18) = 200.0f;
            }
        }

        bool sel = (unsigned)(sub - 10) < 2 || sub == 0x14;
        if (sel || sub == 0x15 || sub == 0x19 || sub == 0x17 || sub == 0x18 || sub == 0xc) {
            /* 0x3fa1c: cross-fade of the selected item (0..1) */
            if (sub == 0x17) {
                if (G_S32(0x3f9f8) != G_S32(0x3f9f4)) {
                    if (G_U8(0x3f898) & 1) {
                        if (G_F32(0x3fa1c) > 0.0f) {
                            G_F32(0x3fa1c) += -0.08f;
                            if (!(G_F32(0x3fa1c) > 0.0f))
                                G_F32(0x3fa1c) = 0.0f;
                        } else {
                            G_S32(0x3f9f8) = G_S32(0x3f9f4);
                        }
                    }
                } else if (G_F32(0x3fa1c) < 1.0f) {
                    G_F32(0x3fa1c) += 0.08f;
                    if (G_F32(0x3fa1c) >= 1.0f)
                        G_F32(0x3fa1c) = 1.0f;
                }
            }
            if (sel || sub == 0xc) {
                int cur = G_S32(0x3f9fc);
                if (G_S32(0x3fa00) != cur) {
                    if (G_U8(0x3f898) & 1) {
                        if (G_F32(0x3fa1c) > 0.0f) {
                            G_F32(0x3fa1c) += sub == 0x14 ? -0.1f : -0.02f;
                            if (!(G_F32(0x3fa1c) > 0.0f))
                                G_F32(0x3fa1c) = 0.0f;
                        } else {
                            /* switch the player car's colours to the new selection */
                            G_S32(0x3fa00) = cur;
                            G_S32(0x48ae0) = ((int *)G(0x470a0))[cur + 1];
                            G_S32(0x48ae4) = ((int *)G(0x47218))[cur + 1];
                            G_S32(0x48ae8) = ((int *)G(0x47258))[cur + 1];
                        }
                    }
                } else if (G_F32(0x3fa1c) < 1.0f) {
                    G_F32(0x3fa1c) += sub == 0x14 ? 0.1f : 0.02f;
                    if (G_F32(0x3fa1c) >= 1.0f)
                        G_F32(0x3fa1c) = 1.0f;
                } else if (G_S32(0x3fa20) < 200) {
                    G_S32(0x3fa20)++;
                } else {
                    G_S32(0x3fa20) = 0;
                    if ((unsigned)(sub - 10) < 2) {
                        /* attract mode: cycle the selection every 200 frames */
                        cur++;
                        if (cur > 10)
                            cur = 0;
                        if (cur < 0)
                            cur = 10;
                        G_S32(0x3f9fc) = cur;
                        fn_0002c13c(0, cur);
                        sub = G_S32(0x3fa24);
                    }
                }
            }
            if ((unsigned)(sub - 10) < 3 || sub == 0x19) {
                G_S32(0x5c568) = 6;
                fn_00030318();
            } else if ((unsigned)(sub - 0x14) < 2) {
                G_S32(0x5c568) = 5;
                G_S32(0x5c570) = 0;
                fn_00035600();
            } else if ((unsigned)(sub - 0x17) < 2) {
                G_S32(0x5c568) = 9;
                G_S32(0x5c570) = 0;
                fn_00035600();
            }
        } else if (sub == 4 || sub == 5) {
            G_S32(0x5c568) = sub == 4 ? 6 : 5;
            G_S32(0x5c570) = 0;
            fn_00030318();
        }

        fn_0002c2d8(0);
        if ((unsigned)(G_S32(0x3fa24) - 4) >= 2) {
            TICK_CAR_PHYSICS(0);
            fn_0000cc3c();
        }
        fn_0002bf2c();
        /* leaving the menus: fade the music out, then start the race */
        if (G_S32(0x3fa24) == 0x1e && (G_U8(0x3f898) & 1))
            G_U8(0x3fed8) = 1;
        else
            G_U8(0x3fed8) = G_S32(0x3f874) != 0;
        if (G_U8(0x3fed8) && !(G_U8(0x3fee0) & 1)) {
            G_U8(0x3fee0) = 1;
            G_S32(0x3f8f0) = 0;
            G_S32(0x3f8f4) = 0xff;
        }
        if (G_U8(0x3fee0) & 1)
            fn_000302d0(TICK_MUSIC(), 0);
        G_U8(0x3fedc) = G_S32(0x3f8f0) == 0xff;
    }
    fn_00020f08();
    if (!(G_U8(0x3fedc) & 1))
        return false;
    G_S32(0x3e9cc) = 5;
    if (G_S32(0x3f9dc) == G_S32(0x3ebe8))
        G_S32(0x3e9cc) = 1;
    memset(G(0x47744), 0, 0x28);
    memset(G(0x48274), 0, 0x14);
    tick_reset_cars();
    G_S32(0x3f8b4) = 0;
    G_U8(0x3f9e4) = 0;
    G_U8(0x3f9e0) = 0;
    if (G_S32(0x3f8c4) != 0)
        G_S32(0x3e9cc) = 1;
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * State 7: the race.
 * --------------------------------------------------------------------------------------------- */

#define FP(p, o) ((float *)((uint8_t *)(p) + (o)))   /* float vector inside a struct */

/* fn_00035450 is declared as (int, int) but reads two more floats (s0/s1, stereo levels). */
typedef void (*tick_engine_snd_fn)(int car, int player, float a, float b);
static void tick_engine_snd(int car, int player, float a, float b)
{
    tick_engine_snd_fn f = (tick_engine_snd_fn)(void *)fn_00035450;
    f(car, player, a, b);
}

/* Scenery movers (0x47f98, 0xac bytes each, G_S32(0x3f97c) of them): trains of 1..5 wagons that
 * cross the track and respawn on the other side. Fields: +4 lane (int), +8/+0xc extent (int),
 * +0x10 position (float), +0x18 lane position (float), +0x20 near-a-player flag, +0x24 active,
 * +0x28 wagon count, +0x2c length (float), +0x30[] wagon kind, +0x58[] has-coupling flag,
 * +0x80[] wagon length (float), +0xa8 direction (-1/0/1). */
static void tick_race_scenery(void)
{
    static const float wagon_len[2] = { 1.5f, 1.0f };   /* table at 0x8534 */
    for (int k = 0; k < G_S32(0x3f97c); k++) {
        uint8_t *o = (uint8_t *)G(0x47f98) + k * 0xac;
        if (AT_S32(o, 0x24) == 0)
            continue;
        AT_F32(o, 0x18) = (float)AT_S32(o, 4) * 812.0f;
        AT_S32(o, 0x20) = 0;
        int mult = 700;
        if (G_S32(0x3e9b0) > 0) {
            for (int j = 0; j < G_S32(0x3e9b0); j++)
                if (fn_00037fa4(FP(CAR(j), 0x2bc), FP(o, 0x10)) < 3248.0f)
                    AT_S32(o, 0x20) = 1;
            if (AT_S32(o, 0x20) != 0)
                mult = 70;   /* slow down near a player */
        }
        float x = AT_F32(o, 0x10) + (float)((mult * AT_S32(o, 0xa8)) / (AT_S32(o, 0x28) + 5));
        AT_F32(o, 0x10) = x;
        float len = AT_F32(o, 0x2c);
        if (!(x < -len) && !(x > len + 16240.0f))
            continue;

        /* off the track area: respawn with a new random train */
        float r = (float)fn_00004400(2) + -0.5f;
        AT_S32(o, 0xa8) = r < 0.0f ? -1 : (r > 0.0f);
        int n = fn_00004400(5) + 1;
        AT_S32(o, 0x28) = n;
        AT_F32(o, 0x2c) = 0.0f;
        float total = 0.0f;
        if (n >= 1) {
            for (int w = 0; w < AT_S32(o, 0x28); w++) {
                float l = wagon_len[fn_00004400(2) == 0 ? 1 : 0];
                ((float *)(o + 0x80))[w] = l;
                AT_F32(o, 0x2c) += l;
                if (w == 0) {
                    ((int *)(o + 0x58))[w] = 0;
                    ((int *)(o + 0x30))[w] = 0;
                } else {
                    ((int *)(o + 0x58))[w] = 1;
                    ((int *)(o + 0x30))[w] = fn_00004400(5);
                }
            }
            total = AT_F32(o, 0x2c) * 320.0f;
        }
        AT_F32(o, 0x2c) = total;
        if (AT_S32(o, 0xa8) == 1) {
            int e = (int)(AT_F32(o, 0x80) * 155.0f);
            AT_S32(o, 0xc) = e;
            AT_S32(o, 8) = (int)((float)e - total);
            AT_F32(o, 0x10) = 0.0f;
        } else {
            int e = (int)(AT_F32(o, 0x80) * -155.0f);
            AT_S32(o, 8) = e;
            AT_S32(o, 0xc) = (int)((float)e + total);
            if (AT_S32(o, 0xa8) == -1)
                AT_F32(o, 0x10) = 16240.0f;
        }
    }
}

/* Race-over detection (asm 0x79c2..0x80c6). Sets G_U8(0x3f8d4) (race over) and, when the race
 * ends, records every car's result with fn_0003517c. Game modes (G_S32(0x3f9dc)):
 * G_S32(0x3ebe0)=1 chase (one target car G_S32(0x3f9b0)), G_S32(0x3ebe4)=2 tag (points in
 * +0x114, out at G_S32(0x3e9d8)), G_S32(0x3ebe8)=3 / G_S32(0x3f9d8) time limited,
 * G_S32(0x3ebec)=4 race of the human players. */
static void tick_race_end_check(void)
{
    int mode = G_S32(0x3f9dc);
    int done, timeout = 0, end;

    if (!(G_U8(0x3f8f8) & 1)) {
        int t = G_S32(0x3f8e4);
        if (t < 450 && (t > 400 || (G_U8(0x3f928) & 1)))
            G_U8(0x3f8f8) = 1;   /* start counting: the countdown is over */
        else
            return;
    }
    if (G_U8(0x3f8d4) & 1)
        return;

    if (mode == G_S32(0x3ebe4)) {
        /* tag: a runner is out when too far from every chaser, or when tagged too often */
        for (int i = 0; i < G_S32(0x3e9c8); i++) {
            uint8_t *c = CAR(i);
            if (AT_S32(c, 0xf4) != 0 || AT_S32(c, 0x10c) == 0)
                continue;
            float best = 1e8f;
            for (int j = 0; j < G_S32(0x3e9c8); j++) {
                uint8_t *o = CAR(j);
                if (AT_S32(o, 0x108) == 0)
                    continue;
                if (!(best < fn_00037fa4(FP(c, 0x2b0), FP(o, 0x2b0))))
                    best = fn_00037fa4(FP(c, 0x2b0), FP(o, 0x2b0));
            }
            if (best > 3248.0f) {
                AT_S32(c, 0xf4) = 1;
                AT_S32(c, 0xf8) = 1;
                fn_0003517c(i, 1);
                G_S32(0x3f9c0)++;
            }
            if (AT_S32(c, 0x114) >= G_S32(0x3e9d8)) {
                AT_S32(c, 0x114) = G_S32(0x3e9d8);
                AT_S32(c, 0xf4) = 1;
                AT_S32(c, 0xf8) = 0;
                fn_0003517c(i, 1);
                G_S32(0x3f9c4)++;
            }
            if (AT_S32(c, 0xe4) >= 2) {
                AT_S32(c, 0xf4) = 1;
                AT_S32(c, 0xf8) = 0;
                G_S32(0x3f9c4)++;
            }
        }
        if (!(G_U8(0x3f9b8) & 1)) {
            int all = 1;
            for (int i = 0; i < G_S32(0x3e9c8); i++)
                if (AT_S32(CAR(i), 0x10c) != 0 && all)
                    all = AT_S32(CAR(i), 0xf4) != 0;
            G_U8(0x3f9b8) = (uint8_t)all;
        }
        if (!(G_U8(0x3f9bc) & 1) && G_F32(0x3f8ec) > (float)G_S32(0x3e9dc))
            G_U8(0x3f9bc) = 1;   /* time is up */
        for (int i = 0; i < G_S32(0x3e9c8); i++) {
            uint8_t *c = CAR(i);
            if (AT_S32(c, 0xf4) != 0)
                continue;
            bool over = (G_U8(0x3f9b8) & 1) || (G_U8(0x3f9bc) & 1);
            if (AT_S32(c, 0x108) != 0) {
                if (over) {
                    fn_0003517c(i, 0);
                    AT_S32(c, 0xf4) = 1;
                }
            } else if (AT_S32(c, 0x10c) != 0 && over) {
                AT_S32(c, 0xf4) = 1;
            }
        }
        if (G_U8(0x3f9b8) & 1) {
            end = 1;
            goto set_end;
        }
        done = G_U8(0x3f9bc) & 1;
    } else if (mode == G_S32(0x3f9d8) || mode == G_S32(0x3ebe8)) {
        if (G_F32(0x3f8ec) > (float)G_S32(0x3e9dc)) {
            /* time is up: every car gets a finishing place */
            int n = G_S32(0x3e9c8);
            for (int k = 0; k < n; k++) {
                int cnt = 0;
                for (int m = 0; m < n; m++)
                    cnt += AT_S32(CAR(m), 0xf4) != 0;
                AT_S32(CAR(k), 0xf4) = cnt + 1;
            }
        }
        done = 1;
        for (int k = 0; k < G_S32(0x3e9b0); k++)
            if (done)
                done = AT_S32(CAR(k), 0xf4) != 0;
    } else {
        int ebe0 = G_S32(0x3ebe0);
        int n = G_S32(0x3e9ac);
        int target = G_S32(0x3f9b0);
        int np = G_S32(0x3e9b0);
        int ebec = G_S32(0x3ebec);
        done = 1;
        for (int i = 0; i < n; i++) {
            if (mode == ebe0) {
                if (target < 0)
                    done = 0;
                else if (done)
                    done = AT_S32(CAR(target), 0xf4) != 0;
            } else if (mode == ebec && i < np) {
                if (done)
                    done = AT_S32(CAR(i), 0xf4) != 0;
            }
        }
        if (mode == ebe0) {
            float lim = (float)G_S32(0x3e9dc);
            float t = G_F32(0x3f8ec);
            if (t > lim) {
                timeout = 1;
                if (G_S32(0x3f9b0) >= 0) {
                    uint8_t *c = CAR(G_S32(0x3f9b0));
                    AT_S32(c, 0xf4) = 1;
                    AT_S32(c, 0x11c) += 5;
                    for (int k = 0; k < n; k++)
                        AT_S32(CAR(k), 0xf4) = 1;
                }
            }
            int nl = G_S32(0x3f948);
            if (nl >= 2) {
                /* all checkpoints 1..nl-1 passed? */
                int all = 1;
                for (int k = 1; k < nl; k++)
                    all = all ? ((int *)G(0x45e38))[k] != 0 : 0;
                if (all != 1)
                    goto resolve;
            }
            end = 1;
            if (G_S32(0x3f9b0) >= 0) {
                uint8_t *c = CAR(G_S32(0x3f9b0));
                AT_S32(c, 0x11c) += (int)((lim - t) / 1000.0f) + 5;
                for (int k = 0; k < n; k++)
                    AT_S32(CAR(k), 0xf4) = 1;
            }
            goto set_end;
        }
    }

resolve:
    if (done || timeout)
        end = 1;
    else if (!(G_U8(0x3f928) & 1))
        end = 0;
    else if (AT_S32(CAR(0), 0x8c) > 0)
        end = 1;
    else
        end = G_U8(0x5c208) != 0;

set_end:
    G_U8(0x3f8d4) = (uint8_t)end;
    if ((G_U8(0x3f928) & 1) && (AT_S32(CAR(0), 0x8c) > 0 || G_U8(0x5c208) != 0)) {
        G_U8(0x3f8d4) = 1;
        G_S32(0x3fee8) = 300;
    } else if (!end) {
        return;
    }
    if (G_S32(0x3f9dc) != G_S32(0x3ebe4)) {
        for (int i = 0; i < G_S32(0x3e9c8); i++) {
            int place = AT_S32(CAR(i), 0xf4);
            if (place < 1)
                place = AT_S32(CAR(i), 0x9c);
            fn_0003517c(i, place);
        }
    }
}

/* Car-to-car collisions (asm 0x8228..0x890e): push apart, exchange velocities, steering kick,
 * crash sound, plus the chase/tag rules applied on contact. */
static void tick_race_collisions(void)
{
    for (int i = 0; i < G_S32(0x3e9c8); i++) {
        TICK_CAR_PHYSICS(i);
        uint8_t *ci = CAR(i);
        for (int j = i + 1; j < G_S32(0x3e9c8); j++) {
            uint8_t *cj = CAR(j);
            if (!(fn_00037fa4(FP(ci, 0x2bc), FP(cj, 0x2bc)) < 105.0f))
                continue;
            if (AT_S32(ci, 0xe8) > 0 || AT_S32(cj, 0xe8) > 0)
                continue;   /* one of them is respawning */

            float n[3], nn[3], tmp[3], e[3], vi[3], vj[3], add[3];
            n[0] = AT_F32(ci, 0x2bc) - AT_F32(cj, 0x2bc);
            n[1] = AT_F32(ci, 0x2c0) - AT_F32(cj, 0x2c0);
            n[2] = AT_F32(ci, 0x2c4) - AT_F32(cj, 0x2c4);
            if (fn_00037e54(n) == 0.0f)
                n[0] = n[1] = n[2] = 50.0f;
            nn[0] = -n[0];
            nn[1] = -n[1];
            nn[2] = -n[2];
            fn_000379a8(n, n);
            fn_000379a8(nn, nn);
            /* the result is not used */
            fn_00038a20(atan2f(n[0], n[2]) * 57.2958f - AT_F32(ci, 0x244));
            fn_00037dec(FP(ci, 0x2f8), FP(ci, 0x2f8), tmp);   /* tmp = 2 * vel_i */
            float dv = fabsf(AT_F32(ci, 0x254) - AT_F32(cj, 0x254)) / 10.0f;

            /* crash sound, positioned at the human player's car */
            int np = G_S32(0x3e9b0);
            if ((double)dv > 0.1 && (np > i || np > j)) {
                float a = 0.0f, b = 0.0f;
                if (np > i)
                    fn_0001e804(AT_F32(ci, 0x2bc), AT_F32(ci, 0x2c0), AT_F32(ci, 0x2c4), i, &a, &b);
                else if (np > j)
                    fn_0001e804(AT_F32(cj, 0x2bc), AT_F32(cj, 0x2c0), AT_F32(cj, 0x2c4), j, &a, &b);
                fn_0003613c(G_S32(0x3ebd8), (int)(a * 18000.0f), (int)(b * 18000.0f));
                fn_000360bc(G_S32(0x3ebd8));
            }

            /* steering kick from the side of the impact (+0x70: car cannot be moved) */
            fn_00037c28(nn, tmp, e);
            if (AT_S32(cj, 0x70) == 0)
                AT_F32(cj, 0x24c) -= (dv + 0.02f) * e[1] / (AT_F32(cj, 0x254) / 10.0f + 20.0f);
            fn_00037c28(n, tmp, e);
            if (AT_S32(ci, 0x70) == 0)
                AT_F32(ci, 0x24c) -= (dv + 0.02f) * e[1] / (AT_F32(ci, 0x254) / 10.0f + 20.0f);

            /* exchange velocities */
            fn_00037e24(FP(ci, 0x2f8), vi, 0.7f);
            fn_00037e24(FP(cj, 0x2f8), vj, 0.7f);
            fn_00037e24(tmp, add, 0.15f);
            fn_00037dec(vi, add, vi);
            fn_00037dec(vj, add, vj);
            if (AT_S32(ci, 0x70) == 0)
                memcpy(FP(ci, 0x2f8), vj, 12);
            if (AT_S32(cj, 0x70) == 0)
                memcpy(FP(cj, 0x2f8), vi, 12);
            if (AT_S32(ci, 0x70) == 0)
                AT_S32(ci, 0x260) = 0;
            if (AT_S32(cj, 0x70) == 0)
                AT_S32(cj, 0x260) = 0;
            if (AT_S32(ci, 0x70) == 0) {
                AT_F32(ci, 0x264) = fn_00037e80(FP(ci, 0x2f8), FP(ci, 0x2e0));
                AT_F32(ci, 0x268) = fn_00037e80(FP(ci, 0x2f8), FP(ci, 0x2ec));
            }
            if (AT_S32(cj, 0x70) == 0) {
                AT_F32(cj, 0x264) = fn_00037e80(FP(cj, 0x2f8), FP(cj, 0x2e0));
                AT_F32(cj, 0x268) = fn_00037e80(FP(cj, 0x2f8), FP(cj, 0x2ec));
            }

            /* push apart (the original loops forever when both cars are immovable) */
            while (fn_00037fa4(FP(ci, 0x2bc), FP(cj, 0x2bc)) < 105.0f) {
                if (AT_S32(ci, 0x70) == 0)
                    fn_00037dec(FP(ci, 0x2bc), n, FP(ci, 0x2bc));
                if (AT_S32(cj, 0x70) == 0)
                    fn_00037dec(FP(cj, 0x2bc), nn, FP(cj, 0x2bc));
            }

            int mode = G_S32(0x3f9dc);
            if (mode == G_S32(0x3ebe0)) {
                /* chase: hitting the target car G_S32(0x3f9b0) */
                int target = G_S32(0x3f9b0);
                if (i == target && G_S32(0x3f9d0) > j) {
                    if ((G_S32(0x3f9ac) < 1 || j != G_S32(0x3f9b4)) &&
                        (AT_S32(cj, 0x9c) > AT_S32(ci, 0x9c) || AT_F32(ci, 0x254) < 5.0f))
                        fn_00010d70(j, 1, -1);
                    G_S32(0x3f9ac) = 30;
                } else if (j == target && G_S32(0x3f9d0) > i) {
                    if ((G_S32(0x3f9ac) < 1 || i != G_S32(0x3f9b4)) &&
                        (AT_S32(ci, 0x9c) > AT_S32(cj, 0x9c) || AT_F32(cj, 0x254) < 5.0f))
                        fn_00010d70(i, 1, -1);
                    G_S32(0x3f9ac) = 30;
                }
                continue;
            }
            if (mode != G_S32(0x3ebe4) || AT_S32(ci, 0xec) != 0)
                continue;

            /* tag: +0x108 chaser, +0x10c runner, +0x114 tag points, +0xec tag cooldown,
             * +0x118 last tagger, +0x11c score */
            bool try_a;
            if (AT_S32(ci, 0x108) != 0 && AT_S32(cj, 0x10c) != 0) {
                try_a = true;
            } else {
                if (AT_S32(cj, 0x108) == 0 || AT_S32(ci, 0x10c) == 0)
                    continue;
                try_a = AT_S32(ci, 0x108) != 0;
            }
            if (try_a) {
                /* i (chaser) tags j */
                bool soft = false;
                if (!(AT_S32(ci, 0x9c) > AT_S32(cj, 0x9c)))
                    soft = AT_F32(cj, 0x254) >= 1.0f;
                if (((double)dv > 0.1 && !soft) || ((double)dv > 0.01 && AT_S32(ci, 0x30) == -1)) {
                    int lim = G_S32(0x3e9d8);
                    if (AT_S32(cj, 0x114) < lim && AT_S32(cj, 0xf8) == 0) {
                        float pts = dv * 10.0f + 5.0f;
                        AT_S32(ci, 0x118) = j;
                        AT_S32(cj, 0xec) = 40;
                        int v = (int)((float)AT_S32(cj, 0x114) + pts);
                        AT_S32(cj, 0x114) = v;
                        AT_S32(ci, 0x11c) = (int)((float)AT_S32(ci, 0x11c) + pts);
                        if (v >= lim)
                            AT_S32(cj, 0x114) = lim;
                    }
                }
            }
            if (AT_S32(cj, 0x108) != 0) {
                /* j (chaser) tags i */
                bool soft = false;
                if (!(AT_S32(cj, 0x9c) > AT_S32(ci, 0x9c)))
                    soft = AT_F32(ci, 0x254) >= 1.0f;
                if (((double)dv > 0.1 && !soft) || ((double)dv > 0.01 && AT_S32(ci, 0x30) == -1)) {
                    int lim = G_S32(0x3e9d8);
                    if (AT_S32(ci, 0x114) < lim && AT_S32(ci, 0xf8) == 0) {
                        float pts = dv * 10.0f + 5.0f;
                        AT_S32(cj, 0x118) = i;
                        AT_S32(ci, 0xec) = 40;
                        int v = (int)((float)AT_S32(ci, 0x114) + pts);
                        AT_S32(ci, 0x114) = v;
                        AT_S32(cj, 0x11c) = (int)((float)AT_S32(cj, 0x11c) + pts);
                        if (v >= lim)
                            AT_S32(ci, 0x114) = lim;
                    }
                }
            }
        }
    }
}

/* Race positions (asm 0x8918..0x8cd2): +0x9c place (1-based), order table 0x476c4,
 * G_S32(0x3f9d4) = car the camera/arrow points at. */
static void tick_race_ranking(void)
{
    int mode = G_S32(0x3f9dc);
    int ebe0 = G_S32(0x3ebe0), ebe4 = G_S32(0x3ebe4);

    if (mode == ebe0) {
        /* chase: score table 0x47704 sorted by +0x11c */
        int n = G_S32(0x3e9ac);
        for (int p = 0; p < n; p++) {
            int s = AT_S32(CAR(p), 0x11c), r = 0;
            for (int q = 0; q < n; q++) {
                if (q == p)
                    continue;
                int t = AT_S32(CAR(q), 0x11c);
                if (t > s)
                    r++;
                else if (t == s)
                    r += q > p;
            }
            ((int *)G(0x47704))[r] = p;
        }
        G_S32(0x3f9d4) = G_S32(0x3f9b0);
    }

    int count = (mode == ebe4 || mode == ebe0 || mode == G_S32(0x3ebec)) ? G_S32(0x3e9ac)
                                                                        : G_S32(0x3e9c8);
    const float *pts = (const float *)G(0x45640);   /* track centre points (x, z) */
    for (int k = 0; k < count; k++) {
        uint8_t *ck = CAR(k);
        int nl = G_S32(0x3f948);
        int pk = nl * AT_S32(ck, 0x90) + AT_S32(ck, 0x84) + ((unsigned)(AT_S32(ck, 0x64) - 7) < 2);
        int seg = AT_S32(ck, 0x84);
        int rank = 0;
        for (int m = 0; m < count; m++) {
            if (m == k)
                continue;
            uint8_t *cm = CAR(m);
            nl = G_S32(0x3f948);
            int pm = nl * AT_S32(cm, 0x90) + AT_S32(cm, 0x84) + ((unsigned)(AT_S32(cm, 0x64) - 7) < 2);
            if (pm > pk) {
                rank++;
            } else if (pm == pk) {
                /* same segment: compare the distance to the middle of the next segment */
                int nseg = seg + 1 <= nl ? seg + 1 : seg + 1 - nl;
                float mid[3];
                mid[0] = (pts[seg * 2] * 812.0f + pts[nseg * 2] * 812.0f) * 0.5f;
                mid[1] = 0.0f;
                mid[2] = (pts[seg * 2 + 1] * 812.0f + pts[nseg * 2 + 1] * 812.0f) * 0.5f;
                float dk = fn_00037fa4(FP(ck, 0x2b0), mid);
                float dm = fn_00037fa4(FP(cm, 0x2b0), mid);
                if (AT_S32(ck, 0x10c) != 0)
                    dk += -30.0f;
                else if (G_S32(0x3f9dc) == G_S32(0x3ebe0) && AT_S32(ck, 0x104) != 0)
                    dk += -30.0f;
                if (AT_S32(cm, 0x10c) != 0)
                    dm += -30.0f;
                else if (G_S32(0x3f9dc) == G_S32(0x3ebe0) && AT_S32(cm, 0x104) != 0)
                    dm += -30.0f;
                if (dk > dm)
                    rank++;
                seg = nseg;   /* as in the original: the segment advances after every tie */
            }
        }
        AT_S32(ck, 0x9c) = rank + 1;
        ((int *)G(0x476c4))[rank] = k;
    }

    mode = G_S32(0x3f9dc);
    if (mode == G_S32(0x3ebe4)) {
        /* tag: point at the best placed runner still in the game */
        int n = G_S32(0x3e9c8);
        for (int r = 0; r < n; r++) {
            int c = ((int *)G(0x476c4))[r];
            if (AT_S32(CAR(c), 0x10c) != 0 && AT_S32(CAR(c), 0x114) < G_S32(0x3e9d8)) {
                G_S32(0x3f9d4) = c;
                break;
            }
        }
    } else if (mode == G_S32(0x3ebe0)) {
        G_S32(0x3f9d4) = G_S32(0x3f9b0);
    }
}

/* Engine sounds and the tag-mode chaser sirens (asm 0x8cf4..0x8eec). */
static void tick_race_sound(void)
{
    if (!(G_U8(0x3f9e0) & 1)) {
        int np = G_S32(0x3e9b0);
        if (np == 1) {
            int car = *(int *)G(0x46ea0);   /* car followed by the camera of player 0 */
            float a = 0.0f, b = 0.0f;
            if (np > car) {
                tick_engine_snd(car, 0, 0.22f, 0.22f);
            } else {
                fn_0001e804(AT_F32(CAR(car), 0x2bc), AT_F32(CAR(car), 0x2c0), AT_F32(CAR(car), 0x2c4),
                            0, &a, &b);
                tick_engine_snd(car, 0, a, b);
            }
        } else if (np >= 1) {
            /* split screen: left/right channel per player */
            for (int i = 0; i < G_S32(0x3e9b0); i++) {
                int nnp = G_S32(0x3e9b0);
                float x = (float)(1 - (i + 1) % 2);   /* 0 for odd players, 1 for even */
                float y = 1.0f - x;
                if (nnp == 2)
                    tick_engine_snd(i, i, x * 0.5f, y * 0.5f);
                else
                    tick_engine_snd(i, i, x / 1.5f, y / 1.5f);
            }
        }
    }

    if (G_S32(0x3f9dc) == G_S32(0x3ebe4)) {
        for (int p = 0; p < G_S32(0x3e9b0); p++) {
            const int *cars = (const int *)G(0x46ea0) + p * 16;
            int snd = ((int *)G(0x46e78))[p];
            for (int k = 0; k < G_S32(0x3e9c8); k++) {
                uint8_t *c = CAR(cars[k]);
                if (AT_S32(c, 0x108) == 0)
                    continue;
                float a = 0.0f, b = 0.0f;
                fn_0001e804(AT_F32(c, 0x2bc), AT_F32(c, 0x2c0), AT_F32(c, 0x2c4), p, &a, &b);
                float v = (float)(10000 / G_S32(0x3e9b0));
                fn_0003613c(snd, (int)(v * b), (int)(v * a));
                fn_00036174(snd, 1.0f);
                break;
            }
        }
    }
}

/* Particle effects (35 entries of 0x48 bytes from 0x5c160+0x5a4): +0x5a4 y, +0x5b0 base y,
 * +0x5c0 x drift, +0x5c4..+0x5cc scale, +0x5d0 age (-1 = free), +0x5d4 size, +0x5d8 type
 * (0x7e smoke, 0x85 splash), +0x5e4 alpha. */
static void tick_race_effects(void)
{
    float a = G_F32(0x3ea18);   /* rain / snow intensities of the weather below */
    float b = G_F32(0x3ea0c);
    for (int k = 0; k < 35; k++) {
        uint8_t *e = (uint8_t *)G(0x5c160) + k * 0x48;
        int type = AT_S32(e, 0x5d8);
        int alpha;
        if (type == 0x85) {
            float t = AT_F32(e, 0x5d0);
            if (!(t > -1.0f))
                continue;
            t += 10.0f;
            AT_F32(e, 0x5d0) = t;
            if (t > 180.0f) {
                AT_F32(e, 0x5d0) = -1.0f;
                continue;
            }
            float s = AT_F32(e, 0x5d4) * (sinf(t * 0.0174533f) + 1.0f);
            AT_F32(e, 0x5c4) = s;
            AT_F32(e, 0x5c8) = s;
            AT_F32(e, 0x5cc) = s;
            AT_F32(e, 0x5a4) = AT_F32(e, 0x5b0) + s;
            if (a + b > 0.0f)
                alpha = (int)(a * 10.0f + b + 50.0f + t * -0.5f);
            else
                alpha = (int)(t * -5.0f + 200.0f);
        } else if (type == 0x7e) {
            float t = AT_F32(e, 0x5d0);
            if (!(t > -1.0f))
                continue;
            t += 0.1f;
            AT_F32(e, 0x5d0) = t;
            if (t > 15.0f) {
                AT_F32(e, 0x5d0) = -1.0f;
                continue;
            }
            double side = (double)(k % 2) + -0.5;
            float dir = side < 0 ? -1.0f : (float)(side > 0);
            alpha = (int)(t * -7.0f + 100.0f);
            AT_F32(e, 0x5a4) += 0.1f;
            AT_F32(e, 0x5c0) += dir;
            float s = (t + 2.0f) * AT_F32(e, 0x5d4);
            AT_F32(e, 0x5c4) = s;
            AT_F32(e, 0x5c8) = s;
            AT_F32(e, 0x5cc) = s;
        } else {
            continue;
        }
        AT_S32(e, 0x5e4) = alpha < 0 ? 0 : (alpha & 0xff);
    }
}

/* Weather of the track (G_S32(0x3f8b8)): each kind ramps an intensity between 0 and its maximum,
 * holds it for a while (counter), then ramps back. */
static void tick_weather_ramp(int cnt, int val, int step, float max, float up, float down)
{
    if (G_S32(cnt) >= 1) {
        G_S32(cnt)--;
        return;
    }
    float v = G_F32(val) + G_F32(step);
    G_F32(val) = v;
    if (v > max) {
        G_S32(cnt) = 1000;
        G_F32(step) = down;
        G_F32(val) = max;
    } else if (!(v > 0.0f)) {
        G_S32(cnt) = 600;
        G_F32(step) = up;
        G_F32(val) = 0.0f;
    }
}

static void tick_race_weather(void)
{
    switch (G_S32(0x3f8b8)) {
    case 1: {
        /* fog: 0x3ea00 density, fog start 0x47f94 / 0x47254 */
        float v;
        if (G_S32(0x3e9fc) >= 1) {
            G_S32(0x3e9fc)--;
            break;
        }
        v = G_F32(0x3ea00) + G_F32(0x3ea04);
        G_F32(0x3ea00) = v;
        if (v > 9.0f) {
            v = 9.0f;
            G_S32(0x3e9fc) = 4200;
            G_F32(0x3ea04) = 0.0f;
            G_F32(0x3ea00) = 9.0f;
        } else if (!(v > 0.0f)) {
            G_S32(0x3e9fc) = 4200;
            G_F32(0x3ea04) = 0.0125f;
            G_F32(0x3ea00) = 0.0f;
            v = 0.0f;
        }
        if (v > 5.0f) {
            if (G_F32(0x47f94) != -2500.0f) {
                G_F32(0x47f94) = -2500.0f;
                G_F32(0x47254) = -2500.0f;
            }
        } else if (G_F32(0x47f94) != -1250.0f) {
            G_F32(0x47f94) = -1250.0f;
            G_F32(0x47254) = -1250.0f;
        }
        break;
    }
    case 2:
        tick_weather_ramp(0x3e9f0, 0x3e9f4, 0x3e9f8, 10.0f, 0.1f, -0.1f);
        break;
    case 3:
        /* storm: 0x3ea0c rain, G_S32(0x5c164) lightning flash timer */
        if (G_S32(0x3ea08) >= 1) {
            G_S32(0x3ea08)--;
            break;
        }
        if (G_S32(0x5c164) >= 1)
            G_S32(0x5c164)--;
        if (G_F32(0x3ea0c) > 5.0f && fn_00004400(50) == 0)
            G_S32(0x5c164) = 25;
        tick_weather_ramp(0x3ea08, 0x3ea0c, 0x3ea10, 10.0f, 0.05f, -0.05f);
        break;
    case 4:
        tick_weather_ramp(0x3ea14, 0x3ea18, 0x3ea1c, 10.0f, 0.05f, -0.05f);
        break;
    default:
        break;
    }
}

/* Viewports and the slots of the race HUD/minimap (asm 0x80c8..0x8220). */
static void tick_race_views(void)
{
    int np;
    if (G_U8(0x3f98c)) {
        G_S32(0x5c568) = 4;
        G_S32(0x5c5e8) = 4;
        G_S32(0x5c668) = 4;
        G_S32(0x5c6e8) = 4;
    }
    np = G_S32(0x3e9b0);
    if (G_U8(0x3f928) & 1) {
        G_S32(0x5c568) = 4;
        G_S32(0x3e9c0) = np == 1 ? 2 : (np >= 3 ? 4 : np);
    } else if (np == 3) {
        G_S32(0x5c6e8) = 4;
        G_S32(0x3e9c0) = 4;
    } else if (np == 1) {
        G_S32(0x5c5e8) = 8;
        G_S32(0x5c5f0) = 0;
        G_S32(0x3e9c0) = 2;
    } else {
        G_S32(0x3e9c0) = np >= 3 ? 4 : np;
    }

    if (G_S32(0x3f8e0) != 0) {
        /* free the 4 slots (0x44590+0x14, 0x34 bytes each) no car refers to */
        int n = G_S32(0x3e9c8);
        uint8_t *s = (uint8_t *)G(0x44590) + 0x14;
        for (int k = 0; k < 4; k++, s += 0x34) {
            int used = 0;
            for (int c = 0; c < n; c++)
                used |= AT_S32(CAR(c), 0x78) == k;
            if (used)
                continue;
            AT_S32(s, 0) = -1;
            AT_S32(s, 4) = -1;
            int c = AT_S32(s, 8);
            if (c >= 0 && AT_S32(CAR(c), 0x74) != k)
                AT_S32(s, 8) = -1;
        }
    }
}

/* One frame of the running race (asm 0x73a8..0x94c4). */
static void tick_race_frame(void)
{
    /* top speed drifts between 1600 and 3000 (G_S32(0x3e920): something slows the game) */
    float spd = G_F32(0x3e9c4);
    if (G_S32(0x3e920) != 0) {
        if (spd > 1600.0f)
            G_F32(0x3e9c4) = spd + spd / -100.0f;
    } else if (spd < 3000.0f) {
        G_F32(0x3e9c4) = spd + spd / 200.0f;
    }

    int seed = G_S32(0x3f988) + 1;
    if (seed > 0x30cbf)
        seed = 0x30cbf;
    G_S32(0x3f988) = seed;   /* frames since the race started (capped) */
    srand(seed);

    G_F32(0x3f8ec) = (float)(G_S32(0x3f8e4)++) * 16.5f;   /* race clock in ms */
    fn_0003485c();

    /* every 180 frames: read back the pixel under the touch point (white = HUD button) */
    int a = G_S32(0x5c16c);
    G_S32(0x5c16c) = a + 2;
    if (a + 2 > 360) {
        G_S32(0x5c16c) = a - 358;
        int px = G_S32(0x5c55c), py = G_S32(0x5c560);
        int w = G_S32(0x40368), h = G_S32(0x40374);
        if (px >= 1 && px < w && py >= 1 && py < h) {
            float sw = (float)G_S32(0x3f854), sh = (float)G_S32(0x3f858);
            int x = (int)(G_F32(0x5c188) * sw / (float)w + (float)px * (G_F32(0x5c1a8) * sw / (float)w));
            int y = (int)(G_F32(0x5c198) * sh / (float)h + (float)py * (G_F32(0x5c1b8) * sh / (float)h));
            uint32_t pix = 0;
            glReadPixels(x, G_S32(0x3f858) - y, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, &pix);
            unsigned r = pix & 0xff, g = (pix >> 8) & 0xff, b = (pix >> 16) & 0xff;
            G_U8(0x5c57c) = r >= 0x9c && g >= 0x9c && b >= 0x9c && r == g && g == b;
        }
    }

    if (++G_S32(0x5c184) >= 3) {
        G_S32(0x5c184) = 0;
        fn_000371d0(G(0x48244), 0.0f, 0.0f, 0.0f);
        /* the original also copies 4 u16 pairs of this model into a local it never reads */
        (void)fn_00038818(((int *)G(0x47770))[8]);
    }
    int ang = G_S32(0x5c180) + 10;
    G_S32(0x5c180) = ang > 360 ? 0 : ang;

    tick_race_scenery();

    if (G_S32(0x3f9dc) == G_S32(0x3ebe0)) {
        /* chase: the target car's beacon */
        int target = G_S32(0x3f9b0);
        uint8_t *c = CAR(target);   /* may be car -1: only the address is formed then */
        float v = AT_F32(c, 0x254);
        float r = G_F32(0x3f9a0) + (v + 5.0f);
        if (r > 360.0f)
            r += -360.0f;
        G_F32(0x3f9a0) = r;
        G_F32(0x3f9a4) = 10.0f / (v / 10.0f + 1.0f) + 2.0f;
        if (G_S32(0x3f9ac) >= 1)
            G_S32(0x3f9ac)--;
        if (target == -1) {
            for (int j = 0; j < G_S32(0x3e9ac); j++)
                if (fn_00037fa4(FP(CAR(j), 0x2bc), (float *)G(0x47244)) < 100.0f)
                    fn_00010d70(j, 1, -1);   /* picked up the target marker */
        }
    }
    fn_00034a98();
    float rot = G_F32(0x3f9a8) + 10.0f;
    if (rot > 360.0f)
        rot += -360.0f;
    G_F32(0x3f9a8) = rot;
    fn_000348d8();

    tick_race_end_check();
    tick_race_views();
    if (G_S32(0x3e9c8) >= 1)
        tick_race_collisions();
    tick_race_ranking();

    for (int i = 0; i < G_S32(0x3e9c0); i++)
        fn_0002c2d8(i);
    tick_race_sound();
    fn_0002bf2c();
    tick_race_effects();
    tick_race_weather();

    if (G_S32(0x3f988) >= 0x30c15 && G_U8(0x3f98c) != 0)
        G_U8(0x3f8d4) = 1;   /* demo/attract race: stop after ~200000 frames */
    if (G_U8(0x3f8d4) & 1) {
        /* race over: wait 300 frames, fade out, then leave */
        int t = ++G_S32(0x3fee8);
        if (t > 300) {
            if (t == 301) {
                G_S32(0x3f8f0) = 0;
                G_S32(0x3f8f4) = 0xff;
            } else if (G_S32(0x3f8f0) == 0xff) {
                G_U8(0x3fee4) = 1;
            }
        }
    }
}

/* Paused: the pause menu buttons of each player (input state at 0x5c160 + 0xa9 + p*0x24:
 * +0 quit, +1 resume). */
static void tick_race_pause(void)
{
    int np = G_S32(0x3e9b0);
    if (np < 1)
        return;
    uint8_t flag = G_U8(0x3f810);
    int e9cc = G_S32(0x3e9cc);
    uint8_t demo = G_U8(0x3f98c);
    for (int p = 0; p < np; p++) {
        const uint8_t *in = (const uint8_t *)G(0x5c160) + 0xa9 + p * 0x24;
        if (in[0] != 0 && !(flag & 1)) {
            /* quit the race */
            G_U8(0x3f8d4) = 1;
            G_S32(0x3fee8) = 300;
            if (demo == 0) {
                G_S32(0x3f8b4) = e9cc;
                G_U8(0x3f9e4) = 1;
            }
            flag = 0;
            G_U8(0x3f884) = 0;
            G_U8(0x3f9e0) = 0;
        }
        if (in[1] != 0) {
            /* resume */
            flag = 0;
            G_U8(0x3f884) = 0;
            G_U8(0x3f9e0) = 0;
        }
    }
    G_U8(0x3f810) = flag;
}

/* Leaving the race (G_U8(0x3fee4) set): stop the sounds and go to the results. */
static void tick_race_leave(void)
{
    memset(G(0x45e38), 0, 0x3fc);
    G_U8(0x3fab4) = 0;
    G_U8(0x3f990) = 0;
    if (G_S32(0x3e9b0) >= 3)
        fn_0000ff60(0x97, "/pics/font_3_b.png", 0);
    for (int k = 0; k < 4; k++) {
        fn_000360e0(((int *)G(0x46e40))[k]);
        fn_000360e0(((int *)G(0x46e88))[k]);
    }
    if (G_S32(0x3f9dc) == G_S32(0x3ebe4))
        for (int p = 0; p < G_S32(0x3e9b0); p++)
            fn_000360e0(((int *)G(0x46e78))[p]);
    int n = G_S32(0x3e9c8);
    for (int i = 0; i < n; i++)
        ((int *)G(0x476c4))[AT_S32(CAR(i), 0xf4) - 1] = i;   /* order by finishing place */

    G_U8(0x3f890) = !(G_U8(0x3f9e4) & 1);
    G_U8(0x3f908) = 0;
    int race = G_S32(0x3f8b4);
    if (G_S32(0x3f848) != 8)
        G_S32(0x3f8b4) = ++race;
    int last = race >= G_S32(0x3e9cc);   /* championship finished */
    G_U8(0x3fac0) = (uint8_t)last;
    G_U8(0x3fabc) = 1;
    G_S32(0x3fac4) = last ? !(G_U8(0x3f9e4) & 1) : 0;
    G_U8(0x3f81c) = 0;
    fn_000209e8();
    if (G_S32(0x3f848) == 2) {
        G_S32(0x3fac4) = 0;
        G_S32(0x3f8b4) = 0;
        fn_0002c13c(2, 0);
    }
}

/* State 7: the race. Returns false when the tick must return without the music update. */
static bool tick_race(void)
{
    int target = (G_S32(0x3e9e0) == 0 && !(G_U8(0x3f928) & 1)) ? 0 : 13000;
    fn_000302d0(TICK_MUSIC(), target);
    fn_00009dc0(0);
    int paused = fn_00004540() & 0xff;
    if (G_U8(0x3f9e0) & 1)
        tick_race_pause();
    else if (!paused)
        tick_race_frame();
    fn_00020f08();
    if (!(G_U8(0x3fee4) & 1))
        return false;
    tick_race_leave();
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * State 8: race results / podium. The camera (0x3fa34, 0x3fa38) glides toward the target
 * (0x4710c, 0x47110), bobbing; leaving fades the music out.
 * --------------------------------------------------------------------------------------------- */
static bool tick_results(void)
{
    fn_00009dc0(0);
    if ((fn_00004540() & 0xff) == 0) {
        if (G_U8(0x3f908) & 1) {
            G_U8(0x3f908) = 0;
            fn_0002c13c(2, 1);
        }
        G_F32(0x3f8ec) = (float)fn_00004208();
        fn_00034a98();
        float rot = G_F32(0x3f9a8) + 10.0f;
        if (rot > 360.0f)
            rot += -360.0f;
        G_F32(0x3f9a8) = rot;
        fn_000348d8();
        fn_0003485c();

        float d[3], dn[3];
        d[0] = G_F32(0x4710c) - G_F32(0x3fa34);
        d[2] = G_F32(0x47110) - G_F32(0x3fa38);
        G_S32(0x471d0) = d[0] < 0.0f ? -1 : (d[0] > 0.0f);
        G_S32(0x471d4) = d[2] < 0.0f ? -1 : (d[2] > 0.0f);
        d[1] = 0.0f;
        if (fn_00037e54(d) > 1.0f) {
            float f = fabsf(G_F32(0x3fa34) - G_F32(0x3fa3c)) /
                      (fabsf(G_F32(0x4710c) - G_F32(0x3fa3c)) + 1.0f);
            G_F32(0x47540) = sinf(f * 180.0f * 0.0174533f) * 25.0f;   /* camera height bob */
            float div = (G_U8(0x3f90c) & 1) ? 200.0f : 400.0f;
            float sx = (G_F32(0x4710c) - G_F32(0x3fa3c)) / div;
            float sz = (G_F32(0x47110) - G_F32(0x3fa40)) / div;
            fn_000379a8(d, dn);
            G_F32(0x3fa48) = atan2f(dn[0], dn[2]) * 57.2958f;   /* camera heading */
            G_F32(0x3fa34) += sx;
            G_F32(0x3fa38) += sz;
        }

        /* screen fade, like the menus but twice as fast */
        if (G_S32(0x3fa28) == G_S32(0x3fa24)) {
            if (G_F32(0x3fa14) < 255.0f) {
                G_F32(0x3fa14) += 20.0f;
                if (G_F32(0x3fa14) >= 255.0f)
                    G_F32(0x3fa14) = 255.0f;
            }
        } else if (G_S32(0x3fa2c) >= 1) {
            G_S32(0x3fa2c)--;
        } else {
            G_F32(0x3fa14) += -20.0f;
            if (!(G_F32(0x3fa14) > 0.0f)) {
                G_S32(0x3fa24) = G_S32(0x3fa28);
                G_F32(0x3fa14) = 0.0f;
                G_F32(0x3fa18) = 200.0f;
            }
        }

        if (G_S32(0x3fac4) != 0)
            G_S32(0x5c568) = 11;
        else
            G_S32(0x5c568) = G_U8(0x3fabc) ? 10 : 7;
        fn_0002c2d8(0);
        fn_0002bf2c();
        if (!(G_U8(0x4776c) & 1) && (G_U8(0x3f904) & 1)) {
            G_U8(0x4776c) = 1;
            TICK_CAR_PHYSICS(0);
            fn_0000cc3c();
        }
        G_U8(0x3f924) = 1;
        fn_00030bf4();
        fn_00030318();

        /* leave the results: fade out (0x3f8f0 -> 0x3f8f4) */
        if ((G_U8(0x3f898) & 1) && !(G_U8(0x3fee0) & 1)) {
            if (((int *)G(0x481f4))[G_S32(0x3f8b0)] != 0 && (G_U8(0x3fa74) & 1)) {
                G_S32(0x3f8f0) = 0;
                G_U8(0x3fee0) = 1;
                G_S32(0x3f8f4) = 0xff;
            }
            if (G_U8(0x3f928) & 1) {
                G_S32(0x3f8f0) = 0;
                G_U8(0x3fee0) = 1;
                G_S32(0x3f8f4) = 0xff;
            }
            if (G_U8(0x3f88c) & 1) {
                G_S32(0x3f8f0) = 0;
                G_U8(0x3fee0) = 1;
                G_S32(0x3f8f4) = 0xff;
            }
            if (G_U8(0x3fabc) == 0) {
                G_U8(0x3fee0) = 1;
                G_S32(0x3f8f0) = 0;
                G_S32(0x3f8f4) = 0xff;
            }
        }
        if (G_U8(0x3fee0) & 1) {
            fn_000302d0(TICK_MUSIC(), 0);
            G_U8(0x3fee4) = G_S32(0x3f8f0) == 0xff;
        }
    }
    fn_00020f08();
    if (!(G_U8(0x3fee4) & 1))
        return false;
    G_U8(0x3fabc) = 0;
    G_S32(0x3fac4) = 0;
    G_U8(0x3f900) = 1;
    G_U8(0x3f904) = 0;
    G_U8(0x3f90c) = 0;
    G_U8(0x3fac0) = 0;
    fn_0000fe34();
    return true;
}

/* State 9: a stand-alone screen G_S32(0x3f844) (left/right pages through the screens);
 * leaving returns to the state saved in G_S32(0x3f850). */
static void tick_screen(void)
{
    fn_00009dc0(0);
    int keys = fn_0000a184(0);
    if (keys & 0x10)
        G_U8(0x3fee4) = 1;
    if (keys & 0x100)
        fn_0000a370(G_S32(0x3f844) + 1);
    if (keys & 0x200)
        fn_0000a370(G_S32(0x3f844) - 1);
    fn_0002bf2c();
    fn_00020f08();
    if (!(G_U8(0x3fee4) & 1))
        return;
    fn_000209e8();
    fn_0000a40c();
    G_U8(0x3fee4) = 0;
    G_U8(0x47250) = 0;
    G_S32(0x3f848) = G_S32(0x3f84c);
    G_S32(0x3f840) = G_S32(0x3f850);
    fn_000209e8();
    fn_000045a4();
}

/* ---------------------------------------------------------------------------------------------
 * FUN_00005ac4: the game tick, called once per frame. render: stored in G_U8(0x3e914).
 * --------------------------------------------------------------------------------------------- */
void fn_00005ac4(int render)
{
    const float *accel = (const float *)G(0x42754);   /* filtered accelerometer (x, y, z) */
    G_U8(0x3e914) = (uint8_t)render;

    /* steering from the device tilt, centred on the calibration G_F32(0x3f828) */
    float tilt = accel[1] + G_F32(0x3f828) * -0.5f;
    float sgn = tilt < 0.0f ? -1.0f : (float)(tilt > 0.0f);
    float steer = (fabsf(tilt) + -2.0f) * sgn * 1.2f;
    G_F32(0x3f824) = steer;
    float c = cosf(accel[2] * 0.0174533f);
    /* 0x3f820: the "Tilting Horizon" roll (camera and sky), smoothed. On the iPhone every turn
     * was a tilt of the device. Port: only while the console itself is tilted to steer; with
     * the Circle Pad or the D-pad the screen doesn't turn, so the horizon stays level. */
    float roll = controls_tilting() ? steer * c / 3.0f : 0.0f;
    float old = G_F32(0x3f820);
    G_F32(0x3f820) = old + (roll - old) / 3.0f;

    switch (G_S32(0x3f840)) {
    case 0:
        tick_boot();
        break;
    case 6:
        if (!tick_menus())
            return;
        break;
    case 7:
        if (!tick_race())
            return;
        break;
    case 8:
        if (!tick_results())
            return;
        break;
    case 9:
        tick_screen();
        return;
    default:   /* 1..5 and anything else: nothing to do */
        return;
    }
    fn_0000a40c();
}

/* ---------------------------------------------------------------------------------------------
 * FUN_00009dc0: per-frame input of player `pl` (always 0 on iOS), with race recording/replay.
 * The input block of player p is at 0x5c160 + p*0x24:
 *   +0x7c..+0x8a  button bits of fn_0000a184 (current keys),
 *   +0x8c/+0x90/+0x94  analog input (-1.0f, steering -100..100, 0),
 *   +0xa0..+0xae  button bits used by the race (from the replay, or from fn_0002bf68).
 * Recording (G_U8(0x3f990)): 16 bytes per frame per player into the buffer G_PTR(0x5c174)
 * (200000 frames each), plus a snapshot of every car each 128 frames into G_PTR(0x5c178).
 * Replay (G_S32(0x3f984) == 1) reads both back.
 * --------------------------------------------------------------------------------------------- */

static void tick_unpack_race_keys(uint8_t *in, unsigned b)
{
    in[0xa6] = (b >> 15) & 1;
    in[0xa2] = (b >> 4) & 1;
    in[0xa3] = (b >> 12) & 1;
    in[0xa9] = (b >> 9) & 1;
    in[0xaa] = (b >> 8) & 1;
    in[0xab] = (b >> 3) & 1;
    in[0xac] = (b >> 2) & 1;
    in[0xad] = b & 1;
    in[0xae] = (b >> 1) & 1;
}

void fn_00009dc0(int pl)
{
    fn_0000442c();   /* the original passes pl; the function takes no argument */
    if (fn_00004540() & 0xff)
        return;   /* paused */

    uint8_t *in = (uint8_t *)G(0x5c160) + pl * 0x24;
    unsigned k = (unsigned)fn_0000a184(pl);
    in[0x82] = (k >> 15) & 1;
    in[0x7c] = (k >> 4) & 1;
    in[0x7d] = (k >> 12) & 1;
    in[0x81] = (k >> 10) & 1;
    in[0x80] = (k >> 11) & 1;
    in[0x83] = (k >> 9) & 1;
    in[0x84] = (k >> 8) & 1;
    in[0x87] = (k >> 3) & 1;
    in[0x88] = (k >> 2) & 1;
    in[0x89] = k & 1;
    in[0x8a] = (k >> 1) & 1;

    if (G_S32(0x3f984) == 1) {
        /* replay */
        int frame = G_S32(0x3f988);
        if (frame >= REC_FRAMES)
            frame = REC_FRAMES - 1; /* 3DS: past the recorded window */
        const uint32_t *e = (const uint32_t *)G_PTR(0x5c174) + (pl * REC_FRAMES + frame) * 4;
        AT_U32(in, 0x8c) = e[0];
        AT_U32(in, 0x90) = e[1];
        AT_U32(in, 0x94) = e[2];
        unsigned w = e[3];
        in[0xa0] = (w >> 4) & 1;
        in[0xa1] = (w >> 12) & 1;
        in[0xa5] = (w >> 10) & 1;
        in[0xa4] = (w >> 11) & 1;
        in[0xa7] = (w >> 9) & 1;
        in[0xa8] = (w >> 8) & 1;
        tick_unpack_race_keys(in, (unsigned)fn_0002bf68(pl));
        frame = G_S32(0x3f988);
        if ((frame & 0x7f) == 0 && frame < REC_FRAMES) {
            int n = G_S32(0x3e9c8);
            if (n > REC_MAX_CARS)
                n = REC_MAX_CARS;
            const uint8_t *src = (const uint8_t *)G_PTR(0x5c178) + (frame / 128) * 0x678;
            for (int i = 0; i < n; i++, src += REC_CAR_STRIDE)
                memcpy(CAR(i), src, 0x678);
        }
        return;
    }

    unsigned b = (unsigned)fn_0002bf68(pl);
    int v = G_S32(0x3f848);   /* (sic) the original converts this when not in mode 5 */
    if (G_S32(0x3f848) == 5) {
        /* steering: tilt, or the on-screen slider (button 12 of the touch table 0x40388) */
        float s;
        if (G_U8(0x40388 + 0x194) == 0) {
            s = G_F32(0x3f824);
        } else {
            s = G_F32(0x40388 + 0x18c) * 90.0f + -1.0f;
            G_F32(0x3f824) = s;
        }
        int n = (int)(fabsf(s) + -1.0f);
        if (n < 0)
            n = 0;
        float t = G_F32(0x3f824);
        int sgn = t < 0.0f ? -100 : (t > 0.0f ? 100 : 0);
        v = (sgn * n) / -30;
        if ((int16_t)v >= 101)
            v = 100;
    }
    AT_F32(in, 0x8c) = -1.0f;
    AT_F32(in, 0x90) = (float)(int16_t)v;
    AT_S32(in, 0x94) = 0;

    if (G_U8(0x3f990) != 0 && G_S32(0x3f988) < REC_FRAMES) {
        /* recording (3DS: only inside the recorded window) */
        uint32_t *e = (uint32_t *)G_PTR(0x5c174) + (pl * REC_FRAMES + G_S32(0x3f988)) * 4;
        e[3] = b;
        e[0] = AT_U32(in, 0x8c);
        e[1] = AT_U32(in, 0x90);
        e[2] = AT_U32(in, 0x94);
        int frame = G_S32(0x3f988);
        if ((frame & 0x7f) == 0) {
            int n = G_S32(0x3e9c8);
            if (n > REC_MAX_CARS)
                n = REC_MAX_CARS;
            uint8_t *dst = (uint8_t *)G_PTR(0x5c178) + (frame / 128) * 0x678;
            for (int i = 0; i < n; i++, dst += REC_CAR_STRIDE)
                memcpy(dst, CAR(i), 0x678);
        }
    }

    in[0xa6] = (b >> 15) & 1;
    in[0xa0] = (b >> 4) & 1;
    in[0xa1] = (b >> 12) & 1;
    in[0xa5] = (b >> 10) & 1;
    in[0xa4] = (b >> 11) & 1;
    in[0xa7] = (b >> 9) & 1;
    in[0xa8] = (b >> 8) & 1;
    tick_unpack_race_keys(in, b);
}

/* Game-side platform glue: replaces the iOS app delegate / view controller.
 *
 *   EAGLView, AppDelegate      -> nothing (sys.c owns the GPU and the main loop)
 *   viewController drawFrame   -> app_tick()
 *   ProcessTouches:            -> feed_touches() (bottom screen into the touch table 0x4268c)
 *   accelerometer:didAccelerate: -> feed_accel() (3DS accelerometer into 0x42754)
 *
 * It also provides the services ios.h declares: the memory image, the linear heap, resource
 * and document paths and the millisecond clock. */
#include <3ds.h>
#include <malloc.h>
#include <sys/stat.h>
#include "ios.h"
#include "input.h"

void snd_shutdown(void); /* m_35df8.c */

uint8_t *g_img;

/* ---------------------------------------------------------------- memory image */

/* Sections of the image that hold absolute iOS addresses (non-PIE binary, no rebase info):
 * __nl_symbol_ptr (local non-lazy pointers, e.g. 0x3d2cc -> 0x4268c touch table) and __data
 * (the global-pointer table 0x3e92c..0x3f3a0 plus ObjC protocol records). A word is a pointer
 * when its value falls inside the image; nothing else in these sections does (0x3e9dc holds
 * 180000, which is below IMG_LO). */
static const struct { uint32_t lo, hi; } k_reloc[] = {
    {0x3d244, 0x3d30c}, /* __nl_symbol_ptr */
    {0x3e870, 0x3f3f0}, /* __data */
};

void ios_image_init(void) {
    const uint32_t size = IMG_HI - IMG_LO;
    g_img = calloc(1, size);
    if (!g_img) svcBreak(USERBREAK_PANIC);
    FILE *f = fopen("romfs:/ios_image.bin", "rb");
    if (!f) {
        sys_log("ios_image.bin missing");
        svcBreak(USERBREAK_PANIC);
    }
    size_t n = fread(g_img, 1, size, f);
    fclose(f);
    sys_log("image: %u bytes", (unsigned)n);
    int moved = 0;
    for (unsigned s = 0; s < sizeof k_reloc / sizeof k_reloc[0]; s++) {
        for (uint32_t a = k_reloc[s].lo; a < k_reloc[s].hi; a += 4) {
            uint32_t v = G_U32(a);
            if (v >= IMG_LO && v < IMG_HI) {
                G_U32(a) = (uint32_t)(uintptr_t)G(v);
                moved++;
            }
        }
    }
    sys_log("image: %d pointers relocated", moved);
}

/* ---------------------------------------------------------------- services */

/* CxAlloc32Mem1. Blocks up to 1 MB come from the linear heap so vertex arrays built in them
 * reach the GPU without the per-draw copy (gfx_c3d.c); bigger ones (the race recording) from
 * the regular heap, which is far larger. Falls back to the other heap when one is full. */
#define LINEAR_MAX (1u << 20)

static bool is_linear(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return (a >= 0x14000000 && a < 0x1C000000) || (a >= 0x30000000 && a < 0x40000000);
}

void *ios_alloc(uint32_t size) {
    uint32_t n = size ? size : 4;
    void *p = NULL;
    if (n <= LINEAR_MAX) p = gfxGL_LinearAlloc(n);
    if (!p) p = memalign(32, n);
    if (!p && n > LINEAR_MAX) p = gfxGL_LinearAlloc(n);
    if (p) memset(p, 0, n);
    else sys_log("ios_alloc(%u) failed", (unsigned)size);
    return p;
}

void ios_free(void *p) {
    if (!p) return;
    if (is_linear(p)) gfxGL_LinearFree(p);
    else free(p);
}

const char *ios_res_path(const char *name, char *buf, int bufsize) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    const char *dot = strrchr(base, '.');
    if (dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".pvr")))
        snprintf(buf, bufsize, "romfs:/%.*s.t3x", (int)(dot - base), base);
    else
        snprintf(buf, bufsize, "romfs:/%s", base);
    return buf;
}

const char *ios_doc_dir(void) {
    static bool made;
    if (!made) {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/PixelRacing3D", 0777);
        made = true;
    }
    return "sdmc:/3ds/PixelRacing3D";
}

uint32_t ios_ms(void) { return sys_ms(); }

/* ---------------------------------------------------------------- input */

/* ProcessTouches: (touch / view size) * virtual size. The view is the iOS landscape view
 * (480x320 or 1024x768 by bit 0 of 0x3f83c); here it is whatever input.c reports, already in
 * virtual-screen units. Table entry: {u8 active; float x, y, prevx, prevy}. */
static void feed_touches(void) {
    InputTouch t[INPUT_MAX_TOUCHES];
    int n = input_touches(t, INPUT_MAX_TOUCHES);
    float vw = (float)G_S32(0x40368), vh = (float)G_S32(0x40374);
    for (int i = 0; i < 10; i++) {
        uint8_t *e = (uint8_t *)G(0x4268c) + i * 0x14;
        if (i >= n || !t[i].down) {
            e[0] = 0;
            continue;
        }
        e[0] = 1;
        AT_F32(e, 4) = t[i].x * vw;
        AT_F32(e, 8) = t[i].y * vh;
        if (t[i].moved) {
            AT_F32(e, 0xc) = t[i].px * vw;
            AT_F32(e, 0x10) = t[i].py * vh;
        } else {
            AT_F32(e, 0xc) = AT_F32(e, 4);
            AT_F32(e, 0x10) = AT_F32(e, 8);
        }
    }
}

/* Physical buttons -> the game's touch buttons (TASKS #28). The engine keeps 20 round buttons
 * at 0x40388 (x, y, r, ..., active @0x16, icon @0x1c; fn_00004500 registers them, fn_0000442c
 * hit-tests the touch table). The race layout (fn_000209e8) reuses the Wii version's remote
 * icons, and fn_0002bf68 turns held buttons into the player's control bits:
 *   idx 2 "2" -> 0x100 (gas)    idx 1 "1" -> 0x200 (brake)   idx 3/4 -> 8/4 (steer zones)
 *   idx 5/6   -> 1/2 (down/up)  idx 7     -> 0x10
 *   idx 11    -> pause: fn_0000a184 opens the pause / info page (state 9), or returns 0x1010
 *                when the race shows its pause icon (0x3fab4 in modes 5 and 8)
 *   idx 14    -> fn_00003ccc, the iOS "rate this app" prompt (0x3f814): not mapped
 * The menus read the same buttons (fn_00030bf4): 3/4 left/right, 6/5 up/down, 2 ok, 1 back.
 * A held 3DS button presses its game button by holding a virtual touch on its centre, so it
 * works wherever the game has that button active and does nothing elsewhere. */
static const struct { uint32_t keys; int idx; } k_button_map[] = {
    {KEY_A, 2},                      /* accelerate ("2" on the Wii Remote) */
    {KEY_B, 1},                      /* brake / reverse ("1") */
    {KEY_DLEFT | KEY_CPAD_LEFT, 3},  /* steer left (static controls) */
    {KEY_DRIGHT | KEY_CPAD_RIGHT, 4},/* steer right */
    {KEY_DUP | KEY_CPAD_UP, 6},       /* menu up (the button above the list) */
    {KEY_DDOWN | KEY_CPAD_DOWN, 5},   /* menu down */
    {KEY_R | KEY_X, 7},
    {KEY_START | KEY_SELECT, 11},    /* pause / info page */
};

static void feed_buttons(void) {
    float vw = (float)G_S32(0x40368), vh = (float)G_S32(0x40374);
    for (unsigned i = 0; i < sizeof k_button_map / sizeof k_button_map[0]; i++) {
        const uint8_t *b = (const uint8_t *)G(0x40388) + k_button_map[i].idx * 0x20;
        bool on = b[0x16] && vw > 0 && vh > 0;
        input_bind_button((int)i, on ? k_button_map[i].keys : 0,
                          on ? AT_F32(b, 0) / vw : 0.0f, on ? AT_F32(b, 4) / vh : 0.0f);
    }
}

/* accelerometer:didAccelerate: stores (int)(g * 90) as float per iOS axis, negated when the
 * landscape-orientation flag 0x3ebb2 is 0. Worked through for both landscape orientations,
 * the stored values come out as x = -Yb, y = Xb, z = +-Zb (body frame of input.h), so the
 * game's steering axis (accel[1], minus half the value latched at the race start) is the 3DS
 * roll: turning the console like a wheel.
 *
 * Tilt modes (Settings, TASKS #29):
 *   TILT_IOS  the iOS reading as is (linear in g).
 *   TILT_WII  the Wii version's steering curve (Wii DOL 0x80027e40, player branch): the raw
 *             Wii Remote tilt t (about +-100 per g) is saturated to +-100 once the remote is
 *             rolled past vertical, then steer = handling * (0.5 t + t |t| / 150). The same
 *             curve is applied to the 3DS roll and scaled so 1 g still reads as 90, the iOS
 *             full lock; the per-car handling factor stays the iOS game's own. */
enum { TILT_IOS = 0, TILT_WII = 1 };
static int s_tilt_mode = TILT_WII;

static float wii_tilt_curve(float t) {
    float a = t < 0 ? -t : t;
    return 0.5f * t + t * a / 150.0f;
}

static void feed_accel(void) {
    float g[3];
    input_accel_g(g);
    float roll = g[0];
    if (s_tilt_mode == TILT_WII) {
        float t = roll * 100.0f;
        if (g[1] >= 0.0f) t = roll < 0 ? -100.0f : 100.0f; /* rolled past vertical */
        if (t > 100.0f) t = 100.0f;
        if (t < -100.0f) t = -100.0f;
        roll = wii_tilt_curve(t) / wii_tilt_curve(100.0f);
    }
    float zs = G_U8(0x3ebb2) ? 1.0f : -1.0f;
    G_F32(0x42754) = (float)(int)(-g[1] * 90.0f);
    G_F32(0x42758) = (float)(int)(roll * 90.0f);
    G_F32(0x4275c) = (float)(int)(zs * g[2] * 90.0f);
}

/* ---------------------------------------------------------------- app */

void app_init(void) {
    ios_image_init();
    input_init();
    G_U8(0x3f814) = 0; /* "do you like this app" prompt: iOS only */
    /* EAGLView createFramebuffer: GL_RENDERBUFFER_WIDTH -> 0x42800, HEIGHT -> 0x427fc. The
     * engine (fn_00004028, fn_00003f7c) derives the viewport and the 3D aspect from these;
     * the 3DS GL window is the 400x240 top screen. */
    G_U32(0x42800) = 400;
    G_U32(0x427fc) = 240;
    /* The game draws nothing on the bottom screen (no on-screen buttons on the 3DS), so it
     * shows the 3D Pixel Racing logo (tools/banner/make_banner.py renders it from the Wii
     * banner) instead of black. */
    gfxGL_SetBottomIdle("romfs:/bottom_logo.t3x", 320, 240);
    sys_log("app_init done");
}

/* Debug log: the game's menu/race state (0x3f848, see m_0a184.c) and the tick's screen
 * state (0x3f840) whenever either changes, so a crash report shows where the player was. */
static void log_state(void) {
    static int s_state = -1, s_screen = -1;
    int state = G_S32(0x3f848), screen = G_S32(0x3f840);
    if (state != s_state || screen != s_screen) {
        sys_log("state %d screen %d", state, screen);
        s_state = state;
        s_screen = screen;
    }
}

/* drawFrame without the iOS pacing (sys.c paces by vsync): the first call runs the
 * one-time init fn_00004254(1); after that every call is one 60 Hz game tick. */
void app_tick(bool render) {
    input_update();
    feed_buttons();
    feed_touches();
    feed_accel();
    if (G_U8(0x3e91c)) {
        fn_00004254(1);
        G_U8(0x3e91c) = 0;
        G_U32(0x3f830) = ios_ms();
        return;
    }
    G_U8(0x3f814) = 0;
    G_U8(0x3e918) = render;
    fn_00005ac4(render ? 1 : 0);
    input_end_tick();
    log_state();
}

void app_exit(void) {
    snd_shutdown();
    input_exit();
}

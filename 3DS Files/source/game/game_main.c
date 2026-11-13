/* Game-side platform glue: replaces the iOS app delegate / view controller.
 *
 *   EAGLView, AppDelegate      -> nothing (sys.c owns the GPU and the main loop)
 *   viewController drawFrame   -> app_tick()
 *   ProcessTouches:            -> feed_touches() (bottom screen into the touch table 0x4268c)
 *   accelerometer:didAccelerate: -> feed_steering() (the steering the player chose, as the
 *                                  tilt the game expects, into 0x42754)
 *
 * It also provides the services ios.h declares: the memory image, the linear heap, resource
 * and document paths and the millisecond clock. */
#include <3ds.h>
#include <malloc.h>
#include <sys/stat.h>
#include "ios.h"
#include "controls.h"
#include "opt_menu.h"
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

/* The game's save file (sg.m writes only "RaceData", 128 bytes). The CIA keeps it in its own
 * save data (meta/app.rsf SaveDataSize), like a retail title: it is backed up, moved and
 * deleted with the title, and written files only count once committed. The .3dsx has no
 * save data of its own and keeps using the SD card, as does the CIA if its save data can't be
 * opened. The first time the CIA finds no save file there it takes the SD card's over, so
 * progress from the builds that saved to the SD card carries on. */
static int s_save_arch = -1; /* -1 not decided, 0 SD card, 1 save data */

static void save_take_over(const char *name) {
    char from[64], to[64];
    snprintf(from, sizeof from, "%s/%s", ios_doc_dir(), name);
    snprintf(to, sizeof to, "save:/%s", name);
    FILE *t = fopen(to, "rb");
    if (t) {
        fclose(t);
        return;
    }
    FILE *f = fopen(from, "rb");
    if (!f) return;
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    t = fopen(to, "wb");
    if (!t) return;
    bool ok = fwrite(buf, 1, n, t) == n;
    fclose(t);
    ios_save_commit();
    sys_log("save: %s taken over from the SD card (%u bytes)%s", name, (unsigned)n, ok ? "" : ", write failed");
}

const char *ios_save_dir(void) {
    if (s_save_arch < 0) {
        s_save_arch = 0;
        if (sys_is_cia()) {
            Result r = archiveMount(ARCHIVE_SAVEDATA, fsMakePath(PATH_EMPTY, ""), "save");
            /* A freshly installed title's save data isn't formatted yet (build 12 on hardware:
             * c8a04555). Retail titles format it on first boot; only "not formatted" is
             * answered with a format, never another error, so existing data can't be wiped. */
            if (r == (Result)0xC8A04554 || r == (Result)0xC8A04555) {
                Result f = FSUSER_FormatSaveData(ARCHIVE_SAVEDATA, fsMakePath(PATH_EMPTY, ""),
                                                 0x200, 8, 8, 11, 11, true);
                sys_log("save: save data not formatted (%08lx), formatting: %08lx",
                        (unsigned long)r, (unsigned long)f);
                if (R_SUCCEEDED(f))
                    r = archiveMount(ARCHIVE_SAVEDATA, fsMakePath(PATH_EMPTY, ""), "save");
            }
            if (R_SUCCEEDED(r)) {
                s_save_arch = 1;
                save_take_over("RaceData");
            }
            sys_log("save: %s (%08lx)", s_save_arch ? "the title's save data" : "SD card, no save data",
                    (unsigned long)r);
        }
    }
    return s_save_arch ? "save:" : ios_doc_dir();
}

void ios_save_commit(void) {
    if (s_save_arch == 1) {
        Result r = archiveCommitSaveData("save");
        if (R_FAILED(r)) sys_log("save: commit failed (%08lx)", (unsigned long)r);
    }
}

uint32_t ios_ms(void) { return sys_ms(); }

/* ---------------------------------------------------------------- input */

/* ProcessTouches: (touch / view size) * virtual size. The view is the iOS landscape view
 * (480x320 or 1024x768 by bit 0 of 0x3f83c); here it is whatever input.c reports, already in
 * virtual-screen units. Table entry: {u8 active; float x, y, prevx, prevy}. */
static bool racing(void);

static void feed_touches(void) {
    InputTouch t[INPUT_MAX_TOUCHES];
    int n = input_touches(t, INPUT_MAX_TOUCHES);
    /* The bottom screen shows the gauges and the map during a race, where a touch would land
     * on the iPhone version's invisible gas / brake / steering buttons: then only the buttons'
     * own virtual touches get through. */
    if (racing()) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if (t[i].virt) t[k++] = t[i];
        n = k;
    }
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
/* The game's button indexes, and which 3DS keys press them. In a race gas, brake and nitro
 * are the player's choice (controls.c) and the Circle Pad only steers; in the menus and on
 * the pause screen A/B are always OK/back and the Circle Pad moves through the lists. */
enum { BTN_BRAKE = 1, BTN_GAS = 2, BTN_LEFT = 3, BTN_RIGHT = 4, BTN_DOWN = 5, BTN_UP = 6,
       BTN_NITRO = 7, BTN_PAUSE = 11 };
enum { NSLOTS = 8 };

/* Racing and not paused (0x3f9e0 is the race's pause flag). */
static bool racing(void) {
    return G_S32(0x3f848) == 5 && G_S32(0x3f840) == 7 && !G_U8(0x3f9e0);
}

static void bind_key(int slot, int idx, uint32_t keys) {
    float vw = (float)G_S32(0x40368), vh = (float)G_S32(0x40374);
    const uint8_t *b = (const uint8_t *)G(0x40388) + idx * 0x20;
    bool on = keys && b[0x16] && vw > 0 && vh > 0;
    input_bind_button(slot, on ? keys : 0, on ? AT_F32(b, 0) / vw : 0.0f,
                      on ? AT_F32(b, 4) / vh : 0.0f);
}

static void feed_buttons(void) {
    const Controls *c = controls_get();
    bool race = racing();
    uint32_t pad_lr = race ? 0 : KEY_CPAD_LEFT | KEY_CPAD_RIGHT;
    uint32_t pad_ud = race ? 0 : KEY_CPAD_UP | KEY_CPAD_DOWN;
    bind_key(0, BTN_GAS, race ? controls_key(c->gas) : KEY_A);
    bind_key(1, BTN_BRAKE, race ? controls_key(c->brake) : KEY_B);
    bind_key(2, BTN_LEFT, KEY_DLEFT | (pad_lr & KEY_CPAD_LEFT));
    bind_key(3, BTN_RIGHT, KEY_DRIGHT | (pad_lr & KEY_CPAD_RIGHT));
    bind_key(4, BTN_UP, KEY_DUP | (pad_ud & KEY_CPAD_UP));
    bind_key(5, BTN_DOWN, KEY_DDOWN | (pad_ud & KEY_CPAD_DOWN));
    bind_key(6, BTN_NITRO, race ? controls_key(c->nitro) : KEY_R | KEY_X);
    bind_key(7, BTN_PAUSE, KEY_START);
}

/* Steering. accelerometer:didAccelerate: stored (int)(g * 90) per axis; the race tick
 * (fn_00005ac4) steers from accel[1] minus half the value latched at the race start
 * (0x3f828): steer = (|tilt| - 2) * 1.2, then the car gets (|steer| - 1) / 30 of full lock.
 * The port works the other way round: controls.c gives the steering it wants, -1..1 (from the
 * Circle Pad, the D-pad or the tilt of the console), and accel[1] is set to the value that
 * produces exactly that, with the calibration held at 0. The per-car handling and the
 * blending with the game's auto-steer stay the game's own. accel[2] tilts the horizon by its
 * cosine: 0 keeps it level. accel[0] keeps the console's pitch, as the iPhone's did. */
static void feed_steering(void) {
    float g[3];
    input_accel_g(g);
    float m = controls_steer();
    float a = fabsf(m);
    float v = a > 0.0f ? (30.0f * a + 1.01f) / 1.2f + 2.0f : 0.0f;
    G_F32(0x42754) = (float)(int)(-g[1] * 90.0f);
    G_F32(0x42758) = m < 0.0f ? -v : v;
    G_F32(0x4275c) = 0.0f;
    G_F32(0x3f828) = 0.0f;
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
    controls_init();
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
    feed_steering();
    if (G_U8(0x3e91c)) {
        fn_00004254(1);
        G_U8(0x3e91c) = 0;
        G_U32(0x3f830) = ios_ms();
        return;
    }
    G_U8(0x3f814) = 0;
    G_U8(0x3e918) = render;
    fn_00005ac4(render ? 1 : 0);
    if (render) controls_bottom_draw(optm_meter());
    input_end_tick();
    log_state();
}

void app_exit(void) {
    snd_shutdown();
    input_exit();
}

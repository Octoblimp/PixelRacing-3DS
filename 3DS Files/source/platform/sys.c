/* Boot and main loop.
 *
 * The iOS build ran drawFrame from a CADisplayLink at 60 Hz and inside it stepped the game
 * `60 / fps` times, rendering only on the last step. The 3DS keeps that shape: the loop is
 * paced by the GPU vsync (C3D_FRAME_SYNCDRAW in gfxGL_EndFrame), and each presented frame runs
 * 1 tick on New 3DS (60 fps) or 2 ticks on Old 3DS (30 fps). */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "gles.h"
#include "sys.h"

static bool s_quit, s_n3ds;
static u64 s_t0;

bool sys_is_new3ds(void) { return s_n3ds; }
uint32_t sys_app_mem(void) { return osGetMemRegionSize(MEMREGION_APPLICATION); }
int sys_render_hz(void) { return s_n3ds ? 60 : 30; }
uint32_t sys_ms(void) { return (uint32_t)((svcGetSystemTick() - s_t0) / CPU_TICKS_PER_MSEC); }
void sys_quit(void) { s_quit = true; }

/* True when running as the installed CIA: the running title is ours (meta/app.rsf UniqueId
 * 0xB3D90 -> title ID 000400000B3D9000). A .3dsx runs under another title (the Homebrew
 * Launcher's host, or none when an emulator boots it directly). */
#define CIA_UNIQUE_ID 0xB3D90u
static bool is_cia(void) {
    static int s_cia = -1;
    if (s_cia < 0) {
        u64 tid = 0;
        s_cia = !envIsHomebrew() && R_SUCCEEDED(APT_GetProgramID(&tid)) &&
                (tid >> 32) == 0x00040000u && ((uint32_t)tid >> 8) == CIA_UNIQUE_ID;
    }
    return s_cia;
}

/* The debug log (on unless built with DEBUG=0): sdmc:/PixelRacing_debug.txt for the CIA, or
 * sdmc:/3ds/PixelRacing_debug.txt for the .3dsx. Each line is stamped with the
 * time since boot and flushed at once, so the log survives a crash up to its last line. The
 * previous run's log is kept as PixelRacing_debug_prev.txt; the file stops growing at LOG_MAX. */
#define LOG_MAX (2u << 20)

void sys_log(const char *fmt, ...) {
#ifdef DEBUG_LOG
    static FILE *f;
    static bool s_failed, s_full;
    static uint32_t s_size;
    if (s_failed || s_full) return;
    if (!f) {
        const char *dir = is_cia() ? "sdmc:/" : "sdmc:/3ds/";
        char path[64], prev[64];
        snprintf(path, sizeof(path), "%sPixelRacing_debug.txt", dir);
        snprintf(prev, sizeof(prev), "%sPixelRacing_debug_prev.txt", dir);
        if (!is_cia()) mkdir("sdmc:/3ds", 0777);
        remove(prev);
        rename(path, prev);
        f = fopen(path, "w");
        if (!f) { s_failed = true; return; }
    }
    char line[512];
    uint32_t ms = sys_ms();
    int n = snprintf(line, sizeof(line), "[%02lu:%02lu.%03lu] ", (unsigned long)(ms / 60000),
                     (unsigned long)(ms / 1000 % 60), (unsigned long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    while (len && line[len - 1] == '\n') line[--len] = 0; /* some game asserts end in a newline */
    if (s_size + len + 1 > LOG_MAX) {
        fputs("[log full]\n", f);
        fflush(f);
        s_full = true;
        return;
    }
    fputs(line, f);
    fputc('\n', f);
    fflush(f);
    s_size += len + 1;
#else
    (void)fmt;
#endif
}

/* Suspend / resume / sleep, so a log that stops mid-race shows whether the HOME Menu or the
 * lid was involved. */
static aptHookCookie s_apt_cookie;
static void apt_hook(APT_HookType type, void *param) {
    (void)param;
    static const char *const k_names[] = {"suspend", "restore", "sleep", "wakeup", "exit"};
    if ((unsigned)type < sizeof(k_names) / sizeof(k_names[0])) sys_log("apt: %s", k_names[type]);
}

/* Every 10 s: presented frame rate, the slowest frame and free linear memory. */
static void perf_frame(void) {
#ifdef DEBUG_LOG
    static uint32_t s_start, s_last, s_frames, s_worst;
    uint32_t now = sys_ms();
    if (!s_start) { s_start = s_last = now; return; }
    uint32_t dt = now - s_last;
    s_last = now;
    if (dt > s_worst) s_worst = dt;
    s_frames++;
    uint32_t span = now - s_start;
    if (span >= 10000) {
        uint32_t fps10 = s_frames * 10000 / span;
        sys_log("perf: %lu.%lu fps, slowest frame %lu ms, linear free %lu KB",
                (unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10),
                (unsigned long)s_worst, (unsigned long)(linearSpaceFree() / 1024));
        s_start = now;
        s_frames = s_worst = 0;
    }
#endif
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    s_n3ds = n3ds;
    if (n3ds) osSetSpeedupEnable(true);
    s_t0 = svcGetSystemTick();

    gfxInitDefault();
    romfsInit();
    gfxGL_Init();
    sys_log("3D Pixel Racing 3DS port, build %s %s (%s)", __DATE__, __TIME__,
            is_cia() ? "CIA" : ".3dsx");
    sys_log("boot: %s, app mem %lu KB, linear free %lu KB, %d fps target",
            n3ds ? "New 3DS" : "Old 3DS", (unsigned long)(sys_app_mem() / 1024),
            (unsigned long)(linearSpaceFree() / 1024), sys_render_hz());
    aptHook(&s_apt_cookie, apt_hook, NULL);

    app_init();
    int ticks = 60 / sys_render_hz();
    while (aptMainLoop() && !s_quit) {
        gfxGL_BeginFrame();
        for (int i = 0; i < ticks; i++) app_tick(i == ticks - 1);
        gfxGL_EndFrame();
        perf_frame();
    }
    sys_log("exit: back to the HOME Menu");
    app_exit();
    aptUnhook(&s_apt_cookie);

    gfxGL_Exit();
    romfsExit();
    gfxExit();
    return 0;
}

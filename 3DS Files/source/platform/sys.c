/* Boot and main loop.
 *
 * The iOS build ran drawFrame from a CADisplayLink at 60 Hz and inside it stepped the game
 * `60 / fps` times, rendering only on the last step. The 3DS keeps that shape: the loop is
 * paced by the GPU vsync (C3D_FRAME_SYNCDRAW in gfxGL_EndFrame), and each presented frame runs
 * 1 tick on New 3DS (60 fps) or 2 ticks on Old 3DS (30 fps). */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "gles.h"
#include "sys.h"

static bool s_quit, s_n3ds;
static u64 s_t0;
static LightLock s_log_lock;
static volatile const char *s_stage = "boot";
static volatile u32 s_frames_done;

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
    LightLock_Lock(&s_log_lock); /* the watchdog thread logs too */
    if (!f) {
        const char *dir = is_cia() ? "sdmc:/" : "sdmc:/3ds/";
        char path[64], prev[64];
        snprintf(path, sizeof(path), "%sPixelRacing_debug.txt", dir);
        snprintf(prev, sizeof(prev), "%sPixelRacing_debug_prev.txt", dir);
        if (!is_cia()) mkdir("sdmc:/3ds", 0777);
        remove(prev);
        rename(path, prev);
        f = fopen(path, "w");
        if (!f) { s_failed = true; LightLock_Unlock(&s_log_lock); return; }
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
        LightLock_Unlock(&s_log_lock);
        return;
    }
    fputs(line, f);
    fputc('\n', f);
    fflush(f);
    s_size += len + 1;
    LightLock_Unlock(&s_log_lock);
#else
    (void)fmt;
#endif
}

void sys_stage(const char *what) { s_stage = what; }

/* Main-loop watchdog. A frozen main loop also freezes the HOME button, so on hardware the
 * only trace a tester can send back is the log. This thread runs at a higher priority than
 * the main thread on the same core and sleeps; each time it wakes it checks that a frame
 * finished in the last 3 s, and if not it logs what the main loop was doing. */
#ifdef DEBUG_LOG
static volatile bool s_wd_quit;
static void watchdog(void *arg) {
    (void)arg;
    u32 last = s_frames_done, since = sys_ms();
    bool reported = false;
    while (!s_wd_quit) {
        svcSleepThread(500000000LL);
        u32 now = sys_ms();
        if (s_frames_done != last) {
            if (reported) sys_log("watchdog: main loop running again after %lu ms",
                                  (unsigned long)(now - since));
            last = s_frames_done;
            since = now;
            reported = false;
        } else if (!reported && now - since > 3000) {
            sys_log("watchdog: no frame for 3 s, main loop in \"%s\" after frame %lu (normal while loading)",
                    (const char *)s_stage, (unsigned long)last);
            reported = true;
        }
    }
}
#endif

/* Suspend / resume / sleep, so a log that stops mid-race shows whether the HOME Menu or the
 * lid was involved. */
static aptHookCookie s_apt_cookie;
static void apt_hook(APT_HookType type, void *param) {
    (void)param;
    static const char *const k_names[] = {"suspend", "restore", "sleep", "wakeup", "exit"};
    if ((unsigned)type < sizeof(k_names) / sizeof(k_names[0])) sys_log("apt: %s", k_names[type]);
}

/* Game ticks to run this frame. The game is written for a fixed 60 Hz tick (iOS drawFrame), so
 * one tick per presented frame makes the whole game run in slow motion whenever a frame takes
 * longer than a vsync -- while sounds play in real time: the "3, 2, 1, GO" sample finished
 * before the countdown on screen did. Instead run as many ticks as real time has passed, in
 * units of the LCD refresh (268111856 / 4481136 = 59.83 Hz, so a full-speed frame is exactly
 * one tick), rounded to the nearest so vsync jitter doesn't alternate 0 and 2. At least one
 * (the rendering tick), at most MAX_TICKS: past that (loading, HOME Menu) the game slows down
 * rather than jumping ahead. */
#define VSYNC_SYSTICKS 4481136
#define MAX_TICKS 4
static int s_ticks_run;
static int ticks_due(void) {
    static u64 s_prev;
    static s64 s_owed;
    u64 now = svcGetSystemTick();
    if (!s_prev) s_prev = now - VSYNC_SYSTICKS * (60 / sys_render_hz());
    s_owed += (s64)(now - s_prev);
    s_prev = now;
    int n = (int)((s_owed + VSYNC_SYSTICKS / 2) / VSYNC_SYSTICKS);
    if (n < 1) n = 1;
    if (n > MAX_TICKS) n = MAX_TICKS;
    s_owed -= (s64)n * VSYNC_SYSTICKS;
    if (s_owed > VSYNC_SYSTICKS) s_owed = VSYNC_SYSTICKS;
    if (s_owed < -VSYNC_SYSTICKS) s_owed = -VSYNC_SYSTICKS;
    s_ticks_run += n;
    return n;
}

/* Every 10 s: presented frame rate, game ticks per second (60 = full speed), the slowest frame
 * and free linear memory. */
static void perf_frame(void) {
#ifdef DEBUG_LOG
    static uint32_t s_start, s_last, s_frames, s_worst;
    uint32_t now = sys_ms();
    if (!s_start) { s_start = s_last = now; s_ticks_run = 0; return; }
    uint32_t dt = now - s_last;
    s_last = now;
    if (dt > s_worst) s_worst = dt;
    s_frames++;
    uint32_t span = now - s_start;
    if (span >= 10000) {
        uint32_t fps10 = s_frames * 10000 / span;
        uint32_t tps = (uint32_t)s_ticks_run * 1000 / span;
        s_ticks_run = 0;
        /* memory: what is free now, and the peak use since boot, so a run on a big console
         * tells what a small one needs */
        static uint32_t s_lin_min = 0xffffffffu, s_heap_max;
        uint32_t lin = linearSpaceFree();
        struct mallinfo mi = mallinfo();
        if (lin < s_lin_min) s_lin_min = lin;
        if ((uint32_t)mi.uordblks > s_heap_max) s_heap_max = (uint32_t)mi.uordblks;
        sys_log("perf: %lu.%lu fps, %lu game ticks/s, slowest frame %lu ms, linear free %lu KB "
                "(peak use %lu KB), malloc %lu KB (peak %lu KB)",
                (unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10), (unsigned long)tps,
                (unsigned long)s_worst,
                (unsigned long)(lin / 1024),
                (unsigned long)((g_heap_info.linear - s_lin_min) / 1024),
                (unsigned long)(mi.uordblks / 1024), (unsigned long)(s_heap_max / 1024));
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
    LightLock_Init(&s_log_lock);

    gfxInitDefault();
    romfsInit();
    gfxGL_Init();
    sys_log("3D Pixel Racing 3DS port, build %s %s (%s)", __DATE__, __TIME__,
            is_cia() ? "CIA" : ".3dsx");
    sys_log("boot: %s, app mem %lu KB, linear free %lu KB, %d fps target",
            n3ds ? "New 3DS" : "Old 3DS", (unsigned long)(sys_app_mem() / 1024),
            (unsigned long)(linearSpaceFree() / 1024), sys_render_hz());
    sys_log("heap: %lu KB to share, linear %lu KB (%lu tries), malloc %lu KB",
            (unsigned long)(g_heap_info.avail >> 10), (unsigned long)(g_heap_info.linear >> 10),
            (unsigned long)g_heap_info.linear_tries, (unsigned long)(g_heap_info.heap >> 10));
    aptHook(&s_apt_cookie, apt_hook, NULL);
#ifdef DEBUG_LOG
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread wd = threadCreate(watchdog, NULL, 16 * 1024, prio > 0x18 ? prio - 1 : 0x18, -2, false);
    if (!wd) sys_log("watchdog: could not start");
#endif

    app_init();
    while (true) {
        sys_stage("system (aptMainLoop)");
        if (!aptMainLoop() || s_quit) break;
        sys_stage("game");
        gfxGL_BeginFrame();
        int ticks = ticks_due();
        for (int i = 0; i < ticks; i++) app_tick(i == ticks - 1);
        sys_stage("render");
        gfxGL_EndFrame();
        perf_frame();
        s_frames_done++;
    }
    sys_log("exit: back to the HOME Menu");
#ifdef DEBUG_LOG
    if (wd) {
        s_wd_quit = true;
        threadJoin(wd, U64_MAX);
        threadFree(wd);
    }
#endif
    app_exit();
    aptUnhook(&s_apt_cookie);

    gfxGL_Exit();
    romfsExit();
    gfxExit();
    return 0;
}

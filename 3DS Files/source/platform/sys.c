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
#include "controls.h"

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
bool sys_is_cia(void) { return is_cia(); }

/* Settings file on the SD card, written with the defaults on first boot so it can be found
 * and edited on a PC: "debug_log 1" turns the debug log on (off by default). */
#define CONFIG_PATH "sdmc:/3ds/PixelRacing3D/config.txt"
static bool s_debug;
bool sys_debug(void) { return s_debug; }

static void config_load(void) {
#ifdef DEBUG_LOG
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/PixelRacing3D", 0777);
        f = fopen(CONFIG_PATH, "w");
        if (f) {
            fputs("# 3D Pixel Racing 3DS settings\n"
                  "# debug_log 1 writes a debug log to the SD card (sdmc:/PixelRacing_debug.txt\n"
                  "# for the CIA, sdmc:/3ds/PixelRacing_debug.txt for the .3dsx). 0 = off.\n"
                  "debug_log 0\n", f);
            fclose(f);
        }
        return;
    }
    char line[128], key[32];
    int v;
    while (fgets(line, sizeof line, f))
        if (line[0] != '#' && sscanf(line, "%31s %d", key, &v) == 2 && !strcmp(key, "debug_log"))
            s_debug = v != 0;
    fclose(f);
#endif
}

/* The debug log (when config.txt says debug_log 1): sdmc:/PixelRacing_debug.txt for the CIA, or
 * sdmc:/3ds/PixelRacing_debug.txt for the .3dsx. Each line is stamped with the
 * time since boot and flushed at once, so the log survives a crash up to its last line. The
 * previous run's log is kept as PixelRacing_debug_prev.txt; the file stops growing at LOG_MAX. */
#define LOG_MAX (2u << 20)

void sys_log(const char *fmt, ...) {
#ifdef DEBUG_LOG
    static FILE *f;
    static bool s_failed, s_full;
    static uint32_t s_size;
    if (!s_debug || s_failed || s_full) return;
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
    /* nothing may talk to the GPU while the HOME Menu has it, or after exit; this hook was
     * registered after citro3d's, so it runs before it */
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONEXIT)
        gfxGL_WaitIdle();
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
static float s_game_ms; /* CPU time of this frame's game ticks */
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
    if (!s_debug) return;
    static uint32_t s_start, s_last, s_frames, s_worst;
    /* where a frame's time goes: game ticks (CPU), waiting for the GPU, building GPU commands
     * (CPU), and the GPU's own drawing time; sums and maxima over the 10 s */
    static float s_sum[5], s_max[5];
    {
        float t[5];
        gfxGL_FrameTimes(&t[1], &t[2], &t[3], &t[4]);
        t[0] = s_game_ms;
        for (int i = 0; i < 5; i++) {
            s_sum[i] += t[i];
            if (t[i] > s_max[i]) s_max[i] = t[i];
        }
    }
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
        if (s_frames) {
            float k = 1.0f / s_frames;
            sys_log("perf: per frame (avg/max ms): game %d/%d, gpu wait %d/%d, build %d/%d, "
                    "gpu %d/%d, render thread wait %d/%d, %d draws",
                    (int)(s_sum[0] * k + 0.5f), (int)(s_max[0] + 0.5f), (int)(s_sum[1] * k + 0.5f),
                    (int)(s_max[1] + 0.5f), (int)(s_sum[2] * k + 0.5f), (int)(s_max[2] + 0.5f),
                    (int)(s_sum[3] * k + 0.5f), (int)(s_max[3] + 0.5f), (int)(s_sum[4] * k + 0.5f),
                    (int)(s_max[4] + 0.5f), gfxGL_LastDrawCount());
        }
        uint32_t tv, tm, vf;
        gfxGL_TexStats(&tv, &tm, &vf);
        sys_log("perf: textures %lu KB in VRAM, %lu KB in main memory, VRAM free %lu KB",
                (unsigned long)tv, (unsigned long)tm, (unsigned long)vf);
        for (int i = 0; i < 5; i++) s_sum[i] = s_max[i] = 0;
        s_start = now;
        s_frames = s_worst = 0;
    }
#endif
}

/* Stereo 3D from the 3D slider: eye separation up to STEREO_MAX in NDC units (0.015 = 3 pixels
 * per eye for distant things), times the 3D page's depth. Off while steering by tilt (unless
 * the 3D page says otherwise): turning the console moves the screen out of the narrow angle in
 * which the 3D picture works. Pop-out sets the screen plane, HUD Depth floats the 2D draws. */
#define STEREO_MAX 0.015f
static void stereo_update(void) {
    static int s_was = -1;
    float slider = osGet3DSliderState();
    const Controls *c = controls_get();
    static const float k_plane[3] = {80.0f, 120.0f, 160.0f};
    bool tilt = c->steer == STEER_TILT && !c->stereo_tilt;
    float iod = tilt ? 0.0f : slider * STEREO_MAX * (float)c->stereo_depth / 100.0f;
    gfxGL_SetStereoDepth(k_plane[c->stereo_pop], c->hud_depth ? 0.6f : 0.0f);
    if (iod < 0.0005f) iod = 0.0f;
    int on = iod > 0.0f ? 1 : 0;
    if (on != s_was) {
        if (s_was >= 0 || on) sys_log("3d: %s%s", on ? "on" : "off", tilt && slider > 0.0f ? " (tilt steering)" : "");
        s_was = on;
    }
    gfxGL_SetStereo(iod);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    s_n3ds = n3ds;
    if (n3ds) osSetSpeedupEnable(true);
    s_t0 = svcGetSystemTick();
    LightLock_Init(&s_log_lock);
    config_load();

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
    Thread wd = !s_debug ? NULL : threadCreate(watchdog, NULL, 16 * 1024, prio > 0x18 ? prio - 1 : 0x18, -2, false);
    if (s_debug && !wd) sys_log("watchdog: could not start");
#endif

    app_init();
    while (true) {
        sys_stage("system (aptMainLoop)");
        if (!aptMainLoop() || s_quit) break;
        sys_stage("game");
        gfxGL_BeginFrame();
        u64 tg = svcGetSystemTick();
        int ticks = ticks_due();
        for (int i = 0; i < ticks; i++) {
            /* The game draws on every tick. Only the last tick of a frame is shown: drop what
             * the earlier ones drew, or the scene is replayed once per tick on top of itself
             * (hair through faces where the game doesn't clear depth in between, and the GPU
             * doing the work twice). */
            if (i > 0) gfxGL_DiscardDraws();
            gfxGL_SkipDraws(i < ticks - 1); /* and don't even record what won't be shown */
            app_tick(i == ticks - 1);
        }
        s_game_ms = (float)(svcGetSystemTick() - tg) / CPU_TICKS_PER_MSEC;
        stereo_update();
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

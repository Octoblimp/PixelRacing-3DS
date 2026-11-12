/* Boot and main loop.
 *
 * The iOS build ran drawFrame from a CADisplayLink at 60 Hz and inside it stepped the game
 * `60 / fps` times, rendering only on the last step. The 3DS keeps that shape: the loop is
 * paced by the GPU vsync (C3D_FRAME_SYNCDRAW in gfxGL_EndFrame), and each presented frame runs
 * 1 tick on New 3DS (60 fps) or 2 ticks on Old 3DS (30 fps). */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
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

void sys_log(const char *fmt, ...) {
#ifdef DEBUG_LOG
    static FILE *f;
    if (!f) {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/PixelRacing3D", 0777);
        f = fopen("sdmc:/3ds/PixelRacing3D/log.txt", "w");
        if (!f) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
#else
    (void)fmt;
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
    sys_log("boot: %s, app mem %lu KB", n3ds ? "New 3DS" : "Old 3DS",
            (unsigned long)(sys_app_mem() / 1024));

    app_init();
    int ticks = 60 / sys_render_hz();
    while (aptMainLoop() && !s_quit) {
        gfxGL_BeginFrame();
        for (int i = 0; i < ticks; i++) app_tick(i == ticks - 1);
        gfxGL_EndFrame();
    }
    app_exit();

    gfxGL_Exit();
    romfsExit();
    gfxExit();
    return 0;
}

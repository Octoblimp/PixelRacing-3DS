/* Platform core: boot, frame pacing, console model / memory mode. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Implemented by the game (src/game) or by the model viewer (tools/modelview). */
void app_init(void);
/* One 60 Hz simulation tick; `render` is true on the tick whose output is presented. */
void app_tick(bool render);
void app_exit(void);

bool sys_is_new3ds(void);
/* Application memory region in bytes (64/80/96/124 MB depending on model and launch). */
uint32_t sys_app_mem(void);
/* Presented frames per second: 60 on New 3DS, 30 on Old 3DS (simulation stays at 60 Hz). */
int sys_render_hz(void);
/* Milliseconds since boot. */
uint32_t sys_ms(void);
/* Ask the main loop to quit (returns to HOME Menu / hbmenu). */
void sys_quit(void);
/* Debug log to sdmc:/3ds/PixelRacing3D/log.txt (no-op unless DEBUG_LOG is defined). */
void sys_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// The PS Vita itself, besides the screen and the sound: its log, its clocks, its battery (host.c).
#ifndef PD_VITA_HOST_H
#define PD_VITA_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ux0:data/pokedaisy/log.txt, the last run's kept as log-prev.txt: what to attach to a bug report.
void host_log_open(const char* path);
void host_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void host_log_close(void);
// While a game runs, the lines wait in memory (no memory card writes mid-game) until hold(false) or a full buffer.
void host_log_hold(bool hold);
void host_log_flush(void);

// Microseconds since the app started.
uint64_t host_us(void);
// Milliseconds, for the session's tick.
uint64_t host_ms(void);
// The battery, -1 when unknown; whether it's charging.
int host_battery_pct(void);
bool host_charging(void);
// Keeps the screen from dimming while a game plays on with no buttons pressed (a cutscene).
void host_keep_awake(void);
// The CPU / GPU at their highest (444 / 222 MHz), as mGBA's own Vita build runs.
void host_clocks_up(void);
// Memory the app's heap has handed out (newlib's mallinfo), for the log.
size_t host_heap_used(void);

#endif

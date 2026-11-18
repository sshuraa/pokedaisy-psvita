#include "host.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

static FILE* logFile;
static bool logHeld;
// Lines held while a game runs: written at hold(false), or when this fills.
static char heldText[16 * 1024];
static size_t heldLen;

void host_log_open(const char* path) {
    char prev[300];
    snprintf(prev, sizeof prev, "%.280s", path);
    char* dot = strrchr(prev, '.');
    if (dot) snprintf(dot, sizeof prev - (size_t) (dot - prev), "-prev.txt");
    remove(prev);
    rename(path, prev);
    logFile = fopen(path, "w");
}

void host_log_flush(void) {
    if (!logFile) return;
    if (heldLen) fwrite(heldText, 1, heldLen, logFile);
    heldLen = 0;
    fflush(logFile);
}

void host_log(const char* format, ...) {
    if (!logFile) return;
    char line[512];
    unsigned long long ms = (unsigned long long) host_ms();
    int n = snprintf(line, sizeof line, "[%6llu.%03llu] ", ms / 1000, ms % 1000);
    va_list args;
    va_start(args, format);
    n += vsnprintf(line + n, sizeof line - (size_t) n - 1, format, args);
    va_end(args);
    if (n > (int) sizeof line - 2) n = (int) sizeof line - 2;
    line[n++] = '\n';
    line[n] = 0;
    if (logHeld) {
        if (heldLen + (size_t) n > sizeof heldText) host_log_flush();
        memcpy(heldText + heldLen, line, (size_t) n);
        heldLen += (size_t) n;
        return;
    }
    fputs(line, logFile);
    fflush(logFile);
}

void host_log_hold(bool hold) {
    logHeld = hold;
    if (!hold) host_log_flush();
}

void host_log_close(void) {
    host_log_flush();
    if (logFile) fclose(logFile);
    logFile = NULL;
}

uint64_t host_us(void) {
    return sceKernelGetProcessTimeWide();
}

uint64_t host_ms(void) {
    return sceKernelGetProcessTimeWide() / 1000;
}

int host_battery_pct(void) {
    int pct = scePowerGetBatteryLifePercent();
    return pct >= 0 && pct <= 100 ? pct : -1;
}

bool host_charging(void) {
    return scePowerIsBatteryCharging() != 0;
}

void host_keep_awake(void) {
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

void host_clocks_up(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
}

size_t host_heap_used(void) {
    struct mallinfo m = mallinfo();
    return (size_t) m.uordblks;
}

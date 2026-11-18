// PokeDaisy for the PS Vita: every game on libmgba, the 3DS port's companion (../3ds/core) and its session (../3ds/host)
// on one screen, as the app has a one-screen device: the library one page, a game the whole screen with the companion
// opened over it (COMPANION VIEW: a WINDOW over the dimmed game, or a SIDE PANEL beside it). A homebrew .vpk; see README.md.
//
// Where it differs from the 3DS port (../3ds/ctr/main.c, which this follows): one core (mGBA: no armGBA or gpSP, whose
// recompilers are the 3DS's), one screen, the front touch screen for the stylus, sceAudioOut at 48 kHz for ndsp, and
// ux0:data/pokedaisy for the SD card's /pokedaisy.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>

#include <mgba-util/vfs.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/gba.h>
// mGBA's threads (EXPAND's worker): its Vita ones here, pthreads in the desktop harness.
#ifdef __vita__
#define PSP2
#else
#define USE_PTHREADS
#endif
#include <mgba-util/threading.h>

#include "../3ds/core/pd_canvas.h"
#include "../3ds/core/pd_game.h"
#include "../3ds/core/pd_games.h"
#include "../3ds/core/pd_library.h"
#include "../3ds/core/pd_menu.h"
#include "../3ds/core/pd_romart.h"
#include "../3ds/core/pd_save.h"
#include "../3ds/core/pd_settings.h"
#include "../3ds/core/pd_style.h"
#include "../3ds/host/pd_session.h"
#include "../app/src/main/cpp/pk_call.h"
#include "../app/src/main/cpp/pk_gb_audio.h"
#include "../app/src/main/cpp/pk_host.h"
#include "audio.h"
#include "host.h"
#include "video.h"
#include "backdrop.h"
#include "sfx.h"
#include "../3ds/core/pd_font.h"

// The app's main thread (the 3DS runs the same code in libctru's 32 KB; plenty of room here).
unsigned int sceUserMainThreadStackSize = 1024 * 1024;
// newlib's heap (its own default, which every VitaSDK app gets): a 32 MB ROM, mGBA, the library's art.
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;

#define BASE_DIR "ux0:data/pokedaisy"
#define ROM_DIR BASE_DIR "/roms"
#define BACKUP_DIR ROM_DIR "/pokedaisy-backups"
#define STATE_DIR BASE_DIR "/states"
#define ROMART_DIR BASE_DIR "/rom-art" // where each ROM's art sits (pd_romart_cache_dir)
#define CARD_DIR BASE_DIR "/cards"
#define LOG_FILE BASE_DIR "/log.txt"
#define SETTINGS_FILE BASE_DIR "/settings.ini"
#define VITA_FILE BASE_DIR "/vita.ini"
#define LIBRARY_FILE BASE_DIR "/library.tsv"
#define MAX_BACKUPS 10
#define MAX_ROMS 256

// mGBA's ROM buffer (FIXED_ROM_BUFFER, CMakeLists.txt): every game is read into it and patched in place, so the
// companion's pointer to the ROM never goes stale. 32 MB, a block of its own, reserved at start; the library reads a ROM
// into it too while no game runs.
#define ROM_BUFFER_SIZE (32 * 1024 * 1024)
uint32_t* romBuffer;
size_t romBufferSize;

static bool rom_buffer_init(void) {
    SceUID block = sceKernelAllocMemBlock("pd_rom", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ROM_BUFFER_SIZE, NULL);
    void* base = NULL;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) return false;
    romBuffer = base;
    romBufferSize = ROM_BUFFER_SIZE;
    return true;
}

#define TOP_W PD_TOP_W
#define TOP_H PD_TOP_H

#ifndef PD_VERSION
#define PD_VERSION "dev"
#endif

static uint32_t topPx[TOP_W * TOP_H];       // the library's top screen: the INFO window's picture
static uint32_t pagePx[VIDEO_PAGE_W * VIDEO_PAGE_H]; // the library's page, its hint strip at the bottom
static uint32_t companionPx[VIDEO_COMPANION_MAX_W * VIDEO_COMPANION_MAX_H];
static struct pd_settings settings;

// The Vita's own settings (vita.ini): whether the face buttons go by position (circle = A) instead of mGBA's Vita
// build's cross = A, and the technical switches. (COMPANION VIEW and BACKGROUND are the app's settings.)
static struct {
    bool positional;
    bool threadedVideo; // mGBA's renderer on a core of its own
    bool nativeMixer;   // the shared pd_mixer: the game's m4a sound mixer run natively (else mGBA emulates it)
    bool soundCapture;  // the first 20 s of a game's sound into two WAVs (finding the crackle)
    int oldBackground;  // v0.7's background=0/1 (-1 = none): moved into the app's BACKGROUND once
} vitaSettings;

static void vita_settings_load(void) {
    vitaSettings.positional = false;
    vitaSettings.threadedVideo = true;
    vitaSettings.nativeMixer = true;
    vitaSettings.soundCapture = false;
    vitaSettings.oldBackground = -1;
    FILE* f = fopen(VITA_FILE, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "positional=%d", &v) == 1) vitaSettings.positional = v != 0;
        if (sscanf(line, "threaded_video=%d", &v) == 1) vitaSettings.threadedVideo = v != 0;
        if (sscanf(line, "native_mixer=%d", &v) == 1) vitaSettings.nativeMixer = v != 0;
        if (sscanf(line, "sound_capture=%d", &v) == 1) vitaSettings.soundCapture = v != 0;
        if (sscanf(line, "background=%d", &v) == 1) vitaSettings.oldBackground = v != 0;
    }
    fclose(f);
}

static void vita_settings_save(void) {
    FILE* f = fopen(VITA_FILE, "w");
    if (!f) return;
    fprintf(f, "# PokeDaisy for the PS Vita\n");
    fprintf(f, "# positional: 0 = cross is A and circle is B (as mGBA's Vita build), 1 = circle is A and cross is B\n");
    fprintf(f, "positional=%d\n", vitaSettings.positional ? 1 : 0);
    fprintf(f, "# threaded_video: 1 = mGBA draws the game on a core of its own (needed for full speed)\n");
    fprintf(f, "threaded_video=%d\n", vitaSettings.threadedVideo ? 1 : 0);
    fprintf(f, "# native_mixer: 1 = the game's sound mixer run natively (faster), 0 = emulated by mGBA\n");
    fprintf(f, "native_mixer=%d\n", vitaSettings.nativeMixer ? 1 : 0);
    fprintf(f, "# sound_capture: 1 = the first 20 seconds of a game's sound saved as sound-*.wav (for finding sound problems)\n");
    fprintf(f, "sound_capture=%d\n", vitaSettings.soundCapture ? 1 : 0);
    fprintf(f, "# (COMPANION VIEW and BACKGROUND are in the app's SETTINGS > SCREEN)\n");
    fclose(f);
}

// mGBA's own threads (its video proxy with threaded_video=1) are made by its threading.h with no core picked: they go on
// the third core, away from the game's (the first) and the sound's (the second). Linked with --wrap (CMakeLists.txt).
SceUID __real_sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr,
                                    int cpuAffinityMask, const SceKernelThreadOptParam* option);
// The CPU meter's threads: the game's (core 0), EXPAND's worker (core 1, with the sound's), mGBA's renderer (core 2).
static SceUID gameThreadId = -1, wideThreadId = -1, rendererThreadId = -1;
static bool creatingWide;
SceUID __wrap_sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr,
                                    int cpuAffinityMask, const SceKernelThreadOptParam* option) {
    bool mgba = !cpuAffinityMask && name && !strcmp(name, "SceThread");
    if (mgba) cpuAffinityMask = SCE_KERNEL_CPU_MASK_USER_2;
    SceUID id = __real_sceKernelCreateThread(name, entry, initPriority, stackSize, attr, cpuAffinityMask, option);
    if (mgba && creatingWide) wideThreadId = id;
    else if (mgba) rendererThreadId = id;
    return id;
}

// A thread's run time so far in microseconds (0: none, or not known).
static uint64_t thread_us(SceUID id) {
    if (id < 0) return 0;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    return sceKernelGetThreadInfo(id, &info) < 0 ? 0 : (uint64_t) info.runClocks;
}

// Each core's share of the last stretch, in percent: [0] the game, [1] the sound and EXPAND's worker, [2] mGBA's renderer.
static void core_load(unsigned out[3], uint64_t elapsedUs) {
    static uint64_t was[4];
    uint64_t now[4] = { thread_us(gameThreadId), thread_us(audio_thread_id()), thread_us(wideThreadId), thread_us(rendererThreadId) };
    uint64_t d[4];
    for (int i = 0; i < 4; i++) d[i] = now[i] >= was[i] ? now[i] - was[i] : 0, was[i] = now[i];
    uint64_t per[3] = { d[0], d[1] + d[2], d[3] };
    for (int i = 0; i < 3; i++) out[i] = elapsedUs ? (unsigned) (per[i] * 100 / elapsedUs) : 0;
    for (int i = 0; i < 3; i++) if (out[i] > 100) out[i] = 100;
}

// FPS / CPU over the game: the three cores the port uses, as the meter's one number hid two of them.
static const uint32_t* perf_picture(unsigned fpsTenths, const unsigned load[3], int* w) {
    static uint32_t px[VIDEO_PERF_W * 16];
    struct pd_canvas c;
    pd_canvas_init(&c, VIDEO_PERF_W, 16, px);
    memset(px, 0, sizeof px);
    char text[64];
    snprintf(text, sizeof text, "%u.%u FPS  CPU %u / %u / %u%%", fpsTenths / 10, fpsTenths % 10, load[0], load[1], load[2]);
    *w = pd_text_width(text) + 8;
    if (*w > VIDEO_PERF_W) *w = VIDEO_PERF_W;
    pd_round_rect(&c, 0, 0, *w, 14, 2, 0xB8121620u);
    pd_text(&c, 4, (14 - 10) / 2 - 4, text, 0xF8F8F8, 0x303840);
    for (int i = 0; i < VIDEO_PERF_W * 16; i++)
        if (px[i] && !(px[i] >> 24)) px[i] |= 0xFF000000u;
    return px;
}

// NATIVE MIXER off (vita.ini): the shared session's calls into pd_mixer.c do nothing, and mGBA emulates the game's own
// mixer. Linked with --wrap (CMakeLists.txt), so the shared code stays as it is.
bool __real_pd_mixer_attach(struct mCore* core);
void __real_pd_mixer_frame(struct mCore* core);
void __real_pd_mixer_release(struct mCore* core);
static bool mixerAttached;
bool __wrap_pd_mixer_attach(struct mCore* core) {
    mixerAttached = vitaSettings.nativeMixer && __real_pd_mixer_attach(core);
    return mixerAttached;
}
void __wrap_pd_mixer_frame(struct mCore* core) {
    if (vitaSettings.nativeMixer) __real_pd_mixer_frame(core);
}
void __wrap_pd_mixer_release(struct mCore* core) {
    if (vitaSettings.nativeMixer) __real_pd_mixer_release(core);
}

// EXPAND's margins on a thread of their own (the 3DS gives them the New 3DS's fourth core): pd_session starts each frame's
// finish (wideGo) and waits for it (wideJoin) before the next one and before the window's picture is shown. On core 1,
// beside the sound's thread (which wakes for a moment every 10 ms); the game keeps core 0, mGBA's renderer core 2.
static Thread wideThread;
static Mutex wideMutex;
static Condition wideStart, wideDone;
static bool wideBusy, wideRunning;

static THREAD_ENTRY wide_main(void* ctx) {
    (void) ctx;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);
    MutexLock(&wideMutex);
    for (;;) {
        while (!wideBusy) ConditionWait(&wideStart, &wideMutex);
        MutexUnlock(&wideMutex);
        pd_session_wide_finish();
        MutexLock(&wideMutex);
        wideBusy = false;
        ConditionWake(&wideDone);
    }
    return 0;
}

static bool wide_thread_start(void) {
    if (wideRunning) return true;
    MutexInit(&wideMutex);
    ConditionInit(&wideStart);
    ConditionInit(&wideDone);
    creatingWide = true;
    wideRunning = !ThreadCreate(&wideThread, wide_main, NULL);
    creatingWide = false;
    return wideRunning;
}

static void wide_go(void) {
    MutexLock(&wideMutex);
    wideBusy = true;
    ConditionWake(&wideStart);
    MutexUnlock(&wideMutex);
}

static void wide_join(void) {
    if (!wideRunning) return;
    MutexLock(&wideMutex);
    while (wideBusy) ConditionWait(&wideDone, &wideMutex);
    MutexUnlock(&wideMutex);
}

// A stereo PCM16 WAV at the Vita's rate.
static bool write_wav(const char* path, const int16_t* px, size_t frames) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t data = (uint32_t) (frames * 4), rate = AUDIO_RATE, byteRate = AUDIO_RATE * 4;
    uint8_t h[44] = { 'R', 'I', 'F', 'F' };
    uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtLen = 16;
    uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    memcpy(h + 16, &fmtLen, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byteRate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    bool ok = fwrite(h, 1, 44, f) == 44 && fwrite(px, 4, frames, f) == frames;
    return fclose(f) == 0 && ok;
}

// --- logging: mGBA's default logger prints every unmapped I/O write ---

static void noopLog(struct mLogger* log, int category, enum mLogLevel level, const char* format, va_list args) {
    (void) log; (void) category; (void) level; (void) format; (void) args;
}
static struct mLogger silentLogger = { .log = noopLog, .filter = NULL };

// --- audio: the core's sound into audio.c's ring ---

static bool hasSound;
static bool audioMuted; // while fast-forwarding
static struct mAVStream stream;
static struct mCore* audioCore;
static double audioBase; // the rate the core's sound is made at for a frame a refresh (before audio_rate_trim)

// mGBA's GBA sound calls this in blip_add_delta's place (CMakeLists.txt): at the GBA's 65536 samples a second four in
// five deltas are nothing, and skipping those can't change a sample.
void pd_blip_delta(blip_t* blip, unsigned clock, int delta) {
    if (delta) blip_add_delta(blip, clock, delta);
}

static void post_audio(struct mAVStream* s, blip_t* left, blip_t* right) {
    (void) s;
    static int16_t chunk[AUDIO_PERIOD * 2];
    int n = blip_samples_avail(left);
    if (n > AUDIO_PERIOD) n = AUDIO_PERIOD;
    blip_read_samples(left, chunk, n, true);
    blip_read_samples(right, chunk + 1, n, true);
    if (hasSound && !audioMuted) audio_push(chunk, (size_t) n);
    // The ring kept near its mark by making the sound a hair faster or slower (half a percent at most, unheard).
    if (audioCore && hasSound) {
        double rate = audioBase * audio_rate_trim();
        blip_set_rates(left, audioCore->frequency(audioCore), rate);
        blip_set_rates(right, audioCore->frequency(audioCore), rate);
    }
}

// A Game Boy game's sound comes from pk_gb_audio (never its core's own audio channels).
static struct pk_gb_audio gbAudio;
static bool gbAudioOn;

static void gb_audio_pump(void) {
    if (!gbAudioOn) return;
    static int16_t chunk[AUDIO_PERIOD * 2];
    for (;;) {
        int n = pk_gb_audio_read(&gbAudio, chunk, AUDIO_PERIOD);
        if (n <= 0) return;
        if (hasSound && !audioMuted) audio_push(chunk, (size_t) n);
        if (n < AUDIO_PERIOD) return;
    }
}

// GAME VOLUME / MUTE GAME.
static void apply_volume(void) {
    audio_set_volume(settings.gameVolume, settings.gameMuted);
}

// --- files ---

static void make_dirs(void) {
    mkdir("ux0:data", 0777);
    mkdir(BASE_DIR, 0777);
    mkdir(ROM_DIR, 0777);
    mkdir(BACKUP_DIR, 0777);
    mkdir(STATE_DIR, 0777);
    mkdir(ROMART_DIR, 0777);
    mkdir(CARD_DIR, 0777);
    mkdir(BASE_DIR "/cheats", 0777);
    pd_romart_cache_dir(ROMART_DIR);
}

// What the app plays: a GBA game, or a Game Boy / Color one.
static bool is_game_file(const char* name) {
    size_t n = strlen(name);
    if (n > 3 && !strcasecmp(name + n - 3, ".gb")) return true;
    if (n > 4 && !strcasecmp(name + n - 4, ".gbc")) return true;
    return n > 4 && !strcasecmp(name + n - 4, ".gba");
}

static int cmp_str(const void* a, const void* b) {
    return strcasecmp(*(const char* const*) a, *(const char* const*) b);
}

// The games in ROM_DIR, sorted; each entry is malloc'd.
static int list_roms(char** out) {
    int n = 0;
    DIR* d = opendir(ROM_DIR);
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < MAX_ROMS) {
        if (e->d_name[0] != '.' && is_game_file(e->d_name)) out[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(out, (size_t) n, sizeof(char*), cmp_str);
    return n;
}

static bool copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return false;
    FILE* out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    static char buf[64 * 1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

// Whether name is one of base's backups: exactly "<base>-YYYYmmdd-HHMMSS.sav"
// (a prefix match would count "<base>-2"'s backups as base's and prune them).
static bool is_backup_of(const char* name, const char* base) {
    size_t baseLen = strlen(base);
    if (strncmp(name, base, baseLen)) return false;
    const char* rest = name + baseLen;
    static const char PATTERN[] = "-########-######.sav";
    if (strlen(rest) != sizeof(PATTERN) - 1) return false;
    for (size_t i = 0; PATTERN[i]; i++) {
        if (PATTERN[i] == '#' ? (rest[i] < '0' || rest[i] > '9') : rest[i] != PATTERN[i]) return false;
    }
    return true;
}

// Save files are sacred (CLAUDE.md): every start - and every state load - copies the save into BACKUP_DIR first,
// keeping the newest MAX_BACKUPS per game.
static void backup_save(const char* base, const char* savePath) {
    struct stat st;
    if (stat(savePath, &st) || st.st_size == 0) return;
    char stamp[32];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    char to[512];
    snprintf(to, sizeof(to), BACKUP_DIR "/%s-%s.sav", base, stamp);
    if (!copy_file(savePath, to)) host_log("backup: couldn't copy %s", savePath);

    // Prune: the names sort by time, so the oldest come first.
    char* names[64];
    int n = 0;
    DIR* d = opendir(BACKUP_DIR);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && n < 64) {
        if (is_backup_of(e->d_name, base)) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t) n, sizeof(char*), cmp_str);
    for (int i = 0; i < n; i++) {
        if (i < n - MAX_BACKUPS) {
            char p[512];
            snprintf(p, sizeof(p), BACKUP_DIR "/%s", names[i]);
            remove(p);
        }
        free(names[i]);
    }
}

// --- input ---

// The 3DS port's buttons, which the companion and the session were written for, from the Vita's: cross / circle are B /
// A by position or A / B as mGBA's Vita build has them; triangle is X (the companion's buttons / BACK), square is Y (the
// QUICK MENU). The right stick: left = ZL (the tab before), right = ZR (FF held), up / down = the LAYOUT.
enum {
    K_A = 1 << 0, K_B = 1 << 1, K_X = 1 << 2, K_Y = 1 << 3, K_L = 1 << 4, K_R = 1 << 5, K_ZL = 1 << 6, K_ZR = 1 << 7,
    K_START = 1 << 8, K_SELECT = 1 << 9, K_UP = 1 << 10, K_DOWN = 1 << 11, K_LEFT = 1 << 12, K_RIGHT = 1 << 13,
    K_LAYOUT_UP = 1 << 14, K_LAYOUT_DOWN = 1 << 15,
};

static SceCtrlData pad;
static uint32_t lastHeld;

// The buttons held now and pressed since the last call.
static void read_buttons(uint32_t* held, uint32_t* down) {
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = pad.buttons, k = 0;
    bool pos = vitaSettings.positional;
    if (b & SCE_CTRL_CROSS) k |= pos ? K_B : K_A;
    if (b & SCE_CTRL_CIRCLE) k |= pos ? K_A : K_B;
    if (b & SCE_CTRL_TRIANGLE) k |= K_X;
    if (b & SCE_CTRL_SQUARE) k |= K_Y;
    if (b & SCE_CTRL_LTRIGGER) k |= K_L;
    if (b & SCE_CTRL_RTRIGGER) k |= K_R;
    if (b & SCE_CTRL_START) k |= K_START;
    if (b & SCE_CTRL_SELECT) k |= K_SELECT;
    if (b & SCE_CTRL_UP) k |= K_UP;
    if (b & SCE_CTRL_DOWN) k |= K_DOWN;
    if (b & SCE_CTRL_LEFT) k |= K_LEFT;
    if (b & SCE_CTRL_RIGHT) k |= K_RIGHT;
    // The left stick is the D-pad too (as the 3DS's circle pad is).
    if (pad.lx < 64) k |= K_LEFT;
    if (pad.lx > 192) k |= K_RIGHT;
    if (pad.ly < 64) k |= K_UP;
    if (pad.ly > 192) k |= K_DOWN;
    if (pad.rx < 64) k |= K_ZL;
    if (pad.rx > 192) k |= K_ZR;
    if (pad.ry < 64) k |= K_LAYOUT_UP;
    if (pad.ry > 192) k |= K_LAYOUT_DOWN;
    *held = k;
    *down = k & ~lastHeld;
    lastHeld = k;
}

// The left stick for the QUICK MENU's ring: degrees clockwise from up, -1 near the middle.
static int stick_angle(void) {
    int dx = (int) pad.lx - 128, dy = 128 - (int) pad.ly;
    if (dx * dx + dy * dy < 72 * 72) return -1;
    float a = atan2f((float) dx, (float) dy) * 180.0f / 3.14159265f;
    return (int) (a < 0 ? a + 360 : a) % 360;
}

// The front touch screen: true while a finger is down on it, (x, y) on the 960x544 screen.
static bool read_touch(int* x, int* y) {
    SceTouchData t;
    memset(&t, 0, sizeof t);
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 0 || t.reportNum < 1) return false;
    // The panel reports 1920x1088 for the screen's 960x544.
    *x = t.report[0].x / 2;
    *y = t.report[0].y / 2;
    if (*x >= VIDEO_W) *x = VIDEO_W - 1;
    if (*y >= VIDEO_H) *y = VIDEO_H - 1;
    return true;
}

// The GBA's keys (PD_KEY_*: GBA_KEY_* bits) from the 3DS-style ones.
static uint32_t gba_keys(uint32_t k) {
    uint32_t g = 0;
    if (k & K_A) g |= PD_KEY_A;
    if (k & K_B) g |= PD_KEY_B;
    if (k & K_SELECT) g |= PD_KEY_SELECT;
    if (k & K_START) g |= PD_KEY_START;
    if (k & K_RIGHT) g |= PD_KEY_RIGHT;
    if (k & K_LEFT) g |= PD_KEY_LEFT;
    if (k & K_UP) g |= PD_KEY_UP;
    if (k & K_DOWN) g |= PD_KEY_DOWN;
    if (k & K_R) g |= PD_KEY_R;
    if (k & K_L) g |= PD_KEY_L;
    return g;
}

// --- the hint strip (under the library's page) ---

// The page's bottom: a strip of what the buttons do, the app's letters (A, B, X, Y) drawn as the Vita's buttons.
#define LIB_AREA_H 256 // the library's own page above it
#define HINT_STRIP_H (VIDEO_PAGE_H - LIB_AREA_H)

// A face button, 11 px: a dark disc, its symbol in its own colour.
static void button_icon(struct pd_canvas* c, int x, int y, char letter) {
    // The app's letter -> the Vita's button: A / B by vita.ini's positional, X = triangle, Y = square.
    char b = letter == 'A' ? (vitaSettings.positional ? 'O' : 'C') : letter == 'B' ? (vitaSettings.positional ? 'C' : 'O')
           : letter == 'X' ? 'T' : 'S';
    static const char* const DISC[11] = { "...xxxxx...", ".xxxxxxxxx.", ".xxxxxxxxx.", "xxxxxxxxxxx", "xxxxxxxxxxx", "xxxxxxxxxxx",
                                          "xxxxxxxxxxx", "xxxxxxxxxxx", ".xxxxxxxxx.", ".xxxxxxxxx.", "...xxxxx..." };
    static const char* const SYM[4][7] = {
        { "x.....x", ".x...x.", "..x.x..", "...x...", "..x.x..", ".x...x.", "x.....x" }, // cross
        { "..xxx..", ".x...x.", "x.....x", "x.....x", "x.....x", ".x...x.", "..xxx.." }, // circle
        { "...x...", "...x...", "..x.x..", "..x.x..", ".x...x.", ".x...x.", "xxxxxxx" }, // triangle
        { "xxxxxxx", "x.....x", "x.....x", "x.....x", "x.....x", "x.....x", "xxxxxxx" }, // square
    };
    static const uint32_t COLOR[4] = { 0x8EC0F0, 0xFF7A7A, 0x52E0A8, 0xE89CF2 };
    int k = b == 'C' ? 0 : b == 'O' ? 1 : b == 'T' ? 2 : 3;
    for (int r = 0; r < 11; r++)
        for (int q = 0; q < 11; q++)
            if (DISC[r][q] == 'x') pd_fill(c, x + q, y + r, 1, 1, 0x20232C);
    for (int r = 0; r < 7; r++)
        for (int q = 0; q < 7; q++)
            if (SYM[k][r][q] == 'x') pd_fill(c, x + 2 + q, y + 2 + r, 1, 1, COLOR[k]);
}

// The strip: [text] in the 3DS hint bar's words ("A: PLAY   L R: VIEWS"), each "A:" / "B:" / "X:" / "Y:" an icon.
static void hint_strip(struct pd_canvas* c, int y, const char* text) {
    bool dark = backdrop_dark();
    pd_fill(c, 0, y, c->w, HINT_STRIP_H, dark ? 0x14161C : 0x353845);
    pd_fill(c, 0, y, c->w, 1, dark ? 0x30343C : 0x4A4E5C);
    // Measured first, then centred.
    int width = 0;
    for (int pass = 0; pass < 2; pass++) {
        int x = pass ? (c->w - width) / 2 : 0;
        if (x < 4) x = 4;
        const char* p = text;
        while (*p) {
            const char* sp = strchr(p, ' ');
            size_t n = sp ? (size_t) (sp - p) : strlen(p);
            char word[64];
            snprintf(word, sizeof word, "%.*s", (int) (n < sizeof word ? n : sizeof word - 1), p);
            if (n == 2 && word[1] == ':' && strchr("ABXY", word[0])) {
                if (pass) button_icon(c, x, y + (HINT_STRIP_H - 11) / 2 + 1, word[0]);
                x += 14;
            } else if (n) {
                if (pass) pd_text(c, x, pd_text_y(y, HINT_STRIP_H), word, 0xF8F8F8, dark ? 0x000000 : 0x202228);
                x += pd_text_width(word);
            }
            if (!sp) break;
            x += pd_text_width(" ") > 0 ? pd_text_width(" ") : 4;
            p = sp + 1;
        }
        width = x - (pass ? 0 : 0);
    }
}

// Before every frame: the backdrop follows THEME, DARK MODE and BACKGROUND as they're changed.
static void follow_look(void) {
    video_set_backdrop(!settings.backgroundOff);
    video_set_frame(settings.overlayFrame);
    backdrop_set_theme(settings.theme, settings.darkMode);
}

// --- messages ---

// The page's see-through: the library fills its backdrop with KEY (near its own page colour, so what the library mixes
// with it - shadows - stays close to what it would be); KEY and KEY dimmed once or twice (the library's pop-ups) become
// see-through and black at the dim's alpha over the animated backdrop.
static uint32_t page_key(void) {
    return pd_dark ? 0x12141D : 0xF7F6FA;
}

static uint32_t dim_of(uint32_t p) {
    struct pd_canvas c;
    pd_canvas_init(&c, 1, 1, &p);
    pd_dim(&c, 150);
    return p;
}

static void page_upload(void) {
    static uint32_t out[VIDEO_PAGE_W * VIDEO_PAGE_H];
    uint32_t key = page_key(), dim1 = dim_of(key), dim2 = dim_of(dim1);
    bool clear = video_backdrop_shown();
    for (int i = 0; i < VIDEO_PAGE_W * VIDEO_PAGE_H; i++) {
        uint32_t p = pagePx[i] & 0xFFFFFF;
        if (clear && p == key) out[i] = 0;
        else if (clear && p == dim1) out[i] = 150u << 24;
        else if (clear && p == dim2) out[i] = 210u << 24;
        else if (p == key) out[i] = 0xFF000000u | (pd_dark ? 0x12141C : 0xF7F6FB);
        else out[i] = 0xFF000000u | p;
    }
    video_set_page(out);
}

// A line on the page while something takes a moment (nothing to press).
static void show_note(const char* line1, const char* line2) {
    struct pd_canvas page;
    pd_canvas_init(&page, VIDEO_PAGE_W, VIDEO_PAGE_H, pagePx);
    pd_fill(&page, 0, 0, VIDEO_PAGE_W, VIDEO_PAGE_H, page_key());
    int w = 440, h = 72, x = (VIDEO_PAGE_W - w) / 2, y = (VIDEO_PAGE_H - h) / 2;
    int f = pd_list_box(&page, x, y, w, h);
    pd_text_fit(&page, x + f + 8, y + f + 10, w - 2 * f - 16, line1, PD_TITLE_TEXT, PD_TITLE_SHADOW);
    pd_text_fit(&page, x + f + 8, y + f + 34, w - 2 * f - 16, line2 ? line2 : "", PD_VALUE, PD_VALUE_SHADOW);
    video_page_swap(VIDEO_SWAP_CUT);
    page_upload();
    video_draw_page();
}

static void show_message(const char* line1, const char* line2) {
    struct pd_canvas page;
    pd_canvas_init(&page, VIDEO_PAGE_W, VIDEO_PAGE_H, pagePx);
    pd_fill(&page, 0, 0, VIDEO_PAGE_W, VIDEO_PAGE_H, page_key());
    int w = 440, h = 96, x = (VIDEO_PAGE_W - w) / 2, y = (VIDEO_PAGE_H - h) / 2;
    int f = pd_list_box(&page, x, y, w, h);
    pd_text_fit(&page, x + f + 8, y + f + 10, w - 2 * f - 16, line1, PD_TITLE_TEXT, PD_TITLE_SHADOW);
    pd_text_fit(&page, x + f + 8, y + f + 34, w - 2 * f - 16, line2 ? line2 : "", PD_VALUE, PD_VALUE_SHADOW);
    pd_text(&page, x + f + 8, y + h - f - 26, "Press any button.", PD_MUTED, PD_MUTED_SHADOW);
    page_upload();
    uint32_t held, down;
    read_buttons(&held, &down);
    for (int i = 0; i < 600; i++) {
        video_draw_page();
        read_buttons(&held, &down);
        if (down) break;
    }
}

static void game_failed(const char* title, const char* why) {
    char detail[480];
    snprintf(detail, sizeof detail, "%s. PokeDaisy %s (PS Vita), heap in use %u KiB", why, PD_VERSION,
             (unsigned) (host_heap_used() >> 10));
    host_log("game: didn't load: %s", detail);
    show_message(title, detail);
}

// --- a game's files ---

struct game_files {
    char base[256];
    char save[520];
};

static void state_path(void* ctx, int slot, char* vf, size_t vfLen, char* path, size_t pathLen) {
    const struct game_files* f = ctx;
    snprintf(path, pathLen, STATE_DIR "/%s.ss%d", f->base, slot + 1);
    snprintf(vf, vfLen, "%s", path);
}

static void backup_for_load(void* ctx) {
    const struct game_files* f = ctx;
    backup_save(f->base, f->save);
}

static void settings_changed(void* ctx) {
    (void) ctx;
    pd_settings_save(&settings, SETTINGS_FILE);
}

static void cheat_path(void* ctx, uint32_t crc, char* path, size_t len) {
    (void) ctx;
    snprintf(path, len, BASE_DIR "/cheats/%08lX.cheats", (unsigned long) crc);
}

static void nuzlocke_path(void* ctx, uint32_t crc, char* path, size_t len) {
    (void) ctx;
    mkdir(BASE_DIR "/nuzlocke", 0777);
    snprintf(path, len, BASE_DIR "/nuzlocke/%08lX.txt", (unsigned long) crc);
}

static void flight_area_path(void* ctx, uint32_t crc, uint32_t key, char* path, size_t len) {
    (void) ctx;
    mkdir(BASE_DIR "/flight-area", 0777);
    snprintf(path, len, BASE_DIR "/flight-area/%08lX-%08lX.txt", (unsigned long) crc, (unsigned long) key);
}

static uint64_t session_clock(void) {
    return host_us();
}

// A Game Boy cart's own screen, not a Super Game Boy border (pk_gb_config).
static void pk_gb_settings(struct mCore* core) {
    if (core->platform(core) != mPLATFORM_GB) return;
    mCoreConfigSetIntValue(&core->config, "sgb.borders", 0);
    core->reloadConfigOption(core, "sgb.borders", &core->config);
    // mGBA's 0x100 overflows the GB mixer's int16 in loud passages (PK_GB_VOLUME).
    mCoreConfigSetIntValue(&core->config, "volume", 0x80);
    core->reloadConfigOption(core, "volume", &core->config);
}

// What a game start works out over the whole ROM - its SHA-1 and CRC-32 - kept per file by its size and time
// (rom-art/<file>.id: "size mtime sha1 crc"), so the next start of the same file skips both passes.
struct rom_id {
    long size;
    long long mtime;
    char sha1[41];
    uint32_t crc;
    bool known;
};

static char pickedSha1[41]; // the library's hash of the picked game's file ("" until it has one)

static void rom_id_path(const char* file, char* path, size_t len) {
    snprintf(path, len, ROMART_DIR "/%s.id", file);
}

static void rom_id_read(const char* romPath, const char* file, struct rom_id* id) {
    memset(id, 0, sizeof(*id));
    struct stat st;
    if (stat(romPath, &st)) return;
    id->size = (long) st.st_size;
    id->mtime = (long long) st.st_mtime;
    if (!id->mtime) return;
    char path[400];
    rom_id_path(file, path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return;
    long size = 0;
    long long mtime = 0;
    char sha1[41] = "";
    unsigned crc = 0;
    if (fscanf(f, "%ld %lld %40s %x", &size, &mtime, sha1, &crc) == 4 && size == id->size && mtime == id->mtime &&
        strlen(sha1) == 40 && crc && (!pickedSha1[0] || !strcasecmp(sha1, pickedSha1))) {
        memcpy(id->sha1, sha1, sizeof(sha1));
        id->crc = crc;
        id->known = true;
    }
    fclose(f);
}

static void rom_id_write(const char* file, const struct rom_id* id, const char* sha1, uint32_t crc) {
    if (strlen(sha1) != 40 || !crc || !id->size || !id->mtime) return;
    char path[400];
    rom_id_path(file, path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%ld %lld %s %08x\n", id->size, id->mtime, sha1, (unsigned) crc);
    fclose(f);
}

static void write_card(const char* file, const struct pd_game* g, const struct pd_snapshot* s);

static float refreshHz = 60.0f / 1.001f;

// Runs one game until the player leaves.
static void run_game(const char* file) {
    char romPath[512], savePath[512], base[256];
    snprintf(romPath, sizeof(romPath), ROM_DIR "/%s", file);
    snprintf(base, sizeof(base), "%s", file);
    char* dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(savePath, sizeof(savePath), ROM_DIR "/%s.sav", base);

    if (!romBuffer) {
        game_failed("The game didn't start:", "there was no room for the 32 MB game buffer when PokeDaisy started");
        return;
    }
    // The game's own menu click (SE_SELECT), rendered from its ROM the first time it's played: before its core, which
    // would share the ROM buffer with the one that renders it.
    {
        size_t n = strlen(file);
        bool gba = n > 4 && !strcasecmp(file + n - 4, ".gba");
        if (gba && !sfx_has_own(base)) {
            show_note("Getting the game's menu click...", "Once per game: its own sound, from its ROM.");
            sfx_render(romPath, base);
        }
        sfx_use(base);
    }
    struct VFile* rom = VFileOpen(romPath, O_RDONLY);
    if (!rom) {
        char why[600];
        snprintf(why, sizeof why, "couldn't open %s (%s)", romPath, strerror(errno));
        game_failed("Couldn't open the game:", why);
        return;
    }
    uint8_t header[0x150] = { 0 }; // a GBA cart's header, and a Game Boy cart's (its logo at 0x104)
    rom->seek(rom, 0, SEEK_SET);
    rom->read(rom, header, sizeof(header));
    rom->seek(rom, 0, SEEK_SET);
    struct pd_game game;
    pd_game_detect(&game, header, (size_t) rom->size(rom));

    struct rom_id romId;
    rom_id_read(romPath, file, &romId);
    backup_save(base, savePath);

    host_log("game: %s, heap in use %u KiB", file, (unsigned) (host_heap_used() >> 10));
    struct mCore* core = mCoreFindVF(rom);
    if (!core || !core->init(core)) {
        rom->close(rom);
        show_message("Not a game PokeDaisy plays:", file);
        return;
    }
    mCoreInitConfig(core, NULL);
    // mGBA's renderer on another core (its video proxy), as its 3DS front end has it on a New 3DS: vita.ini's choice.
    if (vitaSettings.threadedVideo) mCoreConfigSetIntValue(&core->config, "threadedVideo", 1);
    core->setVideoBuffer(core, (color_t*) video_game_buffer(), VIDEO_GAME_STRIDE);
    pk_gb_settings(core);
    core->setAudioBufferSize(core, AUDIO_PERIOD);
    // The whole ROM in memory: the memory card's reads are slow, and the companion reads the ROM's tables.
    if (!mCorePreloadVF(core, rom)) {
        core->deinit(core);
        char why[320];
        snprintf(why, sizeof why, "mGBA couldn't read %s into memory (too big, or the memory card read failed)", file);
        game_failed("The game didn't start:", why);
        return;
    }
    // As mGBA's own front ends have it (gui-runner.c): skips the busy loops games wait in.
    if (core->platform(core) == mPLATFORM_GBA) ((struct GBA*) core->board)->idleOptimization = IDLE_LOOP_DETECT;
    struct VFile* save = VFileOpen(savePath, O_CREAT | O_RDWR);
    if (save) core->loadSave(core, save);
    bool gb = core->platform(core) == mPLATFORM_GB;
    video_set_bars(gb ? 0xFFFFFF : 0);
    // A game frame a refresh: its 59.73 frames' sound in the refreshes the screen shows (both Game Boys' frames are the
    // GBA's length in time).
    double ratio = GBAAudioCalculateRatio(1, refreshHz, 1);
    audioBase = AUDIO_RATE * ratio;
    audio_clear();
    if (gb) {
        gbAudioOn = pk_gb_audio_attach(&gbAudio, core, audioBase);
        audioCore = NULL;
    } else {
        stream.postAudioBuffer = post_audio;
        core->setAVStream(core, &stream);
        blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), audioBase);
        blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), audioBase);
        audioCore = core;
    }
    core->reset(core);
    unsigned videoW = 240, videoH = 160;
    core->desiredVideoDimensions(core, &videoW, &videoH);
    video_set_game_size((int) videoW, (int) videoH);
    pk_heal_rtc(core);
    host_log("game: %s on mGBA, %ux%u, sound at %.1f Hz for %.3f Hz", gb ? "Game Boy" : "GBA", videoW, videoH, audioBase,
             refreshHz);

    static struct pd_session session;
    memset(&session, 0, sizeof(session));
    session.game = game;
    if (romId.known) memcpy(session.game.sha1, romId.sha1, sizeof(romId.sha1));
    else session.game.sha1[0] = 0;
    static struct game_files gameFiles;
    snprintf(gameFiles.base, sizeof(gameFiles.base), "%s", base);
    snprintf(gameFiles.save, sizeof(gameFiles.save), "%s", savePath);
    bool wideThreaded = wide_thread_start();
    struct pd_session_host files = { state_path, backup_for_load, settings_changed, cheat_path, &gameFiles,
                                     wideThreaded ? wide_go : NULL, wideThreaded ? wide_join : NULL, session_clock,
                                     flight_area_path, nuzlocke_path };
    session.host = (struct pd_host_info) { .count = 2, .oneScreen = true };
    session.oneScreen = true;
    session.host.labels[0] = "FILE";
    snprintf(session.host.values[0], sizeof(session.host.values[0]), "%s", file);
    session.host.labels[1] = "SPEED";
    snprintf(session.host.values[1], sizeof(session.host.values[1]), "-");
    pd_session_start(&session, core, video_game_buffer(), &settings, &files);
    if (pickedSha1[0] && strcasecmp(session.game.sha1, pickedSha1))
        host_log("game: the running ROM hashes to %s, its file to %s (not kept)", session.game.sha1, pickedSha1);
    else if (!romId.known)
        rom_id_write(file, &romId, session.game.sha1, session.romCrc);
    {
        char steps[300];
        int n = 0;
        for (int i = 0; i < session.prof.startSteps && n < (int) sizeof(steps) - 32; i++)
            n += snprintf(steps + n, sizeof(steps) - (size_t) n, " %s %lu", session.prof.startName[i],
                          (unsigned long) (session.prof.startTicks[i] / 1000));
        host_log("start (ms):%s", steps);
    }
    apply_volume();
    struct pd_ui* uiPtr = &session.ui;
    struct pd_host_info* hostPtr = &session.host;
    // The companion's canvas: its view's size (the window's or the panel's), opened over the game.
    struct pd_canvas bottom;
    int compW = 0, compH = 0;
    video_set_companion_view(VIDEO_COMPANION_CLOSED);

    int touchOn = VIDEO_HIT_NONE; // what the finger went down on (it stays that one's until let go)
    // The companion's picture as last shown (its transitions): tab, its own page, overlay, size; when the last tab switch was.
    bool compShown = false;
    int compTab = -1, compSub = 0, compOver = 0, compShownW = 0, compShownH = 0;
    uint64_t compSwitchAt = 0;
    unsigned fpsFrames = 0;
    uint64_t busyUs = 0, drawUs = 0, drawUsSum = 0;
    unsigned loadSum[3] = { 0, 0, 0 };
    gameThreadId = sceKernelGetThreadId();
    const uint32_t* perfPicture = NULL;
    int perfWidth = 0;
    uint64_t fpsStart = host_ms();
    uint64_t lastTick = fpsStart;
    uint32_t withheld = 0;
    unsigned gapsLogged = 0, secondsRun = 0, gapsSum = 0, pctMin = 999, cpuMax = 0;
    uint64_t cpuSum = 0;
    audio_take_gaps();
    bool capturing = vitaSettings.soundCapture && hasSound && audio_capture_start(20);
    if (capturing) host_log("sound: recording the first 20 s (native mixer %s)", mixerAttached ? "on" : "off");
    // Falling behind: what the loops are late by builds up, and each whole refresh of it is a frame run undrawn next loop
    // (two at most), as the 3DS port does on an Old 3DS - the game and its sound keep their speed, the picture skips. mGBA
    // leaves an undrawn frame unrendered (its frameskip counter). A long pause (the PS button, a load) owes nothing.
    const int64_t refreshUs = (int64_t) (1000000.0 / refreshHz);
    int64_t behindUs = 0;
    uint64_t loopAt = 0;
    unsigned skippedSum = 0;
    uint64_t videoUs = 0, videoUsSum = 0, loopUs = 0, loopUsSum = 0;
    // No memory card writes for the log while the game runs (host_log_hold).
    host_log_hold(true);

    for (;;) {
        int catchUp = 0;
        {
            uint64_t t = host_us();
            if (loopAt) {
                int64_t over = (int64_t) (t - loopAt) - refreshUs;
                loopUs += t - loopAt;
                if (over > 6 * refreshUs) {
                    behindUs = 0;
                } else {
                    behindUs += over;
                    if (behindUs < 0) behindUs = 0;
                    if (behindUs > 2 * refreshUs) behindUs = 2 * refreshUs;
                }
                if (behindUs >= refreshUs && !gb) {
                    catchUp = behindUs >= 2 * refreshUs ? 2 : 1;
                    behindUs -= catchUp * refreshUs;
                }
            }
            loopAt = t;
        }
        uint32_t held, down;
        read_buttons(&held, &down);

        // The companion, as the app opens it on a one-screen device: triangle (X) opens it in COMPANION VIEW's view (the
        // QUICK MENU's COMPANION and the HUD's parts too) and its buttons are the companion's: the D-pad and A its cursor,
        // L / R the tabs, B back. The WINDOW closes on triangle, START, or B with nothing left to go back from; the SIDE
        // PANEL stays beside the game and gives the buttons back on those (triangle takes them again), START closes it.
        int want = settings.companionPanel ? VIDEO_COMPANION_PANEL : VIDEO_COMPANION_WINDOW;
        bool open = video_companion_view() != VIDEO_COMPANION_CLOSED;
        if (open && video_companion_view() != want) video_set_companion_view(want); // COMPANION VIEW changed with it open
        bool openNow = false, closeNow = false, giveBack = false;
        if (session.companionAsk) {
            session.companionAsk = false;
            openNow = true;
        }
        if (uiPtr->keyFocus && open) {
            static const uint32_t DIRS[4] = { K_UP, K_DOWN, K_LEFT, K_RIGHT };
            static int heldFor[4];
            bool panel = video_companion_view() == VIDEO_COMPANION_PANEL;
            if (down & (K_START | K_X | K_B | K_A | K_L | K_R | K_ZL | K_ZR)) sfx_click(settings.clickSound);
            if (down & K_START) closeNow = true;
            else if (down & K_X) panel ? (void) (giveBack = true) : (void) (closeNow = true);
            else if ((down & K_B) && !pd_ui_back(uiPtr)) panel ? (void) (giveBack = true) : (void) (closeNow = true);
            else if (down & K_A) pd_ui_focus_press(uiPtr);
            else if (down & (K_L | K_R | K_ZL | K_ZR)) pd_ui_next_tab(uiPtr, down & (K_L | K_ZL) ? -1 : 1);
            for (int i = 0; i < 4; i++) {
                heldFor[i] = held & DIRS[i] ? heldFor[i] + 1 : 0;
                if ((down & DIRS[i]) || (heldFor[i] >= 24 && heldFor[i] % 6 == 0)) {
                    pd_ui_focus_move(uiPtr, i);
                    session.redraw = true;
                }
            }
            if (down) session.redraw = true;
            withheld |= held;
        } else if (down & K_X) {
            openNow = true; // closed: opens; the panel open without the buttons: takes them
            withheld |= held;
            sfx_click(settings.clickSound);
        } else if (open && (down & K_ZL)) {
            pd_ui_next_tab(uiPtr, -1);
            session.redraw = true;
        }
        // Keys pressed on the companion stay away from the game until they're let go.
        withheld &= held;

        // The finger: on the companion it's the 3DS's stylus; the window's X, the panel's tab and the dim around the
        // window close it; on the game it taps the HUD and the QUICK MENU.
        int sx, sy;
        bool touchingNow = read_touch(&sx, &sy);
        if (touchingNow && touchOn == VIDEO_HIT_NONE) touchOn = video_hit(sx, sy);
        if (touchOn == VIDEO_HIT_COMPANION) {
            int cx, cy;
            if (touchingNow && video_to_companion(sx, sy, &cx, &cy)) session.redraw |= pd_ui_touch(uiPtr, cx, cy, true);
            else if (!touchingNow) {
                session.redraw |= pd_ui_touch(uiPtr, 0, 0, false);
                sfx_click(settings.clickSound);
            }
        } else if (touchOn == VIDEO_HIT_GAME) {
            int tx, ty;
            if (touchingNow && video_to_top(sx, sy, &tx, &ty)) pd_session_touch(&session, tx, ty, true);
            else if (!touchingNow && pd_session_touch(&session, 0, 0, false)) sfx_click(settings.clickSound);
        } else if (!touchingNow && (touchOn == VIDEO_HIT_CLOSE || touchOn == VIDEO_HIT_DIM)) {
            closeNow = true;
            sfx_click(settings.clickSound);
        }
        if (!touchingNow) touchOn = VIDEO_HIT_NONE;

        if (closeNow) {
            video_set_companion_view(VIDEO_COMPANION_CLOSED);
            pd_session_set_focus(&session, false);
            compShown = false;
        } else if (giveBack) {
            pd_session_set_focus(&session, false);
        } else if (openNow) {
            if (!open) video_set_companion_view(want);
            pd_session_set_focus(&session, true);
            session.quickOpen = false;
            uiPtr->focusId = -1;
            session.redraw = true;
        }
        open = video_companion_view() != VIDEO_COMPANION_CLOSED;
        // The canvas follows the view (the window's and the panel's shapes differ).
        if (open) {
            int w, h;
            video_companion_size(video_companion_view(), &w, &h);
            if (w != compW || h != compH) {
                compW = w, compH = h;
                pd_canvas_init(&bottom, compW, compH, companionPx);
                session.redraw = true;
            }
        }

        bool leave = false;
        enum pd_action action = pd_ui_take_action(uiPtr);
        // FLY from the MAP and TAKE OFF are to be watched: the window gets out of the way (the app's flyAction).
        if ((action == PD_ACTION_FLY_TO || action == PD_ACTION_FLIGHT) && video_companion_view() == VIDEO_COMPANION_WINDOW) {
            video_set_companion_view(VIDEO_COMPANION_CLOSED);
            pd_session_set_focus(&session, false);
            compShown = false;
        }
        if (!pd_session_action(&session, action)) {
            switch (action) {
            case PD_ACTION_LEAVE_GAME:
                leave = true;
                break;
            case PD_ACTION_RESTART_GAME:
                pk_finish_save(core, PK_FINISH_SAVE_FRAMES, NULL);
                core->reset(core);
                pk_heal_rtc(core);
                break;
            case PD_ACTION_SETTINGS_CHANGED:
                pd_settings_save(&settings, SETTINGS_FILE);
                apply_volume();
                break;
            default:
                break;
            }
        }
        if (leave) break;

        // The QUICK MENU's buttons click too: opening it, a pick, B.
        if (!uiPtr->keyFocus && ((down & K_Y) || (session.quickOpen && (down & (K_A | K_B))))) sfx_click(settings.clickSound);
        struct pd_input in = { .held = gba_keys(held & ~withheld), .down = gba_keys(down & ~withheld),
                               .quickMenu = (down & K_Y) && !uiPtr->keyFocus, .ffHeld = (held & K_ZR) && !uiPtr->keyFocus,
                               .stickAngle = stick_angle(), .catchUp = catchUp };
        // The session runs the extra frames first and the drawn one last: mGBA skips rendering until its counter runs out.
        if (catchUp && !session.ff) ((struct GBA*) core->board)->video.frameskipCounter = catchUp;
        uint64_t busyStart = host_us();
        uint64_t tickNow = host_ms();
        unsigned before = session.frame;
        pd_session_tick(&session, &in, (unsigned) (tickNow - lastTick));
        gb_audio_pump();
        lastTick = tickNow;
        unsigned frames = session.frame - before;
        audioMuted = session.ff;
        busyUs += host_us() - busyStart;

        fpsFrames += frames;
        uint64_t now = host_ms();
        if (now - fpsStart >= 1000) {
            // Against the GBA's own 268111856 / 4481136 = 59.73 frames a second.
            unsigned pct = (unsigned) ((uint64_t) fpsFrames * 1000 * 100 * 4481136 / ((now - fpsStart) * 268111856ULL));
            snprintf(hostPtr->values[1], sizeof(hostPtr->values[1]), "%u%% (%u FPS)", pct, fpsFrames);
            // The game's thread, how much of the last second it was busy: 100 and the game can't keep up.
            unsigned cpu = (unsigned) (busyUs / 10 / (now - fpsStart));
            unsigned load[3];
            core_load(load, (now - fpsStart) * 1000);
            perfPicture = settings.showPerf ? perf_picture((unsigned) ((uint64_t) fpsFrames * 10000 / (now - fpsStart)), load, &perfWidth)
                                            : NULL;
            for (int i = 0; i < 3; i++) loadSum[i] += load[i];
            gapsSum += audio_take_gaps();
            if (pct < pctMin) pctMin = pct;
            if (cpu > cpuMax) cpuMax = cpu;
            cpuSum += busyUs / 1000;
            secondsRun++;
            // Every 10 s: how the game kept up, then the lines go to the memory card, so a crash leaves them behind.
            if (secondsRun - gapsLogged >= 10) {
                unsigned skipped = session.catchUpFrames - skippedSum;
                skippedSum = session.catchUpFrames;
                // Where a second went, in ms (averaged over the 10): the core's frames, the session's reads, EXPAND, the
                // companion's drawing, the screen (its textures, its quads and the wait for the refresh), the whole loop.
                host_log("%us: speed %u%% (lowest %u%%), skipped %u frames, sound gaps %u, queued %u, heap %u KiB | ms a second: "
                         "core %u, reads %u, expand %u, companion %u, screen+vsync %u, session total %u, loop %u | "
                         "cores %u%% (game) %u%% (sound+expand) %u%% (renderer)",
                         secondsRun, pct, pctMin, skipped, gapsSum, (unsigned) audio_queued(), (unsigned) (host_heap_used() >> 10),
                         (unsigned) (session.prof.run / 10000), (unsigned) (session.prof.snap / 10000),
                         (unsigned) (session.prof.wide / 10000), (unsigned) (drawUsSum / 10000), (unsigned) (videoUsSum / 10000),
                         (unsigned) (cpuSum / 10), (unsigned) (loopUsSum / 10000), loadSum[0] / 10, loadSum[1] / 10, loadSum[2] / 10);
                loadSum[0] = loadSum[1] = loadSum[2] = 0;
                session.prof.run = session.prof.snap = session.prof.wide = session.prof.wait = 0;
                session.prof.flightTick = session.prof.flightDraw = 0;
                videoUsSum = loopUsSum = 0;
                cpuSum = 0;
                host_log_flush();
                gapsLogged = secondsRun;
                gapsSum = 0, pctMin = 999, cpuMax = 0, drawUsSum = 0;
            }
            drawUsSum += drawUs;
            videoUsSum += videoUs;
            loopUsSum += loopUs;
            busyUs = 0;
            drawUs = 0;
            videoUs = 0;
            loopUs = 0;
            fpsFrames = 0;
            fpsStart = now;
            // The companion's ROM is mGBA's: with FIXED_ROM_BUFFER it can't move, but if it ever did, follow it (a stale
            // one was v0.1's crash) and say so.
            if (!gb) {
                size_t romNow = 0;
                const uint8_t* at = core->getMemoryBlock(core, 0x08, &romNow);
                if (at && at != session.game.rom) {
                    host_log("game: mGBA's ROM moved (%p -> %p): the companion follows it", (const void*) session.game.rom,
                             (const void*) at);
                    session.game.rom = session.rom = at;
                }
            }
            if (capturing) {
                const int16_t *capIn, *capOut;
                size_t capFrames;
                if (audio_capture_take(&capIn, &capOut, &capFrames)) {
                    const char* tag = mixerAttached ? "mixer-on" : "mixer-off";
                    char pin[200], pout[200];
                    snprintf(pin, sizeof pin, BASE_DIR "/sound-%s-game.wav", tag);
                    snprintf(pout, sizeof pout, BASE_DIR "/sound-%s-output.wav", tag);
                    bool ok = write_wav(pin, capIn, capFrames) && write_wav(pout, capOut, capFrames);
                    host_log("sound: recording %s (%s, %s)", ok ? "saved" : "NOT saved", pin, pout);
                    audio_capture_free();
                    capturing = false;
                    // A one-off: turned off again in vita.ini (left on, every game recorded its start - v0.9's log).
                    vitaSettings.soundCapture = false;
                    vita_settings_save();
                    behindUs = 0; // the writing's pause owes no catching up
                    loopAt = 0;
                }
            }
            hostPtr->batteryPct = host_battery_pct();
            hostPtr->charging = host_charging();
            if (uiPtr->tab == PD_TAB_SETTINGS) session.redraw = true;
            host_keep_awake();
        }

        // The companion is drawn only while it shows (sliding out, its last picture stays).
        if (session.redraw && open && compW) {
            uint64_t drawStart = host_us();
            pd_session_draw(&session, &bottom);
            // Another tab: it slides in from its side of the bar; a tab's own page (a summary, a DEX entry, a GUIDE
            // entry) zooms; an overlay fades. As CompanionScreen's AnimatedContent, cut when switched faster than 300 ms
            // apart. Not while the companion opens (its slide is the move) or its canvas changes size.
            int tab = uiPtr->tab, sub = (uiPtr->summarySlot >= 0) | (uiPtr->dexOpen > 0) << 1 | (uiPtr->guideOpen > 0) << 2;
            int over = uiPtr->overlay;
            if (compShown && compShownW == compW && compShownH == compH) {
                int split = compH - (pd_font_size_small() ? 22 : 28) - 4;
                uint64_t nowUs = host_us();
                if (tab != compTab) {
                    bool rapid = nowUs - compSwitchAt < 300000;
                    compSwitchAt = nowUs;
                    int from = pd_ui_bar_index(uiPtr, (enum pd_tab) compTab), to = pd_ui_bar_index(uiPtr, (enum pd_tab) tab);
                    video_companion_swap(rapid ? VIDEO_SWAP_CUT : to >= from ? VIDEO_SWAP_LEFT : VIDEO_SWAP_RIGHT, split);
                } else if (sub != compSub) {
                    video_companion_swap(VIDEO_SWAP_ZOOM, split);
                } else if (over != compOver) {
                    video_companion_swap(VIDEO_SWAP_FADE, compH);
                }
            }
            compTab = tab, compSub = sub, compOver = over;
            compShown = true, compShownW = compW, compShownH = compH;
            video_set_companion(companionPx, compW, compH);
            drawUs += host_us() - drawStart;
        }
        video_set_perf(perfPicture, session.perfRight - perfWidth, 3, settings.showPerf ? perfWidth : 0);
        perfPicture = NULL;
        video_hold_game(pd_session_holds_frame(&session));
        if (session.wideReady) wide_join(); // its margins drawn
        video_set_wide(session.wideReady ? pd_session_wide(&session) : NULL, session.wideW, session.wideH);
        const struct pd_wide_hole* hole = &session.wideHole;
        video_set_wide_hole(hole->x, hole->y, hole->w, hole->h, hole->gameX, hole->gameY);
        // The QUICK MENU opening or closing fades (the picture before it fades out as this one comes in).
        static bool quickWas;
        if (session.quickOpen != quickWas) {
            quickWas = session.quickOpen;
            video_overlay_fade();
            video_set_overlay(pd_session_overlay(&session));
        } else if (session.overlayDirty) {
            video_set_overlay(pd_session_overlay(&session));
        }
        video_show_overlay(session.overlayShown, session.overlayDim);
        uint64_t videoStart = host_us();
        video_draw_game(pd_session_screen(&session));
        videoUs += host_us() - videoStart;
    }

    host_log_hold(false);
    video_set_companion_view(VIDEO_COMPANION_CLOSED);
    if (capturing) audio_capture_free();
    host_log("game: %u frames run undrawn to keep up", session.catchUpFrames);
    // A save the game is writing finishes, unheard, before the core stops; then deinit writes it back.
    audioMuted = true;
    pk_finish_save(core, PK_FINISH_SAVE_FRAMES, NULL);
    audioMuted = false;
    audio_clear();
    if (gbAudioOn) pk_gb_audio_detach(&gbAudio, core);
    gbAudioOn = false;
    wide_join(); // EXPAND's last margins done before the core goes
    audioCore = NULL;
    write_card(file, &session.game, &session.snap);
    core->deinit(core);
    host_log("game: closed");
}

// --- the game list ---

static void base_of(const char* file, char* base, size_t len) {
    snprintf(base, len, "%s", file);
    char* dot = strrchr(base, '.');
    if (dot) *dot = 0;
}

static long lib_read_rom(void* ctx, const char* file, long offset, void* buf, long len) {
    (void) ctx;
    char path[512];
    snprintf(path, sizeof path, ROM_DIR "/%s", file);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = fseek(f, offset, SEEK_SET) ? 0 : (long) fread(buf, 1, (size_t) len, f);
    fclose(f);
    return n;
}

static void save_path_of(const char* file, char* path, size_t len) {
    char base[256];
    base_of(file, base, sizeof base);
    snprintf(path, len, ROM_DIR "/%s.sav", base);
}

static long lib_read_save(void* ctx, const char* file, uint8_t* buf, long max) {
    (void) ctx;
    char path[600];
    save_path_of(file, path, sizeof path);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long) fread(buf, 1, (size_t) max, f);
    fclose(f);
    return n;
}

static bool same_file(const char* path, const uint8_t* bytes, long len) {
    static uint8_t back[PD_SAVE_MAX_SIZE];
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    long n = (long) fread(back, 1, sizeof back, f);
    fclose(f);
    return n == len && !memcmp(back, bytes, (size_t) len);
}

// Save files are sacred: each is backed up, then written beside itself and read back, and only once every one has, put in
// place. Nothing is replaced while any of them is in doubt.
static bool lib_write_saves(void* ctx, int n, const char* const* files, const uint8_t* const* bytes, const long* lens, char* why,
                            size_t whyLen) {
    (void) ctx;
    char paths[2][600], temps[2][620];
    if (n > 2) return false;
    for (int i = 0; i < n; i++) {
        char base[256];
        base_of(files[i], base, sizeof base);
        save_path_of(files[i], paths[i], sizeof paths[i]);
        snprintf(temps[i], sizeof temps[i], "%.590s.new", paths[i]);
        backup_save(base, paths[i]);
        FILE* f = fopen(temps[i], "wb");
        bool ok = f && fwrite(bytes[i], 1, (size_t) lens[i], f) == (size_t) lens[i];
        if (f && fclose(f)) ok = false;
        if (!ok || !same_file(temps[i], bytes[i], lens[i])) {
            for (int k = 0; k <= i; k++) remove(temps[k]);
            snprintf(why, whyLen, "Couldn't write %s", files[i]);
            host_log("save tools: %s didn't write", temps[i]);
            return false;
        }
    }
    for (int i = 0; i < n; i++) {
        remove(paths[i]);
        if (rename(temps[i], paths[i])) {
            snprintf(why, whyLen, "Couldn't put %s in place", files[i]);
            host_log("save tools: %s didn't rename; its backup is in " BACKUP_DIR, temps[i]);
            return false;
        }
        host_log("save tools: wrote %s", paths[i]);
    }
    return true;
}

// UTF-8 <-> the IME dialog's UTF-16 (the editor's names are the games' charset: Latin is plenty).
static void to_utf16(const char* in, uint16_t* out, size_t max) {
    size_t n = 0;
    while (*in && n + 1 < max) {
        unsigned c = (unsigned char) *in++;
        if (c >= 0xC0 && (*in & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | ((unsigned char) *in++ & 0x3F);
        }
        out[n++] = (uint16_t) c;
    }
    out[n] = 0;
}

static void from_utf16(const uint16_t* in, char* out, size_t max) {
    size_t n = 0;
    for (; *in && n + 3 < max; in++) {
        unsigned c = *in;
        if (c < 0x80) {
            out[n++] = (char) c;
        } else if (c < 0x800) {
            out[n++] = (char) (0xC0 | (c >> 6));
            out[n++] = (char) (0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
}

// The console's keyboard (the IME dialog) for the save editor's names: OK / cancel, never an empty name. The characters
// are checked by the editor (the games' charset), which says so when one can't be written.
static bool lib_edit_text(void* ctx, const char* hint, const char* initial, int max, char* out, size_t outLen) {
    (void) ctx;
    static uint16_t title[64], text[64], input[64];
    to_utf16(hint, title, 64);
    to_utf16(initial, text, 64);
    memset(input, 0, sizeof input);
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = 0;
    param.title = title;
    param.maxTextLength = (SceUInt32) (max > 0 && max < 63 ? max : 63);
    param.initialText = text;
    param.inputTextBuffer = input;
    if (sceImeDialogInit(&param) < 0) return false;
    bool ok = false;
    for (;;) {
        SceCommonDialogStatus status = sceImeDialogGetStatus();
        if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult result;
            memset(&result, 0, sizeof result);
            sceImeDialogGetResult(&result);
            if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
                from_utf16(input, out, outLen);
                ok = out[0] != 0;
            }
            sceImeDialogTerm();
            break;
        }
        video_draw_page();
    }
    lastHeld = ~0u; // the button that closed the dialog doesn't reach the library
    return ok;
}

// The whole ROM in the buffer the games use (no game runs while the library is up).
static const uint8_t* lib_load_rom(void* ctx, const char* file, size_t* size) {
    (void) ctx;
    char path[512];
    snprintf(path, sizeof path, ROM_DIR "/%s", file);
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = romBuffer && len > 0 && (size_t) len <= romBufferSize && fread(romBuffer, 1, (size_t) len, f) == (size_t) len;
    fclose(f);
    if (!ok) return NULL;
    *size = (size_t) len;
    return (const uint8_t*) romBuffer;
}

static void card_path(const char* file, char* path, size_t len) {
    char base[256];
    base_of(file, base, sizeof base);
    snprintf(path, len, CARD_DIR "/%s.card", base);
}

static bool lib_read_card(void* ctx, const char* file, struct pd_game_card* out) {
    (void) ctx;
    char path[600];
    card_path(file, path, sizeof path);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(out, 1, sizeof *out, f) == sizeof *out;
    fclose(f);
    return ok && out->magic == PD_CARD_MAGIC;
}

// What the game looked like as it was left, for the library's top screen.
static void write_card(const char* file, const struct pd_game* g, const struct pd_snapshot* s) {
    static struct pd_game_card card, before;
    if (!pd_game_card_fill(&card, g, s, (uint32_t) time(NULL))) return;
    char path[600];
    card_path(file, path, sizeof path);
    // Left before the map was up (the title screen): the place it was last left in still stands.
    if (!card.place[0] && lib_read_card(NULL, file, &before)) snprintf(card.place, sizeof card.place, "%s", before.place);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(&card, 1, sizeof card, f);
    fclose(f);
}

static void lib_settings_changed(void* ctx) {
    (void) ctx;
    pd_settings_save(&settings, SETTINGS_FILE);
    apply_volume();
}

static struct pd_library library;
static bool cacheDirty;

static void lib_cache_changed(void* ctx) {
    (void) ctx;
    cacheDirty = true;
}

static void write_cache(void) {
    static char text[PD_LIBRARY_MAX * 400];
    size_t n = pd_library_cache_write(&library, text, sizeof text);
    FILE* f = fopen(LIBRARY_FILE, "wb");
    if (!f) return;
    fwrite(text, 1, n, f);
    fclose(f);
    cacheDirty = false;
}

static void read_cache(void) {
    static char text[PD_LIBRARY_MAX * 400 + 1];
    FILE* f = fopen(LIBRARY_FILE, "rb");
    if (!f) return;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    pd_library_cache_read(&library, text);
}

static unsigned library_keys(uint32_t down) {
    unsigned k = 0;
    if (down & K_A) k |= PD_LIB_A;
    if (down & K_B) k |= PD_LIB_B;
    if (down & K_X) k |= PD_LIB_X;
    if (down & K_Y) k |= PD_LIB_Y;
    if (down & (K_L | K_ZL)) k |= PD_LIB_L;
    if (down & (K_R | K_ZR)) k |= PD_LIB_R;
    if (down & K_UP) k |= PD_LIB_UP;
    if (down & K_DOWN) k |= PD_LIB_DOWN;
    if (down & K_LEFT) k |= PD_LIB_LEFT;
    if (down & K_RIGHT) k |= PD_LIB_RIGHT;
    if (down & K_START) k |= PD_LIB_START;
    if (down & K_SELECT) k |= PD_LIB_SELECT;
    return k;
}

// Picks a game; false when the app should close.
static bool pick_rom(char* out, size_t outLen) {
    static char* names[MAX_ROMS];
    static int page, view = -1;
    static char lastFile[256];
    static struct pd_library_host host = {
        .read_rom = lib_read_rom, .read_save = lib_read_save, .write_saves = lib_write_saves, .load_rom = lib_load_rom,
        .read_card = lib_read_card, .settings_changed = lib_settings_changed, .cache_changed = lib_cache_changed,
        .version = PD_VERSION " (PS Vita)", .edit_text = lib_edit_text,
    };
    int n = list_roms(names);
    pd_library_screen(VIDEO_PAGE_W, LIB_AREA_H, page_key());
    pd_library_init(&library, &settings, &host);
    for (int i = 0; i < n && i < PD_LIBRARY_MAX; i++) library.games[library.count++].file = names[i];
    pd_library_refresh(&library);
    read_cache();
    // Back where it was left: the page and the view.
    if (view >= 0) library.view = view;
    library.page = page == PD_PAGE_SETTINGS ? page : PD_PAGE_GAMES;
    pd_library_refresh(&library);
    pd_library_select(&library, lastFile);
    pd_look_apply(NULL, &settings);
    struct pd_canvas top, lib, strip;
    pd_canvas_init(&top, TOP_W, TOP_H, topPx);
    pd_canvas_init(&lib, VIDEO_PAGE_W, LIB_AREA_H, pagePx); // the page's rows above the strip (same stride)
    pd_canvas_init(&strip, VIDEO_PAGE_W, VIDEO_PAGE_H, pagePx);
    library.dirty = true;

    // INFO: the 3DS top screen's picture of what the cursor is on (a game's save and party, a setting's words, the save
    // tools' Pokémon and trades) in a window over the page - triangle on GAMES and SETTINGS (whose X the Vita has no use
    // for: no GBA CORE to pick, mGBA plays them all), the right stick on the save tools (their X / Y are theirs).
    bool info = false, touching = false, picked = false, running = true;
    bool drawnInfo = false, drawnDark = false, drawnClear = false;
    int shownPage = -1;
    uint32_t shownScreen = 0;
    for (;;) {
        uint32_t held, down;
        read_buttons(&held, &down);
        bool toolPage = library.page == PD_PAGE_EDITOR || library.page == PD_PAGE_TRADE;
        bool infoKey = (!toolPage && (down & K_X) && library.pickRow < 0) || (down & (K_LAYOUT_UP | K_LAYOUT_DOWN));
        uint32_t pass = down;
        if (info) {
            // Open: the D-pad, L / R and A still go to the page (the window follows the cursor); the rest close it.
            if (infoKey || (down & (K_B | K_X | K_Y | K_START | K_SELECT))) info = false;
            pass &= K_UP | K_DOWN | K_LEFT | K_RIGHT | K_L | K_R | K_ZL | K_ZR | K_A;
        } else if (infoKey) {
            info = true;
            pass &= ~K_X;
        } else if (!toolPage) {
            pass &= ~K_X;
        }
        pd_library_keys(&library, library_keys(pass));
        // CLICK SOUND: the game's click on a button (the D-pad's moves are quiet, as the app's focus is).
        if (down & (K_A | K_B | K_X | K_Y | K_L | K_R | K_SELECT | K_LAYOUT_UP | K_LAYOUT_DOWN)) sfx_click(settings.clickSound);
        if (library.quit) {
            running = false;
            break;
        }
        int sx, sy;
        if (read_touch(&sx, &sy)) {
            // The page at 2x; a touch on the INFO window, or anywhere, closes it.
            if (info) info = false, touching = false;
            else if (sy / 2 < LIB_AREA_H) {
                pd_library_touch(&library, sx / 2, sy / 2, true);
                touching = true;
            }
        } else if (touching) {
            touching = false;
            pd_library_touch(&library, 0, 0, false);
            sfx_click(settings.clickSound);
        }
        if (library.picked >= 0) {
            // LOADING before the game is read.
            pd_library_draw_loading(&library, &lib, &top);
            hint_strip(&strip, LIB_AREA_H, "Loading...");
            page_upload();
            video_draw_page();
            picked = true;
            break;
        }
        pd_library_tick(&library);
        {
            // The battery in every page's title, read once a second.
            static uint64_t batteryAt;
            uint64_t now = host_ms();
            if (!batteryAt || now - batteryAt >= 1000) {
                batteryAt = now;
                int pct = host_battery_pct();
                bool charge = host_charging();
                if (pct != library.batteryPct || charge != library.charging) library.dirty = true;
                library.batteryPct = pct;
                library.charging = charge;
            }
        }
        // The page's see-through follows DARK MODE and BACKGROUND.
        bool clear = video_backdrop_shown();
        if (pd_dark != drawnDark || clear != drawnClear || info != drawnInfo) library.dirty = true;
        if (library.dirty) {
            toolPage = library.page == PD_PAGE_EDITOR || library.page == PD_PAGE_TRADE;
            pd_library_screen(VIDEO_PAGE_W, LIB_AREA_H, page_key());
            // The top screen first: its hint bar's words are the strip's.
            pd_library_draw_top(&library, &top);
            char hint[160];
            const char* words = lib_last_hint;
            if (info) {
                words = library.page == PD_PAGE_GAMES ? "UP DOWN: GAMES   A: PLAY   B: CLOSE" : "B: CLOSE";
            } else if (library.page == PD_PAGE_GAMES && library.pickRow < 0) {
                bool none = library.list.count == 0;
                const struct pd_library_game* g = none ? NULL : &library.games[library.list.items[library.list.selected]];
                snprintf(hint, sizeof hint, "%s   X: INFO   L R: VIEWS   SELECT: PAGES   START: CLOSE",
                         !g ? "" : g->supported || g->console != PD_CONSOLE_GBA ? "A: PLAY" : "A: PLAY (no companion yet)");
                words = hint;
            } else if (library.page == PD_PAGE_SETTINGS && library.pickRow < 0) {
                words = "A: CHANGE   B: GAMES   X: INFO   SELECT: PAGES";
            } else if (toolPage) {
                snprintf(hint, sizeof hint, "%s   R-STICK: INFO", lib_last_hint);
                words = hint;
            }
            pd_library_draw(&library, &lib);
            if (info) {
                // The top screen in a window over the dimmed page, less its own hint bar.
                pd_dim(&lib, 150);
                const int tw = TOP_W, th = TOP_H - 22, tx = (VIDEO_PAGE_W - tw) / 2, ty = (LIB_AREA_H - th) / 2;
                pd_fill(&lib, tx - 3, ty - 3, tw + 6, th + 6, 0xF4F4F8);
                pd_fill(&lib, tx - 1, ty - 1, tw + 2, th + 2, 0x353845);
                for (int y = 0; y < th; y++) memcpy(pagePx + (ty + y) * VIDEO_PAGE_W + tx, topPx + y * TOP_W, sizeof(uint32_t) * tw);
            }
            hint_strip(&strip, LIB_AREA_H, words);
            // The page changing to another: the next page slides up from below (the previous one down), a pick-list, a
            // dialog, INFO or one of the save tools' screens fades in or out.
            uint32_t screen = pd_library_screen_key(&library) | (uint32_t) info << 31;
            if (shownPage >= 0 && library.page != shownPage) video_page_swap(library.page > shownPage ? VIDEO_SWAP_UP : VIDEO_SWAP_DOWN);
            else if (shownPage >= 0 && screen != shownScreen) video_page_swap(VIDEO_SWAP_FADE);
            shownPage = library.page, shownScreen = screen;
            page_upload();
            drawnInfo = info, drawnDark = pd_dark, drawnClear = clear;
        }
        // The names the library worked out, kept for the next start (written between games, not every frame).
        if (cacheDirty && library.hashing < 0) write_cache();
        video_draw_page();
    }
    page = library.page;
    view = library.view;
    if (picked) {
        pd_library_played(&library, library.picked, (uint32_t) time(NULL));
        snprintf(pickedSha1, sizeof pickedSha1, "%s", library.games[library.picked].sha1);
        write_cache();
        snprintf(out, outLen, "%s", library.games[library.picked].file);
        snprintf(lastFile, sizeof lastFile, "%s", out);
    }
    for (int i = 0; i < n; i++) free(names[i]);
    return running && picked;
}

int main(void) {
    // First, while memory is whole: the 32 MB every game is read into.
    bool romBufferOk = rom_buffer_init();
    // The game's thread on the first core, the sound's on the second (audio.c): neither waits for the other.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    host_clocks_up();
    make_dirs();
    host_log_open(LOG_FILE);
    host_log("PokeDaisy %s for the PS Vita: starting", PD_VERSION);
    if (!romBufferOk) host_log("no memory for the 32 MB game buffer: no game can start");
    if (!video_init()) {
        host_log("the screen (vita2d) didn't start");
        host_log_close();
        sceKernelExitProcess(0);
        return 1;
    }
    video_before_frame(follow_look);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    {
        SceAppUtilInitParam init;
        SceAppUtilBootParam boot;
        memset(&init, 0, sizeof init);
        memset(&boot, 0, sizeof boot);
        sceAppUtilInit(&init, &boot);
        SceCommonDialogConfigParam config;
        sceCommonDialogConfigParamInit(&config);
        sceCommonDialogSetConfigParam(&config);
    }
    float hz = 0;
    if (sceDisplayGetRefreshRate(&hz) >= 0 && hz > 50 && hz < 70) refreshHz = hz;
    mLogSetDefaultLogger(&silentLogger);
    pd_settings_load(&settings, SETTINGS_FILE);
    vita_settings_load();
    if (vitaSettings.oldBackground == 0 && !settings.backgroundOff) {
        // v0.7's vita.ini switch, now the app's BACKGROUND.
        settings.backgroundOff = true;
        pd_settings_save(&settings, SETTINGS_FILE);
    }
    video_set_backdrop(!settings.backgroundOff);
    hasSound = audio_init();
    sfx_init();
    host_log("screen %.3f Hz, sound %s, %s buttons, renderer %s, native mixer %s, sound capture %s, background %s, companion %s",
             refreshHz, hasSound ? "on" : "OFF (sceAudioOut refused)", vitaSettings.positional ? "positional" : "cross = A",
             vitaSettings.threadedVideo ? "threaded" : "on the game's thread", vitaSettings.nativeMixer ? "on" : "off",
             vitaSettings.soundCapture ? "on" : "off", settings.backgroundOff ? "off" : "animated",
             settings.companionPanel ? "side panel" : "window");
    vita_settings_save(); // the file with every key and its comments, for whoever wants to change one
    char file[256];
    while (pick_rom(file, sizeof(file))) run_game(file);
    host_log("closing");
    audio_exit();
    video_exit();
    host_log_close();
    sceKernelExitProcess(0);
    return 0;
}

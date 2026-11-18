// The menu click, as the app has it (GameClickSound, FfMusicRenderer.recordSfx): not a bundled sound but the game's own
// SE_SELECT, played once by the ROM's sound engine on a core of its own and kept as a file. The app renders it on a second
// core beside the game; the Vita's ROM lives in one fixed 32 MB buffer (FIXED_ROM_BUFFER), so it's rendered before the game
// starts, the first time a ROM is played (a few seconds, once), then that core goes and the game's comes.
//
// How (FfMusicRenderer's RenderCore): boot the game ~5 s with no picture drawn, wait for its music (its sound engine set
// up), park its main loop on a `b .` (pkRenderPark: only VBlank's sound tick runs), then call the ROM's own
// m4aSongNumStart - found by its code (M4aSongs.kt's signatures) - first with MUS_DUMMY (silence), then SE_SELECT,
// recording its quarter second. A game whose sound engine isn't recognised (pokeemerald-expansion), or a Game Boy game,
// borrows the newest click another game left, as the app does.
#include "sfx.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mgba-util/vfs.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>

#include "../app/src/main/cpp/pk_call.h"
#include "audio.h"
#include "host.h"

#define ROM_BASE 0x08000000u
#define SE_SELECT 5
#define MUS_DUMMY 0
#define BOOT_FRAMES 300
#define MAX_BOOT_FRAMES 1500 // the app waits 30 s; a few seconds more than FireRed's intro here, once
#define SILENCE_FRAMES 20
#define CLICK_FRAMES 45
#define SAFE_CALL_FRAMES 60
#define SILENT 8 // |sample| at or below: silence (GameClickSound's SILENCE)
#define MAX_CLICK (AUDIO_RATE / 2)

extern uint32_t* romBuffer;

static int16_t click[MAX_CLICK * 2];
static size_t clickFrames;

// --- M4aSongs.kt: m4aSongNumStart by its code ---

// "." = any nibble; where its gSongTable load sits isn't needed for the click (only the function).
static const char* const SIGNATURES[] = {
    "00b50004074a0849400b401883885900c918890089180a680168101c00f0a2fb",
    "00b5..4b0004400bc018828853009b18..4a9b000168d058........01bc0047",
    "00b5..4a03045b0b9b1899884a005218..4992008858..4a416b914202d11968........01bc0047",
    "000430b5000c002916d0..4d2b685a1c12d02a0000210c3206e00c3213000c3b1b6801315c1c07d08342f6d1"
    "4b005b189b000433eb1802e0..4bc000c31899884a005218..4992008858..4a416b914202d11968........30bc01bc0047",
    "00b50004..4a..49400b401883885900c918890089180a680168802000064140101c",
};

static int nibble(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static long find_pattern(const uint8_t* rom, size_t size, const char* pattern) {
    size_t n = strlen(pattern) / 2;
    uint8_t value[128], mask[128];
    if (n > sizeof value) return -1;
    for (size_t k = 0; k < n; k++) {
        int hi = nibble(pattern[2 * k]), lo = nibble(pattern[2 * k + 1]);
        value[k] = (uint8_t) ((hi < 0 ? 0 : hi) << 4 | (lo < 0 ? 0 : lo));
        mask[k] = (uint8_t) ((hi < 0 ? 0 : 0xF0) | (lo < 0 ? 0 : 0x0F));
    }
    for (size_t i = 0; i + n <= size; i += 2) {
        size_t j = 0;
        while (j < n && (rom[i + j] & mask[j]) == value[j]) j++;
        if (j == n) return (long) i;
    }
    return -1;
}

// --- FfMusicKey.kt: the BGM player is playing ---

static bool in_ram(uint32_t a) {
    return (a >= 0x02000000u && a < 0x02040000u) || (a >= 0x03000000u && a < 0x03008000u);
}

static bool music_playing(struct mCore* core) {
    uint32_t info = core->busRead32(core, 0x03007FF0u);
    if (!in_ram(info)) return false;
    uint32_t p = core->busRead32(core, info + 0x24), bgm = 0;
    for (int n = 0; p; n++) {
        if (!in_ram(p) || n >= 12) return false;
        bgm = p;
        p = core->busRead32(core, p + 0x3C);
    }
    if (!bgm || !(core->busRead32(core, bgm + 0x04) & 0xFFFF)) return false;
    uint32_t header = core->busRead32(core, bgm);
    return header >= ROM_BASE && header < ROM_BASE + 0x02000000u;
}

// --- pokedaisy_jni.c's render core ---

static int16_t scratch[4096 * 2];

// A frame, no picture drawn (the GBA's frameskip), its sound into scratch: how many stereo frames.
static int frame(struct mCore* core) {
    ((struct GBA*) core->board)->video.frameskipCounter = 2;
    core->runFrame(core);
    pk_rest_note(core); // where its main loop waits: pk_call's other safe moment, if parking failed
    struct blip_t* left = core->getAudioChannel(core, 0);
    struct blip_t* right = core->getAudioChannel(core, 1);
    int avail = blip_samples_avail(left);
    if (avail > 4096) avail = 4096;
    if (avail <= 0) return 0;
    blip_read_samples(left, scratch, avail, 1);
    blip_read_samples(right, scratch + 1, avail, 1);
    return avail;
}

// pkRenderPark: once the main loop runs with VBlank's interrupt on, its PC goes to the `b .`.
static bool park(struct mCore* core, uint32_t spin) {
    struct ARMCore* cpu = (struct ARMCore*) core->cpu;
    for (int i = 0; i < 5000000; i++) {
        if ((cpu->privilegeMode == MODE_SYSTEM || cpu->privilegeMode == MODE_USER) && !cpu->cpsr.i && !cpu->halted &&
            (core->busRead16(core, 0x04000208) & 1) && (core->busRead16(core, 0x04000200) & 1) &&
            (core->busRead16(core, 0x04000004) & 8)) {
            cpu->gprs[ARM_PC] = (int32_t) (spin & ~1u);
            _ARMSetMode(cpu, MODE_THUMB);
            ThumbWritePC(cpu);
            return true;
        }
        core->step(core);
    }
    return false;
}

// FfMusicRenderer.forceSong: m4aSongNumStart(n), a frame at a time until a frame lets it run.
static bool force_song(struct mCore* core, uint32_t fn, uint32_t n) {
    const uint32_t args[4] = { n, 0, 0, 0 };
    for (int i = 0; i < SAFE_CALL_FRAMES; i++) {
        if (pk_call(core, fn, args, NULL)) return true;
        frame(core);
    }
    return false;
}

static void click_path(const char* key, char* out, size_t len) {
    snprintf(out, len, SFX_DIR "/%s-select.raw", key);
}

static bool load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(click, 4, MAX_CLICK, f);
    fclose(f);
    clickFrames = n;
    return n > 0;
}

// The newest click any game left (a game without its own borrows it, as GameClickSound does).
static bool load_newest(void) {
    DIR* d = opendir(SFX_DIR);
    if (!d) return false;
    char best[300] = "";
    time_t bestAt = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 11 || strcmp(e->d_name + n - 11, "-select.raw")) continue;
        char path[300];
        snprintf(path, sizeof path, SFX_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0 && (!best[0] || st.st_mtime > bestAt)) {
            snprintf(best, sizeof best, "%s", path);
            bestAt = st.st_mtime;
        }
    }
    closedir(d);
    return best[0] && load(best);
}

void sfx_init(void) {
    mkdir(SFX_DIR, 0777);
    load_newest();
}

bool sfx_has_own(const char* key) {
    char path[300];
    click_path(key, path, sizeof path);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) return true;
    snprintf(path, sizeof path, SFX_DIR "/%s-select.none", key);
    return stat(path, &st) == 0; // tried, and it has none of its own
}

bool sfx_use(const char* key) {
    char path[300];
    click_path(key, path, sizeof path);
    return load(path) || load_newest();
}

bool sfx_render(const char* romPath, const char* key) {
    uint64_t start = host_ms();
    struct VFile* rom = VFileOpen(romPath, O_RDONLY);
    if (!rom) return false;
    struct mCore* core = mCoreFindVF(rom);
    if (!core || core->platform(core) != mPLATFORM_GBA || !core->init(core)) {
        if (core) core->deinit(core);
        rom->close(rom);
        return false;
    }
    mCoreInitConfig(core, NULL);
    static uint32_t video[240 * 160];
    core->setVideoBuffer(core, (color_t*) video, 240);
    core->setAudioBufferSize(core, 2048);
    blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), AUDIO_RATE);
    blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), AUDIO_RATE);
    if (!mCorePreloadVF(core, rom)) {
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        return false;
    }
    ((struct GBA*) core->board)->idleOptimization = IDLE_LOOP_DETECT;
    core->reset(core);

    // The ROM (FIXED_ROM_BUFFER: in romBuffer) for M4aSongs' search, and a `b .` to park on.
    size_t size = ((struct GBA*) core->board)->memory.romSize;
    const uint8_t* bytes = (const uint8_t*) romBuffer;
    long at = -1;
    for (unsigned i = 0; i < sizeof SIGNATURES / sizeof *SIGNATURES && at < 0; i++) at = find_pattern(bytes, size, SIGNATURES[i]);
    long spin = -1;
    for (size_t i = 0; i + 2 <= size; i += 2)
        if (bytes[i] == 0xFE && bytes[i + 1] == 0xE7) {
            spin = (long) i;
            break;
        }
    bool ok = false;
    size_t got = 0;
    if (at < 0) {
        host_log("sound: %s's sound engine isn't one the click is rendered from (it borrows another game's)", key);
    } else {
        uint32_t fn = ROM_BASE + (uint32_t) at + 1; // Thumb
        int frames = 0;
        while (frames < BOOT_FRAMES) frame(core), frames++;
        while (!music_playing(core) && frames < MAX_BOOT_FRAMES) frame(core), frames++;
        bool parked = spin >= 0 && park(core, ROM_BASE + (uint32_t) spin);
        if (force_song(core, fn, MUS_DUMMY)) {
            for (int i = 0; i < SILENCE_FRAMES; i++) frame(core);
            if (force_song(core, fn, SE_SELECT)) {
                static int16_t rec[MAX_CLICK * 2];
                for (int i = 0; i < CLICK_FRAMES; i++) {
                    int n = frame(core);
                    if (got + (size_t) n > MAX_CLICK) n = (int) (MAX_CLICK - got);
                    memcpy(rec + got * 2, scratch, (size_t) n * 4);
                    got += (size_t) n;
                }
                // Silence off both ends (GameClickSound.write).
                size_t first = 0, last = got;
                while (first < got && abs(rec[first * 2]) <= SILENT && abs(rec[first * 2 + 1]) <= SILENT) first++;
                while (last > first && abs(rec[(last - 1) * 2]) <= SILENT && abs(rec[(last - 1) * 2 + 1]) <= SILENT) last--;
                if (last > first) {
                    char path[300];
                    click_path(key, path, sizeof path);
                    FILE* f = fopen(path, "wb");
                    if (f) {
                        ok = fwrite(rec + first * 2, 4, last - first, f) == last - first;
                        fclose(f);
                    }
                    got = last - first;
                }
            }
        }
        host_log("sound: the click (SE_SELECT) %s for %s in %u ms (%d frames booted, %s, %u samples)", ok ? "rendered" : "NOT rendered",
                 key, (unsigned) (host_ms() - start), frames, parked ? "parked" : "not parked", (unsigned) got);
    }
    mCoreConfigDeinit(&core->config);
    core->deinit(core);
    pk_rest_reset(); // that game's main loop isn't the next one's
    if (!ok) {
        // Not tried again at every start (a game whose engine isn't known stays unknown): a marker says so.
        char path[300];
        snprintf(path, sizeof path, SFX_DIR "/%s-select.none", key);
        FILE* f = fopen(path, "wb");
        if (f) fclose(f);
    }
    return ok;
}

void sfx_click(bool on) {
    if (on && clickFrames) audio_sfx(click, clickFrames);
}

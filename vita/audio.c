// The sound: the game pushes stereo frames into a ring as it makes them (audio_push, never waiting), a thread of its own
// takes a period at a time and hands it to sceAudioOut, which blocks until the period before has gone out. An empty ring
// plays silence for that period. One writer (the game's thread) and one reader (this thread): no lock, just the two
// counters and barriers.
#include "audio.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#define RING (AUDIO_PERIOD * 16) // a power of two
static int16_t ring[RING * 2];
static volatile uint32_t written, played; // frames, ever (they wrap together)
static volatile bool running;
static volatile unsigned gaps;
static int port = -1;
static SceUID thread = -1;

int audio_thread_id(void) {
    return thread;
}
static double trim = 1.0, trimSlow = 1.0;
// A sound over the game's (the menu click): its frames and how far it's played; the thread mixes it in.
static const int16_t* volatile sfxPcm;
static volatile size_t sfxFrames, sfxAt;

static void mix_sfx(int16_t* period) {
    const int16_t* pcm = sfxPcm;
    if (!pcm) return;
    size_t at = sfxAt, n = sfxFrames - at < AUDIO_PERIOD ? sfxFrames - at : AUDIO_PERIOD;
    for (size_t i = 0; i < n * 2; i++) {
        int v = period[i] + pcm[at * 2 + i];
        period[i] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    sfxAt = at + n;
    if (sfxAt >= sfxFrames) sfxPcm = NULL;
}

void audio_sfx(const int16_t* stereo, size_t frames) {
    sfxPcm = NULL;
    __sync_synchronize();
    sfxFrames = frames;
    sfxAt = 0;
    __sync_synchronize();
    sfxPcm = stereo;
}
static int16_t *capIn, *capOut;
static size_t capFrames;
static volatile size_t capInAt, capOutAt;

static void capture(int16_t* cap, volatile size_t* at, const int16_t* stereo, size_t frames) {
    if (!cap || *at >= capFrames) return;
    size_t n = capFrames - *at < frames ? capFrames - *at : frames;
    memcpy(cap + *at * 2, stereo, n * 2 * sizeof(int16_t));
    *at += n;
}

static int audio_main(SceSize args, void* argp) {
    (void) args;
    (void) argp;
    // sceAudioOut goes on reading the buffer it was handed until the call after returns (mGBA's psp2-context.c holds its
    // samples that long too): each period gets a buffer of its own, in turn. v0.1-v0.5 refilled one buffer straight away,
    // and every period's start was the next one's - a crackle 93.75 times a second (48000 / 512), found in a recording of
    // the Vita's speaker.
    static int16_t periods[3][AUDIO_PERIOD * 2];
    unsigned turn = 0;
    while (running) {
        int16_t* period = periods[turn];
        turn = (turn + 1) % 3;
        uint32_t have = written - played;
        if (have >= AUDIO_PERIOD) {
            uint32_t at = played & (RING - 1);
            uint32_t first = RING - at < AUDIO_PERIOD ? RING - at : AUDIO_PERIOD;
            memcpy(period, ring + at * 2, first * 2 * sizeof(int16_t));
            if (first < AUDIO_PERIOD) memcpy(period + first * 2, ring, (AUDIO_PERIOD - first) * 2 * sizeof(int16_t));
            __sync_synchronize();
            played += AUDIO_PERIOD;
            capture(capOut, &capOutAt, period, AUDIO_PERIOD);
        } else {
            memset(period, 0, sizeof periods[0]);
            gaps++;
            if (capOut && capOutAt) capture(capOut, &capOutAt, period, AUDIO_PERIOD); // a gap, once the game's begun
        }
        mix_sfx(period);
        sceAudioOutOutput(port, period);
    }
    return sceKernelExitDeleteThread(0);
}

bool audio_init(void) {
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_PERIOD, AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) return false;
    running = true;
    // A core of its own (the game's is pinned to the first, main.c): beside the game at the same priority it waited for the
    // game's frame to end before it could hand sceAudioOut the next period, and the sound crackled (v0.1 on a Vita).
    thread = sceKernelCreateThread("pd_audio", audio_main, 0x10000100, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (thread < 0) {
        sceAudioOutReleasePort(port);
        port = -1;
        return false;
    }
    sceKernelStartThread(thread, 0, NULL);
    audio_set_volume(100, false);
    return true;
}

void audio_exit(void) {
    if (port < 0) return;
    running = false;
    SceUInt timeout = 500000;
    sceKernelWaitThreadEnd(thread, NULL, &timeout);
    sceAudioOutReleasePort(port);
    port = -1;
}

void audio_push(const int16_t* stereo, size_t frames) {
    if (port < 0) return;
    capture(capIn, &capInAt, stereo, frames);
    uint32_t room = RING - (written - played);
    if (frames > room) frames = room;
    uint32_t at = written & (RING - 1);
    uint32_t first = RING - at < frames ? RING - at : (uint32_t) frames;
    memcpy(ring + at * 2, stereo, first * 2 * sizeof(int16_t));
    if (first < frames) memcpy(ring, stereo + first * 2, (frames - first) * 2 * sizeof(int16_t));
    __sync_synchronize();
    written += (uint32_t) frames;
}

size_t audio_queued(void) {
    return written - played;
}

void audio_clear(void) {
    // Only the reader moves played: skip what's queued by catching written up to it.
    written = played;
    trim = trimSlow = 1.0;
}

void audio_set_volume(int percent, bool muted) {
    if (port < 0) return;
    int v = muted ? 0 : SCE_AUDIO_VOLUME_0DB * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100;
    int vol[2] = { v, v };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
}

// A game under full speed makes its sound too slowly, and a dry ring is a hole in the sound many times a second: the buzz
// v0.3 had. So the sound follows how fast the game really runs (as the 3DS port's audio_follow): the ring's fill steers
// the rate, a little of it at once and the rest slowly, so a slow game's sound glides lower instead of breaking up, and at
// full speed it settles back to 1. Called each time the core hands over a period (~94 a second).
double audio_rate_trim(void) {
    double off = ((double) AUDIO_TARGET - (double) audio_queued()) / AUDIO_TARGET;
    if (off > 1) off = 1;
    if (off < -1) off = -1;
    trimSlow += 0.0004 * off;
    if (trimSlow < 0.995) trimSlow = 0.995;
    if (trimSlow > 1.18) trimSlow = 1.18;
    double want = trimSlow + 0.005 * off;
    trim += (want - trim) * 0.1;
    return trim;
}

unsigned audio_take_gaps(void) {
    unsigned g = gaps;
    gaps = 0;
    return g;
}

bool audio_capture_start(int seconds) {
    audio_capture_free();
    size_t frames = (size_t) seconds * AUDIO_RATE;
    int16_t* in = malloc(frames * 4);
    int16_t* out = malloc(frames * 4);
    if (!in || !out) {
        free(in);
        free(out);
        return false;
    }
    capInAt = capOutAt = 0;
    capFrames = frames;
    __sync_synchronize();
    capIn = in;
    capOut = out;
    return true;
}

bool audio_capture_take(const int16_t** in, const int16_t** out, size_t* frames) {
    if (!capIn || capInAt < capFrames || capOutAt < capFrames) return false;
    *in = capIn;
    *out = capOut;
    *frames = capFrames;
    return true;
}

void audio_capture_free(void) {
    int16_t* in = capIn;
    int16_t* out = capOut;
    capIn = capOut = NULL;
    __sync_synchronize();
    // The sound thread may be inside capture() on the old buffer for one more period.
    sceKernelDelayThread(30000);
    free(in);
    free(out);
}

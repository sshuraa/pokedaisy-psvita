// The game's sound on the Vita: a ring of stereo PCM16 at 48 kHz, played by a thread of its own on sceAudioOut (audio.c).
#ifndef PD_VITA_AUDIO_H
#define PD_VITA_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_RATE 48000
// One sceAudioOut period, in frames (stereo samples): mGBA's own Vita build plays 512.
#define AUDIO_PERIOD 512
// Where the ring's fill is kept: three periods, 32 ms.
#define AUDIO_TARGET (AUDIO_PERIOD * 3)

bool audio_init(void);
void audio_exit(void);
// Stereo frames into the ring; what doesn't fit is dropped (never waited for).
void audio_push(const int16_t* stereo, size_t frames);
// Frames waiting in the ring.
size_t audio_queued(void);
// Empties the ring (a game closing, a state loading).
void audio_clear(void);
// GAME VOLUME / MUTE GAME (0-100).
void audio_set_volume(int percent, bool muted);
// The rate the game's sound should be made at for the ring to stay at AUDIO_TARGET: 1.0 +- half a percent, eased.
double audio_rate_trim(void);
// Times the ring ran dry since the last call (a hole in the sound), for the log.
unsigned audio_take_gaps(void);

// A recording for finding what's wrong with the sound: the first [seconds] of what the game makes (audio_push) and of
// what goes to sceAudioOut, each into memory. audio_capture_take hands them over once both are full (then NULL).
bool audio_capture_start(int seconds);
bool audio_capture_take(const int16_t** in, const int16_t** out, size_t* frames);
void audio_capture_free(void);

// A sound over the game's, from its start (the menu click; the buffer must stay): replaces one still playing.
void audio_sfx(const int16_t* stereo, size_t frames);

// The sound thread's id (its CPU time: main.c's meter), -1 = none.
int audio_thread_id(void);

#endif

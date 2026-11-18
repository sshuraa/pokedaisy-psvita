// The menu click (the game's own SE_SELECT, rendered from its ROM once): see sfx.c.
#ifndef PD_VITA_SFX_H
#define PD_VITA_SFX_H

#include <stdbool.h>

#define SFX_DIR "ux0:data/pokedaisy/sfx"

// The folder, and the newest click any game left (the library's until a game's own is loaded).
void sfx_init(void);
// Whether [key]'s game (its file's name) has had its click rendered (or tried: a game whose engine isn't known).
bool sfx_has_own(const char* key);
// Renders [key]'s click from the ROM at [romPath] (a GBA game; a few seconds, no other core may be running: it uses the
// ROM buffer). False: not rendered (the game's sound engine isn't known, or it wasn't heard).
bool sfx_render(const char* romPath, const char* key);
// The click to play from now on: [key]'s own, else the newest another game left.
bool sfx_use(const char* key);
// Plays it (CLICK SOUND on), at the game's volume.
void sfx_click(bool on);

#endif

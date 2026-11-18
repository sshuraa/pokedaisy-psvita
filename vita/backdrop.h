// The animated backdrop behind the Vita's two screens: the app's DaisyBackdrop (its page, soft blobs drifting, the dot
// grid, the mark or the theme's motif floating and turning) in the selected theme, drawn by the GPU (backdrop.c).
#ifndef PD_VITA_BACKDROP_H
#define PD_VITA_BACKDROP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A 16x16 (or 32x32) pixel motif: rows of keys, each key's colour (0xAARRGGBB).
struct bd_motif {
    int w, h;
    const char* const* rows;
    const char* keys;
    const uint32_t* argb;
};

// A theme's backdrop, day and night (themes_gen.c, from the app's Kotlin).
struct bd_theme {
    int id;
    const char* label;
    uint32_t page;
    int blobCount;
    uint32_t blobs[5];
    uint32_t nightPage;
    int nightBlobCount;
    uint32_t nightBlobs[5];
    bool darkPage;             // dark in light mode too (HALLOWEEN, NEW YEAR)
    const struct bd_motif* motif; // NULL: the PokeDaisy mark
};

extern const struct bd_theme bd_themes[];
extern const int bd_theme_count;

bool backdrop_init(void);
void backdrop_exit(void);
// The theme (settings' id) and dark mode as they are now.
void backdrop_set_theme(int id, bool night);
// Whether text straight on the backdrop should be light (a dark page).
bool backdrop_dark(void);
// Draws it over the whole screen, [seconds] into its animation. (hx, hy, hw, hh): what the screens cover; the floating
// pictures go where they'll be seen (a band across the whole screen or down its whole height), w 0 = nothing.
void backdrop_draw(double seconds, float hx, float hy, float hw, float hh);

#endif

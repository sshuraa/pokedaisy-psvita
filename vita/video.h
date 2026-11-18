// The Vita's one 960x544 screen, drawn by the GPU (vita2d over GXM), as the app has a one-screen device: the library is
// one page (480x272 at 2x), and a game fills the screen with the companion opened over it - a WINDOW over the dimmed game
// or a SIDE PANEL beside it (COMPANION VIEW) - behind them all the theme's animated backdrop (backdrop.c).
#ifndef PD_VITA_VIDEO_H
#define PD_VITA_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_W 960
#define VIDEO_H 544
#define VIDEO_GAME_STRIDE 240 // the game buffer's row length in pixels

// The library's page: 480x272 (0xAARRGGBB, see-through where the backdrop shows), drawn at exactly 2x.
#define VIDEO_PAGE_W 480
#define VIDEO_PAGE_H 272

// How the companion shows over a game.
enum video_companion {
    VIDEO_COMPANION_CLOSED, // the game alone, the screen's height
    VIDEO_COMPANION_WINDOW, // framed in the middle at 2x (88% of the height) over the dimmed game, which keeps running
    VIDEO_COMPANION_PANEL,  // the right half; the game moves to the left half (a GBA game at exactly 2x)
};
// The companion's canvas in each open view: the window's 336x240 (the app's window shape on a 16:9 screen), the panel's
// 320x362 (1.5x the screen's height).
#define VIDEO_COMPANION_MAX_W 336
#define VIDEO_COMPANION_MAX_H 362
void video_companion_size(int view, int* w, int* h);

bool video_init(void);
void video_exit(void);

// mGBA's video buffer: 32-bit pixels (0xXXBBGGRR), VIDEO_GAME_STRIDE x 160.
uint32_t* video_game_buffer(void);
// The colour beside the game's picture (0 = black, the backdrop when it's shown): a Game Boy game's is white.
void video_set_bars(uint32_t rgb);
// The core's picture size: 240 x 160, a Game Boy's 160 x 144.
void video_set_game_size(int w, int h);
// New pictures, copied before they return: the library's page (0xAARRGGBB), the companion's canvas (0xRRGGBB, w x h).
void video_set_page(const uint32_t* px);

// The next picture (call before its upload) replaces the one shown with a transition (the app's AnimatedContent): a cut,
// a fade, a slide from the right / left (a tab forward / back), from below / above (the next / previous page), or a zoom
// (a tab's own page opening). Only the rows above the split move: the page's hint strip, the companion's tab bar stay.
enum { VIDEO_SWAP_CUT, VIDEO_SWAP_FADE, VIDEO_SWAP_LEFT, VIDEO_SWAP_RIGHT, VIDEO_SWAP_UP, VIDEO_SWAP_DOWN, VIDEO_SWAP_ZOOM };
#define VIDEO_PAGE_SPLIT 256
void video_page_swap(int kind);
void video_companion_swap(int kind, int split);
void video_set_companion(const uint32_t* px, int w, int h);
// EXPAND's window for this frame (w x h, 0xXXBBGGRR, at most 400 x 240): shown instead of the game; NULL = the game.
void video_set_wide(const uint32_t* px, int w, int h);
// The window's rectangle at (x, y), w x h, that the game's frame fills from (gameX, gameY); w 0: none.
void video_set_wide_hole(int x, int y, int w, int h, int gameX, int gameY);
// The overlay over the game's 400x240 (0xAARRGGBB: the HUD, the QUICK MENU, notices), when it changed; shown says
// whether there's one, dim darkens the game under it.
void video_set_overlay(const uint32_t* px);
void video_show_overlay(bool shown, bool dim);
// The QUICK MENU opened or closed: the overlay's next picture crossfades from the one shown now (call before its upload).
void video_overlay_fade(void);
// OVERLAY's frame round the game (PD_FRAME_NONE, _DAISY, _BEZEL): with one, the game shows at a whole size inside it.
void video_set_frame(int style);
// Keep showing the game's last frame (the touch controls drive its party menu behind it).
void video_hold_game(bool hold);
// FPS / CPU over everything: a new picture (VIDEO_PERF_W x 16, 0xAARRGGBB) when px isn't NULL, then where it goes; w 0
// hides it.
#define VIDEO_PERF_W 192
void video_set_perf(const uint32_t* px, int x, int y, int w);

// The companion's view; a change slides it in or out (180 ms, the app's).
void video_set_companion_view(int view);
int video_companion_view(void);
// Where a point on the screen (960x544) lands.
enum video_hit { VIDEO_HIT_NONE, VIDEO_HIT_GAME, VIDEO_HIT_COMPANION, VIDEO_HIT_CLOSE, VIDEO_HIT_DIM };
int video_hit(int sx, int sy);
// ... in the companion's canvas / the game's 400x240 (false: not on it).
bool video_to_companion(int sx, int sy, int* cx, int* cy);
bool video_to_top(int sx, int sy, int* tx, int* ty);

// The animated backdrop behind everything (else black), and whether it's drawn (on, and its textures made).
void video_set_backdrop(bool on);
bool video_backdrop_shown(void);
// Called at the start of every frame, before anything is drawn (main.c follows the theme there).
void video_before_frame(void (*fn)(void));

// One frame of the library (the page) / of a game (in screenMode, enum pd_screen_mode, with the companion as its view
// has it); then the swap, which waits for the screen's refresh.
void video_draw_page(void);
void video_draw_game(int screenMode);

#endif

// The screen: every picture is written into a texture by the CPU and drawn by the GPU as a scaled quad (vita2d).
//
// The game keeps the 3DS port's 400x240 "top screen" (the game in its screen mode - SHARP 1.5x, STRETCH, EXPAND ... - with
// the HUD and the QUICK MENU over it), placed at the screen's height; with the SIDE PANEL open it moves to the left half
// at 4/3 (a GBA game at exactly 2x). The companion's canvas is its view's size (video_companion_size) at 2x in the
// WINDOW, 1.5x in the panel. A picture that lands on whole Vita pixels is drawn nearest; any other scale uses mGBA's
// "sharp bilinear" look, as the 3DS port does: the frame at 2x nearest (built by the CPU here), sampled linearly to size.
#include "video.h"

#include <math.h>
#include <string.h>

#include <psp2/gxm.h>
#include <vita2d.h>

#include "../3ds/core/pd_canvas.h"
#include "../3ds/core/pd_ui.h"
#include "backdrop.h"
#include "host.h"

#define TOP_W 400
#define TOP_H 240
#define GBA_W 240
#define GBA_H 160
#define SLIDE_US 180000

static uint32_t gameBuf[VIDEO_GAME_STRIDE * GBA_H];
static vita2d_texture *gameTex, *game2xTex, *wideTex, *wide2xTex;
static vita2d_texture *pageTex[2], *companionTex[2], *overlayTex[2], *perfTex, *chromeTex, *frameTex;

// A picture changing to another (a library page, the companion's tab): the one shown (cur) and the one before it, drawn
// together for a moment as the app's AnimatedContent has them - the new one fading and sliding (or zooming) in, the old one
// out. Only the rows above `split` move: the library's hint strip and the companion's tab bar stay.
struct swap {
    int cur, kind, split;
    uint64_t at;
};
static struct swap pageSwap, companionSwap;
#define SWAP_IN_US 220000  // CompanionScreen's slideIn(tween(220)) / fadeIn(200)
#define SWAP_OUT_US 150000 // ... fadeOut(150)

static int gameW = GBA_W, gameH = GBA_H;
static uint32_t barColor;
static bool holdGame;
// The overlay's two pictures: the one shown (cur) and the one before it, fading out while this one fades in when the
// QUICK MENU opens or closes (video_overlay_fade).
static int overlayCur;
static bool overlayShown[2], overlayDim[2];
static uint64_t fadeAt;
#ifndef FADE_US
#define FADE_US 150000
#endif
// OVERLAY's frame (PD_FRAME_*), and the game's view this frame (enum pd_screen_mode).
static int frameStyle, screenNow;
// The frame's picture as made: its style, its pixel size, where the game's window sits in it, its see-through field.
static struct { int style, f, w, h, gx, gy, ww, wh; bool clear; float x, y; } frameMade;
static const uint32_t* widePx;
static int wideW, wideH;
static struct { int x, y, w, h, gameX, gameY; } wideHole;
static int perfX, perfY, perfW;
static int companionW = VIDEO_COMPANION_MAX_W, companionH = 240;
static bool backdropOn = true, backdropReady;
static void (*beforeFrame)(void);

// The companion's view: the one asked for, the one drawn while it slides (shown), how far in it is (0 closed, 1 open).
static int view, shownView;
static float slide;
static uint64_t slideAt;

// Where a picture sits on the Vita's screen and at what scale.
struct rect {
    float x, y, w, h, k;
};
static struct rect topRect, compRect, closeRect;
static float dimAlpha;

// OVERLAY's frame: made at its own pixel size (2x or 3x on screen), as big as that makes the screen.
#define FRAME_MAX_W (VIDEO_W / 2 + 4)
#define FRAME_MAX_H (VIDEO_H / 2 + 4)

// The chrome: the close button's X, drawn once, 2x on screen.
#define CHROME_W 24
#define CHROME_H 24
enum { X_AT = 0, X_SIZE = 22 };

void video_companion_size(int v, int* w, int* h) {
    if (v == VIDEO_COMPANION_PANEL) *w = 320, *h = VIDEO_COMPANION_MAX_H;
    else *w = VIDEO_COMPANION_MAX_W, *h = 240;
}

static vita2d_texture* make(int w, int h, SceGxmTextureFormat format, SceGxmTextureFilter filter) {
    vita2d_texture* t = vita2d_create_empty_texture_format((unsigned) w, (unsigned) h, format);
    if (t) {
        vita2d_texture_set_filters(t, filter, filter);
        memset(vita2d_texture_get_datap(t), 0, vita2d_texture_get_stride(t) * (unsigned) h);
    }
    return t;
}

// Rows of w pixels into a texture (its own stride); the GPU must be done with it first.
static void put(vita2d_texture* t, const uint32_t* px, int w, int h, int srcStride) {
    uint8_t* d = vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    for (int y = 0; y < h; y++) memcpy(d + (unsigned) y * stride, px + y * srcStride, (size_t) w * 4);
}

// The same rows at 2x nearest (the sharp bilinear's first half).
static void put2x(vita2d_texture* t, const uint32_t* px, int w, int h, int srcStride) {
    uint8_t* d = vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    for (int y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*) (d + (unsigned) (2 * y) * stride);
        const uint32_t* s = px + y * srcStride;
        for (int x = 0; x < w; x++) row[2 * x] = row[2 * x + 1] = s[x];
        memcpy(d + (unsigned) (2 * y + 1) * stride, row, (size_t) w * 8);
    }
}

static void make_chrome(void) {
    static uint32_t px[CHROME_W * CHROME_H];
    memset(px, 0, sizeof px);
    // The X: a dark disc with a light ring, a white cross.
    for (int y = 0; y < X_SIZE; y++)
        for (int x = 0; x < X_SIZE; x++) {
            float dx = x + 0.5f - X_SIZE / 2.0f, dy = y + 0.5f - X_SIZE / 2.0f, d = sqrtf(dx * dx + dy * dy);
            if (d < X_SIZE / 2.0f - 2) px[y * CHROME_W + X_AT + x] = 0xF0353845u;
            else if (d < X_SIZE / 2.0f) px[y * CHROME_W + X_AT + x] = 0xF0F4F4F8u;
        }
    for (int i = 6; i < X_SIZE - 6; i++)
        for (int t = 0; t < 2; t++) {
            px[i * CHROME_W + X_AT + i + t] = 0xFFFFFFFFu;
            px[i * CHROME_W + X_AT + X_SIZE - 1 - i - t] = 0xFFFFFFFFu;
        }
    put(chromeTex, px, CHROME_W, CHROME_H, CHROME_W);
}

bool video_init(void) {
    if (vita2d_init() < 0) return false;
    vita2d_set_clear_color(0xFF000000);
    vita2d_set_vblank_wait(1);
    const SceGxmTextureFormat GAME = SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR, CANVAS = SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB;
    const SceGxmTextureFormat ALPHA = SCE_GXM_TEXTURE_FORMAT_A8R8G8B8;
    gameTex = make(GBA_W, GBA_H, GAME, SCE_GXM_TEXTURE_FILTER_POINT);
    game2xTex = make(2 * GBA_W, 2 * GBA_H, GAME, SCE_GXM_TEXTURE_FILTER_LINEAR);
    wideTex = make(TOP_W, TOP_H, GAME, SCE_GXM_TEXTURE_FILTER_POINT);
    wide2xTex = make(2 * TOP_W, 2 * TOP_H, GAME, SCE_GXM_TEXTURE_FILTER_LINEAR);
    for (int i = 0; i < 2; i++) {
        pageTex[i] = make(VIDEO_PAGE_W, VIDEO_PAGE_H, ALPHA, SCE_GXM_TEXTURE_FILTER_POINT);
        companionTex[i] = make(VIDEO_COMPANION_MAX_W, VIDEO_COMPANION_MAX_H, SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB,
                               SCE_GXM_TEXTURE_FILTER_POINT);
    }
    // Nearest at the window's 2x; the panel's 1.5x is drawn linearly (set per frame).
    overlayTex[0] = make(TOP_W, TOP_H, ALPHA, SCE_GXM_TEXTURE_FILTER_LINEAR);
    overlayTex[1] = make(TOP_W, TOP_H, ALPHA, SCE_GXM_TEXTURE_FILTER_LINEAR);
    frameTex = make(FRAME_MAX_W, FRAME_MAX_H, ALPHA, SCE_GXM_TEXTURE_FILTER_POINT);
    // The game's pictures are drawn as part of a texture (and linearly at most sizes): an edge mustn't wrap round to
    // the far side's pixels (GXM repeats by default - EXPAND 1x's outline round the game).
    vita2d_texture* edged[] = { gameTex, game2xTex, wideTex, wide2xTex };
    for (unsigned i = 0; i < 4; i++)
        if (edged[i]) {
            sceGxmTextureSetUAddrMode(&edged[i]->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
            sceGxmTextureSetVAddrMode(&edged[i]->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
        }
    perfTex = make(VIDEO_PERF_W, 16, ALPHA, SCE_GXM_TEXTURE_FILTER_LINEAR);
    chromeTex = make(CHROME_W, CHROME_H, ALPHA, SCE_GXM_TEXTURE_FILTER_POINT);
    backdropReady = backdrop_init();
    if (chromeTex) make_chrome();
    return gameTex && game2xTex && wideTex && wide2xTex && pageTex[0] && pageTex[1] && companionTex[0] && companionTex[1] && overlayTex[0] && overlayTex[1] && perfTex &&
           chromeTex && frameTex;
}

void video_exit(void) {
    vita2d_wait_rendering_done();
    backdrop_exit();
    vita2d_texture* all[] = { gameTex, game2xTex, wideTex, wide2xTex, pageTex[0], pageTex[1], companionTex[0], companionTex[1], overlayTex[0], overlayTex[1],
                              perfTex, chromeTex, frameTex };
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++)
        if (all[i]) vita2d_free_texture(all[i]);
    vita2d_fini();
}

uint32_t* video_game_buffer(void) {
    return gameBuf;
}

void video_set_bars(uint32_t rgb) {
    barColor = rgb;
}

void video_set_game_size(int w, int h) {
    gameW = w;
    gameH = h;
}

// Pictures change now and then: the GPU may still be drawing the frame that reads the texture, so it finishes first
// (a frame's few quads: no real wait).
void video_set_page(const uint32_t* px) {
    vita2d_wait_rendering_done();
    put(pageTex[pageSwap.cur], px, VIDEO_PAGE_W, VIDEO_PAGE_H, VIDEO_PAGE_W);
}

void video_set_companion(const uint32_t* px, int w, int h) {
    if (w > VIDEO_COMPANION_MAX_W || h > VIDEO_COMPANION_MAX_H) return;
    vita2d_wait_rendering_done();
    put(companionTex[companionSwap.cur], px, w, h, w);
    companionW = w;
    companionH = h;
}

void video_set_wide(const uint32_t* px, int w, int h) {
    widePx = px;
    wideW = w;
    wideH = h;
}

void video_set_wide_hole(int x, int y, int w, int h, int gameX, int gameY) {
    wideHole.x = x, wideHole.y = y, wideHole.w = w, wideHole.h = h, wideHole.gameX = gameX, wideHole.gameY = gameY;
}

void video_set_overlay(const uint32_t* px) {
    vita2d_wait_rendering_done();
    put(overlayTex[overlayCur], px, TOP_W, TOP_H, TOP_W);
}

void video_set_backdrop(bool on) {
    backdropOn = on;
}

bool video_backdrop_shown(void) {
    return backdropOn && backdropReady;
}

void video_before_frame(void (*fn)(void)) {
    beforeFrame = fn;
}

void video_show_overlay(bool shown, bool dim) {
    overlayShown[overlayCur] = shown;
    overlayDim[overlayCur] = dim;
}

void video_overlay_fade(void) {
    overlayCur ^= 1;
    overlayShown[overlayCur] = overlayDim[overlayCur] = false;
    fadeAt = host_us();
}

void video_set_frame(int style) {
    frameStyle = style;
}

void video_hold_game(bool hold) {
    holdGame = hold;
}

void video_set_perf(const uint32_t* px, int x, int y, int w) {
    if (px) {
        vita2d_wait_rendering_done();
        put(perfTex, px, VIDEO_PERF_W, 16, VIDEO_PERF_W);
    }
    perfX = x, perfY = y, perfW = w;
}

void video_set_companion_view(int v) {
    if (v == view) return;
    // Straight from one open view to the other: no slide (COMPANION VIEW changed with it open).
    if (v != VIDEO_COMPANION_CLOSED && view != VIDEO_COMPANION_CLOSED) slide = 1;
    view = v;
    if (v != VIDEO_COMPANION_CLOSED) shownView = v;
    slideAt = host_us();
}

int video_companion_view(void) {
    return view;
}

// The slide toward the view asked for, by the time since the last frame.
static void step_slide(void) {
    uint64_t now = host_us();
    float step = slideAt ? (float) (now - slideAt) / SLIDE_US : 0;
    slideAt = now;
    if (step > 0.25f) step = 0.25f; // a long frame (a load) doesn't jump it
    float want = view != VIDEO_COMPANION_CLOSED ? 1.0f : 0.0f;
    slide = slide < want ? fminf(want, slide + step) : fmaxf(want, slide - step);
    if (slide == 0) shownView = VIDEO_COMPANION_CLOSED;
}

static float ease(float t) {
    return t * t * (3 - 2 * t);
}

// The scale of the game's 400x240 with OVERLAY's frame round it: the game's picture at a whole size, as the app has a
// frame (BuiltInFrames.scaleFor, 8 px kept beside it, 4 above and below): SHARP's picture (the 400x240's height) at the
// biggest that fits - a GBA game 3x, a Game Boy's 3x - and every other view at 2x (EXPAND's 400x240 window exactly).
static float frame_k(void) {
    if (widePx || screenNow == PD_SCREEN_STRETCH || screenNow == PD_SCREEN_PIXEL || screenNow == PD_SCREEN_SHARP_125) return 2;
    float fit = fminf((float) (VIDEO_W - 16) / gameW, (float) (VIDEO_H - 8) / gameH);
    float eff = floorf(fit), s = (float) TOP_H / gameH;
    return eff < 1 ? (float) VIDEO_H / TOP_H : eff / s;
}

static bool framed(void) {
    return frameStyle != PD_FRAME_NONE && shownView != VIDEO_COMPANION_PANEL;
}

static void place(void) {
    float e = ease(slide);
    // The game alone: the screen's height, centred; with a frame, its whole size (centred on whole pixels).
    float k0 = frameStyle != PD_FRAME_NONE ? frame_k() : (float) VIDEO_H / TOP_H;
    struct rect alone = { roundf((VIDEO_W - TOP_W * k0) / 2), roundf((VIDEO_H - TOP_H * k0) / 2), TOP_W * k0, TOP_H * k0, k0 };
    topRect = alone;
    compRect = closeRect = (struct rect) { 0 };
    dimAlpha = 0;
    if (shownView == VIDEO_COMPANION_PANEL) {
        // The left half at 4/3 (a GBA game at SHARP 1.5x: 480 px, exactly 2x), the panel sliding in from the right.
        float k1 = 4.0f / 3.0f;
        struct rect beside = { (VIDEO_W / 2 - TOP_W * k1) / 2, (VIDEO_H - TOP_H * k1) / 2, TOP_W * k1, TOP_H * k1, k1 };
        topRect.k = k0 + (k1 - k0) * e;
        topRect.x = alone.x + (beside.x - alone.x) * e;
        topRect.y = alone.y + (beside.y - alone.y) * e;
        topRect.w = TOP_W * topRect.k;
        topRect.h = TOP_H * topRect.k;
        float k = (float) VIDEO_H / companionH;
        compRect = (struct rect) { VIDEO_W - VIDEO_W / 2 * e, (VIDEO_H - companionH * k) / 2, companionW * k, companionH * k, k };
        // The close tab: outside the panel's bottom-left corner (the app's SidePanelCloseTab).
        closeRect = (struct rect) { compRect.x - 2 * X_SIZE - 6, VIDEO_H - 2 * X_SIZE - 10, 2 * X_SIZE, 2 * X_SIZE, 2 };
    } else if (shownView == VIDEO_COMPANION_WINDOW) {
        // At 2x in the middle, rising a little as it fades in over the dim.
        float k = 2, w = companionW * k, h = companionH * k;
        compRect = (struct rect) { (VIDEO_W - w) / 2, (VIDEO_H - h) / 2 + 24 * (1 - e), w, h, k };
        closeRect = (struct rect) { compRect.x + w - X_SIZE - 4, compRect.y - X_SIZE + 4, 2 * X_SIZE, 2 * X_SIZE, 2 };
        dimAlpha = 0.6f * e;
    }
}

static bool inside(const struct rect* r, int x, int y) {
    return r->w && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

int video_hit(int sx, int sy) {
    place();
    if (slide < 1 && view != VIDEO_COMPANION_CLOSED) return VIDEO_HIT_NONE; // still sliding in
    if (inside(&closeRect, sx, sy)) return VIDEO_HIT_CLOSE;
    if (inside(&compRect, sx, sy)) return VIDEO_HIT_COMPANION;
    if (shownView == VIDEO_COMPANION_WINDOW) return VIDEO_HIT_DIM;
    return inside(&topRect, sx, sy) ? VIDEO_HIT_GAME : VIDEO_HIT_NONE;
}

bool video_to_companion(int sx, int sy, int* cx, int* cy) {
    place();
    if (!inside(&compRect, sx, sy)) return false;
    *cx = (int) ((sx - compRect.x) / compRect.k);
    *cy = (int) ((sy - compRect.y) / compRect.k);
    if (*cx >= companionW) *cx = companionW - 1;
    if (*cy >= companionH) *cy = companionH - 1;
    return true;
}

bool video_to_top(int sx, int sy, int* tx, int* ty) {
    place();
    if (!inside(&topRect, sx, sy)) return false;
    *tx = (int) ((sx - topRect.x) / topRect.k);
    *ty = (int) ((sy - topRect.y) / topRect.k);
    if (*tx >= TOP_W) *tx = TOP_W - 1;
    if (*ty >= TOP_H) *ty = TOP_H - 1;
    return true;
}

static bool whole(float scale) {
    return fabsf(scale - roundf(scale)) < 0.01f;
}

// A w x h picture at (u, v) in the game's virtual 400x240, s of them a picture pixel; nearest when that lands on whole
// Vita pixels, else the 2x copy sampled linearly.
static void draw_picture(vita2d_texture* t1, vita2d_texture* t2, const uint32_t* px, int stride, int w, int h, float u, float v,
                         float s, bool upload) {
    float k = topRect.k, eff = s * k;
    float x = topRect.x + u * k, y = topRect.y + v * k;
    if (whole(eff)) {
        if (upload) put(t1, px, w, h, stride);
        vita2d_draw_texture_part_scale(t1, x, y, 0, 0, (float) w, (float) h, eff, eff);
    } else {
        if (upload) put2x(t2, px, w, h, stride);
        vita2d_draw_texture_part_scale(t2, x, y, 0, 0, (float) (2 * w), (float) (2 * h), eff / 2, eff / 2);
    }
}

static unsigned abgr(uint32_t rgb, unsigned alpha) {
    return RGBA8((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

static void begin(float hx, float hy, float hw, float hh) {
    // The textures this frame writes are read by the frame before: it's done first (its few quads take no time).
    vita2d_wait_rendering_done();
    if (beforeFrame) beforeFrame();
    vita2d_start_drawing();
    vita2d_clear_screen();
    if (video_backdrop_shown()) backdrop_draw((double) host_us() / 1e6, hx, hy, hw, hh);
}

static void end(void) {
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

static void swap_start(struct swap* sw, int kind, int split) {
    sw->cur ^= 1;
    sw->kind = kind;
    sw->split = split;
    sw->at = kind == VIDEO_SWAP_CUT ? 0 : host_us();
}

void video_page_swap(int kind) {
    swap_start(&pageSwap, kind, VIDEO_PAGE_SPLIT);
}

void video_companion_swap(int kind, int split) {
    // The old picture must be the same size as the new (a view change resizes the canvas: no animation then).
    swap_start(&companionSwap, kind, split);
}

// Part (sx, sy, sw, sh) of a texture at (x, y), k screen pixels a texel, alpha a.
static void part(vita2d_texture* t, float x, float y, float sx, float sy, float sw, float sh, float k, float a) {
    if (sw <= 0 || sh <= 0 || a <= 0) return;
    vita2d_draw_texture_tint_part_scale(t, x + sx * k, y + sy * k, sx, sy, sw, sh, k, k, RGBA8(0xFF, 0xFF, 0xFF, (unsigned) (255 * a + 0.5f)));
}

// The rows above the split, offset by (dx, dy) texels inside their own rectangle (what slides past an edge isn't drawn),
// or zoomed about their middle by z.
static void moved(vita2d_texture* t, float x, float y, int w, int split, float k, float dx, float dy, float z, float a) {
    if (z != 1) {
        float zw = w * k * z, zh = split * k * z;
        vita2d_draw_texture_tint_part_scale(t, x + (w * k - zw) / 2, y + (split * k - zh) / 2, 0, 0, (float) w, (float) split, k * z,
                                            k * z, RGBA8(0xFF, 0xFF, 0xFF, (unsigned) (255 * a + 0.5f)));
        return;
    }
    float sx = dx < 0 ? -dx : 0, sy = dy < 0 ? -dy : 0;
    float sw = w - fabsf(dx), sh = split - fabsf(dy);
    vita2d_draw_texture_tint_part_scale(t, x + (sx + dx) * k, y + (sy + dy) * k, sx, sy, sw, sh, k, k,
                                        RGBA8(0xFF, 0xFF, 0xFF, (unsigned) (255 * a + 0.5f)));
}

// A swapping picture w x h at (x, y), k screen pixels a texel, under alpha `all`.
static void draw_swapped(vita2d_texture* tex[2], struct swap* sw, float x, float y, int w, int h, float k, float all) {
    vita2d_texture* now = tex[sw->cur];
    float t = sw->at ? (float) (host_us() - sw->at) : SWAP_IN_US;
    if (t >= SWAP_IN_US || sw->split <= 0) {
        sw->at = 0;
        part(now, x, y, 0, 0, (float) w, (float) h, k, all);
        return;
    }
    float tin = ease(t / SWAP_IN_US), tout = t >= SWAP_OUT_US ? 1 : ease(t / SWAP_OUT_US);
    int split = sw->split < h ? sw->split : h;
    vita2d_texture* old = tex[sw->cur ^ 1];
    float dxIn = 0, dyIn = 0, dxOut = 0, dyOut = 0, zIn = 1, zOut = 1;
    float sixth = w / 6.0f, twelfth = split / 12.0f;
    switch (sw->kind) {
    case VIDEO_SWAP_LEFT: dxIn = sixth * (1 - tin), dxOut = -sixth * tout; break;   // forward: the new one from the right
    case VIDEO_SWAP_RIGHT: dxIn = -sixth * (1 - tin), dxOut = sixth * tout; break;
    case VIDEO_SWAP_UP: dyIn = twelfth * (1 - tin), dyOut = -twelfth * tout; break; // the next page: from below
    case VIDEO_SWAP_DOWN: dyIn = -twelfth * (1 - tin), dyOut = twelfth * tout; break;
    case VIDEO_SWAP_ZOOM: zIn = 0.94f + 0.06f * tin, zOut = 1 - 0.06f * tout; break;
    default: break;
    }
    moved(old, x, y, w, split, k, roundf(dxOut), roundf(dyOut), zOut, all * (1 - tout));
    moved(now, x, y, w, split, k, roundf(dxIn), roundf(dyIn), zIn, all * tin);
    part(now, x, y, 0, (float) split, (float) w, (float) (h - split), k, all);
}

void video_draw_page(void) {
    begin(0, 0, 0, 0);
    draw_swapped(pageTex, &pageSwap, 0, 0, VIDEO_PAGE_W, VIDEO_PAGE_H, 2, 1);
    end();
}

// The game's picture this frame, in the 400x240 (EXPAND's window, or the game in its view).
static void picture_rect(int screenMode, float* u, float* v, float* w, float* h, float* s) {
    if (widePx) {
        bool full = wideW >= TOP_W;
        *s = full ? 1.0f : (float) TOP_H / wideH;
        *w = wideW * *s, *h = wideH * *s;
        *u = (TOP_W - *w) / 2, *v = full ? (TOP_H - wideH) / 2.0f : 0;
    } else if (screenMode == PD_SCREEN_STRETCH) {
        *s = 1, *u = *v = 0, *w = TOP_W, *h = TOP_H;
    } else {
        *s = screenMode == PD_SCREEN_PIXEL ? 1.0f : screenMode == PD_SCREEN_SHARP_125 ? 1.25f : (float) TOP_H / gameH;
        *w = gameW * *s, *h = gameH * *s;
        *u = (TOP_W - *w) / 2, *v = (TOP_H - *h) / 2;
    }
}

// EXPAND's window with the game's own frame in its hole, as one picture (the 3DS writes them together too): drawn as two,
// the window's pixels under the hole (left stale by the cached margins) bled into the hole's edge when scaled - the dark
// line round the game.
static const uint32_t* wide_composed(void) {
    static uint32_t px[TOP_W * TOP_H];
    if (!wideHole.w) return widePx;
    memcpy(px, widePx, (size_t) wideW * wideH * 4);
    for (int y = 0; y < wideHole.h && wideHole.y + y < wideH; y++) {
        int w = wideHole.x + wideHole.w > wideW ? wideW - wideHole.x : wideHole.w;
        memcpy(px + (wideHole.y + y) * wideW + wideHole.x, gameBuf + (wideHole.gameY + y) * VIDEO_GAME_STRIDE + wideHole.gameX,
               (size_t) w * 4);
    }
    return px;
}

// A rectangle with pixel-stepped corners of radius r (the app's Canvas.roundRect / PixelRoundedShape).
static void frame_round(uint32_t* px, int stride, int fw, int fh, int x, int y, int rw, int rh, int r, uint32_t c) {
    int rr = r < rw / 2 ? r : rw / 2;
    if (rr > rh / 2) rr = rh / 2;
    for (int row = 0; row < rh; row++) {
        int fromEdge = row < rh - 1 - row ? row : rh - 1 - row, inset = 0;
        if (fromEdge < rr) {
            float d = rr - fromEdge - 0.5f;
            inset = (int) floorf(rr - sqrtf(rr * rr - d * d) + 0.5f);
        }
        int yy = y + row;
        if (yy < 0 || yy >= fh) continue;
        for (int xx = x + inset; xx < x + rw - inset; xx++)
            if (xx >= 0 && xx < fw) px[yy * stride + xx] = c;
    }
}

static void frame_fill(uint32_t* px, int stride, int fw, int fh, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < fh; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < fw; xx++) px[yy * stride + xx] = c;
}

// OVERLAY's frame round a picture at (x, y) w x h on the screen: the app's BuiltInFrames (DAISY, BEZEL) drawn at the
// game's pixel size f. DAISY's blue field and dots are the animated backdrop when that's shown (its rim stays); BEZEL
// is the console's dark face either way.
static void make_frame(float x, float y, float w, float h, int f) {
    bool clear = frameStyle == PD_FRAME_DAISY && video_backdrop_shown();
    int ww = (int) roundf(w / f), wh = (int) roundf(h / f);
    int gx = (int) ceilf(x / f), gy = (int) ceilf(y / f);
    int fw = gx + ww + (int) ceilf((VIDEO_W - x - w) / f), fh = gy + wh + (int) ceilf((VIDEO_H - y - h) / f);
    if (fw > FRAME_MAX_W) fw = FRAME_MAX_W;
    if (fh > FRAME_MAX_H) fh = FRAME_MAX_H;
    float ox = x - gx * f, oy = y - gy * f;
    if (frameMade.style == frameStyle && frameMade.f == f && frameMade.w == fw && frameMade.h == fh && frameMade.gx == gx &&
        frameMade.gy == gy && frameMade.ww == ww && frameMade.wh == wh && frameMade.clear == clear) {
        frameMade.x = ox, frameMade.y = oy;
        return;
    }
    vita2d_wait_rendering_done();
    uint32_t* px = vita2d_texture_get_datap(frameTex);
    int stride = (int) (vita2d_texture_get_stride(frameTex) / 4);
    memset(px, 0, (size_t) stride * FRAME_MAX_H * 4);
    // The surround: as thick as the margins allow, at most 8 of the frame's pixels (the app's 5 looked thin round a
    // 3x game on the Vita: the user asked for wider).
    int margin = gx < gy ? gx : gy, b = margin - 1 < 1 ? 1 : margin - 1 > 8 ? 8 : margin - 1;
    if (frameStyle == PD_FRAME_DAISY) {
        if (!clear) {
            frame_fill(px, stride, fw, fh, 0, 0, fw, fh, 0xFF3396FF);
            // The website's dot grid, lined up on the screen's corner.
            for (int yy = (gy % 6) - 3; yy < fh; yy += 6)
                for (int xx = (gx % 6) - 3; xx < fw; xx += 6) frame_fill(px, stride, fw, fh, xx, yy, 1, 1, 0xFF5CADFF);
        }
        frame_round(px, stride, fw, fh, gx - b + 1, gy - b + 1, ww + 2 * b, wh + 2 * b, b + 2, clear ? 0x60000000 : 0xFF2275D6);
        frame_round(px, stride, fw, fh, gx - b, gy - b, ww + 2 * b, wh + 2 * b, b + 2, 0xFF141C26);
        frame_round(px, stride, fw, fh, gx - b + 1, gy - b + 1, ww + 2 * b - 2, wh + 2 * b - 2, b + 1, 0xFF2A3646);
        frame_fill(px, stride, fw, fh, gx - b + 3, gy - b + 1, ww + 2 * b - 6, 1, 0xFF46566B);
        if (b > 1) frame_fill(px, stride, fw, fh, gx - 1, gy - 1, ww + 2, wh + 2, 0xFF0C1118);
    } else {
        frame_fill(px, stride, fw, fh, 0, 0, fw, fh, 0xFF16181C);
        frame_round(px, stride, fw, fh, gx - b, gy - b, ww + 2 * b, wh + 2 * b, b + 2, 0xFF24272D);
        frame_fill(px, stride, fw, fh, gx + 2, gy - b + 1, ww - 4, 1, 0xFF33373F);
        if (b > 1) frame_fill(px, stride, fw, fh, gx - 1, gy - 1, ww + 2, wh + 2, 0xFF0A0B0D);
    }
    // The game's own rectangle is see-through: it shows from under the frame.
    frame_fill(px, stride, fw, fh, gx, gy, ww, wh, 0);
    frameMade.style = frameStyle, frameMade.f = f, frameMade.w = fw, frameMade.h = fh, frameMade.gx = gx, frameMade.gy = gy;
    frameMade.ww = ww, frameMade.wh = wh, frameMade.clear = clear, frameMade.x = ox, frameMade.y = oy;
}

static void draw_game(int screenMode) {
    bool frame = framed();
    // A GBA game's black bars are the backdrop when it's shown (a Game Boy game's white ones stay: its look); a frame
    // stands in for them.
    if (!frame && (barColor || !video_backdrop_shown()))
        vita2d_draw_rectangle(topRect.x, topRect.y, topRect.w, topRect.h, abgr(barColor, 0xFF));
    bool upload = !holdGame;
    float u, v, w, h, s;
    picture_rect(screenMode, &u, &v, &w, &h, &s);
    if (widePx) {
        draw_picture(wideTex, wide2xTex, wide_composed(), wideW, wideW, wideH, u, v, s, true);
    } else if (screenMode == PD_SCREEN_STRETCH) {
        // Fills the 400x240: two scales, drawn linearly from the 2x copy.
        float k = topRect.k;
        if (upload) put2x(game2xTex, gameBuf, gameW, gameH, VIDEO_GAME_STRIDE);
        vita2d_draw_texture_part_scale(game2xTex, topRect.x, topRect.y, 0, 0, (float) (2 * gameW), (float) (2 * gameH),
                                       TOP_W * k / (2 * gameW), TOP_H * k / (2 * gameH));
    } else {
        draw_picture(gameTex, game2xTex, gameBuf, VIDEO_GAME_STRIDE, gameW, gameH, u, v, s, upload);
    }
    if (frame) {
        float k = topRect.k, eff = s * k;
        int f = (int) floorf(eff + 0.01f);
        if (f < 2) f = 2;
        make_frame(topRect.x + u * k, topRect.y + v * k, w * k, h * k, f);
        vita2d_draw_texture_part_scale(frameTex, frameMade.x, frameMade.y, 0, 0, (float) frameMade.w, (float) frameMade.h, (float) f,
                                       (float) f);
    }
    // The overlay (HUD, QUICK MENU) at the 400x240's scale, nearest when that's whole; a change of the QUICK MENU
    // crossfades from the picture before (150 ms).
    float t = fadeAt ? (float) (host_us() - fadeAt) / FADE_US : 1;
    if (t >= 1) t = 1, fadeAt = 0;
    t = ease(t);
    SceGxmTextureFilter of = whole(topRect.k) ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
    for (int layer = 0; layer < 2; layer++) {
        int i = layer ? overlayCur : overlayCur ^ 1;
        float a = layer ? t : 1 - t;
        if (a <= 0 || !overlayShown[i]) continue;
        // The QUICK MENU's dim: over the whole screen (the panel's half aside).
        if (overlayDim[i]) {
            unsigned dim = (unsigned) (0x55 * a);
            if (shownView == VIDEO_COMPANION_PANEL) vita2d_draw_rectangle(topRect.x, topRect.y, topRect.w, topRect.h, RGBA8(0, 0, 0, dim));
            else vita2d_draw_rectangle(0, 0, VIDEO_W, VIDEO_H, RGBA8(0, 0, 0, dim));
        }
        vita2d_texture_set_filters(overlayTex[i], of, of);
        vita2d_draw_texture_tint_part_scale(overlayTex[i], topRect.x, topRect.y, 0, 0, TOP_W, TOP_H, topRect.k, topRect.k,
                                            RGBA8(0xFF, 0xFF, 0xFF, (unsigned) (255 * a)));
    }
    if (perfW) {
        vita2d_texture_set_filters(perfTex, of, of);
        vita2d_draw_texture_part_scale(perfTex, topRect.x + perfX * topRect.k, topRect.y + perfY * topRect.k, 0, 0, (float) perfW, 16,
                                       topRect.k, topRect.k);
    }
}

void video_draw_game(int screenMode) {
    screenNow = screenMode;
    step_slide();
    place();
    // BEZEL covers the screen: no backdrop under it.
    bool hidden = framed() && frameStyle == PD_FRAME_BEZEL;
    bool on = backdropOn;
    if (hidden) backdropOn = false;
    // The backdrop's floating pictures go beside the game's picture (a column down each side).
    float u, v, w, h, sc;
    picture_rect(screenMode, &u, &v, &w, &h, &sc);
    float px = topRect.x + u * topRect.k, pw = w * topRect.k;
    if (shownView == VIDEO_COMPANION_PANEL) begin(topRect.x, topRect.y, topRect.w, topRect.h);
    else begin(px - 16, 0, pw + 32, VIDEO_H);
    backdropOn = on;
    draw_game(screenMode);
    if (shownView == VIDEO_COMPANION_CLOSED) {
        end();
        return;
    }
    float e = ease(slide);
    unsigned fade = (unsigned) (255 * e);
    if (dimAlpha > 0) vita2d_draw_rectangle(0, 0, VIDEO_W, VIDEO_H, RGBA8(0, 0, 0, (unsigned) (255 * dimAlpha)));
    const struct rect* r = &compRect;
    if (shownView == VIDEO_COMPANION_WINDOW) {
        // The window's frame: a shadow, a light edge, a dark line inside it.
        vita2d_draw_rectangle(r->x - 4, r->y + 2, r->w + 8, r->h + 8, RGBA8(0, 0, 0, fade * 5 / 10));
        vita2d_draw_rectangle(r->x - 4, r->y - 4, r->w + 8, r->h + 8, RGBA8(0xF4, 0xF4, 0xF8, fade));
        vita2d_draw_rectangle(r->x - 2, r->y - 2, r->w + 4, r->h + 4, RGBA8(0x35, 0x38, 0x45, fade));
    } else {
        vita2d_draw_rectangle(r->x - 4, 0, 4, VIDEO_H, RGBA8(0, 0, 0, 0x60));
        vita2d_draw_rectangle(r->x, 0, VIDEO_W - r->x, VIDEO_H, RGBA8(0x35, 0x38, 0x45, 0xFF));
    }
    SceGxmTextureFilter f = whole(r->k) ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
    vita2d_texture_set_filters(companionTex[0], f, f);
    vita2d_texture_set_filters(companionTex[1], f, f);
    draw_swapped(companionTex, &companionSwap, r->x, r->y, companionW, companionH, r->k,
                 shownView == VIDEO_COMPANION_WINDOW ? fade / 255.0f : 1);
    // The X: the window's top-right corner, the panel's tab outside its bottom-left one. (The tab bar's own L / R
    // chips name the shoulder buttons.)
    vita2d_draw_texture_tint_part_scale(chromeTex, closeRect.x, closeRect.y, X_AT, 0, X_SIZE, X_SIZE, 2, 2, RGBA8(0xFF, 0xFF, 0xFF, fade));
    end();
}

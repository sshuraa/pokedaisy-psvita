// The app's DaisyBackdrop (app/.../companion/ui/DaisyBackdrop.kt) on the Vita's GPU, behind the two screens: the theme's
// page, its soft blobs drifting and swelling (the site's CSS: 30 s ease-in-out each way), the faint dot grid, and six
// copies of the PokeDaisy mark - or the theme's own motif - floating up and back while turning once, in 48 steps. Dark
// mode: the theme's night page and glows. Every picture is a small texture made once (a blob is one soft white circle,
// tinted), so a frame is a dozen quads.
#include "backdrop.h"

#include <math.h>
#include <string.h>

#include <psp2/gxm.h>
#include <vita2d.h>

#include "../3ds/core/pd_icons_gen.h"
#include "video.h"

// The app's sizes are in dp; the Vita's screen is about 1.25 px a dp at the app's scale for a 5" screen.
#define DP 1.25f
#define BLOB_TEX 128
#define LOOP_S 600.0

static vita2d_texture *blobTex, *dotTex, *motifTex;
static const struct bd_theme* theme;
static int themeId = -1;
static bool night, dark;

// A blob: centre at x vw, y vh + yw of the longer side; radius r of the longer side; its drift [delay] s in.
static const struct { float x, y, yw, r, delay; } PLACES[5] = {
    { 11, 0, 9, 23, 0 }, { 90, 18, 20, 20, 10 }, { 45, 100, 5, 25, 20 }, { 80, 85, 0, 18, 5 }, { 30, 45, 0, 16, 15 },
};
// The site's petals: left %, top %, size, delay s, duration s.
static const struct { float x, y; int scale; float delay, duration; } PETALS[6] = {
    { 4, 22, 1, 0, 22 }, { 91, 10, 2, 4, 26 }, { 70, 62, 1, 9, 24 }, { 10, 78, 2, 2, 28 }, { 46, 92, 1, 6, 25 }, { 95, 70, 1, 12, 30 },
};

static vita2d_texture* make(int w, int h, SceGxmTextureFilter filter) {
    vita2d_texture* t = vita2d_create_empty_texture_format((unsigned) w, (unsigned) h, SCE_GXM_TEXTURE_FORMAT_A8R8G8B8);
    if (!t) return NULL;
    vita2d_texture_set_filters(t, filter, filter);
    memset(vita2d_texture_get_datap(t), 0, vita2d_texture_get_stride(t) * (unsigned) h);
    return t;
}

bool backdrop_init(void) {
    // A soft white circle: Compose's radialGradient stops (alpha .55 at the centre, .5 where the blob's own edge would
    // be, 0 at the blur's reach), the colour coming from the tint.
    blobTex = make(BLOB_TEX, BLOB_TEX, SCE_GXM_TEXTURE_FILTER_LINEAR);
    dotTex = make(VIDEO_W, VIDEO_H, SCE_GXM_TEXTURE_FILTER_POINT);
    if (!blobTex || !dotTex) return false;
    uint32_t* px = vita2d_texture_get_datap(blobTex);
    unsigned stride = vita2d_texture_get_stride(blobTex) / 4;
    for (int y = 0; y < BLOB_TEX; y++)
        for (int x = 0; x < BLOB_TEX; x++) {
            float dx = (x + 0.5f) / (BLOB_TEX / 2.0f) - 1, dy = (y + 0.5f) / (BLOB_TEX / 2.0f) - 1;
            float d = sqrtf(dx * dx + dy * dy), a;
            const float EDGE = 0.36f;
            if (d >= 1) a = 0;
            else if (d < EDGE) a = 0.55f - 0.05f * d / EDGE;
            else a = 0.5f * (1 - (d - EDGE) / (1 - EDGE));
            px[y * stride + x] = ((uint32_t) (a * 255 + 0.5f) << 24) | 0xFFFFFF;
        }
    // The dot grid: a 2 px dot every 18 px, white (tinted to the page's ink).
    px = vita2d_texture_get_datap(dotTex);
    stride = vita2d_texture_get_stride(dotTex) / 4;
    for (int y = 0; y < VIDEO_H; y++)
        for (int x = 0; x < VIDEO_W; x++)
            if (x % 18 < 2 && y % 18 < 2) px[y * stride + x] = 0xFFFFFFFF;
    backdrop_set_theme(8, false);
    return true;
}

void backdrop_exit(void) {
    vita2d_texture* all[] = { blobTex, dotTex, motifTex };
    for (unsigned i = 0; i < 3; i++)
        if (all[i]) vita2d_free_texture(all[i]);
    blobTex = dotTex = motifTex = NULL;
}

// The motif (or the mark) as a texture, its own pixels, drawn nearest.
static void make_motif(void) {
    int w, h;
    if (theme->motif) w = theme->motif->w, h = theme->motif->h;
    else w = PD_LOGO.w, h = PD_LOGO.h;
    if (motifTex) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(motifTex);
    }
    motifTex = make(w, h, SCE_GXM_TEXTURE_FILTER_POINT);
    if (!motifTex) return;
    uint32_t* px = vita2d_texture_get_datap(motifTex);
    unsigned stride = vita2d_texture_get_stride(motifTex) / 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            char ch = theme->motif ? theme->motif->rows[y][x] : PD_LOGO.rows[y][x];
            const char* keys = theme->motif ? theme->motif->keys : PD_LOGO.keys;
            const char* k = ch && ch != '.' ? strchr(keys, ch) : NULL;
            if (!k) continue;
            uint32_t c = theme->motif ? theme->motif->argb[k - keys] : (0xFF000000u | PD_LOGO.colors[k - keys]);
            px[y * stride + x] = c;
        }
}

void backdrop_set_theme(int id, bool isNight) {
    if (id == themeId && isNight == night && theme) return;
    bool motifChanged = id != themeId || !theme;
    themeId = id;
    night = isNight;
    theme = &bd_themes[0];
    for (int i = 0; i < bd_theme_count; i++)
        if (bd_themes[i].id == id) theme = &bd_themes[i];
    dark = night || theme->darkPage;
    if (motifChanged) make_motif();
}

bool backdrop_dark(void) {
    return dark;
}

static unsigned rgba(uint32_t rgb, unsigned alpha) {
    return RGBA8((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

// Where a petal at fraction f across an extent of n goes when [from, from + len) of it is covered: f of what's left,
// the covered band skipped.
static float around(float f, float n, float from, float len, float side) {
    float left = n - len, c = f * left;
    if (left < side) return f * n; // nothing to see beside it: as it was
    if (c >= from) c += len;
    return c;
}

void backdrop_draw(double seconds, float hx, float hy, float hw, float hh) {
    if (!theme) return;
    float t = (float) fmod(seconds, LOOP_S);
    const float W = VIDEO_W, H = VIDEO_H, vw = W / 100, vh = H / 100, big = (W > H ? W : H) / 100;
    vita2d_draw_rectangle(0, 0, W, H, rgba(night ? theme->nightPage : theme->page, 0xFF));
    int count = night ? theme->nightBlobCount : theme->blobCount;
    const uint32_t* colors = night ? theme->nightBlobs : theme->blobs;
    for (int i = 0; i < count && blobTex; i++) {
        float p = (1 - cosf((float) M_PI * fmodf((t + PLACES[i].delay) / 30.0f, 2.0f))) / 2;
        float cx = PLACES[i].x * vw + 6 * vw * p, cy = PLACES[i].y * vh + PLACES[i].yw * big + 4 * vh * p;
        float r = PLACES[i].r * big * (1 + 0.08f * p), reach = r + 80 * DP;
        float s = 2 * reach / BLOB_TEX;
        vita2d_draw_texture_tint_scale(blobTex, cx - reach, cy - reach, s, s, rgba(colors[i], 0xFF));
    }
    // The site's ink at 9%; at night white at about 6%.
    if (dotTex) vita2d_draw_texture_tint(dotTex, 0, 0, dark ? RGBA8(0xFF, 0xFF, 0xFF, 0x10) : RGBA8(0x1D, 0x1B, 0x26, 0x17));
    if (!motifTex) return;
    int mw = (int) vita2d_texture_get_width(motifTex);
    bool band = hw >= W - 1, column = hh >= H - 1 && !band;
    // A theme's picture is clearer than the mark (it's the theme), but still a backdrop.
    unsigned alpha = theme->motif ? (dark ? 72 : 96) : (dark ? 44 : 60);
    for (int i = 0; i < 6; i++) {
        // As big as the 32-pixel mark at 2 or 3 screen pixels a pixel: a whole number of them a motif pixel.
        float cell = roundf((PETALS[i].scale == 1 ? 64.0f : 88.0f) / mw);
        if (cell < 1) cell = 1;
        float side = cell * mw;
        float x = PETALS[i].x / 100 * W, y = PETALS[i].y / 100 * H;
        float room = H;
        if (band && H - hh >= side) {
            // Side by side the two screens leave a strip above and one below: four in the top one, two in the bottom
            // one (the legend's), each a little off the strip's middle by the site's height for it.
            static const float ACROSS[6] = { 4, 94, 33, 6, 64, 91 };
            bool top = i != 1 && i != 3;
            float from = top ? 0 : hy + hh, size = top ? hy : H - hy - hh;
            x = ACROSS[i] / 100 * W;
            y = from + size * (0.5f + 0.3f * (PETALS[i].y / 100 - 0.5f));
            room = size;
        } else if (column) {
            x = around(PETALS[i].x / 100, W, hx, hw, side / 2);
        }
        x = fmaxf(side / 2 + 4, fminf(W - side / 2 - 4, x));
        // Up and back over its time, turning once: 48 steps, the site's pixel look.
        float phase = floorf(fmodf((t + PETALS[i].delay) / PETALS[i].duration, 1.0f) * 48) / 48;
        float rise = room * 0.3f < 75 ? room * 0.3f : 75;
        float lift = -rise * (1 - fabsf(2 * phase - 1));
        vita2d_draw_texture_tint_scale_rotate(motifTex, x, y + lift, cell, cell, phase * 2 * (float) M_PI, RGBA8(0xFF, 0xFF, 0xFF, alpha));
    }
}

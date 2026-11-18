// pd_vita_sim's PS Vita: the Sony calls and the vita2d calls the Vita port makes (../main.c, ../video.c, ../audio.c,
// ../host.c), on a desktop with no Vita - vita2d drawn in software into a 960x544 picture, the buttons, the sticks and the
// front touch screen played from a script, a PNG of the screen on `shot`. The port's own files are built unchanged.
//
//   PD_VITA_SCRIPT=<file> PD_VITA_SHOTS=<dir> pd_vita_sim      (run where ux0:data/pokedaisy/ should be: it's relative)
//
// Script lines (# starts a comment), each running for its frames, one frame a vita2d_swap_buffers:
//   wait N                  N frames, nothing held
//   press BUTTONS [N]       BUTTONS (cross+up ...) held N frames (3), then 6 frames free
//   touch X Y [N]           a finger at (X, Y) on the 960x544 screen N frames (3), then 6 frames free
//   rstick DIR [N]          the right stick pushed up / down / left / right N frames (3), then 6 frames free
//   lstick DIR [N]          the left stick, the same
//   shot NAME               NAME.png: the screen as it is
//   quit                    the app ends there
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include "../../3ds/desktop/pd_png.h"

#define W 960
#define H 544

// --- the script ---

static FILE* script;
static const char* shotDir = ".";
static unsigned frame;
static uint32_t buttons;
static uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
static int touchX = -1, touchY = -1;
static int holdLeft, gapLeft;
static uint32_t screen[W * H];

static uint32_t parse_buttons(const char* text) {
    static const struct { const char* name; uint32_t bit; } B[] = {
        { "cross", SCE_CTRL_CROSS }, { "circle", SCE_CTRL_CIRCLE }, { "square", SCE_CTRL_SQUARE },
        { "triangle", SCE_CTRL_TRIANGLE }, { "l", SCE_CTRL_LTRIGGER }, { "r", SCE_CTRL_RTRIGGER },
        { "start", SCE_CTRL_START }, { "select", SCE_CTRL_SELECT }, { "up", SCE_CTRL_UP }, { "down", SCE_CTRL_DOWN },
        { "left", SCE_CTRL_LEFT }, { "right", SCE_CTRL_RIGHT },
    };
    char buf[128];
    snprintf(buf, sizeof buf, "%s", text);
    uint32_t bits = 0;
    for (char* k = strtok(buf, "+"); k; k = strtok(NULL, "+"))
        for (size_t i = 0; i < sizeof B / sizeof *B; i++)
            if (!strcasecmp(k, B[i].name)) bits |= B[i].bit;
    return bits;
}

static void stick(const char* dir, uint8_t* x, uint8_t* y) {
    *x = *y = 128;
    if (!strcasecmp(dir, "up")) *y = 0;
    if (!strcasecmp(dir, "down")) *y = 255;
    if (!strcasecmp(dir, "left")) *x = 0;
    if (!strcasecmp(dir, "right")) *x = 255;
}

static void release(void) {
    buttons = 0;
    lx = ly = rx = ry = 128;
    touchX = touchY = -1;
}

// One frame of the script: what's held now (and shots / quit, which take no frame).
static void script_step(void) {
    frame++;
    if (holdLeft > 0 && --holdLeft == 0) {
        release();
        gapLeft = 6;
    }
    if (holdLeft > 0) return;
    if (gapLeft > 0 && --gapLeft > 0) return;
    if (!script) return;
    char line[256];
    while (fgets(line, sizeof line, script)) {
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        char cmd[32] = "", a[128] = "", b[64] = "", c[64] = "";
        int n = sscanf(line, "%31s %127s %63s %63s", cmd, a, b, c);
        if (n < 1) continue;
        if (!strcmp(cmd, "shot")) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s.png", shotDir, a);
            pd_png_write(path, W, H, screen);
            printf("shot %s (frame %u)\n", path, frame);
            fflush(stdout);
            continue;
        }
        if (!strcmp(cmd, "quit")) {
            printf("quit at frame %u\n", frame);
            exit(0);
        }
        if (!strcmp(cmd, "wait")) {
            gapLeft = atoi(a) + 1;
            return;
        }
        if (!strcmp(cmd, "press")) {
            buttons = parse_buttons(a);
            holdLeft = n >= 3 ? atoi(b) : 3;
            return;
        }
        if (!strcmp(cmd, "touch")) {
            touchX = atoi(a);
            touchY = atoi(b);
            holdLeft = n >= 4 ? atoi(c) : 3;
            return;
        }
        if (!strcmp(cmd, "rstick") || !strcmp(cmd, "lstick")) {
            if (cmd[0] == 'r') stick(a, &rx, &ry);
            else stick(a, &lx, &ly);
            holdLeft = n >= 3 ? atoi(b) : 3;
            return;
        }
        fprintf(stderr, "script: what's '%s'?\n", cmd);
    }
    printf("script ended at frame %u\n", frame);
    exit(0);
}

__attribute__((constructor)) static void sim_init(void) {
    const char* s = getenv("PD_VITA_SCRIPT");
    if (s) script = fopen(s, "r");
    const char* d = getenv("PD_VITA_SHOTS");
    if (d) shotDir = d;
}

// --- vita2d in software ---

struct ftex {
    int w, h, stride; // stride in pixels
    SceGxmTextureFormat format;
    SceGxmTextureFilter minf, magf;
    uint32_t* data;
};

static unsigned clearColor;

int vita2d_init(void) { return 1; }
int vita2d_fini(void) { return 1; }
void vita2d_wait_rendering_done(void) {}
void vita2d_set_vblank_wait(int enable) { (void) enable; }
void vita2d_set_clear_color(unsigned int color) { clearColor = color; }
void vita2d_start_drawing(void) {}
void vita2d_end_drawing(void) {}
int vita2d_common_dialog_update(void) { return 0; }

// RGBA8 (0xAABBGGRR) to the PNG's 0xRRGGBB.
static uint32_t abgr_rgb(unsigned c) {
    return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

void vita2d_clear_screen(void) {
    uint32_t c = abgr_rgb(clearColor);
    for (int i = 0; i < W * H; i++) screen[i] = c;
}

void vita2d_swap_buffers(void) {
    script_step();
}

vita2d_texture* vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format) {
    struct ftex* t = calloc(1, sizeof *t);
    t->w = (int) w;
    t->h = (int) h;
    t->stride = (int) ((w + 7) & ~7u);
    t->format = format;
    t->data = calloc((size_t) t->stride * h, 4);
    return (vita2d_texture*) t;
}

void vita2d_free_texture(vita2d_texture* texture) {
    struct ftex* t = (struct ftex*) texture;
    if (!t) return;
    free(t->data);
    free(t);
}

void* vita2d_texture_get_datap(const vita2d_texture* texture) { return ((const struct ftex*) texture)->data; }
unsigned int vita2d_texture_get_stride(const vita2d_texture* texture) { return (unsigned) ((const struct ftex*) texture)->stride * 4; }
unsigned int vita2d_texture_get_width(const vita2d_texture* texture) { return (unsigned) ((const struct ftex*) texture)->w; }
unsigned int vita2d_texture_get_height(const vita2d_texture* texture) { return (unsigned) ((const struct ftex*) texture)->h; }
void vita2d_texture_set_filters(vita2d_texture* texture, SceGxmTextureFilter min_filter, SceGxmTextureFilter mag_filter) {
    struct ftex* t = (struct ftex*) texture;
    t->minf = min_filter;
    t->magf = mag_filter;
}

// A texel as 0xAARRGGBB, by the texture's format.
static uint32_t texel(const struct ftex* t, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= t->w) x = t->w - 1;
    if (y >= t->h) y = t->h - 1;
    uint32_t p = t->data[y * t->stride + x];
    switch (t->format) {
    case SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR:
        return 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
    case SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB:
        return 0xFF000000u | (p & 0xFFFFFF);
    case SCE_GXM_TEXTURE_FORMAT_A8R8G8B8:
        return p;
    default: // vita2d's default, A8B8G8R8
        return (p & 0xFF000000u) | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
    }
}

static uint32_t sample(const struct ftex* t, float u, float v, bool linear) {
    if (!linear) return texel(t, (int) floorf(u), (int) floorf(v));
    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = (int) floorf(fu), y0 = (int) floorf(fv);
    float ax = fu - x0, ay = fv - y0;
    uint32_t q[4] = { texel(t, x0, y0), texel(t, x0 + 1, y0), texel(t, x0, y0 + 1), texel(t, x0 + 1, y0 + 1) };
    float wgt[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float s = 0;
        for (int i = 0; i < 4; i++) s += wgt[i] * (float) ((q[i] >> sh) & 0xFF);
        out |= (uint32_t) (s + 0.5f) << sh;
    }
    return out;
}

static void blend(int x, int y, uint32_t argb) {
    unsigned a = argb >> 24;
    if (!a) return;
    uint32_t* d = &screen[y * W + x];
    if (a == 0xFF) {
        *d = argb & 0xFFFFFF;
        return;
    }
    uint32_t o = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        unsigned s = (argb >> sh) & 0xFF, b = (*d >> sh) & 0xFF;
        o |= ((s * a + b * (255 - a)) / 255) << sh;
    }
    *d = o;
}

void vita2d_draw_texture_part_scale(const vita2d_texture* texture, float x, float y, float tex_x, float tex_y, float tex_w,
                                    float tex_h, float x_scale, float y_scale) {
    const struct ftex* t = (const struct ftex*) texture;
    bool linear = (x_scale >= 1 ? t->magf : t->minf) == SCE_GXM_TEXTURE_FILTER_LINEAR;
    int x0 = (int) ceilf(x - 0.5f), y0 = (int) ceilf(y - 0.5f);
    int x1 = (int) ceilf(x + tex_w * x_scale - 0.5f), y1 = (int) ceilf(y + tex_h * y_scale - 0.5f);
    for (int py = y0 < 0 ? 0 : y0; py < y1 && py < H; py++)
        for (int px = x0 < 0 ? 0 : x0; px < x1 && px < W; px++) {
            float u = tex_x + (px + 0.5f - x) / x_scale, v = tex_y + (py + 0.5f - y) / y_scale;
            blend(px, py, sample(t, u, v, linear));
        }
}

void vita2d_draw_texture_scale(const vita2d_texture* texture, float x, float y, float x_scale, float y_scale) {
    const struct ftex* t = (const struct ftex*) texture;
    vita2d_draw_texture_part_scale(texture, x, y, 0, 0, (float) t->w, (float) t->h, x_scale, y_scale);
}

// vita2d's tint shader: the texel times the colour (RGBA8), alpha included.
static uint32_t tinted(uint32_t argb, unsigned color) {
    uint32_t mul = (color & 0xFF000000u) | abgr_rgb(color), o = 0;
    for (int sh = 0; sh < 32; sh += 8) o |= ((((argb >> sh) & 0xFF) * ((mul >> sh) & 0xFF) + 127) / 255) << sh;
    return o;
}

void vita2d_draw_texture_tint_scale(const vita2d_texture* texture, float x, float y, float x_scale, float y_scale, unsigned int color) {
    const struct ftex* t = (const struct ftex*) texture;
    bool linear = (x_scale >= 1 ? t->magf : t->minf) == SCE_GXM_TEXTURE_FILTER_LINEAR;
    int x0 = (int) ceilf(x - 0.5f), y0 = (int) ceilf(y - 0.5f);
    int x1 = (int) ceilf(x + t->w * x_scale - 0.5f), y1 = (int) ceilf(y + t->h * y_scale - 0.5f);
    for (int py = y0 < 0 ? 0 : y0; py < y1 && py < H; py++)
        for (int px = x0 < 0 ? 0 : x0; px < x1 && px < W; px++) {
            float u = (px + 0.5f - x) / x_scale, v = (py + 0.5f - y) / y_scale;
            blend(px, py, tinted(sample(t, u, v, linear), color));
        }
}

// The real GXM's edge modes; the fake samples clamped anyway.
int sceGxmTextureSetUAddrMode(SceGxmTexture* texture, SceGxmTextureAddrMode mode) { (void) texture; (void) mode; return 0; }
int sceGxmTextureSetVAddrMode(SceGxmTexture* texture, SceGxmTextureAddrMode mode) { (void) texture; (void) mode; return 0; }

void vita2d_draw_texture_tint_part_scale(const vita2d_texture* texture, float x, float y, float tex_x, float tex_y, float tex_w,
                                         float tex_h, float x_scale, float y_scale, unsigned int color) {
    const struct ftex* t = (const struct ftex*) texture;
    bool linear = (x_scale >= 1 ? t->magf : t->minf) == SCE_GXM_TEXTURE_FILTER_LINEAR;
    int x0 = (int) ceilf(x - 0.5f), y0 = (int) ceilf(y - 0.5f);
    int x1 = (int) ceilf(x + tex_w * x_scale - 0.5f), y1 = (int) ceilf(y + tex_h * y_scale - 0.5f);
    for (int py = y0 < 0 ? 0 : y0; py < y1 && py < H; py++)
        for (int px = x0 < 0 ? 0 : x0; px < x1 && px < W; px++) {
            float u = tex_x + (px + 0.5f - x) / x_scale, v = tex_y + (py + 0.5f - y) / y_scale;
            blend(px, py, tinted(sample(t, u, v, linear), color));
        }
}

void vita2d_draw_texture_tint(const vita2d_texture* texture, float x, float y, unsigned int color) {
    vita2d_draw_texture_tint_scale(texture, x, y, 1, 1, color);
}

// Turned by rad about its centre, which lands on (x, y).
void vita2d_draw_texture_tint_scale_rotate(const vita2d_texture* texture, float x, float y, float x_scale, float y_scale, float rad,
                                           unsigned int color) {
    const struct ftex* t = (const struct ftex*) texture;
    bool linear = (x_scale >= 1 ? t->magf : t->minf) == SCE_GXM_TEXTURE_FILTER_LINEAR;
    float hw = t->w * x_scale / 2, hh = t->h * y_scale / 2, reach = sqrtf(hw * hw + hh * hh);
    float c = cosf(rad), s = sinf(rad);
    for (int py = (int) (y - reach); py <= (int) (y + reach); py++)
        for (int px = (int) (x - reach); px <= (int) (x + reach); px++) {
            if (px < 0 || py < 0 || px >= W || py >= H) continue;
            float dx = px + 0.5f - x, dy = py + 0.5f - y;
            float lx = c * dx + s * dy, ly = -s * dx + c * dy;
            if (lx < -hw || lx >= hw || ly < -hh || ly >= hh) continue;
            blend(px, py, tinted(sample(t, (lx + hw) / x_scale, (ly + hh) / y_scale, linear), color));
        }
}

void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color) {
    uint32_t argb = (color & 0xFF000000u) | abgr_rgb(color);
    for (int py = (int) y; py < (int) (y + h) && py < H; py++)
        for (int px = (int) x; px < (int) (x + w) && px < W; px++)
            if (px >= 0 && py >= 0) blend(px, py, argb);
}

// --- the Sony calls ---

int sceCtrlSetSamplingMode(SceCtrlPadInputMode mode) { (void) mode; return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData* pad, int count) {
    (void) port; (void) count;
    memset(pad, 0, sizeof *pad);
    pad->buttons = buttons;
    pad->lx = lx, pad->ly = ly, pad->rx = rx, pad->ry = ry;
    return 1;
}

int sceTouchSetSamplingState(SceUInt32 port, SceTouchSamplingState state) { (void) port; (void) state; return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData* data, SceUInt32 count) {
    (void) port; (void) count;
    memset(data, 0, sizeof *data);
    if (touchX >= 0) {
        data->reportNum = 1;
        data->report[0].x = (SceUInt16) (touchX * 2);
        data->report[0].y = (SceUInt16) (touchY * 2);
    }
    return 1;
}

int sceSysmoduleLoadModule(SceSysmoduleModuleId id) { (void) id; return 0; }
int sceAppUtilInit(SceAppUtilInitParam* init, SceAppUtilBootParam* boot) { (void) init; (void) boot; return 0; }
int sceCommonDialogSetConfigParam(const SceCommonDialogConfigParam* p) { (void) p; return 0; }
int sceImeDialogInit(const SceImeDialogParam* p) { (void) p; return -1; }
SceCommonDialogStatus sceImeDialogGetStatus(void) { return SCE_COMMON_DIALOG_STATUS_FINISHED; }
int sceImeDialogGetResult(SceImeDialogResult* r) { (void) r; return 0; }
int sceImeDialogTerm(void) { return 0; }
int sceDisplayGetRefreshRate(float* rate) { *rate = 59.94f; return 0; }

int sceKernelExitProcess(int res) { exit(res); }
SceUInt64 sceKernelGetProcessTimeWide(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (SceUInt64) t.tv_sec * 1000000u + (SceUInt64) t.tv_nsec / 1000u;
}
int sceKernelPowerTick(SceKernelPowerTickType type) { (void) type; return 0; }

int scePowerSetArmClockFrequency(int f) { (void) f; return 0; }
int scePowerSetBusClockFrequency(int f) { (void) f; return 0; }
int scePowerSetGpuClockFrequency(int f) { (void) f; return 0; }
int scePowerSetGpuXbarClockFrequency(int f) { (void) f; return 0; }
int scePowerGetBatteryLifePercent(void) { return 87; }
SceBool scePowerIsBatteryCharging(void) { return SCE_FALSE; }

// sceAudioOut: a period takes its time to play.
int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode) {
    (void) type; (void) len; (void) freq; (void) mode;
    return 1;
}
// Like the Vita's, the buffer handed over is read while the call after it waits (mGBA's psp2-context.c keeps its samples
// until the next call for that reason): the period before is "played" - copied into PD_VITA_PLAYED, a raw stereo PCM16 file
// - only once this period's wait is over.
// The start of a period is fetched when it's handed over, the rest as it plays (after this call has returned).
#define FETCHED 32
static const int16_t* playing;
static int16_t playingHead[FETCHED * 2];
static FILE* played;
int sceAudioOutOutput(int port, const void* buf) {
    (void) port;
    usleep(512 * 1000000 / 48000);
    if (!played && getenv("PD_VITA_PLAYED")) played = fopen(getenv("PD_VITA_PLAYED"), "wb");
    if (playing && played) {
        fwrite(playingHead, 4, FETCHED, played);
        fwrite(playing + FETCHED * 2, 4, 512 - FETCHED, played);
    }
    memcpy(playingHead, buf, sizeof playingHead);
    playing = buf;
    return 0;
}
int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag ch, int* vol) { (void) port; (void) ch; (void) vol; return 0; }
int sceAudioOutReleasePort(int port) { (void) port; return 0; }

// Threads on pthreads.
struct thread_start {
    SceKernelThreadEntry entry;
    pthread_t t;
};
static struct thread_start threads[8];
static int threadCount;

static void* thread_main(void* arg) {
    struct thread_start* s = arg;
    s->entry(0, NULL);
    return NULL;
}

SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr,
                             int cpuAffinityMask, const SceKernelThreadOptParam* option) {
    (void) name; (void) initPriority; (void) stackSize; (void) attr; (void) cpuAffinityMask; (void) option;
    if (threadCount >= 8) return -1;
    threads[threadCount].entry = entry;
    return threadCount++;
}

int sceKernelStartThread(SceUID thid, SceSize arglen, void* argp) {
    (void) arglen; (void) argp;
    return pthread_create(&threads[thid].t, NULL, thread_main, &threads[thid]);
}

int sceKernelWaitThreadEnd(SceUID thid, int* stat, SceUInt* timeout) {
    (void) stat; (void) timeout;
    return pthread_join(threads[thid].t, NULL);
}

int sceKernelExitDeleteThread(int status) {
    (void) status;
    pthread_exit(NULL);
}

// The headers' inline param initialisers (sceImeDialogParamInit ...) call it.
void* sceClibMemset(void* dst, int ch, SceSize len) {
    return memset(dst, ch, len);
}

int sceKernelGetThreadId(void) { return 0; }
// No run clocks here: the meter reads 0 in the harness.
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo* info) { (void) thid; info->runClocks = 0; return 0; }
int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int mask) { (void) thid; (void) mask; return 0; }

// Memory blocks: plain allocations here.
#include <psp2/kernel/sysmem.h>
static void* blocks[16];
static int blockCount;
SceUID sceKernelAllocMemBlock(const char* name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt* opt) {
    (void) name; (void) type; (void) opt;
    if (blockCount >= 16) return -1;
    blocks[blockCount] = calloc(1, size);
    return blocks[blockCount] ? blockCount++ : -1;
}
int sceKernelGetMemBlockBase(SceUID uid, void** base) {
    if (uid < 0 || uid >= blockCount) return -1;
    *base = blocks[uid];
    return 0;
}

int sceKernelDelayThread(SceUInt delay) { usleep(delay); return 0; }

// PokeDaisy's sound test ROM: 10 s of a pure 418 Hz sine on DirectSound A (timer 0 at 13379 Hz, DMA 1 refilling the FIFO,
// restarted each vblank - the way the games' m4a engine plays its music), then 10 s of a 440 Hz square on the Game Boy
// channel 2, then silence. Screen: blue for the sine, green for the square, grey after.
#include <stdint.h>
#define REG16(a) (*(volatile uint16_t*) (a))
#define REG32(a) (*(volatile uint32_t*) (a))
#define DISPCNT REG16(0x04000000)
#define VCOUNT REG16(0x04000006)
#define SOUNDCNT_L REG16(0x04000080)
#define SOUNDCNT_H REG16(0x04000082)
#define SOUNDCNT_X REG16(0x04000084)
#define SOUND2CNT_L REG16(0x04000068)
#define SOUND2CNT_H REG16(0x0400006C)
#define TM0CNT_L REG16(0x04000100)
#define TM0CNT_H REG16(0x04000102)
#define DMA1SAD REG32(0x040000BC)
#define DMA1DAD REG32(0x040000C0)
#define DMA1CNT_L REG16(0x040000C4)
#define DMA1CNT_H REG16(0x040000C6)
#define BG_COLOR REG16(0x05000000)

static const int8_t SINE32[32] = { 0, 23, 45, 66, 84, 98, 108, 114, 116, 114, 108, 98, 84, 66, 45, 23,
                                   0, -23, -45, -66, -84, -98, -108, -114, -116, -114, -108, -98, -84, -66, -45, -23 };
// One frame of samples at 13379 Hz: 224, exactly 7 cycles of the 32-sample sine (1254 cycles a sample * 224 = a frame).
static int8_t buffer[224] __attribute__((aligned(4)));

static void wait_vblank(void) {
    while (VCOUNT != 160) {}
}

int main(void) {
    DISPCNT = 0x0080; // forced blank: the backdrop colour shows
    for (int i = 0; i < 224; i++) buffer[i] = SINE32[i & 31];
    SOUNDCNT_X = 0x0080;                          // master on
    SOUNDCNT_L = 0;
    SOUNDCNT_H = 0x0004 | 0x0100 | 0x0200 | 0x0800; // A at 100%, left + right, timer 0, FIFO reset
    TM0CNT_L = 65536 - 1254;
    TM0CNT_H = 0x0080;
    DISPCNT = 0;
    BG_COLOR = 0x7C00; // blue
    unsigned frame = 0;
    for (;;) {
        wait_vblank();
        if (frame < 600) {
            DMA1CNT_H = 0;
            DMA1SAD = (uint32_t) buffer;
            DMA1DAD = 0x040000A0;
            DMA1CNT_L = 4;
            DMA1CNT_H = 0x8000 | 0x3000 | 0x0400 | 0x0200 | 0x0040; // on, FIFO timing, 32-bit, repeat, dest fixed
        } else if (frame == 600) {
            DMA1CNT_H = 0;
            SOUNDCNT_H = 0x0002 | 0x0800;   // PSG at 100%, A off (FIFO reset)
            SOUNDCNT_L = 0x7700 | 0x2200 | 0x0077; // channel 2 left + right, full PSG volume
            SOUND2CNT_L = 0xF080;           // duty 50%, envelope: volume 15, no sweep
            SOUND2CNT_H = 0x8000 | (2048 - 131072 / 440); // restart, 440 Hz
            BG_COLOR = 0x03E0; // green
        } else if (frame == 1200) {
            SOUNDCNT_L = 0;
            BG_COLOR = 0x4210; // grey
        }
        frame++;
        while (VCOUNT == 160) {}
    }
}

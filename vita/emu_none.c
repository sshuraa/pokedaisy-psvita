// The Vita build plays every game on mGBA: armGBA (the 3DS's ARM11 recompiler) and gpSP 3D aren't built. The shared code
// asks pd_armgba_is() / pd_gpsp_is() before it reaches either core, so here both are never the core, and the calls behind
// those checks are never made.
#include "../3ds/emu/pd_armgba.h"
#include "../3ds/emu/pd_gpsp.h"

bool pd_armgba_is(const struct mCore* core) { (void) core; return false; }
bool pd_gpsp_is(const struct mCore* core) { (void) core; return false; }

bool pd_armgba_write(uint32_t addr, const void* data, size_t len) { (void) addr; (void) data; (void) len; return false; }
bool pd_gpsp_write(uint32_t addr, const void* data, size_t len) { (void) addr; (void) data; (void) len; return false; }
bool pd_armgba_rom_write(uint32_t addr, const void* data, size_t len) { (void) addr; (void) data; (void) len; return false; }
bool pd_gpsp_rom_write(uint32_t addr, const void* data, size_t len) { (void) addr; (void) data; (void) len; return false; }
void pd_armgba_set_bkpt(bool (*handler)(uint32_t address, unsigned immediate)) { (void) handler; }
void pd_gpsp_set_bkpt(bool (*hook)(uint32_t pc, unsigned immediate)) { (void) hook; }
bool pd_armgba_save_state(const char* path) { (void) path; return false; }
bool pd_gpsp_save_state(const char* path) { (void) path; return false; }
enum pd_armgba_state pd_armgba_load_state(const char* path) { (void) path; return PD_ARMGBA_STATE_NO_FILE; }
enum pd_gpsp_state pd_gpsp_load_state(const char* path) { (void) path; return PD_GPSP_STATE_NO_FILE; }
uint32_t pd_armgba_reg(int n) { (void) n; return 0; }
uint32_t pd_gpsp_reg(int n) { (void) n; return 0; }
void pd_armgba_set_reg(int n, uint32_t value) { (void) n; (void) value; }
void pd_gpsp_set_reg(int n, uint32_t v) { (void) n; (void) v; }
int pd_armgba_vcount(void) { return 0; }
int pd_gpsp_vcount(void) { return 0; }
void pd_armgba_skip_video(bool skip) { (void) skip; }
void pd_gpsp_skip_video(bool skip) { (void) skip; }
void pd_armgba_jump(uint32_t to) { (void) to; }
void pd_gpsp_jump(uint32_t to) { (void) to; }
bool pd_armgba_fast_flash_saves(long* start, long* stop) { (void) start; (void) stop; return false; }

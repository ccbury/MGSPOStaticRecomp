// Shared helpers for the firmware library implementations. Not part of the public runtime API.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <psp/kernel.h>
#include <psp/module.h>
#include <psp/runtime.h>

namespace psp {
namespace k {

// Error codes returned to the guest.
constexpr uint32_t E_ERROR = 0x80020001;
constexpr uint32_t E_UNKNOWN_UID = 0x800200CB;
constexpr uint32_t E_NO_MEMORY = 0x80020190;
constexpr uint32_t E_ILLEGAL_ATTR = 0x80020191;
constexpr uint32_t E_ILLEGAL_ENTRY = 0x80020192;
constexpr uint32_t E_ILLEGAL_PRIORITY = 0x80020193;
constexpr uint32_t E_ILLEGAL_STACK = 0x80020194;
constexpr uint32_t E_ILLEGAL_MODE = 0x80020195;
constexpr uint32_t E_ILLEGAL_MASK = 0x80020196;
constexpr uint32_t E_ILLEGAL_THREAD = 0x80020197;
constexpr uint32_t E_NOT_FOUND_THREAD = 0x80020198;
constexpr uint32_t E_NOT_FOUND_SEMA = 0x80020199;
constexpr uint32_t E_NOT_FOUND_EVF = 0x8002019A;
constexpr uint32_t E_NOT_FOUND_FPL = 0x8002019D;
constexpr uint32_t E_NOT_FOUND_ALARM = 0x8002019F;
constexpr uint32_t E_NOT_FOUND_CALLBACK = 0x800201A1;
constexpr uint32_t E_NOT_FOUND_MUTEX = 0x800201A2;
constexpr uint32_t E_DORMANT = 0x800201A3;
constexpr uint32_t E_NOT_DORMANT = 0x800201A4;
constexpr uint32_t E_NOT_SUSPEND = 0x800201A5;
constexpr uint32_t E_THREAD_TERMINATED = 0x800201A7;
constexpr uint32_t E_WAIT_TIMEOUT = 0x800201A8;
constexpr uint32_t E_WAIT_CANCEL = 0x800201A9;
constexpr uint32_t E_WAIT_DELETE = 0x800201AA;
constexpr uint32_t E_SEMA_ZERO = 0x800201AD;
constexpr uint32_t E_SEMA_OVF = 0x800201AE;
constexpr uint32_t E_EVF_COND = 0x800201AF;
constexpr uint32_t E_EVF_MULTI = 0x800201B0;
constexpr uint32_t E_ILLEGAL_COUNT = 0x800201B1;
constexpr uint32_t E_MUTEX_LOCKED = 0x800201B3;
constexpr uint32_t E_MUTEX_UNLOCKED = 0x800201B4;
constexpr uint32_t E_MUTEX_OVF = 0x800201B5;
constexpr uint32_t E_FPL_NOMEM = 0x800201B8;

constexpr uint32_t E_IO_NOENT = 0x80010002;
constexpr uint32_t E_IO_IO = 0x80010005;
constexpr uint32_t E_IO_BADF = 0x80010009;
constexpr uint32_t E_IO_ACCES = 0x8001000D;
constexpr uint32_t E_IO_EXIST = 0x80010011;
constexpr uint32_t E_IO_NODEV = 0x80010013;
constexpr uint32_t E_IO_NOTDIR = 0x80010014;
constexpr uint32_t E_IO_ISDIR = 0x80010015;
constexpr uint32_t E_IO_INVAL = 0x80010016;
constexpr uint32_t E_IO_MFILE = 0x80010018;
constexpr uint32_t E_IO_ROFS = 0x8001001E;

constexpr uint32_t E_NET_UNAVAILABLE = 0x80410001;

constexpr int kIntrVblank = 30;

// Syscall argument registers: a0-a3 then t0-t3.
inline uint32_t arg(const Cpu& c, int i) { return c.r[4 + i]; }
inline void ret(Cpu& c, uint32_t v) { c.r[2] = v; }
inline void ret64(Cpu& c, uint64_t v) {
  c.r[2] = static_cast<uint32_t>(v);
  c.r[3] = static_cast<uint32_t>(v >> 32);
}
inline uint64_t arg64(const Cpu& c, int i) { return (static_cast<uint64_t>(c.r[5 + i]) << 32) | c.r[4 + i]; }

// Writes through a guest pointer; null pointers are ignored.
inline void put32(uint32_t addr, uint32_t v) {
  if (addr) sw(addr, v);
}
inline void put64(uint32_t addr, uint64_t v) {
  if (addr) {
    sw(addr, static_cast<uint32_t>(v));
    sw(addr + 4, static_cast<uint32_t>(v >> 32));
  }
}

std::string cstr(uint32_t addr, size_t max = 1024);
void write_cstr(uint32_t addr, const std::string& s, size_t max);

const KernelConfig& config();

// ---- time ----
uint64_t now_us();  // microseconds since kernel_init

// ---- uids ----
int32_t alloc_uid();

// ---- sysmem ----
void sysmem_init(uint32_t start, uint32_t end);
uint32_t sysmem_alloc(uint32_t size, uint32_t align, bool high, const char* name);
uint32_t sysmem_alloc_at(uint32_t addr, uint32_t size, const char* name);
bool sysmem_free(uint32_t addr);
void register_sysmem();

// ---- scheduler / thread manager ----
void sched_init(uint32_t gp);
void register_threadman();

// Blocks the current thread until the guest clock reaches wakeUs. Callbacks are run if cb is set.
void sleep_until(Cpu& c, uint64_t wakeUs, bool cb);
// Runs notified callbacks of the current thread; returns true if any ran.
bool run_callbacks(Cpu& c);
// Calls a guest function on a copy of the caller's register state.
uint32_t call_guest(Cpu& c, uint32_t addr, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);
// Queues a guest function to be run in interrupt context at the next opportunity.
void raise_guest_call(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
// Runs registered sub interrupt handlers for an interrupt number.
void dispatch_intr(int intno);
bool intr_handlers_registered(int intno);
// Notifies a callback object (by uid) from the host side, e.g. the exit callback.
void notify_callback(int32_t cbid, uint32_t arg);
void set_exit_callback(int32_t cbid);
// Wakes the scheduler's idle loop services (vblank etc.); returns the next time something is due.
void service_events();
void request_exit(int code);
[[noreturn]] void game_exit();
void preempt_check();

// ---- modules ----
void register_modulemgr();
void modules_init(const ModuleDesc* mods, size_t count);

// ---- io ----
void io_init();
void register_io();
std::string io_path_of_fd(int fd);
// Reads a whole file through the guest path mapping (disc0:/, ms0:/).
bool io_read_file(const std::string& vpath, std::vector<uint8_t>& out);

// ---- display / ctrl / ge / audio ----
void display_init();
void display_service();            // delivers vblank interrupts that are due
uint64_t display_next_event_us();  // UINT64_MAX when nothing is waiting
uint64_t display_next_vblank_us(uint64_t after);
uint32_t display_vcount();
// Frame-time profile (--profile): a summary line goes to stderr every few seconds.
struct ProfileCounters {
  static constexpr uint64_t kVblankUs = 16683;
  bool enabled = false;
  uint64_t geUs = 0, geLists = 0;
  uint64_t presentUs = 0, presents = 0, flips = 0, skipped = 0;
  uint64_t flipGap[6] = {};  // observed vblank intervals: 0, 1, 2, 3, 4, 5+
  uint64_t flipIntervals = 0, flipIntervalUs = 0;
  uint64_t vblankWaits = 0, vblankWaitUs = 0;
  uint64_t lastFlipUs = 0;
  bool hasLastFlip = false;

  void record_flip(uint64_t now) {
    ++flips;
    if (!enabled) {
      hasLastFlip = false;
      return;
    }
    if (hasLastFlip) {
      const uint64_t gap = now / kVblankUs - lastFlipUs / kVblankUs;
      ++flipGap[gap < 5 ? gap : 5];
      ++flipIntervals;
      flipIntervalUs += now - lastFlipUs;
    }
    lastFlipUs = now;
    hasLastFlip = true;
  }
};
ProfileCounters& profile();
void register_display();
void register_ctrl();
void register_ge();
void ge_init();
void register_audio();
void audio_init();
void register_sas();
void sas_init();
void register_atrac();
void atrac_init();
void register_misc();
void register_mpeg();
void register_font();

}  // namespace k
}  // namespace psp

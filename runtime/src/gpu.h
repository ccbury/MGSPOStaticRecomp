// Software implementation of the PSP Graphics Engine command interpreter.
//
// The GE state machine and rasteriser write straight into emulated VRAM in the pixel format the game
// picked, so 16/32 bit framebuffers, render-to-texture and copy transfers behave like the hardware
// without any extra bookkeeping. The host presenter then only has to put the display buffer on screen.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace psp {
namespace ge {

struct ListContext {
  struct Frame {
    uint32_t pc, base, offset;
  };
  uint32_t pc = 0;
  uint32_t stall = 0;
  uint32_t origin = 0;
  std::vector<Frame> stack;
  uint32_t signalArg = 0;
};

enum class Stop {
  Stalled,  // reached the stall address, more commands may be appended
  Ended,    // END was executed, the list is finished
  Paused,   // a signal with the pause behaviour asked the list to wait for sceGeContinue
  Fault,    // bad address or runaway list
};

// Receives the SIGNAL / FINISH interrupts a list raises when it reaches END.
struct Host {
  virtual void on_signal(uint32_t data) = 0;  // (behaviour << 16) | signal id
  virtual void on_finish(uint32_t data) = 0;  // low 16 bits of the FINISH command
  virtual ~Host() = default;
};

struct Stats {
  uint64_t commands[256] = {};
  uint64_t lists = 0;
  uint64_t draws = 0;  // PRIM / BEZIER / SPLINE commands
  uint64_t primCount[8] = {};
  uint64_t vertices = 0;
  uint64_t triangles = 0;  // triangles that reached the rasteriser
  uint64_t culled = 0;
  uint64_t clears = 0;
  uint64_t throughDraws = 0;
  uint64_t transforms = 0;  // TRXKICK
  uint64_t transformBytes = 0;
  uint64_t texturedDraws = 0;
  uint64_t lightedDraws = 0;
  uint64_t blendedDraws = 0;
  uint64_t skinnedDraws = 0;
  uint64_t clutLoads = 0;
  std::map<uint32_t, uint64_t> vertexTypes;
  std::map<uint32_t, uint64_t> texFormats;  // key: format | swizzle << 8
  std::map<uint32_t, uint64_t> fbFormats;
};

class Gpu {
 public:
  Gpu();
  void reset();

  // Executes commands until END, the stall address, or a pause. The Host is notified for signal/finish.
  Stop run(ListContext& list, Host* host);

  // State access used by sceGeGetCmd/GetMtx and context save/restore.
  uint32_t cmd(uint32_t op) const { return r_[op & 0xFF]; }
  bool get_matrix(int type, float* out) const;  // type: 0..7 bone, 8 world, 9 view, 10 proj, 11 tgen
  static constexpr size_t kContextWords = 256 + 8 * 12 + 12 + 12 + 16 + 12;
  void save_context(uint32_t* out) const;
  void restore_context(const uint32_t* in);

  // Executes a single command word outside of a list (used by tests and context restore).
  void exec_word(uint32_t word);

  // Debugging hooks.
  void set_dump(std::FILE* f) { dump_ = f; }
  void set_stats(Stats* s) { stats_ = s; }
  static const char* command_name(uint32_t op);

 private:
  friend struct Raster;
  uint32_t r_[256];
  float bone_[8 * 12];
  float world_[12], view_[12], tgen_[12];
  float proj_[16];
  uint32_t boneIdx_ = 0, worldIdx_ = 0, viewIdx_ = 0, projIdx_ = 0, tgenIdx_ = 0;
  uint32_t vaddr_ = 0, iaddr_ = 0, base_ = 0, offset_ = 0;
  uint32_t clut_[1024];  // raw bytes of the loaded palette
  uint32_t clutBytes_ = 0;
  uint32_t bboxVisible_ = 1;
  std::FILE* dump_ = nullptr;
  Stats* stats_ = nullptr;
  bool fault_ = false;

  struct Exec;
  void execute(uint32_t word, ListContext* l, Host* host, Stop* stop);
  uint32_t rel_addr(uint32_t arg) const { return ((base_ | (arg & 0xFFFFFF)) + offset_) & 0x0FFFFFFF; }
  void draw_prim(uint32_t arg);
  void draw_patch(uint32_t arg, bool spline);
  void bbox(uint32_t arg);
  void load_clut(uint32_t arg);
  void transfer();
};

}  // namespace ge
}  // namespace psp

// sceGe_user: display list queueing, sync and callbacks on top of the software GE in gpu.cpp.
//
// Lists are executed as soon as they are enqueued (up to their stall address), so by the time a
// game calls a sync function the drawing is already in VRAM. The sync calls still report the state
// a real list would be in and block the calling thread while a list is stalled or paused.
#include <cstdlib>
#include <cstring>
#include <map>

#include <psp/ge_debug.h>

#include "gpu.h"
#include "kernel_internal.h"

namespace psp {

namespace {
GeDebugOptions g_dbgOpt;
bool g_dbgSet = false;
ge::Stats g_stats;
bool g_statsOn = false;
std::FILE* g_dump = nullptr;
}  // namespace

void ge_set_debug(const GeDebugOptions& opt) {
  g_dbgOpt = opt;
  g_dbgSet = true;
}

void ge_write_stats(std::FILE* out) {
  if (!g_statsOn || !out) return;
  const ge::Stats& s = g_stats;
  std::fprintf(out, "GE statistics\n");
  std::fprintf(out, "  lists %llu, draw commands %llu, vertices %llu, triangles rasterised %llu, culled %llu\n",
               (unsigned long long)s.lists, (unsigned long long)s.draws, (unsigned long long)s.vertices,
               (unsigned long long)s.triangles, (unsigned long long)s.culled);
  static const char* kPrim[] = {"points", "lines", "line strips", "triangles", "tri strips", "tri fans", "sprites", "?"};
  for (int i = 0; i < 7; ++i)
    if (s.primCount[i]) std::fprintf(out, "  prim %-12s %llu\n", kPrim[i], (unsigned long long)s.primCount[i]);
  std::fprintf(out, "  clears %llu, through-mode draws %llu, textured %llu, lit %llu, blended %llu, skinned %llu\n",
               (unsigned long long)s.clears, (unsigned long long)s.throughDraws, (unsigned long long)s.texturedDraws,
               (unsigned long long)s.lightedDraws, (unsigned long long)s.blendedDraws,
               (unsigned long long)s.skinnedDraws);
  std::fprintf(out, "  block transfers %llu (%llu bytes), CLUT loads %llu\n", (unsigned long long)s.transforms,
               (unsigned long long)s.transformBytes, (unsigned long long)s.clutLoads);
  for (const auto& kv : s.fbFormats)
    std::fprintf(out, "  framebuffer format %u: %llu draws\n", kv.first, (unsigned long long)kv.second);
  for (const auto& kv : s.texFormats)
    std::fprintf(out, "  texture format %u%s: %llu draws\n", kv.first & 0xFF, (kv.first & 0x100) ? " (swizzled)" : "",
                 (unsigned long long)kv.second);
  for (const auto& kv : s.vertexTypes)
    std::fprintf(out, "  vertex type %06X: %llu draws\n", kv.first, (unsigned long long)kv.second);
  std::fprintf(out, "  commands:\n");
  for (int op = 0; op < 256; ++op)
    if (s.commands[op])
      std::fprintf(out, "    %02X %-20s %llu\n", op, ge::Gpu::command_name(static_cast<uint32_t>(op)),
                   (unsigned long long)s.commands[op]);
  std::fflush(out);
}

namespace k {

namespace {

enum ListState : uint32_t { kCompleted = 0, kQueued = 1, kDrawing = 2, kStalling = 3, kPaused = 4 };
constexpr uint32_t E_INVALID_ID = 0x80000100;

struct GeCallback {
  uint32_t signalFunc = 0, signalArg = 0, finishFunc = 0, finishArg = 0;
};

struct List {
  int id = 0;
  ge::ListContext ctx;
  int cbid = -1;
  uint32_t state = kQueued;
};

struct CallbackHost : ge::Host {
  int listId = 0;
  GeCallback cb;
  bool have = false;
  void on_signal(uint32_t data) override {
    if (have && cb.signalFunc)
      raise_guest_call(cb.signalFunc, data & 0xFFFF, cb.signalArg, static_cast<uint32_t>(listId), 0);
  }
  void on_finish(uint32_t data) override {
    if (have && cb.finishFunc) raise_guest_call(cb.finishFunc, data & 0xFFFF, cb.finishArg, static_cast<uint32_t>(listId), 0);
  }
};

ge::Gpu g_gpu;
std::map<int, List> g_lists;
std::map<int, GeCallback> g_cbs;
int g_nextList = 0;
int g_nextCb = 0;
bool g_warnedFault = false;

void run_list(List& l) {
  CallbackHost host;
  host.listId = l.id;
  auto it = g_cbs.find(l.cbid);
  if (it != g_cbs.end()) {
    host.cb = it->second;
    host.have = true;
  }
  ProfileCounters& pf = profile();
  uint64_t t0 = pf.enabled ? now_us() : 0;
  ge::Stop stop = g_gpu.run(l.ctx, &host);
  if (pf.enabled) {
    pf.geUs += now_us() - t0;
    ++pf.geLists;
  }
  switch (stop) {
    case ge::Stop::Stalled: l.state = kStalling; break;
    case ge::Stop::Paused: l.state = kPaused; break;
    case ge::Stop::Ended: l.state = kCompleted; break;
    case ge::Stop::Fault:
      if (!g_warnedFault) {
        std::fprintf(stderr, "ge: display list %d stopped at 0x%08X (bad address or runaway list)\n", l.id, l.ctx.pc);
        g_warnedFault = true;
      }
      l.state = kCompleted;
      break;
  }
}

// finished lists nobody synced on would otherwise pile up
void prune() {
  while (g_lists.size() > 64) {
    auto it = g_lists.begin();
    for (; it != g_lists.end(); ++it)
      if (it->second.state == kCompleted) break;
    if (it == g_lists.end()) break;
    g_lists.erase(it);
  }
}

int enqueue(uint32_t listAddr, uint32_t stall, int cbid) {
  prune();
  int id = g_nextList++ & 0x3FFF;
  List& l = g_lists[id];
  l = List();
  l.id = id;
  l.ctx.pc = listAddr;
  l.ctx.stall = stall;
  l.cbid = cbid;
  if (g_statsOn) g_stats.lists++;
  if (g_dump) std::fprintf(g_dump, "# list %d at %08X stall %08X\n", id, listAddr, stall);
  run_list(l);
  return id;
}

// args: list, stall, cbid, optional arg struct. Execution is immediate, so queue order is call order
// and the "head" variant needs no special handling.
void h_enqueue(Cpu& c) { ret(c, static_cast<uint32_t>(enqueue(arg(c, 0), arg(c, 1), static_cast<int>(arg(c, 2))))); }

void h_update_stall(Cpu& c) {
  auto it = g_lists.find(static_cast<int>(arg(c, 0)));
  if (it == g_lists.end()) return ret(c, E_INVALID_ID);
  List& l = it->second;
  l.ctx.stall = arg(c, 1);
  if (l.state == kStalling || l.state == kQueued) run_list(l);
  ret(c, 0);
}

bool list_pending(int id) {
  auto it = g_lists.find(id);
  return it != g_lists.end() && it->second.state != kCompleted;
}

bool any_pending(int) {
  for (auto& kv : g_lists)
    if (kv.second.state != kCompleted) return true;
  return false;
}

// Blocks the calling thread in short sleeps while a list is waiting for more commands.
void wait_while(Cpu& c, bool (*waiting)(int), int id) {
  for (int tries = 0; tries < 5000 && waiting(id); ++tries) sleep_until(c, now_us() + 1000, false);
}

uint32_t first_pending_state() {
  for (auto& kv : g_lists)
    if (kv.second.state != kCompleted) return kv.second.state;
  return kCompleted;
}

void h_list_sync(Cpu& c) {
  int id = static_cast<int>(arg(c, 0));
  if (g_lists.find(id) == g_lists.end()) return ret(c, E_INVALID_ID);
  if (arg(c, 1) == 0) wait_while(c, list_pending, id);
  auto it = g_lists.find(id);
  uint32_t st = it == g_lists.end() ? kCompleted : it->second.state;
  if (st == kCompleted && it != g_lists.end() && arg(c, 1) == 0) g_lists.erase(it);
  ret(c, st);
}

void h_draw_sync(Cpu& c) {
  if (arg(c, 0) == 0) {
    wait_while(c, any_pending, 0);
    return ret(c, 0);
  }
  ret(c, first_pending_state());
}

void h_break(Cpu& c) { ret(c, 0); }

void h_continue(Cpu& c) {
  for (auto& kv : g_lists)
    if (kv.second.state == kPaused) run_list(kv.second);
  ret(c, 0);
}

void h_dequeue(Cpu& c) {
  auto it = g_lists.find(static_cast<int>(arg(c, 0)));
  if (it == g_lists.end()) return ret(c, E_INVALID_ID);
  g_lists.erase(it);
  ret(c, 0);
}

void h_set_callback(Cpu& c) {
  uint32_t p = arg(c, 0);
  GeCallback cb;
  if (p) {
    cb.signalFunc = lw(p);
    cb.signalArg = lw(p + 4);
    cb.finishFunc = lw(p + 8);
    cb.finishArg = lw(p + 12);
  }
  int id = g_nextCb++;
  g_cbs[id] = cb;
  ret(c, static_cast<uint32_t>(id));
}

void h_unset_callback(Cpu& c) {
  g_cbs.erase(static_cast<int>(arg(c, 0)));
  ret(c, 0);
}

void h_get_cmd(Cpu& c) {
  uint32_t op = arg(c, 0) & 0xFF;
  ret(c, (op << 24) | g_gpu.cmd(op));
}

void h_get_mtx(Cpu& c) {
  float m[16];
  if (!g_gpu.get_matrix(static_cast<int>(arg(c, 0)), m)) return ret(c, E_ERROR);
  int n = arg(c, 0) == 10 ? 16 : 12;
  if (arg(c, 1)) std::memcpy(ptr(arg(c, 1)), m, n * sizeof(float));
  ret(c, 0);
}

void h_save_context(Cpu& c) {
  uint32_t dst = arg(c, 0);
  if (!dst) return ret(c, E_ERROR);
  uint32_t words[ge::Gpu::kContextWords];
  g_gpu.save_context(words);
  std::memcpy(ptr(dst), words, sizeof words);
  ret(c, 0);
}

void h_restore_context(Cpu& c) {
  uint32_t src = arg(c, 0);
  if (!src) return ret(c, E_ERROR);
  uint32_t words[ge::Gpu::kContextWords];
  std::memcpy(words, ptr(src), sizeof words);
  g_gpu.restore_context(words);
  ret(c, 0);
}

void h_edram_size(Cpu& c) { ret(c, 0x00200000); }
void h_edram_addr(Cpu& c) { ret(c, 0x04000000); }
void h_zero(Cpu& c) { ret(c, 0); }

}  // namespace

void ge_init() {
  g_lists.clear();
  g_cbs.clear();
  g_nextList = 0;
  g_nextCb = 0;
  g_warnedFault = false;
  g_gpu.reset();

  GeDebugOptions opt = g_dbgOpt;
  if (!g_dbgSet) {
    if (const char* d = std::getenv("MGSPO_GE_DUMP")) opt.dumpPath = d;
    if (const char* s = std::getenv("MGSPO_GE_STATS")) opt.stats = s[0] && s[0] != '0';
  }
  if (g_dump) {
    std::fclose(g_dump);
    g_dump = nullptr;
  }
  if (!opt.dumpPath.empty()) g_dump = std::fopen(opt.dumpPath.c_str(), "w");
  g_statsOn = opt.stats;
  g_stats = ge::Stats();
  g_gpu.set_dump(g_dump);
  g_gpu.set_stats(g_statsOn ? &g_stats : nullptr);
}

void register_ge() {
  const char* m = "sceGe_user";
  register_hle(m, 0xAB49E76A, "sceGeListEnQueue", h_enqueue);
  register_hle(m, 0x1C0D95A6, "sceGeListEnQueueHead", h_enqueue);
  register_hle(m, 0xE0D68148, "sceGeListUpdateStallAddr", h_update_stall);
  register_hle(m, 0x5FB86AB0, "sceGeListDeQueue", h_dequeue);
  register_hle(m, 0x03444EB4, "sceGeListSync", h_list_sync);
  register_hle(m, 0xB287BD61, "sceGeDrawSync", h_draw_sync);
  register_hle(m, 0xB448EC0D, "sceGeBreak", h_break);
  register_hle(m, 0x4C06E472, "sceGeContinue", h_continue);
  register_hle(m, 0xA4FC06A4, "sceGeSetCallback", h_set_callback);
  register_hle(m, 0x05DB22CE, "sceGeUnsetCallback", h_unset_callback);
  register_hle(m, 0xDC93CFEF, "sceGeGetCmd", h_get_cmd);
  register_hle(m, 0x57C8945B, "sceGeGetMtx", h_get_mtx);
  register_hle(m, 0xE66CB92E, "sceGeGetStack", h_zero);
  register_hle(m, 0x438A385A, "sceGeSaveContext", h_save_context);
  register_hle(m, 0x0BF608FB, "sceGeRestoreContext", h_restore_context);
  register_hle(m, 0xB77905EA, "sceGeEdramSetAddrTranslation", h_zero);
  register_hle(m, 0x1F6752AD, "sceGeEdramGetSize", h_edram_size);
  register_hle(m, 0xE47E40E4, "sceGeEdramGetAddr", h_edram_addr);
}

}  // namespace k
}  // namespace psp

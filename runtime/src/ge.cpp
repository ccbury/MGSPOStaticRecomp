// sceGe_user: command lists are accepted and walked for control flow only (no rasterisation yet).
#include <map>
#include <vector>

#include "kernel_internal.h"

namespace psp {
namespace k {

namespace {

struct List {
  uint32_t pc = 0, stall = 0, base = 0, offset = 0;
  uint32_t arg = 0;
  std::vector<uint32_t> stack;
  bool done = false;
  int cbid = -1;
};

struct GeCallback {
  uint32_t signalFunc = 0, signalArg = 0, finishFunc = 0, finishArg = 0;
};

std::map<int, List> g_lists;
std::map<int, GeCallback> g_cbs;
int g_nextList = 1;
int g_nextCb = 0;

bool addr_ok(uint32_t a) {
  a &= 0x0FFFFFFF;
  return a >= 0x04000000 && a < 0x0C000000;
}

uint32_t target(const List& l, uint32_t arg24) { return ((l.base | arg24) + l.offset) & 0x0FFFFFFC; }

void walk(int id, List& l) {
  int guard = 0;
  while (!l.done && (!l.stall || (l.pc & 0x0FFFFFFF) != (l.stall & 0x0FFFFFFF))) {
    if (!addr_ok(l.pc) || ++guard > 4000000) {
      l.done = true;
      break;
    }
    uint32_t w = lw(l.pc);
    uint32_t op = w >> 24, a = w & 0xFFFFFF;
    l.pc += 4;
    switch (op) {
      case 0x08: l.pc = target(l, a); break;
      case 0x0A:
        l.stack.push_back(l.pc);
        l.pc = target(l, a);
        break;
      case 0x0B:
        if (!l.stack.empty()) {
          l.pc = l.stack.back();
          l.stack.pop_back();
        }
        break;
      case 0x0C: l.done = true; break;
      case 0x0E: {
        auto it = g_cbs.find(l.cbid);
        if (it != g_cbs.end() && it->second.signalFunc)
          raise_guest_call(it->second.signalFunc, static_cast<uint32_t>(id), it->second.signalArg, a & 0xFFFF, 0);
        break;
      }
      case 0x0F: {
        auto it = g_cbs.find(l.cbid);
        if (it != g_cbs.end() && it->second.finishFunc)
          raise_guest_call(it->second.finishFunc, static_cast<uint32_t>(id), it->second.finishArg, 0, 0);
        break;
      }
      case 0x10: l.base = (a & 0x0F0000) << 8; break;
      case 0x13: l.offset = a << 8; break;
      default: break;
    }
  }
}

int enqueue(uint32_t listAddr, uint32_t stall, int cbid, uint32_t arg) {
  int id = g_nextList++;
  List& l = g_lists[id];
  l.pc = listAddr;
  l.stall = stall;
  l.cbid = cbid;
  l.arg = arg;
  walk(id, l);
  return id;
}

// args: list, stall, cbid, optional arg struct
void h_enqueue(Cpu& c) { ret(c, static_cast<uint32_t>(enqueue(arg(c, 0), arg(c, 1), static_cast<int>(arg(c, 2)), arg(c, 3)))); }

void h_update_stall(Cpu& c) {
  auto it = g_lists.find(static_cast<int>(arg(c, 0)));
  if (it == g_lists.end()) return ret(c, E_ERROR);
  it->second.stall = arg(c, 1);
  walk(it->first, it->second);
  ret(c, 0);
}

void h_sync(Cpu& c) { ret(c, 0); }  // lists complete synchronously

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

void h_edram_size(Cpu& c) { ret(c, 0x00200000); }
void h_edram_addr(Cpu& c) { ret(c, 0x04000000); }
void h_zero(Cpu& c) { ret(c, 0); }

}  // namespace

void ge_init() {
  g_lists.clear();
  g_cbs.clear();
  g_nextList = 1;
  g_nextCb = 0;
}

void register_ge() {
  const char* m = "sceGe_user";
  register_hle(m, 0xAB49E76A, "sceGeListEnQueue", h_enqueue);
  register_hle(m, 0x1C0D95A6, "sceGeListEnQueueHead", h_enqueue);
  register_hle(m, 0xE0D68148, "sceGeListUpdateStallAddr", h_update_stall);
  register_hle(m, 0x03444EB4, "sceGeListSync", h_sync);
  register_hle(m, 0xB287BD61, "sceGeDrawSync", h_sync);
  register_hle(m, 0xB448EC0D, "sceGeBreak", h_zero);
  register_hle(m, 0x4C06E472, "sceGeContinue", h_zero);
  register_hle(m, 0xA4FC06A4, "sceGeSetCallback", h_set_callback);
  register_hle(m, 0x05DB22CE, "sceGeUnsetCallback", h_unset_callback);
  register_hle(m, 0x1F6752AD, "sceGeEdramGetSize", h_edram_size);
  register_hle(m, 0xE47E40E4, "sceGeEdramGetAddr", h_edram_addr);
}

}  // namespace k
}  // namespace psp

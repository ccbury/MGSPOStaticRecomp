#include "render_surface.h"
#include "raster_fast.h"
#include "hardware_renderer.h"
#include <psp/memory.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

namespace psp::ge::surface {
namespace {
int g_scale = 1;
thread_local int g_internal = 0;
std::atomic<uint64_t> g_access{0};
uint64_t g_tick = 0;
std::vector<std::shared_ptr<Plane>> g_planes;
std::vector<std::string> g_diagnostics;
constexpr size_t kPlanes = 8;
struct Pending {
  std::shared_ptr<Plane> colour, depth;
  hardware::Job job;
} g_pending;

size_t cost(const Plane& p) { return p.native.size() + p.pixels.size(); }
uint32_t physical(uint32_t addr) {
  addr &= 0x3FFFFFFF;
  return addr < kVramSize ? addr + kVramBase : addr;
}
void seed_pixel(Plane& p, size_t i, const uint8_t* src) {
  size_t x = i % p.stride, y = i / p.stride;
  for (int dy = 0; dy < p.scale; ++dy)
    for (int dx = 0; dx < p.scale; ++dx)
      std::memcpy(p.pixels.data() + ((y * p.scale + dy) * p.stride * p.scale + x * p.scale + dx) * p.bpp, src, p.bpp);
}
}

NativeScope::NativeScope() { ++g_internal; }
NativeScope::~NativeScope() { --g_internal; }
void native_access() {
  if ((g_scale > 1 || hardware::selected() != hardware::Backend::Software) && !g_internal)
    g_access.fetch_add(1, std::memory_order_relaxed);
}
int scale() { return g_scale; }
void reset() {
  fast::sync();
  flush_hardware();
  g_planes.clear();
  g_diagnostics.clear();
  g_access = 0;
  g_tick = 0;
}
void set_scale(int scale) {
  if (scale < 1 || scale > 8) throw std::invalid_argument("render scale must be 1 through 8");
  reset();
  g_scale = scale;
}
void diagnostic(const char* reason) {
  if (std::find(g_diagnostics.begin(), g_diagnostics.end(), reason) != g_diagnostics.end()) return;
  g_diagnostics.emplace_back(reason);
  std::fprintf(stderr, "[render %dx] %s\n", g_scale, reason);
}
bool overlaps(uint32_t a, size_t an, uint32_t b, size_t bn) {
  return an && bn && uint64_t(physical(a)) < uint64_t(physical(b)) + bn &&
         uint64_t(physical(b)) < uint64_t(physical(a)) + an;
}
size_t allocated_bytes() {
  size_t bytes = 0;
  for (const auto& p : g_planes) bytes += cost(*p);
  return bytes;
}
size_t budget_bytes() {
  // Three native 512x272 RGBA-sized planes at the selected scale, with a 64 MiB floor.
  return std::max<size_t>(64u << 20, size_t(512) * 272 * 4 * (1 + g_scale * g_scale) * 3);
}
void Plane::refresh(bool deferHardware) {
  if (!deferHardware) flush_hardware();
  NativeScope scope;
  const uint8_t* src = ptr(addr);
  const uint64_t epoch = g_access.load(std::memory_order_relaxed);
  if (dirty || accessEpoch != epoch) {
    diagnostic("CPU/HLE access or aliased view: reseeding host samples from authoritative native VRAM.");
    reseed();
  } else if (std::memcmp(src, native.data(), native.size()) != 0) {
    flush_hardware();
    // Also catches writes through a retained host pointer, without changing generated guest code.
    for (size_t i = 0; i < native.size() / bpp; ++i)
      if (std::memcmp(src + i * bpp, native.data() + i * bpp, bpp) != 0)
        seed_pixel(*this, i, src + i * bpp);
    std::memcpy(native.data(), src, native.size());
    diagnostic("External native pixel changes: affected host samples reseeded.");
  }
}
void Plane::reseed() {
  flush_hardware();
  NativeScope scope;
  const uint8_t* src = ptr(addr);
  for (size_t i = 0; i < native.size() / bpp; ++i) seed_pixel(*this, i, src + i * bpp);
  std::memcpy(native.data(), src, native.size());
  accessEpoch = g_access.load(std::memory_order_relaxed);
  dirty = false;
}
void Plane::accept_native() {
  NativeScope scope;
  std::memcpy(native.data(), ptr(addr), native.size());
  accessEpoch = g_access.load(std::memory_order_relaxed);
  for (auto& p : g_planes)
    if (p.get() != this && overlaps(addr, native.size(), p->addr, p->native.size())) p->dirty = true;
}
void written(uint32_t addr, size_t bytes) {
  if (g_scale == 1 && hardware::selected() == hardware::Backend::Software) return;
  if ((g_pending.colour && overlaps(addr, bytes, g_pending.colour->addr, g_pending.colour->native.size())) ||
      (g_pending.depth && overlaps(addr, bytes, g_pending.depth->addr, g_pending.depth->native.size())))
    flush_hardware();
  for (auto& p : g_planes)
    if (overlaps(addr, bytes, p->addr, p->native.size())) p->dirty = true;
}
bool is_target(uint32_t addr, size_t bytes) {
  for (const auto& p : g_planes)
    if (overlaps(addr, bytes, p->addr, p->native.size())) return true;
  return false;
}
std::shared_ptr<Plane> acquire(uint32_t addr, uint32_t stride, int format, int rows, bool deferHardware) {
  if (!deferHardware) flush_hardware();
  if (g_scale == 1 && hardware::selected() == hardware::Backend::Software) return {};
  NativeScope scope;
  addr = physical(addr);
  const int bpp = format == 3 ? 4 : 2;
  if (format < -1 || format > 3 || !stride || stride > 1024 || rows < 1 || rows > 1024) {
    diagnostic("Unsupported target bounds/location: native rendering only (host shadows support bounded VRAM targets).");
    return {};
  }
  const uint64_t bytes = uint64_t(stride) * rows * bpp;
  if (addr < kVramBase || uint64_t(addr) + bytes > uint64_t(kVramBase) + kVramSize) {
    diagnostic("Unsupported target bounds/location: native rendering only (host shadows support bounded VRAM targets).");
    return {};
  }
  std::shared_ptr<Plane> old;
  for (auto& p : g_planes) {
    if (p->addr == addr && p->stride == stride && p->format == format) {
      if (p->rows >= rows) {
        p->used = ++g_tick;
        p->refresh(deferHardware);
        return p;
      }
      old = p;
      old->refresh(deferHardware);
      break;
    }
  }
  const size_t incoming = static_cast<size_t>(bytes) * (1 + g_scale * g_scale);
  while (allocated_bytes() + incoming > budget_bytes() || g_planes.size() >= kPlanes) {
    auto victim = g_planes.end();
    for (auto it = g_planes.begin(); it != g_planes.end(); ++it)
      if (it->use_count() == 1 && (victim == g_planes.end() || (*it)->used < (*victim)->used)) victim = it;
    if (victim == g_planes.end()) {
      throw std::runtime_error("Internal-resolution surface budget exhausted while targets are pinned; select a lower render scale.");
    }
    g_planes.erase(victim);
    diagnostic("Host surface cache evicted a target; it will be reseeded on reuse.");
  }
  std::shared_ptr<Plane> p;
  try {
    p = std::make_shared<Plane>();
    p->native.resize(static_cast<size_t>(bytes));
    p->pixels.resize(static_cast<size_t>(bytes) * g_scale * g_scale);
  } catch (const std::bad_alloc&) {
    throw std::runtime_error("Cannot allocate internal-resolution colour/depth storage; close other applications or select a lower render scale.");
  }
  p->addr = addr; p->stride = stride; p->format = format; p->bpp = bpp; p->rows = rows;
  p->scale = g_scale; p->used = ++g_tick;
  p->reseed();
  if (old) {
    std::copy(old->pixels.begin(), old->pixels.end(), p->pixels.begin());
    g_planes.erase(std::find(g_planes.begin(), g_planes.end(), old));
  }
  g_planes.push_back(p);
  return p;
}
std::shared_ptr<Plane> display(uint32_t addr, uint32_t stride, int format) {
  if (g_scale == 1 && hardware::selected() == hardware::Backend::Software) return {};
  fast::sync();
  flush_hardware();
  for (auto& p : g_planes)
    if (p->addr == physical(addr) && p->stride == stride && p->format == format && p->rows >= 272) {
      p->refresh();
      p->used = ++g_tick;
      return p;
    }
  diagnostic("Display has no matching host target: presenting native-resolution pixels.");
  return {};
}

void submit_hardware() {
  auto& job = g_pending.job;
  if (job.commands.empty()) return;
  job.readbackTargets = !hardware::retains_targets();
  hardware::run(job);
  job.commands.clear();
  job.uploadTargets = !hardware::retains_targets();
  job.readbackTargets = true;
}

void flush_hardware() {
  if (!g_pending.colour) return;
  auto& job = g_pending.job;
  if (!job.commands.empty() || !job.uploadTargets) hardware::run(job);
  auto pack = [](const std::vector<uint32_t>& input, Plane& plane) {
    if (plane.bpp == 4) {
      std::memcpy(plane.pixels.data(), input.data(), plane.pixels.size());
    } else for (size_t i = 0; i < input.size(); ++i) {
      const uint16_t pixel = static_cast<uint16_t>(input[i]);
      std::memcpy(plane.pixels.data() + i * 2, &pixel, 2);
    }
  };
  pack(job.colour, *g_pending.colour);
  if (g_pending.depth) pack(job.depth, *g_pending.depth);
  g_pending = {};
}

hardware::Job& hardware_draw(const std::shared_ptr<Plane>& colour,
                            const std::shared_ptr<Plane>& depth, std::vector<uint32_t> texture) {
  hardware::set_flush_callback(flush_hardware);
  if (g_pending.colour != colour || g_pending.depth != depth ||
      (!hardware::retains_targets() && g_pending.job.texture != texture))
    flush_hardware();
  if (g_pending.colour && g_pending.job.texture != texture) {
    submit_hardware();
    g_pending.job.texture = std::move(texture);
  }
  if (!g_pending.colour) {
    const size_t entries = colour->pixels.size() / colour->bpp +
                           (depth ? depth->pixels.size() / depth->bpp : 1) + texture.size();
    if (entries > (128u << 20) / sizeof(uint32_t))
      throw std::runtime_error("Hardware shadow conversion exceeds bounded upload/storage budget; select a lower internal scale.");
    auto expand = [](const Plane& plane, std::vector<uint32_t>& output) {
      output.resize(plane.pixels.size() / plane.bpp);
      if (plane.bpp == 4) {
        std::memcpy(output.data(), plane.pixels.data(), plane.pixels.size());
      } else for (size_t i = 0; i < output.size(); ++i) {
        uint16_t pixel;
        std::memcpy(&pixel, plane.pixels.data() + i * 2, 2);
        output[i] = pixel;
      }
    };
    expand(*colour, g_pending.job.colour);
    if (depth) expand(*depth, g_pending.job.depth);
    else g_pending.job.depth.push_back(65535);
    g_pending.job.texture = std::move(texture);
    g_pending.colour = colour;
    g_pending.depth = depth;
  }
  return g_pending.job;
}
}  // namespace psp::ge::surface

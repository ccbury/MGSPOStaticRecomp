#include "hardware_renderer.h"
#include <cstdio>
#include <atomic>
#include <algorithm>
#include <stdexcept>

namespace psp::ge::hardware {
namespace {
Backend g_backend = Backend::Software;
std::unique_ptr<Device> g_device;
Counters g_counters;
std::atomic<uint64_t> g_submissions{0}, g_dispatches{0}, g_primitives{0};
std::unique_ptr<Device> create(Backend backend) {
#ifdef _WIN32
  if (backend == Backend::D3D11) return create_d3d11();
  if (backend == Backend::D3D12) return create_d3d12();
#ifdef MGSPO_HAS_VULKAN
  if (backend == Backend::Vulkan) return create_vulkan();
#else
  if (backend == Backend::Vulkan)
    throw std::runtime_error("Vulkan is unavailable in this build: rebuild with MGSPO_DXC pointing to a SPIR-V-enabled DXC (the Windows SDK DXC may lack SPIR-V codegen), or select Software/Direct3D.");
#endif
#endif
  throw std::runtime_error(std::string(name(backend)) + " hardware renderer is unavailable in this build.");
}
}
Backend parse(const std::string& value) {
  if (value == "software") return Backend::Software;
  if (value == "d3d11") return Backend::D3D11;
  if (value == "d3d12") return Backend::D3D12;
  if (value == "vulkan") return Backend::Vulkan;
  throw std::invalid_argument("Renderer must be software, d3d11, d3d12 or vulkan.");
}
const char* name(Backend backend) {
  switch (backend) {
    case Backend::Software: return "software";
    case Backend::D3D11: return "d3d11";
    case Backend::D3D12: return "d3d12";
    case Backend::Vulkan: return "vulkan";
  }
  throw std::invalid_argument("Invalid renderer backend.");
}
void select(Backend backend) {
  auto device = backend == Backend::Software ? nullptr : create(backend);
  g_device = std::move(device);
  g_backend = backend;
  g_counters = {};
  g_submissions = 0; g_dispatches = 0; g_primitives = 0;
  std::fprintf(stderr, "[renderer] %s: %s; native guest pass remains CPU-authoritative\n", name(backend),
               g_device ? g_device->description().c_str() : "CPU software rasterizer");
}
Backend selected() { return g_backend; }
bool built(Backend backend) {
  if (backend == Backend::Software) return true;
#ifdef _WIN32
  if (backend == Backend::D3D11 || backend == Backend::D3D12) return true;
#ifdef MGSPO_HAS_VULKAN
  if (backend == Backend::Vulkan) return true;
#endif
#endif
  return false;
}
Counters counters() { return {g_submissions.load(), g_dispatches.load(), g_primitives.load()}; }
void report() {
  const auto c = counters();
  std::fprintf(stderr, "[renderer] GPU submissions=%llu dispatches=%llu primitives=%llu\n",
               (unsigned long long)c.submissions, (unsigned long long)c.dispatches, (unsigned long long)c.primitives);
}
void run(Job& job) {
  if (!g_device) throw std::logic_error("Hardware draw submitted without an initialized device.");
  constexpr size_t budget = 128u << 20;
  const size_t bytes = (job.colour.size() + job.depth.size() + job.texture.size()) * sizeof(uint32_t);
  if (bytes > budget || job.commands.size() > 4096)
    throw std::runtime_error("Hardware draw exceeds bounded upload/storage budget; select a lower internal scale.");
  for (const auto& c : job.commands) {
    const uint64_t x = uint64_t(c.bounds[0]) + c.bounds[2], y = uint64_t(c.bounds[1]) + c.bounds[3];
    if (c.sizes[2] > 3 || !c.sizes[0] || c.sizes[0] > 8192 || c.sizes[1] > 8192 ||
        c.clip[0] < 0 || c.clip[1] < 0 || c.clip[2] < c.clip[0] || c.clip[3] < c.clip[1] ||
        c.clip[2] >= static_cast<int>(c.sizes[0]) || x > c.sizes[0] ||
        !y || y > 8192 || uint64_t(c.sizes[0]) * y > job.colour.size() ||
        ((c.features[0] & (DepthTest | ClearDepth)) &&
          (!c.sizes[1] || x > c.sizes[1] || uint64_t(c.sizes[1]) * y > job.depth.size())) ||
        ((c.features[0] & Texture) &&
          (!c.modes[0] || c.modes[0] > 1024 || !c.modes[1] || c.modes[1] > 1024 ||
           uint64_t(c.modes[0]) * c.modes[1] > job.texture.size())))
      throw std::runtime_error("Invalid hardware draw bounds/state; GPU submission refused.");
  }
  auto commands = std::move(job.commands);
  const auto primitivesBefore = g_counters.primitives;
  auto flush = [&] {
    if (job.commands.empty()) return;
    g_device->run(job, g_counters);
    g_counters.primitives = primitivesBefore;
    g_submissions = g_counters.submissions; g_dispatches = g_counters.dispatches;
    job.commands.clear();
  };
  try {
    // At most 64 disjoint 256x256 tiles per fenced submission; preserve primitive order.
    for (const auto& command : commands) {
      for (uint32_t y = 0; y < command.bounds[3]; y += 256) {
        for (uint32_t x = 0; x < command.bounds[2]; x += 256) {
          auto tile = command;
          tile.bounds[0] += x; tile.bounds[1] += y;
          tile.bounds[2] = std::min(256u, command.bounds[2] - x);
          tile.bounds[3] = std::min(256u, command.bounds[3] - y);
          job.commands.push_back(tile);
          if (job.commands.size() == 64) flush();
        }
      }
    }
    flush();
    g_counters.primitives += commands.size();
    g_primitives = g_counters.primitives;
  } catch (...) {
    job.commands = std::move(commands);
    throw;
  }
  job.commands = std::move(commands);
}
void reset() {
  if (g_device) report();
  g_device.reset();
  g_backend = Backend::Software;
}
std::string probe(Backend backend) {
  return backend == Backend::Software ? "CPU software rasterizer available" : create(backend)->description();
}
}

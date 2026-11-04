#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace psp::ge::hardware {
enum class Backend { Software, D3D11, D3D12, Vulkan };
struct ShaderCode { const uint32_t* words; size_t bytes; };
ShaderCode vulkan_shader();
Backend parse(const std::string& value);
const char* name(Backend backend);
struct Vertex {
  float position[4];
  float colour[4];
  float uvFog[4];
  float spec[4];
};
struct alignas(16) Command {
  uint32_t registers[256]{};
  uint32_t sizes[4]{};
  int32_t clip[4]{};
  uint32_t features[4]{};  // flags, immutable texture snapshot offset, reserved, reserved
  uint32_t modes[4]{};
  Vertex vertices[3]{};
  uint32_t bounds[4]{};
};
static_assert(sizeof(Command) == 1296);
enum Feature : uint32_t {
  Texture = 1, Fog = 2, Specular = 4, AlphaTest = 8, ColourTest = 16, Stencil = 32,
  DepthTest = 64, Blend = 128, Logic = 256, Clear = 512, ClearColour = 1024,
  ClearAlpha = 2048, ClearDepth = 4096, ClipDepth = 8192
};
struct Job {
  std::vector<uint32_t> colour, depth, texture;
  std::vector<Command> commands;
  uint32_t textureOffset = 0;  // copied into each newly recorded command, never read at execution
  // False transfer flags require a retained-target device and unchanged target dimensions.
  bool uploadTargets = true, readbackTargets = true;
};
struct Counters {
  uint64_t submissions = 0, dispatches = 0, primitives = 0;
  uint64_t uploads = 0, readbacks = 0;
};
class Device {
 public:
  virtual ~Device() = default;
  virtual std::string description() const = 0;
  virtual bool retains_targets() const { return false; }
  virtual void run(Job& job, Counters& counters) = 0;
};
std::unique_ptr<Device> create_d3d11();
std::unique_ptr<Device> create_d3d12();
std::unique_ptr<Device> create_vulkan();
void select(Backend backend);
Backend selected();
bool built(Backend backend);
bool retains_targets();
Counters counters();
void report();
// Invoked on the GE thread before replacing/destroying the device.
void set_flush_callback(void (*flush)());
void run(Job& job);
void reset();
std::string probe(Backend backend);
}

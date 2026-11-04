#include "hardware_renderer.h"
#include <stdexcept>
#ifdef MGSPO_HAS_VULKAN
#include "hardware_spirv.h"
#endif

namespace psp::ge::hardware {
ShaderCode vulkan_shader() {
#ifdef MGSPO_HAS_VULKAN
  return {reinterpret_cast<const uint32_t*>(kHardwareSpirv), sizeof(kHardwareSpirv)};
#else
  throw std::runtime_error("Vulkan shader unavailable: configure with a SPIR-V-enabled DXC.");
#endif
}
}

#define VK_NO_PROTOTYPES
#include <vulkan_core.h>
#include "hardware_win32.h"

namespace psp::ge::hardware {
namespace {
void vkcheck(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) +
                             "). Check Vulkan driver/device availability; no silent software fallback.");
}
#define INSTANCE_FUNCTIONS(X) \
  X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
  X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
  X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) \
  X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(DestroyBuffer) X(FreeMemory) \
  X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
  X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
  X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
  X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
  X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(CmdPipelineBarrier) \
  X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)
struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  size_t bytes = 0;
};
struct Submission {
  std::vector<Buffer> buffers;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  VkCommandPool commands = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
};
class VulkanDevice final : public Device {
  HMODULE loader_ = nullptr;
  PFN_vkGetInstanceProcAddr getInstance_ = nullptr;
#define DECLARE(name) PFN_vk##name name##_ = nullptr;
  INSTANCE_FUNCTIONS(DECLARE)
  DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties_{};
  VkPhysicalDeviceMemoryProperties memory_{};
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  VkDescriptorSetLayout descriptors_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkShaderModule shader_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  std::unique_ptr<Submission> pending_;
  bool failed_ = false;
  void release(Submission& s) {
    if (s.fence) DestroyFence_(device_, s.fence, nullptr);
    if (s.commands) DestroyCommandPool_(device_, s.commands, nullptr);
    if (s.descriptors) DestroyDescriptorPool_(device_, s.descriptors, nullptr);
    for (auto& b : s.buffers) {
      if (b.mapped) UnmapMemory_(device_, b.memory);
      if (b.buffer) DestroyBuffer_(device_, b.buffer, nullptr);
      if (b.memory) FreeMemory_(device_, b.memory, nullptr);
    }
  }
  void cleanup() {
    if (failed_ && pending_) {
      // Retain this single unfinished submission/device until process teardown, without retry.
      pending_.release();
      return;
    }
    if (device_) {
      if (pending_) release(*pending_);
      if (pipeline_) DestroyPipeline_(device_, pipeline_, nullptr);
      if (shader_) DestroyShaderModule_(device_, shader_, nullptr);
      if (layout_) DestroyPipelineLayout_(device_, layout_, nullptr);
      if (descriptors_) DestroyDescriptorSetLayout_(device_, descriptors_, nullptr);
      if (DestroyDevice_) DestroyDevice_(device_, nullptr);
    }
    if (instance_ && DestroyInstance_) DestroyInstance_(instance_, nullptr);
    if (loader_) FreeLibrary(loader_);
  }
  size_t buffer(Submission& s, size_t bytes, VkBufferUsageFlags usage, const void* input) {
    s.buffers.emplace_back();
    auto& b = s.buffers.back();
    b.bytes = std::max<size_t>(4, bytes);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = b.bytes; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkcheck(CreateBuffer_(device_, &info, nullptr, &b.buffer), "Vulkan bounded buffer");
    VkMemoryRequirements requirements{};
    GetBufferMemoryRequirements_(device_, b.buffer, &requirements);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
      if ((requirements.memoryTypeBits & (1u << i)) &&
          (memory_.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
    if (type == UINT32_MAX) throw std::runtime_error("Vulkan adapter has no coherent host-visible storage memory.");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
    vkcheck(AllocateMemory_(device_, &allocation, nullptr, &b.memory), "Vulkan buffer memory");
    vkcheck(BindBufferMemory_(device_, b.buffer, b.memory, 0), "Vulkan bind memory");
    vkcheck(MapMemory_(device_, b.memory, 0, b.bytes, 0, &b.mapped), "Vulkan coherent mapping");
    std::memset(b.mapped, 0, b.bytes);
    if (bytes && input) std::memcpy(b.mapped, input, bytes);
    return s.buffers.size() - 1;
  }
  void init() {
    loader_ = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader_) throw std::runtime_error("Vulkan loader is unavailable. Install the GPU vendor's Vulkan-capable driver or select Software.");
    getInstance_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader_, "vkGetInstanceProcAddr"));
    if (!getInstance_) throw std::runtime_error("Vulkan loader lacks vkGetInstanceProcAddr.");
    auto create = reinterpret_cast<PFN_vkCreateInstance>(getInstance_(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) throw std::runtime_error("Vulkan loader lacks vkCreateInstance.");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MGSPO hardware rasterizer"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
    vkcheck(create(&info, nullptr, &instance_), "Vulkan instance");
#define LOAD_INSTANCE(name) \
    name##_ = reinterpret_cast<PFN_vk##name>(getInstance_(instance_, "vk" #name)); \
    if (!name##_) throw std::runtime_error("Vulkan loader lacks vk" #name);
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    uint32_t count = 0;
    vkcheck(EnumeratePhysicalDevices_(instance_, &count, nullptr), "Vulkan adapter count");
    std::vector<VkPhysicalDevice> devices(count);
    vkcheck(EnumeratePhysicalDevices_(instance_, &count, devices.data()), "Vulkan adapters");
    for (auto candidate : devices) {
      VkPhysicalDeviceProperties props{};
      GetPhysicalDeviceProperties_(candidate, &props);
      if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
          props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) continue;
      uint32_t n = 0; GetPhysicalDeviceQueueFamilyProperties_(candidate, &n, nullptr);
      std::vector<VkQueueFamilyProperties> families(n);
      GetPhysicalDeviceQueueFamilyProperties_(candidate, &n, families.data());
      for (uint32_t i = 0; i < n; ++i) if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        physical_ = candidate; properties_ = props; family_ = i; break;
      }
      if (physical_) break;
    }
    if (!physical_) throw std::runtime_error("No Vulkan integrated/discrete hardware adapter with a compute queue; software ICDs are not selected.");
    if (properties_.limits.maxUniformBufferRange < sizeof(Command))
      throw std::runtime_error("Vulkan adapter cannot hold raster constants.");
    GetPhysicalDeviceMemoryProperties_(physical_, &memory_);
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family_; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
    vkcheck(CreateDevice_(physical_, &device, nullptr, &device_), "Vulkan hardware device");
#define LOAD_DEVICE(name) \
    name##_ = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr_(device_, "vk" #name)); \
    if (!name##_) throw std::runtime_error("Vulkan device lacks vk" #name);
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    GetDeviceQueue_(device_, family_, 0, &queue_);
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
      bindings[i].binding = i; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      bindings[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptors.bindingCount = 4; descriptors.pBindings = bindings;
    vkcheck(CreateDescriptorSetLayout_(device_, &descriptors, nullptr, &descriptors_), "Vulkan raster descriptors");
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &descriptors_;
    vkcheck(CreatePipelineLayout_(device_, &layout, nullptr, &layout_), "Vulkan raster layout");
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    auto code = vulkan_shader();
    shader.codeSize = code.bytes; shader.pCode = code.words;
    vkcheck(CreateShaderModule_(device_, &shader, nullptr, &shader_), "Vulkan SPIR-V raster shader");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = shader_;
    pipeline.stage.pName = "main"; pipeline.layout = layout_;
    vkcheck(CreateComputePipelines_(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_), "Vulkan raster pipeline");
  }
 public:
  VulkanDevice() { try { init(); } catch (...) { cleanup(); throw; } }
  ~VulkanDevice() override { cleanup(); }
  std::string description() const override {
    return "Vulkan " + std::to_string(VK_VERSION_MAJOR(properties_.apiVersion)) + "." +
      std::to_string(VK_VERSION_MINOR(properties_.apiVersion)) + " hardware compute rasterizer / " + properties_.deviceName +
      " (driver " + std::to_string(properties_.driverVersion) + ")";
  }
  void run(Job& job, Counters& counters) override {
    if (failed_) throw std::runtime_error("Vulkan renderer stopped after a failed submission; restart with a lower scale.");
    auto commands = tiles(job);
    if (commands.empty()) return;
    pending_ = std::make_unique<Submission>();
    auto& s = *pending_;
    try {
      for (const auto* input : {&job.colour, &job.depth, &job.texture}) {
        if (input->size() * 4 > properties_.limits.maxStorageBufferRange)
          throw std::runtime_error("Vulkan storage range exceeds adapter limit; select a lower internal scale.");
        buffer(s, input->size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, input->data());
      }
      const size_t alignment = std::max<size_t>(16, properties_.limits.minUniformBufferOffsetAlignment);
      const size_t stride = (sizeof(Command) + alignment - 1) / alignment * alignment;
      size_t constants = buffer(s, stride * commands.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr);
      for (size_t i = 0; i < commands.size(); ++i)
        std::memcpy(static_cast<uint8_t*>(s.buffers[constants].mapped) + i * stride, &commands[i], sizeof(Command));
      VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}};
      VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pool.maxSets = 1; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
      vkcheck(CreateDescriptorPool_(device_, &pool, nullptr, &s.descriptors), "Vulkan descriptor pool");
      VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc.descriptorPool = s.descriptors; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &descriptors_;
      VkDescriptorSet set;
      vkcheck(AllocateDescriptorSets_(device_, &alloc, &set), "Vulkan raster descriptor set");
      VkDescriptorBufferInfo buffers[4]{};
      VkWriteDescriptorSet writes[4]{};
      for (uint32_t i = 0; i < 4; ++i) {
        buffers[i].buffer = s.buffers[i].buffer; buffers[i].range = i == 3 ? sizeof(Command) : s.buffers[i].bytes;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set; writes[i].dstBinding = i;
        writes[i].descriptorCount = 1; writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
      }
      UpdateDescriptorSets_(device_, 4, writes, 0, nullptr);
      VkCommandPoolCreateInfo commandPool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      commandPool.queueFamilyIndex = family_;
      vkcheck(CreateCommandPool_(device_, &commandPool, nullptr, &s.commands), "Vulkan bounded command pool");
      VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cb.commandPool = s.commands; cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cb.commandBufferCount = 1;
      VkCommandBuffer list;
      vkcheck(AllocateCommandBuffers_(device_, &cb, &list), "Vulkan raster command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkcheck(BeginCommandBuffer_(list, &begin), "Vulkan begin raster");
      VkMemoryBarrier initial{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      initial.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; initial.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      CmdPipelineBarrier_(list, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &initial, 0, nullptr, 0, nullptr);
      CmdBindPipeline_(list, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
      for (size_t i = 0; i < commands.size(); ++i) {
        uint32_t offset = static_cast<uint32_t>(i * stride);
        CmdBindDescriptorSets_(list, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 1, &offset);
        CmdDispatch_(list, (commands[i].bounds[2] + 7) / 8, (commands[i].bounds[3] + 7) / 8, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        CmdPipelineBarrier_(list, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &barrier, 0, nullptr, 0, nullptr);
      }
      VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      CmdPipelineBarrier_(list, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
      vkcheck(EndCommandBuffer_(list), "Vulkan end raster");
      VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vkcheck(CreateFence_(device_, &fence, nullptr, &s.fence), "Vulkan completion fence");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &list;
      failed_ = true;
      vkcheck(QueueSubmit_(queue_, 1, &submit, s.fence), "Vulkan geometry submission");
      vkcheck(WaitForFences_(device_, 1, &s.fence, VK_TRUE, 5000000000ull), "Vulkan bounded 5-second completion");
      std::memcpy(job.colour.data(), s.buffers[0].mapped, job.colour.size() * 4);
      std::memcpy(job.depth.data(), s.buffers[1].mapped, job.depth.size() * 4);
      failed_ = false;
      release(s); pending_.reset();
      ++counters.submissions; counters.dispatches += commands.size(); counters.primitives += job.commands.size();
      ++counters.uploads; ++counters.readbacks;
    } catch (...) {
      if (!failed_) { release(s); pending_.reset(); }
      throw;
    }
  }
};
}
std::unique_ptr<Device> create_vulkan() { return std::make_unique<VulkanDevice>(); }
}

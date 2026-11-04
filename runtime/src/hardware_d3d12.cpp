#include "hardware_win32.h"
#include <d3d12.h>
#include <dxgi1_4.h>

namespace psp::ge::hardware {
namespace {
struct Submission {
  std::vector<ComPtr<ID3D12Resource>> resources;
  size_t cursor = 0, bytes = 0;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
};
class D3D12Device final : public Device {
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pipeline_;
  ComPtr<ID3D12Fence> fence_;
  uint64_t value_ = 0;
  HANDLE event_ = nullptr;
  std::string description_;
  std::unique_ptr<Submission> pending_;
  std::unique_ptr<Submission> idle_;
  ComPtr<ID3D12Resource> buffer(Submission& s, size_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    const size_t index = s.cursor++;
    bytes = std::max<size_t>(4, bytes);
    if (index < s.resources.size() && s.resources[index]->GetDesc().Width >= bytes)
      return s.resources[index];
    if (index < s.resources.size()) {
      s.bytes -= static_cast<size_t>(s.resources[index]->GetDesc().Width);
      s.resources[index].Reset();
    } else s.resources.emplace_back();
    if (bytes > (384u << 20) - s.bytes)
      throw std::runtime_error("D3D12 retained buffer budget exceeded; restart at a lower internal scale.");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<size_t>(4, bytes); desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                            IID_PPV_ARGS(&result)), "D3D12 bounded buffer allocation");
    s.resources[index] = result;
    s.bytes += bytes;
    return result;
  }
  void upload(ID3D12Resource* resource, const void* input, size_t bytes) {
    void* memory = nullptr;
    D3D12_RANGE noRead{0, 0};
    check(resource->Map(0, &noRead, &memory), "D3D12 upload map");
    std::memcpy(memory, input, bytes);
    D3D12_RANGE written{0, bytes};
    resource->Unmap(0, &written);
  }
  void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
  }
 public:
  D3D12Device() {
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "D3D12 DXGI factory");
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> adapter;
      HRESULT hr = factory->EnumAdapters1(i, &adapter);
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      check(hr, "D3D12 enumerate adapter");
      DXGI_ADAPTER_DESC1 desc{};
      check(adapter->GetDesc1(&desc), "D3D12 adapter description");
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
      if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        description_ = "Direct3D 12 FL11.0 hardware compute rasterizer / " + narrow(desc.Description);
        break;
      }
    }
    if (!device_) throw std::runtime_error("No Direct3D12 hardware adapter supports feature level 11.0. Update the GPU driver or select Software.");
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_)), "D3D12 command queue");
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[2].Descriptor.ShaderRegister = 1;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters = 4; signature.pParameters = parameters;
    ComPtr<ID3DBlob> blob, errors;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "D3D12 raster root signature");
    check(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)), "D3D12 root signature");
    auto code = shader();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_.Get();
    pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    check(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)), "D3D12 raster pipeline");
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "D3D12 completion fence");
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) throw std::runtime_error("Cannot create D3D12 bounded completion event.");
  }
  ~D3D12Device() override {
    // A terminal timeout must not free resources still referenced by an unfinished GPU list.
    // This single bounded submission is reclaimed by process teardown, never retried.
    if (pending_) { pending_.release(); return; }
    if (event_) CloseHandle(event_);
  }
  std::string description() const override { return description_; }
  void run(Job& job, Counters& counters) override {
    if (pending_) throw std::runtime_error("D3D12 renderer is stopped after an incomplete submission; restart with a lower scale.");
    auto commands = tiles(job);
    if (commands.empty()) return;
    auto submission = idle_ ? std::move(idle_) : std::make_unique<Submission>();
    auto& s = *submission;
    s.cursor = 0;
    if (s.allocator) {
      check(s.allocator->Reset(), "D3D12 completed allocator reset");
      check(s.list->Reset(s.allocator.Get(), pipeline_.Get()), "D3D12 completed raster list reset");
    } else {
      check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)), "D3D12 allocator");
      check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), pipeline_.Get(),
                                       IID_PPV_ARGS(&s.list)), "D3D12 raster list");
    }
    std::vector<uint32_t>* values[] = {&job.colour, &job.depth};
    ComPtr<ID3D12Resource> targets[2], readback[2];
    for (int i = 0; i < 2; ++i) {
      const size_t bytes = std::max<size_t>(4, values[i]->size() * 4);
      targets[i] = buffer(s, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      readback[i] = buffer(s, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      auto input = buffer(s, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      uint32_t zero = 0;
      upload(input.Get(), values[i]->empty() ? &zero : values[i]->data(), bytes);
      s.list->CopyBufferRegion(targets[i].Get(), 0, input.Get(), 0, bytes);
      transition(s.list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    auto texture = buffer(s, std::max<size_t>(4, job.texture.size() * 4), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint32_t zero = 0;
    upload(texture.Get(), job.texture.empty() ? &zero : job.texture.data(), std::max<size_t>(4, job.texture.size() * 4));
    constexpr size_t stride = (sizeof(Command) + 255) & ~size_t(255);
    auto constants = buffer(s, stride * commands.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    std::vector<uint8_t> data(stride * commands.size());
    for (size_t i = 0; i < commands.size(); ++i) std::memcpy(data.data() + i * stride, &commands[i], sizeof(Command));
    upload(constants.Get(), data.data(), data.size());
    s.list->SetComputeRootSignature(root_.Get());
    s.list->SetComputeRootUnorderedAccessView(1, targets[0]->GetGPUVirtualAddress());
    s.list->SetComputeRootUnorderedAccessView(2, targets[1]->GetGPUVirtualAddress());
    s.list->SetComputeRootShaderResourceView(3, texture->GetGPUVirtualAddress());
    for (size_t i = 0; i < commands.size(); ++i) {
      s.list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress() + i * stride);
      s.list->Dispatch((commands[i].bounds[2] + 7) / 8, (commands[i].bounds[3] + 7) / 8, 1);
      D3D12_RESOURCE_BARRIER ordering{}; ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      s.list->ResourceBarrier(1, &ordering);
    }
    for (int i = 0; i < 2; ++i) {
      transition(s.list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      s.list->CopyBufferRegion(readback[i].Get(), 0, targets[i].Get(), 0, std::max<size_t>(4, values[i]->size() * 4));
      transition(s.list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    check(s.list->Close(), "D3D12 close raster list");
    ID3D12CommandList* list = s.list.Get();
    pending_ = std::move(submission);
    queue_->ExecuteCommandLists(1, &list);
    check(queue_->Signal(fence_.Get(), ++value_), "D3D12 submit fence");
    check(fence_->SetEventOnCompletion(value_, event_), "D3D12 completion event");
    if (WaitForSingleObject(event_, 5000) != WAIT_OBJECT_0)
      throw std::runtime_error("D3D12 GPU completion exceeded 5 seconds; renderer stopped without retry.");
    check(device_->GetDeviceRemovedReason(), "D3D12 device health");
    for (int i = 0; i < 2; ++i) {
      void* output = nullptr;
      D3D12_RANGE range{0, values[i]->size() * 4};
      check(readback[i]->Map(0, &range, &output), "D3D12 completed readback");
      std::memcpy(values[i]->data(), output, range.End);
      D3D12_RANGE noWrite{0, 0}; readback[i]->Unmap(0, &noWrite);
    }
    idle_ = std::move(pending_);
    ++counters.submissions; counters.dispatches += commands.size(); counters.primitives += job.commands.size();
  }
};
}
std::unique_ptr<Device> create_d3d12() { return std::make_unique<D3D12Device>(); }
}

#include "hardware_win32.h"
#include <d3d11.h>
#include <dxgi.h>

namespace psp::ge::hardware {
namespace {
struct Buffer {
  ComPtr<ID3D11Buffer> data, readback;
  ComPtr<ID3D11UnorderedAccessView> uav;
  ComPtr<ID3D11ShaderResourceView> srv;
};
class D3D11Device final : public Device {
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11ComputeShader> shader_;
  ComPtr<ID3D11Buffer> constants_;
  ComPtr<ID3D11Query> fence_;
  std::string description_;
  Buffer buffer(const std::vector<uint32_t>& values, bool writable) {
    Buffer b;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(std::max<size_t>(1, values.size()) * sizeof(uint32_t));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = writable ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(uint32_t);
    uint32_t zero = 0;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = values.empty() ? &zero : values.data();
    check(device_->CreateBuffer(&desc, &init, &b.data), "D3D11 GPU buffer");
    if (writable) {
      check(device_->CreateUnorderedAccessView(b.data.Get(), nullptr, &b.uav), "D3D11 storage view");
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      check(device_->CreateBuffer(&desc, nullptr, &b.readback), "D3D11 readback buffer");
    } else check(device_->CreateShaderResourceView(b.data.Get(), nullptr, &b.srv), "D3D11 texture view");
    return b;
  }
  void read(Buffer& buffer, std::vector<uint32_t>& output) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(buffer.readback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped), "D3D11 completed readback");
    std::memcpy(output.data(), mapped.pData, output.size() * sizeof(uint32_t));
    context_->Unmap(buffer.readback.Get(), 0);
  }
 public:
  D3D11Device() {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                            &level, 1, D3D11_SDK_VERSION, &device_, nullptr, &context_), "D3D11 hardware device");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    check(device_.As(&dxgi), "D3D11 DXGI device");
    check(dxgi->GetAdapter(&adapter), "D3D11 adapter");
    DXGI_ADAPTER_DESC desc{};
    check(adapter->GetDesc(&desc), "D3D11 adapter description");
    description_ = "Direct3D 11 FL11.0 hardware compute rasterizer / " + narrow(desc.Description);
    auto code = shader();
    check(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_), "D3D11 raster shader");
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Command); cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device_->CreateBuffer(&cb, nullptr, &constants_), "D3D11 raster constants");
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    check(device_->CreateQuery(&query, &fence_), "D3D11 completion query");
  }
  std::string description() const override { return description_; }
  void run(Job& job, Counters& counters) override {
    auto commands = tiles(job);
    if (commands.empty()) return;
    Buffer colour = buffer(job.colour, true), depth = buffer(job.depth, true), texture = buffer(job.texture, false);
    ID3D11UnorderedAccessView* outputs[] = {colour.uav.Get(), depth.uav.Get()};
    ID3D11ShaderResourceView* input = texture.srv.Get();
    ID3D11Buffer* cb = constants_.Get();
    context_->CSSetShader(shader_.Get(), nullptr, 0);
    context_->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
    context_->CSSetShaderResources(0, 1, &input);
    context_->CSSetConstantBuffers(0, 1, &cb);
    for (const auto& command : commands) {
      context_->UpdateSubresource(constants_.Get(), 0, nullptr, &command, 0, 0);
      context_->Dispatch((command.bounds[2] + 7) / 8, (command.bounds[3] + 7) / 8, 1);
    }
    ID3D11UnorderedAccessView* none[] = {nullptr, nullptr};
    context_->CSSetUnorderedAccessViews(0, 2, none, nullptr);
    input = nullptr; context_->CSSetShaderResources(0, 1, &input);
    context_->CopyResource(colour.readback.Get(), colour.data.Get());
    context_->CopyResource(depth.readback.Get(), depth.data.Get());
    context_->End(fence_.Get());
    context_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    BOOL ready = FALSE;
    for (;;) {
      HRESULT hr = context_->GetData(fence_.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      check(hr, "D3D11 GPU completion");
      if (hr == S_OK && ready) break;
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("D3D11 GPU completion exceeded 5 seconds; renderer stopped without retry.");
      Sleep(1);
    }
    read(colour, job.colour); read(depth, job.depth);
    ++counters.submissions; counters.dispatches += commands.size(); counters.primitives += job.commands.size();
  }
};
}
std::unique_ptr<Device> create_d3d11() { return std::make_unique<D3D11Device>(); }
}

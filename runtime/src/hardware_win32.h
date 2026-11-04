#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "hardware_renderer.h"
#include "hardware_shader_source.h"

namespace psp::ge::hardware {
using Microsoft::WRL::ComPtr;
inline void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s failed (HRESULT 0x%08lX). Check GPU drivers/device availability; no software fallback was selected.",
                  operation, static_cast<unsigned long>(hr));
    throw std::runtime_error(text);
  }
}
inline ComPtr<ID3DBlob> shader() {
  ComPtr<ID3DBlob> code, errors;
  HRESULT hr = D3DCompile(kHardwareShader, sizeof(kHardwareShader) - 1, "hardware_raster.hlsl", nullptr, nullptr,
                         "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                         &code, &errors);
  if (FAILED(hr) && errors)
    throw std::runtime_error("GPU shader compilation failed: " +
                             std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
  check(hr, "D3DCompile");
  return code;
}
inline std::string narrow(const wchar_t* text) {
  int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes < 1) throw std::runtime_error("Cannot decode GPU adapter name.");
  std::string out(bytes, 0);
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  out.pop_back();
  return out;
}
inline std::vector<Command> tiles(const Job& job) {
  std::vector<Command> output;
  for (const auto& source : job.commands) {
    if (source.bounds[2] > 8192 || source.bounds[3] > 8192)
      throw std::runtime_error("Hardware primitive bounds exceed supported target dimensions.");
    for (uint32_t y = 0; y < source.bounds[3]; y += 256)
      for (uint32_t x = 0; x < source.bounds[2]; x += 256) {
        if (output.size() == 4096)
          throw std::runtime_error("Hardware draw exceeds 4096 bounded tiles; select a lower internal scale.");
        Command tile = source;
        tile.bounds[0] += x; tile.bounds[1] += y;
        tile.bounds[2] = std::min(256u, source.bounds[2] - x);
        tile.bounds[3] = std::min(256u, source.bounds[3] - y);
        output.push_back(tile);
      }
  }
  return output;
}
}

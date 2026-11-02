#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mgspo {

using Bytes = std::vector<uint8_t>;

class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

inline uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline Bytes readFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw Error("cannot open " + path.string());
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  Bytes out(static_cast<size_t>(n));
  if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
  if (!f) throw Error("failed reading " + path.string());
  return out;
}

inline void writeFile(const std::filesystem::path& path, const Bytes& data) {
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw Error("cannot write " + path.string());
  if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!f) throw Error("failed writing " + path.string());
}

}  // namespace mgspo

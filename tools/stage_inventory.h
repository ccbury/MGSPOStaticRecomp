#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mgspo::stages {
namespace fs = std::filesystem;

inline void validate(const fs::path& path) {
  if (fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path))
    throw std::runtime_error("stage must be a regular, non-symlink file: " + path.u8string());
  std::ifstream in(path, std::ios::binary);
  char magic[4] = {};
  in.read(magic, sizeof(magic));
  if (in.gcount() != sizeof(magic) || magic[0] != '\x7F' || magic[1] != 'E' ||
      magic[2] != 'L' || magic[3] != 'F')
    throw std::runtime_error("unsupported/encrypted stage (expected plain ELF; decrypt your own copy first): " +
                             path.u8string());
}

inline std::vector<fs::path> normalize(std::vector<fs::path> paths) {
  for (auto& path : paths) {
    validate(path);
    path = fs::canonical(path);
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

inline std::vector<fs::path> enumerate(const fs::path& root) {
  if (!fs::is_directory(root) || fs::is_symlink(fs::symlink_status(root)))
    throw std::runtime_error("stage directory is missing or is a symlink: " + root.u8string());
  std::vector<fs::path> paths;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_symlink())
      throw std::runtime_error("symlinks are not supported in a stage directory: " + entry.path().u8string());
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().u8string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".prx") paths.push_back(entry.path());
  }
  if (paths.empty()) throw std::runtime_error("no stage PRX modules found in " + root.u8string());
  return normalize(std::move(paths));
}

inline std::string identity(const fs::path& path) {
  if (!fs::exists(path)) return "missing";
  if (fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path))
    throw std::runtime_error("stage input must be a regular, non-symlink file: " + path.u8string());
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read stage input: " + path.u8string());
  uint64_t hash = 14695981039346656037ull, bytes = 0;
  std::array<char, 65536> buffer;
  while (in.read(buffer.data(), buffer.size()) || in.gcount()) {
    bytes += static_cast<uint64_t>(in.gcount());
    for (std::streamsize i = 0; i < in.gcount(); ++i) {
      hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(i)]);
      hash *= 1099511628211ull;
    }
  }
  if (!in.eof()) throw std::runtime_error("cannot finish reading stage input: " + path.u8string());
  return std::to_string(bytes) + ":" + std::to_string(hash);
}

inline std::string manifest(const std::vector<fs::path>& paths) {
  std::ostringstream out;
  out << "stage-inventory-v1\n";
  for (const auto& path : paths)
    out << path.generic_u8string() << "|" << identity(path) << "|_zar="
        << identity(path.parent_path() / "_zar") << "\n";
  return out.str();
}

inline bool matches(const fs::path& file, const std::string& expected) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::string actual((std::istreambuf_iterator<char>(in)), {});
  return !in.bad() && actual == expected;
}

inline void writeManifest(const fs::path& file, const std::string& content) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  if (!out) throw std::runtime_error("cannot write completed stage inventory: " + file.u8string());
}
}  // namespace mgspo::stages

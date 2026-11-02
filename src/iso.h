#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bytes.h"

namespace mgspo {

struct IsoEntry {
  std::string path;  // relative, '/' separated, no version suffix
  uint32_t lba = 0;
  uint32_t size = 0;
  bool isDir = false;
};

// Minimal read-only ISO9660 reader (plain 2048-byte sector images).
class IsoImage {
public:
  explicit IsoImage(const std::string& file);

  const std::string& volumeId() const { return volumeId_; }
  const std::vector<IsoEntry>& entries() const { return entries_; }

  // Case-insensitive lookup; leading '/' is ignored.
  const IsoEntry* find(const std::string& path) const;

  Bytes read(const IsoEntry& e) const;
  void extract(const IsoEntry& e, const std::filesystem::path& dest) const;

private:
  Bytes readRange(uint64_t offset, uint64_t len) const;
  void scanDir(const std::string& base, uint32_t lba, uint32_t size, int depth);

  mutable std::ifstream f_;
  uint64_t fileSize_ = 0;
  std::string volumeId_;
  std::vector<IsoEntry> entries_;
};

}  // namespace mgspo

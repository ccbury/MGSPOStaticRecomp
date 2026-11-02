#include "iso.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mgspo {

namespace {

constexpr uint64_t kSector = 2048;
constexpr size_t kMaxEntries = 200000;
constexpr uint32_t kMaxDirSize = 64u * 1024 * 1024;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool safeName(const std::string& n) {
  if (n.empty() || n == "." || n == "..") return false;
  return n.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

}  // namespace

IsoImage::IsoImage(const std::string& file) : f_(file, std::ios::binary) {
  if (!f_) throw Error("cannot open " + file);
  f_.seekg(0, std::ios::end);
  fileSize_ = static_cast<uint64_t>(f_.tellg());

  Bytes pvd = readRange(16 * kSector, kSector);
  if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
    throw Error("not a plain ISO9660 image (no primary volume descriptor); CSO/ZSO must be decompressed first");

  volumeId_.assign(reinterpret_cast<const char*>(pvd.data() + 40), 32);
  while (!volumeId_.empty() && (volumeId_.back() == ' ' || volumeId_.back() == '\0')) volumeId_.pop_back();

  const uint8_t* root = pvd.data() + 156;
  scanDir("", rd32(root + 2), rd32(root + 10), 0);
}

Bytes IsoImage::readRange(uint64_t offset, uint64_t len) const {
  if (offset + len > fileSize_) throw Error("read past end of image");
  Bytes out(static_cast<size_t>(len));
  f_.clear();
  f_.seekg(static_cast<std::streamoff>(offset));
  if (len) f_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
  if (!f_) throw Error("image read failed");
  return out;
}

void IsoImage::scanDir(const std::string& base, uint32_t lba, uint32_t size, int depth) {
  if (depth > 16) throw Error("directory nesting too deep");
  if (size > kMaxDirSize) throw Error("directory too large");
  Bytes dir = readRange(static_cast<uint64_t>(lba) * kSector, size);

  size_t pos = 0;
  while (pos < dir.size()) {
    uint8_t len = dir[pos];
    if (len == 0) {
      pos = (pos / kSector + 1) * kSector;
      continue;
    }
    if (len < 34 || pos + len > dir.size()) throw Error("corrupt directory record");
    const uint8_t* r = &dir[pos];
    size_t nlen = r[32];
    if (33 + nlen > len) throw Error("corrupt directory record name");
    std::string name(reinterpret_cast<const char*>(r + 33), nlen);
    pos += len;

    if (nlen == 1 && (name[0] == '\0' || name[0] == '\1')) continue;
    size_t semi = name.find(';');
    if (semi != std::string::npos) name.resize(semi);
    if (!name.empty() && name.back() == '.') name.pop_back();
    if (!safeName(name)) continue;

    IsoEntry e;
    e.path = base.empty() ? name : base + "/" + name;
    e.lba = rd32(r + 2);
    e.size = rd32(r + 10);
    e.isDir = (r[25] & 2) != 0;
    if (entries_.size() >= kMaxEntries) throw Error("too many entries");
    entries_.push_back(e);
    if (e.isDir) scanDir(e.path, e.lba, e.size, depth + 1);
  }
}

const IsoEntry* IsoImage::find(const std::string& path) const {
  std::string p = lower(path);
  while (!p.empty() && (p[0] == '/' || p[0] == '\\')) p.erase(0, 1);
  for (const auto& e : entries_)
    if (lower(e.path) == p) return &e;
  return nullptr;
}

Bytes IsoImage::read(const IsoEntry& e) const {
  return readRange(static_cast<uint64_t>(e.lba) * kSector, e.size);
}

void IsoImage::extract(const IsoEntry& e, const std::filesystem::path& dest) const {
  if (e.isDir) {
    std::filesystem::create_directories(dest);
    return;
  }
  if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path());
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) throw Error("cannot write " + dest.string());
  constexpr uint64_t chunk = 1u << 20;
  uint64_t base = static_cast<uint64_t>(e.lba) * kSector;
  for (uint64_t done = 0; done < e.size;) {
    uint64_t n = std::min<uint64_t>(chunk, e.size - done);
    Bytes b = readRange(base + done, n);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    done += n;
  }
  if (!out) throw Error("failed writing " + dest.string());
}

}  // namespace mgspo

#include "elf.h"

#include <cstring>

namespace mgspo {

namespace {

constexpr uint16_t kTypePrx = 0xFFA0;

class Image {
public:
  explicit Image(const Bytes& d, const std::vector<ElfSegment>& segs) : d_(d), segs_(segs) {}

  // Resolve a virtual address to a file offset using the loadable segments.
  const uint8_t* at(uint32_t addr, uint32_t len) const {
    for (const auto& s : segs_) {
      if (s.type != 1) continue;
      if (addr >= s.vaddr && static_cast<uint64_t>(addr) + len <= static_cast<uint64_t>(s.vaddr) + s.fileSize) {
        uint64_t off = static_cast<uint64_t>(s.offset) + (addr - s.vaddr);
        if (off + len <= d_.size()) return &d_[static_cast<size_t>(off)];
      }
    }
    throw Error("address out of mapped range");
  }

  std::string cstr(uint32_t addr) const {
    if (addr == 0) return "";
    std::string out;
    for (uint32_t i = 0; i < 256; ++i) {
      char c = static_cast<char>(*at(addr + i, 1));
      if (!c) break;
      out.push_back(c);
    }
    return out;
  }

private:
  const Bytes& d_;
  const std::vector<ElfSegment>& segs_;
};

}  // namespace

ElfInfo parseElf(const Bytes& d) {
  if (d.size() < 52 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0) throw Error("not an ELF file");
  if (d[4] != 1 || d[5] != 1) throw Error("only 32-bit little-endian ELF is supported");

  ElfInfo info;
  info.type = rd16(&d[16]);
  info.machine = rd16(&d[18]);
  info.entry = rd32(&d[24]);
  uint32_t phoff = rd32(&d[28]);
  uint32_t shoff = rd32(&d[32]);
  info.flags = rd32(&d[36]);
  uint16_t phentsize = rd16(&d[42]), phnum = rd16(&d[44]);
  uint16_t shentsize = rd16(&d[46]), shnum = rd16(&d[48]), shstrndx = rd16(&d[50]);
  info.isPrx = (info.type == kTypePrx);

  if (phnum && (phentsize < 32 || static_cast<uint64_t>(phoff) + static_cast<uint64_t>(phnum) * phentsize > d.size()))
    throw Error("program header table out of range");
  for (uint16_t i = 0; i < phnum; ++i) {
    const uint8_t* p = &d[phoff + static_cast<size_t>(i) * phentsize];
    ElfSegment s;
    s.type = rd32(p);
    s.offset = rd32(p + 4);
    s.vaddr = rd32(p + 8);
    s.paddr = rd32(p + 12);
    s.fileSize = rd32(p + 16);
    s.memSize = rd32(p + 20);
    s.flags = rd32(p + 24);
    info.segments.push_back(s);
  }

  if (shnum) {
    if (shentsize < 40 || static_cast<uint64_t>(shoff) + static_cast<uint64_t>(shnum) * shentsize > d.size())
      throw Error("section header table out of range");
    uint32_t strOff = 0, strSize = 0;
    if (shstrndx < shnum) {
      const uint8_t* s = &d[shoff + static_cast<size_t>(shstrndx) * shentsize];
      strOff = rd32(s + 16);
      strSize = rd32(s + 20);
    }
    for (uint16_t i = 0; i < shnum; ++i) {
      const uint8_t* p = &d[shoff + static_cast<size_t>(i) * shentsize];
      ElfSection s;
      uint32_t nameIdx = rd32(p);
      s.type = rd32(p + 4);
      s.flags = rd32(p + 8);
      s.addr = rd32(p + 12);
      s.offset = rd32(p + 16);
      s.size = rd32(p + 20);
      if (nameIdx < strSize && static_cast<uint64_t>(strOff) + nameIdx < d.size()) {
        for (uint64_t q = static_cast<uint64_t>(strOff) + nameIdx; q < d.size() && d[q]; ++q)
          s.name.push_back(static_cast<char>(d[q]));
      }
      info.sections.push_back(s);
    }
  }

  Image img(d, info.segments);

  // Locate the module info block: prefer the named section, else program header 0's paddr.
  const uint8_t* mi = nullptr;
  for (const auto& s : info.sections) {
    if (s.name == ".rodata.sceModuleInfo" && s.size >= 52 && static_cast<uint64_t>(s.offset) + 52 <= d.size()) {
      mi = &d[s.offset];
      break;
    }
  }
  if (!mi && !info.segments.empty()) {
    uint32_t pa = info.segments[0].paddr;
    try {
      if (info.isPrx) {
        uint64_t off = pa & 0x7FFFFFFFu;
        if (off + 52 <= d.size()) mi = &d[static_cast<size_t>(off)];
      } else {
        mi = img.at(pa, 52);
      }
    } catch (const Error&) {
      mi = nullptr;
    }
  }
  if (!mi) return info;

  ModuleInfo& m = info.module;
  m.present = true;
  m.attr = rd16(mi);
  m.version = rd16(mi + 2);
  m.name.assign(reinterpret_cast<const char*>(mi + 4), strnlen(reinterpret_cast<const char*>(mi + 4), 27));
  m.gp = rd32(mi + 32);
  m.entTop = rd32(mi + 36);
  m.entBottom = rd32(mi + 40);
  m.stubTop = rd32(mi + 44);
  m.stubBottom = rd32(mi + 48);

  // Import (library stub) table.
  for (uint32_t a = m.stubTop; a < m.stubBottom;) {
    const uint8_t* e = img.at(a, 20);
    uint8_t entLen = e[8];
    if (entLen < 5) break;
    ImportedModule mod;
    mod.name = img.cstr(rd32(e));
    mod.version = rd16(e + 4);
    mod.flags = rd16(e + 6);
    mod.varCount = e[9];
    uint16_t funcCount = rd16(e + 10);
    uint32_t nidPtr = rd32(e + 12);
    uint32_t stubPtr = rd32(e + 16);
    if (funcCount) {
      const uint8_t* nids = img.at(nidPtr, static_cast<uint32_t>(funcCount) * 4);
      for (uint16_t i = 0; i < funcCount; ++i)
        mod.funcs.push_back({rd32(nids + i * 4), stubPtr + static_cast<uint32_t>(i) * 8});
    }
    info.imports.push_back(std::move(mod));
    a += static_cast<uint32_t>(entLen) * 4;
  }

  // Export table (summary only).
  for (uint32_t a = m.entTop; a < m.entBottom;) {
    const uint8_t* e = img.at(a, 16);
    uint8_t entLen = e[8];
    if (entLen < 4) break;
    ExportedLib lib;
    lib.name = img.cstr(rd32(e));
    lib.version = rd16(e + 4);
    lib.attr = rd16(e + 6);
    lib.varCount = e[9];
    lib.funcCount = rd16(e + 10);
    info.exports.push_back(std::move(lib));
    a += static_cast<uint32_t>(entLen) * 4;
  }
  return info;
}

}  // namespace mgspo

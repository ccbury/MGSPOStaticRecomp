#include "prx.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace mgspo {

namespace {

constexpr uint32_t kShtPrxRel = 0x700000A0;
constexpr uint32_t kShfExec = 4;

enum RelType : uint32_t {
  R_NONE = 0,
  R_16 = 1,
  R_32 = 2,
  R_REL32 = 3,
  R_26 = 4,
  R_HI16 = 5,
  R_LO16 = 6,
  R_GPREL16 = 7,
};

}  // namespace

uint32_t PrxImage::word(uint32_t addr) const {
  if (!contains(addr)) throw Error("address outside module image");
  return rd32(&mem[addr - base]);
}

bool PrxImage::inCode(uint32_t addr) const {
  for (const auto& r : code)
    if (addr >= r.start && addr < r.end) return true;
  return false;
}

PrxImage loadPrx(const Bytes& elf, uint32_t base) {
  PrxImage img;
  img.base = base;
  img.info = parseElf(elf);
  const ElfInfo& info = img.info;
  if (!info.isPrx) throw Error("only relocatable PRX modules are supported");
  if (info.machine != 8) throw Error("not a MIPS module");

  uint64_t size = 0;
  for (const auto& s : info.segments)
    if (s.type == 1) size = std::max<uint64_t>(size, static_cast<uint64_t>(s.vaddr) + s.memSize);
  if (size == 0 || size > 0x2000000) throw Error("implausible module size");
  img.mem.assign(static_cast<size_t>(size), 0);

  for (const auto& s : info.segments) {
    if (s.type != 1) continue;
    if (static_cast<uint64_t>(s.offset) + s.fileSize > elf.size()) throw Error("segment past end of file");
    std::memcpy(&img.mem[s.vaddr], &elf[s.offset], s.fileSize);
  }

  for (const auto& s : info.sections) {
    if ((s.flags & kShfExec) && s.name != ".sceStub.text" && s.size)
      img.code.push_back({base + s.addr, base + s.addr + s.size, s.name});
  }
  std::sort(img.code.begin(), img.code.end(), [](const CodeRange& a, const CodeRange& b) { return a.start < b.start; });

  auto segBase = [&](uint32_t idx) -> uint32_t {
    if (idx >= info.segments.size()) throw Error("relocation refers to a missing segment");
    return base + info.segments[idx].vaddr;
  };
  auto rd = [&](uint32_t addr) { return img.word(addr); };
  auto wr = [&](uint32_t addr, uint32_t v) {
    if (!img.contains(addr)) throw Error("relocation target outside image");
    for (int i = 0; i < 4; ++i) img.mem[addr - base + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  auto noteTaken = [&](uint32_t v) {
    if (img.inCode(v)) img.addressTaken.insert(v);
  };

  std::vector<uint32_t> pendingHi;
  std::map<uint32_t, uint32_t> ptrSites;
  for (const auto& sec : info.sections) {
    if (sec.type != kShtPrxRel) continue;
    if (static_cast<uint64_t>(sec.offset) + sec.size > elf.size()) throw Error("relocation section past end of file");
    for (uint32_t o = 0; o + 8 <= sec.size; o += 8) {
      uint32_t off = rd32(&elf[sec.offset + o]);
      uint32_t inf = rd32(&elf[sec.offset + o + 4]);
      uint32_t type = inf & 0xFF;
      uint32_t ofsSeg = (inf >> 8) & 0xFF;
      uint32_t valSeg = (inf >> 16) & 0xFF;
      uint32_t addr = segBase(ofsSeg) + off;

      switch (type) {
        case R_NONE: break;
        case R_32: {
          uint32_t v = rd(addr) + segBase(valSeg);
          wr(addr, v);
          noteTaken(v);
          if (img.inCode(v)) ptrSites[addr] = v;
          break;
        }
        case R_26: {
          uint32_t d = rd(addr);
          wr(addr, (d & ~0x03FFFFFFu) | ((d + (segBase(valSeg) >> 2)) & 0x03FFFFFFu));
          break;
        }
        case R_HI16: pendingHi.push_back(addr); break;
        case R_LO16: {
          uint32_t lo = rd(addr);
          int32_t loVal = static_cast<int16_t>(lo & 0xFFFF);
          uint32_t vb = segBase(valSeg);
          for (uint32_t h : pendingHi) {
            uint32_t hd = rd(h);
            uint32_t full = ((hd & 0xFFFF) << 16) + static_cast<uint32_t>(loVal) + vb;
            wr(h, (hd & ~0xFFFFu) | (((full + 0x8000) >> 16) & 0xFFFF));
            noteTaken(full);
          }
          pendingHi.clear();
          wr(addr, (lo & ~0xFFFFu) | ((static_cast<uint32_t>(loVal) + vb) & 0xFFFF));
          break;
        }
        case R_16: case R_REL32: case R_GPREL16:
        default: ++img.unknownRelocs; break;
      }
    }
  }

  img.entry = base + info.entry;
  img.gp = base + info.module.gp;

  // Runs of adjacent code pointers that stay within a small window look like switch
  // tables; their targets are labels inside a function, not function starts.
  for (auto it = ptrSites.begin(); it != ptrSites.end();) {
    auto runStart = it;
    uint32_t lo = it->second, hi = it->second, prev = it->first;
    size_t n = 1;
    // Tables sit next to other pointer arrays; start a new run once the targets stop being local.
    for (++it; it != ptrSites.end() && it->first == prev + 4; ++it) {
      uint32_t nlo = std::min(lo, it->second), nhi = std::max(hi, it->second);
      if (nhi - nlo >= 0x2000) break;
      prev = it->first;
      lo = nlo;
      hi = nhi;
      ++n;
    }
    if (n >= 2 && hi - lo < 0x2000) {
      // A table of functions looks the same as a switch table, but every entry follows a return.
      constexpr uint32_t kJrRa = 0x03E00008;
      auto afterReturn = [&](uint32_t t) {
        uint32_t p = t;
        int skipped = 0;
        while (p >= base + 4 && rd(p - 4) == 0 && skipped < 64) {
          p -= 4;
          ++skipped;
        }
        return p >= base + 8 && (rd(p - 4) == kJrRa || rd(p - 8) == kJrRa);
      };
      PrxImage::JumpTable jt;
      jt.start = runStart->first;
      jt.end = prev + 4;
      for (auto j = runStart; j != it; ++j) {
        if (afterReturn(j->second)) continue;
        img.jumpTargets.insert(j->second);
        jt.targets.push_back(j->second);
      }
      if (!jt.targets.empty()) img.jumpTables.push_back(std::move(jt));
    }
  }

  // Exported functions: the library table lists NIDs, then addresses (already relocated).
  if (info.module.present) {
    for (uint32_t a = base + info.module.entTop; a < base + info.module.entBottom;) {
      uint8_t entLen = img.mem[a - base + 8];
      if (entLen < 4) break;
      uint8_t varCount = img.mem[a - base + 9];
      uint16_t funcCount = rd16(&img.mem[a - base + 10]);
      uint32_t table = img.word(a + 12);
      std::string lib;
      if (uint32_t np = img.word(a); np && img.contains(np)) {
        for (uint32_t q = np; img.contains(q, 1) && img.mem[q - base]; ++q) lib.push_back(static_cast<char>(img.mem[q - base]));
      }
      for (uint16_t i = 0; i < funcCount; ++i) {
        uint32_t fn = img.word(table + (static_cast<uint32_t>(funcCount) + varCount) * 4 + i * 4);
        if (img.inCode(fn)) img.exportedFuncs.push_back(fn);
        img.exports.push_back({lib, img.word(table + i * 4), fn});
      }
      a += static_cast<uint32_t>(entLen) * 4;
    }
  }
  return img;
}

}  // namespace mgspo

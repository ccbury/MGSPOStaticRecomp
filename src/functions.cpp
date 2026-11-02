#include "functions.h"

#include <algorithm>

#include "mips.h"
#include "vfpu.h"

namespace mgspo {

namespace {

constexpr uint32_t kJrRa = 0x03E00008;

bool looksLikeStart(const PrxImage& img, uint32_t addr, bool afterJump = false) {
  uint32_t p = addr;
  int skipped = 0;
  while (p >= img.base + 4 && img.word(p - 4) == 0 && skipped < 64) {
    p -= 4;
    ++skipped;
  }
  if (p < img.base + 4) return true;
  if (img.word(p - 4) == kJrRa) return true;
  if (p < img.base + 8) return false;
  uint32_t prev = img.word(p - 8);
  if (prev == kJrRa) return true;
  // for pointers the code may also follow an unconditional branch or jump and its delay slot
  return afterJump && ((prev >> 16) == 0x1000 || (prev >> 26) == 2);
}

}  // namespace

const Function* Program::find(uint32_t addr) const {
  auto it = std::lower_bound(funcs.begin(), funcs.end(), addr,
                             [](const Function& f, uint32_t a) { return f.start < a; });
  return (it != funcs.end() && it->start == addr) ? &*it : nullptr;
}

const Function* Program::containing(uint32_t addr) const {
  auto it = std::upper_bound(funcs.begin(), funcs.end(), addr,
                             [](uint32_t a, const Function& f) { return a < f.start; });
  if (it == funcs.begin()) return nullptr;
  --it;
  return addr < it->end ? &*it : nullptr;
}

Program discover(const PrxImage& img, uint32_t importBase) {
  Program prog;
  prog.img = &img;

  uint32_t idx = importBase;
  for (const auto& m : img.info.imports) {
    for (const auto& f : m.funcs) {
      ImportRef r;
      r.index = idx++;
      r.module = m.name;
      r.nid = f.nid;
      r.stub = f.stubAddr + img.base;
      prog.stubIndex[r.stub] = r.index;
      prog.imports.push_back(std::move(r));
    }
  }

  std::set<uint32_t> roots;
  std::set<uint32_t> jumpCandidates;
  std::vector<std::pair<uint32_t, uint32_t>> branchRefs;
  DiscoveryStats& st = prog.stats;

  for (const auto& r : img.code) roots.insert(r.start);
  if (img.inCode(img.entry)) roots.insert(img.entry);
  for (uint32_t e : img.exportedFuncs)
    if (img.inCode(e)) roots.insert(e);

  for (const auto& r : img.code) {
    for (uint32_t a = r.start; a + 4 <= r.end; a += 4) {
      if (prog.isStub(a)) {
        a += 4;  // stubs are two words
        continue;
      }
      Insn in = decode(img.word(a), a);
      ++st.totalInsns;
      switch (in.op) {
        case Op::Vfpu:
          ++st.vfpuInsns;
          if (decodeVfpu(in.raw) == psp::vfpu::Op::Invalid) ++st.vfpuUndecoded;
          break;
        case Op::Bvf: case Op::Bvt: case Op::Bvfl: case Op::Bvtl: ++st.vfpuBranches; break;
        case Op::Cop0: ++st.cop0Insns; break;
        case Op::Unknown:
        case Op::Invalid: ++st.unknownInsns; break;
        case Op::Jal:
          if (prog.isStub(in.target)) break;
          if (img.inCode(in.target)) {
            if (roots.insert(in.target).second) ++st.callTargets;
          } else {
            ++st.callsOutsideCode;
          }
          break;
        case Op::J:
          if (img.inCode(in.target) && !prog.isStub(in.target)) jumpCandidates.insert(in.target);
          break;
        case Op::Beq: case Op::Bne: case Op::Blez: case Op::Bgtz: case Op::Beql: case Op::Bnel:
        case Op::Blezl: case Op::Bgtzl: case Op::Bltz: case Op::Bgez: case Op::Bltzl: case Op::Bgezl:
          if (img.inCode(in.target)) branchRefs.emplace_back(a, in.target);
          break;
        default: break;
      }
    }
  }

  for (uint32_t t : jumpCandidates) {
    if (roots.count(t)) continue;
    if (looksLikeStart(img, t)) {
      roots.insert(t);
      ++st.jumpRoots;
    }
  }
  for (uint32_t t : img.addressTaken) {
    if (roots.count(t) || img.jumpTargets.count(t) || prog.isStub(t)) continue;
    if (!img.inCode(t)) continue;
    if (looksLikeStart(img, t, true)) {
      roots.insert(t);
      ++st.pointerRoots;
    }
  }
  // A branch that lands in a different function right after a return is really a tail jump to a
  // function that nothing else points at.
  for (const auto& br : branchRefs) {
    uint32_t t = br.second;
    if (roots.count(t) || prog.isStub(t)) continue;
    auto ts = roots.upper_bound(t);
    auto ss = roots.upper_bound(br.first);
    if (ts == roots.begin() || ss == roots.begin() || *std::prev(ts) == *std::prev(ss)) continue;
    uint32_t p = t;
    while (p >= img.base + 4 && img.word(p - 4) == 0 && t - p < 256) p -= 4;
    if (p >= img.base + 8 && (img.word(p - 4) == kJrRa || img.word(p - 8) == kJrRa)) {
      roots.insert(t);
      ++st.jumpRoots;
    }
  }
  st.roots = static_cast<uint32_t>(roots.size());

  // Boundaries that terminate a function even when they are not roots.
  std::set<uint32_t> stops(roots);
  for (const auto& kv : prog.stubIndex) stops.insert(kv.first);

  for (const auto& r : img.code) {
    auto it = stops.lower_bound(r.start);
    for (; it != stops.end() && *it < r.end; ++it) {
      if (!roots.count(*it)) continue;
      auto nx = std::next(it);
      uint32_t end = (nx != stops.end() && *nx < r.end) ? *nx : r.end;
      prog.funcs.push_back({*it, end});
    }
  }
  std::sort(prog.funcs.begin(), prog.funcs.end(),
            [](const Function& a, const Function& b) { return a.start < b.start; });
  return prog;
}

}  // namespace mgspo

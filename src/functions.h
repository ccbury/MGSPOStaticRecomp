#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "prx.h"

namespace mgspo {

struct ImportRef {
  uint32_t index = 0;  // position in the flattened import table
  std::string module;
  uint32_t nid = 0;
  uint32_t stub = 0;  // absolute guest address of the 8 byte stub
};

struct Function {
  uint32_t start = 0;
  uint32_t end = 0;  // exclusive
};

struct DiscoveryStats {
  uint32_t roots = 0;
  uint32_t callTargets = 0;
  uint32_t jumpRoots = 0;
  uint32_t pointerRoots = 0;
  uint32_t callsOutsideCode = 0;  // jal targets that are neither code nor an import stub
  uint32_t vfpuInsns = 0;
  uint32_t vfpuUndecoded = 0;  // VFPU words the lifter could not identify
  uint32_t vfpuBranches = 0;   // bvf/bvt family (counted separately from vfpuInsns)
  uint32_t cop0Insns = 0;
  uint32_t unknownInsns = 0;
  uint32_t totalInsns = 0;
};

struct Program {
  const PrxImage* img = nullptr;
  std::vector<ImportRef> imports;
  std::map<uint32_t, uint32_t> stubIndex;  // stub address -> import index
  std::vector<Function> funcs;             // sorted by start
  DiscoveryStats stats;

  const Function* find(uint32_t addr) const;       // function starting exactly at addr
  const Function* containing(uint32_t addr) const;
  bool isStub(uint32_t addr) const { return stubIndex.count(addr) != 0; }
};

Program discover(const PrxImage& img);

}  // namespace mgspo

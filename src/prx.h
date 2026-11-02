#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "bytes.h"
#include "elf.h"

namespace mgspo {

struct CodeRange {
  uint32_t start = 0;  // absolute guest addresses, end exclusive
  uint32_t end = 0;
  std::string name;
};

struct ExportedFunc {
  std::string lib;  // empty for the module's own table (module_start and friends)
  uint32_t nid = 0;
  uint32_t addr = 0;  // absolute guest address
};

constexpr uint32_t kNidModuleStart = 0xD632ACDB;
constexpr uint32_t kNidModuleStop = 0xCEE8593C;
constexpr uint32_t kNidModuleBootstart = 0xD3744BE0;

// A PRX placed at a fixed guest base with its relocations applied, ready to be
// disassembled or copied into emulated memory.
struct PrxImage {
  uint32_t base = 0;
  Bytes mem;  // flat image; byte 0 is guest address `base`
  ElfInfo info;
  uint32_t entry = 0;
  uint32_t gp = 0;
  std::vector<CodeRange> code;           // executable sections, stub tables excluded
  std::vector<uint32_t> exportedFuncs;   // absolute addresses of exported functions
  std::vector<ExportedFunc> exports;     // every exported function with its library and NID
  std::set<uint32_t> addressTaken;       // code addresses produced by relocations
  std::set<uint32_t> jumpTargets;        // subset of addressTaken that looks like switch-table entries
  uint32_t unknownRelocs = 0;

  bool contains(uint32_t addr, uint32_t len = 4) const {
    return addr >= base && static_cast<uint64_t>(addr - base) + len <= mem.size();
  }
  uint32_t word(uint32_t addr) const;
  bool inCode(uint32_t addr) const;
};

constexpr uint32_t kDefaultBase = 0x08804000;

PrxImage loadPrx(const Bytes& elf, uint32_t base = kDefaultBase);

}  // namespace mgspo

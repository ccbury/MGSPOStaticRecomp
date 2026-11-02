#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "bytes.h"

namespace mgspo {

struct ElfSection {
  std::string name;
  uint32_t type = 0, flags = 0, addr = 0, offset = 0, size = 0;
};

struct ElfSegment {
  uint32_t type = 0, offset = 0, vaddr = 0, paddr = 0, fileSize = 0, memSize = 0, flags = 0;
};

struct ImportedFunc {
  uint32_t nid = 0;
  uint32_t stubAddr = 0;
};

struct ImportedModule {
  std::string name;
  uint16_t version = 0;
  uint16_t flags = 0;
  uint16_t varCount = 0;
  std::vector<ImportedFunc> funcs;
};

struct ExportedLib {
  std::string name;  // empty for the module's own export table
  uint16_t version = 0;
  uint16_t attr = 0;
  uint16_t funcCount = 0;
  uint8_t varCount = 0;
};

struct ModuleInfo {
  bool present = false;
  uint16_t attr = 0;
  uint16_t version = 0;
  std::string name;
  uint32_t gp = 0;
  uint32_t entTop = 0, entBottom = 0, stubTop = 0, stubBottom = 0;
};

struct ElfInfo {
  uint16_t type = 0;
  uint16_t machine = 0;
  uint32_t entry = 0;
  uint32_t flags = 0;
  bool isPrx = false;
  std::vector<ElfSegment> segments;
  std::vector<ElfSection> sections;
  ModuleInfo module;
  std::vector<ImportedModule> imports;
  std::vector<ExportedLib> exports;
};

ElfInfo parseElf(const Bytes& data);

}  // namespace mgspo

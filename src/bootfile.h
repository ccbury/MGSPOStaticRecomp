#pragma once
#include <cstdint>
#include <string>

#include "bytes.h"

namespace mgspo {

enum class BootKind { Elf, EncryptedPsp, Pbp, Unknown };

const char* bootKindName(BootKind k);
BootKind classifyBoot(const Bytes& data);

// Header of a "~PSP" container (the encrypted/packed form retail EBOOT.BIN uses).
struct PspHeader {
  uint16_t attribute = 0;
  uint16_t compAttribute = 0;
  std::string moduleName;
  uint8_t segmentCount = 0;
  uint32_t elfSize = 0;
  uint32_t pspSize = 0;
  uint32_t bootEntry = 0;
  uint32_t modinfoOffset = 0;
  uint32_t bssSize = 0;
};

PspHeader parsePspHeader(const Bytes& data);

// PBP packages: sections are PARAM.SFO, ICON0, ICON1, PIC0, PIC1, SND0, DATA.PSP, DATA.PSAR.
constexpr int kPbpSectionCount = 8;
const char* pbpSectionName(int index);
Bytes pbpSection(const Bytes& data, int index);

}  // namespace mgspo

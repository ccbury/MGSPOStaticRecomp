#include "bootfile.h"

#include <algorithm>
#include <cstring>

namespace mgspo {

const char* bootKindName(BootKind k) {
  switch (k) {
    case BootKind::Elf: return "plain ELF/PRX";
    case BootKind::EncryptedPsp: return "~PSP container";
    case BootKind::Pbp: return "PBP package";
    default: return "unknown";
  }
}

BootKind classifyBoot(const Bytes& d) {
  if (d.size() >= 4) {
    if (std::memcmp(d.data(), "\x7f" "ELF", 4) == 0) return BootKind::Elf;
    if (std::memcmp(d.data(), "~PSP", 4) == 0) return BootKind::EncryptedPsp;
    if (std::memcmp(d.data(), "\0PBP", 4) == 0) return BootKind::Pbp;
  }
  return BootKind::Unknown;
}

PspHeader parsePspHeader(const Bytes& d) {
  if (d.size() < 0x40 || std::memcmp(d.data(), "~PSP", 4) != 0) throw Error("not a ~PSP file");
  PspHeader h;
  h.attribute = rd16(&d[4]);
  h.compAttribute = rd16(&d[6]);
  const char* name = reinterpret_cast<const char*>(&d[0x0A]);
  h.moduleName.assign(name, strnlen(name, 28));
  h.segmentCount = d[0x27];
  h.elfSize = rd32(&d[0x28]);
  h.pspSize = rd32(&d[0x2C]);
  h.bootEntry = rd32(&d[0x30]);
  h.modinfoOffset = rd32(&d[0x34]);
  h.bssSize = rd32(&d[0x38]);
  return h;
}

const char* pbpSectionName(int i) {
  static const char* names[kPbpSectionCount] = {"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG",
                                                "PIC1.PNG",  "SND0.AT3",  "DATA.PSP",  "DATA.PSAR"};
  return (i >= 0 && i < kPbpSectionCount) ? names[i] : "?";
}

Bytes pbpSection(const Bytes& d, int index) {
  if (index < 0 || index >= kPbpSectionCount) throw Error("bad PBP section index");
  if (d.size() < 0x28 || std::memcmp(d.data(), "\0PBP", 4) != 0) throw Error("not a PBP file");
  uint32_t off[kPbpSectionCount + 1];
  for (int i = 0; i < kPbpSectionCount; ++i) off[i] = rd32(&d[8 + i * 4]);
  off[kPbpSectionCount] = static_cast<uint32_t>(d.size());

  uint32_t start = off[index];
  uint32_t end = static_cast<uint32_t>(d.size());
  for (int i = 0; i <= kPbpSectionCount; ++i)
    if (off[i] > start && off[i] < end) end = off[i];
  if (start > d.size()) throw Error("PBP section out of range");
  return Bytes(d.begin() + start, d.begin() + end);
}

}  // namespace mgspo

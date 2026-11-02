#include "sfo.h"

#include <cstring>

namespace mgspo {

Sfo Sfo::parse(const Bytes& d) {
  if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) throw Error("not a PARAM.SFO file");
  uint32_t keyTable = rd32(&d[8]);
  uint32_t dataTable = rd32(&d[12]);
  uint32_t count = rd32(&d[16]);
  if (count > 1024 || 20 + static_cast<uint64_t>(count) * 16 > d.size()) throw Error("bad SFO entry count");

  Sfo out;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = &d[20 + i * 16];
    uint16_t keyOff = rd16(e);
    uint16_t fmt = rd16(e + 2);
    uint32_t len = rd32(e + 4);
    uint32_t dataOff = rd32(e + 12);

    uint64_t kp = static_cast<uint64_t>(keyTable) + keyOff;
    if (kp >= d.size()) throw Error("SFO key out of range");
    SfoValue v;
    for (uint64_t p = kp; p < d.size() && d[p]; ++p) v.key.push_back(static_cast<char>(d[p]));

    uint64_t dp = static_cast<uint64_t>(dataTable) + dataOff;
    if (dp + len > d.size()) throw Error("SFO data out of range");
    if (fmt == 0x0404) {
      if (len < 4) throw Error("bad SFO integer");
      v.isInt = true;
      v.num = rd32(&d[dp]);
    } else {
      v.str.assign(reinterpret_cast<const char*>(&d[dp]), len);
      size_t z = v.str.find('\0');
      if (z != std::string::npos) v.str.resize(z);
    }
    out.values_.push_back(std::move(v));
  }
  return out;
}

const SfoValue* Sfo::get(const std::string& key) const {
  for (const auto& v : values_)
    if (v.key == key) return &v;
  return nullptr;
}

std::string Sfo::str(const std::string& key) const {
  const SfoValue* v = get(key);
  if (!v) return "";
  return v->isInt ? std::to_string(v->num) : v->str;
}

}  // namespace mgspo

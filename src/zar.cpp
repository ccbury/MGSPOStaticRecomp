#include "zar.h"

#include <cstring>

namespace mgspo {

namespace {

struct BitReader {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;
  uint32_t bits = 0;
  int count = 0;
  bool bad = false;

  uint32_t get(int need) {
    while (count < need) {
      if (pos >= n) {
        bad = true;
        return 0;
      }
      bits |= static_cast<uint32_t>(p[pos++]) << count;
      count += 8;
    }
    uint32_t v = bits & ((1u << need) - 1);
    bits >>= need;
    count -= need;
    return v;
  }
};

struct Huffman {
  uint16_t counts[16] = {};
  uint16_t symbols[288] = {};

  void build(const uint8_t* lengths, int n) {
    std::memset(counts, 0, sizeof counts);
    for (int i = 0; i < n; ++i) ++counts[lengths[i]];
    counts[0] = 0;
    uint16_t offs[16] = {};
    for (int i = 1; i < 16; ++i) offs[i] = static_cast<uint16_t>(offs[i - 1] + counts[i - 1]);
    for (int i = 0; i < n; ++i)
      if (lengths[i]) symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
  }

  int decode(BitReader& br) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= static_cast<int>(br.get(1));
      if (br.bad) return -1;
      int c = counts[len];
      if (code - c < first) return symbols[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    return -1;
  }
};

const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& br, Bytes& out, const Huffman& lit, const Huffman& dist) {
  for (;;) {
    int sym = lit.decode(br);
    if (sym < 0) return false;
    if (sym < 256) {
      out.push_back(static_cast<uint8_t>(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      size_t len = kLenBase[sym] + br.get(kLenExtra[sym]);
      int ds = dist.decode(br);
      if (ds < 0 || ds >= 30) return false;
      size_t d = kDistBase[ds] + br.get(kDistExtra[ds]);
      if (br.bad || d > out.size()) return false;
      for (size_t i = 0; i < len; ++i) out.push_back(out[out.size() - d]);
    }
  }
}

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

}  // namespace

bool zlibInflate(const uint8_t* data, size_t size, Bytes& out) {
  out.clear();
  if (size < 2 || (data[0] & 0x0F) != 8) return false;
  BitReader br{data + 2, size - 2};
  bool last = false;
  while (!last) {
    last = br.get(1) != 0;
    uint32_t type = br.get(2);
    if (br.bad) return false;
    if (type == 0) {
      br.bits = 0;
      br.count = 0;
      if (br.pos + 4 > br.n) return false;
      uint32_t len = br.p[br.pos] | (br.p[br.pos + 1] << 8);
      br.pos += 4;
      if (br.pos + len > br.n) return false;
      out.insert(out.end(), br.p + br.pos, br.p + br.pos + len);
      br.pos += len;
    } else if (type == 1) {
      uint8_t l[288];
      for (int i = 0; i < 144; ++i) l[i] = 8;
      for (int i = 144; i < 256; ++i) l[i] = 9;
      for (int i = 256; i < 280; ++i) l[i] = 7;
      for (int i = 280; i < 288; ++i) l[i] = 8;
      Huffman lit, dist;
      lit.build(l, 288);
      uint8_t dl[30];
      for (auto& v : dl) v = 5;
      dist.build(dl, 30);
      if (!inflateBlock(br, out, lit, dist)) return false;
    } else if (type == 2) {
      int nlen = static_cast<int>(br.get(5)) + 257, ndist = static_cast<int>(br.get(5)) + 1, ncode = static_cast<int>(br.get(4)) + 4;
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(br.get(3));
      Huffman codes;
      codes.build(lengths, 19);
      uint8_t all[320] = {};
      int i = 0;
      while (i < nlen + ndist) {
        int s = codes.decode(br);
        if (s < 0) return false;
        if (s < 16) {
          all[i++] = static_cast<uint8_t>(s);
        } else {
          uint8_t prev = 0;
          int rep;
          if (s == 16) {
            if (i == 0) return false;
            prev = all[i - 1];
            rep = 3 + static_cast<int>(br.get(2));
          } else if (s == 17) {
            rep = 3 + static_cast<int>(br.get(3));
          } else {
            rep = 11 + static_cast<int>(br.get(7));
          }
          if (i + rep > nlen + ndist) return false;
          while (rep--) all[i++] = prev;
        }
      }
      Huffman lit, dist;
      lit.build(all, nlen);
      dist.build(all + nlen, ndist);
      if (!inflateBlock(br, out, lit, dist)) return false;
    } else {
      return false;
    }
    if (br.bad) return false;
  }
  return true;
}

Bytes findLinkTable(const Bytes& zar) {
  Bytes archive;
  if (zar.size() < 8 || !zlibInflate(zar.data() + 4, zar.size() - 4, archive)) return {};
  static const char kExt[] = ".rlc";
  for (size_t i = 0; i + 4 < archive.size(); ++i) {
    if (std::memcmp(&archive[i], kExt, 5) != 0) continue;  // includes the terminating NUL
    // The member header has a few bytes of padding and a size whose layout varies, so look for the
    // size word and a start of the table where every entry has a sensible relocation type.
    for (size_t p = i + 5; p < i + 14 && p + 4 <= archive.size(); ++p) {
      uint32_t len = le32(&archive[p]);
      if (len == 0 || len % 8) continue;
      for (size_t s = p + 4; s <= p + 16 && s + len <= archive.size(); ++s) {
        bool ok = true;
        for (size_t e = 0; e < len && ok; e += 8) {
          uint32_t info = le32(&archive[s + e + 4]);
          uint32_t type = info & 0xFF;
          ok = (type == 2 || type == 4 || type == 5 || type == 6) && ((info >> 8) & 0xFF) < 4 && ((info >> 16) & 0xFF) < 4;
        }
        if (ok) return Bytes(archive.begin() + s, archive.begin() + s + len);
      }
    }
    return {};
  }
  return {};
}

void applyLinkTable(PrxImage& img, const Bytes& table, const std::vector<uint32_t>& mainSegBases,
                    std::vector<uint32_t>* targets) {
  const ElfInfo& info = img.info;
  auto ofsBase = [&](uint32_t idx) { return img.base + (idx < info.segments.size() ? info.segments[idx].vaddr : 0u); };
  auto valBase = [&](uint32_t idx) { return idx < mainSegBases.size() ? mainSegBases[idx] : 0u; };
  auto rd = [&](uint32_t a) { return img.contains(a) ? img.word(a) : 0u; };
  auto wr = [&](uint32_t a, uint32_t v) {
    if (!img.contains(a)) return;
    for (int i = 0; i < 4; ++i) img.mem[a - img.base + i] = static_cast<uint8_t>(v >> (8 * i));
  };

  size_t n = table.size() / 8;
  for (size_t e = 0; e < n; ++e) {
    uint32_t off = le32(&table[e * 8]);
    uint32_t inf = le32(&table[e * 8 + 4]);
    uint32_t type = inf & 0xFF;
    uint32_t base = ofsBase((inf >> 8) & 0xFF);
    uint32_t val = valBase((inf >> 16) & 0xFF);
    uint32_t addr = base + off;
    switch (type) {
      case 2:
        wr(addr, rd(addr) + val);
        if (targets) targets->push_back(rd(addr));
        break;
      case 4: {
        uint32_t w = rd(addr);
        uint32_t t = w << 2;
        if (t & (1u << 27)) t |= 0xF0000000u;
        wr(addr, (w & ~0x03FFFFFFu) | (((t + val) >> 2) & 0x03FFFFFFu));
        if (targets) targets->push_back(((t + val) & 0x0FFFFFFFu) | (val & 0xF0000000u));
        break;
      }
      case 5: {
        // the matching low half is the next LO16 entry
        size_t k = e + 1;
        while (k < n && (le32(&table[k * 8 + 4]) & 0xFF) != 6) ++k;
        if (k >= n) break;
        uint32_t lowAddr = base + le32(&table[k * 8]);
        uint32_t w = rd(addr);
        int32_t lo = static_cast<int16_t>(rd(lowAddr) & 0xFFFF);
        uint32_t full = (w << 16) + static_cast<uint32_t>(lo) + val;
        uint32_t hi = (full - static_cast<uint32_t>(static_cast<int16_t>(full))) >> 16;
        wr(addr, (w & ~0xFFFFu) | hi);
        if (targets) targets->push_back(full);
        break;
      }
      case 6: wr(addr, (rd(addr) & ~0xFFFFu) | (((rd(addr) & 0xFFFF) + val) & 0xFFFF)); break;
      default: break;
    }
  }
}

}  // namespace mgspo

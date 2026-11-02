// Self-contained tests built on synthetic data; no game files are needed.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "bootfile.h"
#include "elf.h"
#include "iso.h"
#include "sfo.h"

using namespace mgspo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

static void put16(Bytes& b, size_t o, uint16_t v) {
  b[o] = v & 0xFF;
  b[o + 1] = v >> 8;
}
static void put32(Bytes& b, size_t o, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + i] = (v >> (8 * i)) & 0xFF;
}

// ---- synthetic ISO9660 ----------------------------------------------------

struct Node {
  std::string name;
  bool dir = false;
  Bytes data;
  std::vector<Node> kids;
  uint32_t lba = 0;
};

static void alloc(Node& n, uint32_t& next) {
  n.lba = next;
  if (n.dir) {
    next += 1;
    for (auto& k : n.kids) alloc(k, next);
  } else {
    next += static_cast<uint32_t>(std::max<size_t>(1, (n.data.size() + 2047) / 2048));
  }
}

static Bytes record(const std::string& name, uint32_t lba, uint32_t size, bool dir) {
  size_t len = 33 + name.size();
  if (len % 2) ++len;
  Bytes r(len, 0);
  r[0] = static_cast<uint8_t>(len);
  put32(r, 2, lba);
  put32(r, 10, size);
  r[25] = dir ? 2 : 0;
  r[32] = static_cast<uint8_t>(name.size());
  std::memcpy(&r[33], name.data(), name.size());
  return r;
}

static void emit(const Node& n, uint32_t parentLba, Bytes& img) {
  if (!n.dir) {
    if (!n.data.empty()) std::memcpy(&img[n.lba * 2048], n.data.data(), n.data.size());
    return;
  }
  Bytes d;
  auto add = [&](const Bytes& r) { d.insert(d.end(), r.begin(), r.end()); };
  add(record(std::string(1, '\0'), n.lba, 2048, true));
  add(record(std::string(1, '\1'), parentLba, 2048, true));
  for (const auto& k : n.kids) {
    uint32_t sz = k.dir ? 2048 : static_cast<uint32_t>(k.data.size());
    add(record(k.dir ? k.name : k.name + ";1", k.lba, sz, k.dir));
  }
  std::memcpy(&img[n.lba * 2048], d.data(), d.size());
  for (const auto& k : n.kids) emit(k, n.lba, img);
}

static Bytes buildIso(Node& root) {
  root.dir = true;
  uint32_t next = 18;
  alloc(root, next);
  Bytes img(static_cast<size_t>(next) * 2048, 0);
  Bytes pvd(2048, 0);
  pvd[0] = 1;
  std::memcpy(&pvd[1], "CD001", 5);
  pvd[6] = 1;
  std::memset(&pvd[40], ' ', 32);
  std::memcpy(&pvd[40], "TESTDISC", 8);
  Bytes rr = record(std::string(1, '\0'), root.lba, 2048, true);
  std::memcpy(&pvd[156], rr.data(), rr.size());
  std::memcpy(&img[16 * 2048], pvd.data(), pvd.size());
  img[17 * 2048] = 255;
  std::memcpy(&img[17 * 2048 + 1], "CD001", 5);
  emit(root, root.lba, img);
  return img;
}

// ---- synthetic SFO / ELF ---------------------------------------------------

static Bytes buildSfo(const std::map<std::string, std::string>& strs, const std::map<std::string, uint32_t>& ints) {
  size_t count = strs.size() + ints.size();
  Bytes keys, data;
  std::vector<uint8_t> index;
  Bytes idx(count * 16, 0);
  size_t i = 0;
  auto addKey = [&](const std::string& k) {
    uint16_t off = static_cast<uint16_t>(keys.size());
    keys.insert(keys.end(), k.begin(), k.end());
    keys.push_back(0);
    return off;
  };
  for (const auto& kv : strs) {
    put16(idx, i * 16, addKey(kv.first));
    put16(idx, i * 16 + 2, 0x0204);
    put32(idx, i * 16 + 4, static_cast<uint32_t>(kv.second.size() + 1));
    put32(idx, i * 16 + 8, static_cast<uint32_t>(kv.second.size() + 1));
    put32(idx, i * 16 + 12, static_cast<uint32_t>(data.size()));
    data.insert(data.end(), kv.second.begin(), kv.second.end());
    data.push_back(0);
    ++i;
  }
  for (const auto& kv : ints) {
    put16(idx, i * 16, addKey(kv.first));
    put16(idx, i * 16 + 2, 0x0404);
    put32(idx, i * 16 + 4, 4);
    put32(idx, i * 16 + 8, 4);
    put32(idx, i * 16 + 12, static_cast<uint32_t>(data.size()));
    Bytes v(4);
    put32(v, 0, kv.second);
    data.insert(data.end(), v.begin(), v.end());
    ++i;
  }
  Bytes out(20, 0);
  std::memcpy(out.data(), "\0PSF", 4);
  put32(out, 4, 0x101);
  put32(out, 8, static_cast<uint32_t>(20 + idx.size()));
  put32(out, 12, static_cast<uint32_t>(20 + idx.size() + keys.size()));
  put32(out, 16, static_cast<uint32_t>(count));
  out.insert(out.end(), idx.begin(), idx.end());
  out.insert(out.end(), keys.begin(), keys.end());
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

// Tiny PRX with one import library ("TestLib", two functions).
static Bytes buildPrx() {
  Bytes b(0x200, 0);
  std::memcpy(b.data(), "\x7f" "ELF", 4);
  b[4] = 1;
  b[5] = 1;
  b[6] = 1;
  put16(b, 16, 0xFFA0);
  put16(b, 18, 8);
  put32(b, 24, 0x40);
  put32(b, 28, 52);
  put16(b, 40, 52);
  put16(b, 42, 32);
  put16(b, 44, 1);
  put32(b, 52 + 0, 1);       // PT_LOAD
  put32(b, 52 + 4, 0);       // offset
  put32(b, 52 + 8, 0);       // vaddr
  put32(b, 52 + 12, 0x80);   // paddr -> module info offset
  put32(b, 52 + 16, 0x200);  // filesz
  put32(b, 52 + 20, 0x300);  // memsz
  put32(b, 52 + 24, 5);
  // module info at 0x80
  put16(b, 0x80, 0x0000);
  put16(b, 0x82, 0x0101);
  std::memcpy(&b[0x84], "TESTMOD", 7);
  put32(b, 0x80 + 32, 0x1000);  // gp
  put32(b, 0x80 + 36, 0x180);   // ent top
  put32(b, 0x80 + 40, 0x190);   // ent bottom
  put32(b, 0x80 + 44, 0x100);   // stub top
  put32(b, 0x80 + 48, 0x114);   // stub bottom
  // import entry at 0x100
  put32(b, 0x100, 0x120);  // name ptr
  put16(b, 0x104, 0x0101);
  put16(b, 0x106, 0x0011);
  b[0x108] = 5;
  b[0x109] = 0;
  put16(b, 0x10A, 2);
  put32(b, 0x10C, 0x140);  // nid table
  put32(b, 0x110, 0x150);  // stub table
  std::memcpy(&b[0x120], "TestLib", 7);
  put32(b, 0x140, 0xAABBCCDD);
  put32(b, 0x144, 0x11223344);
  // export entry at 0x180 (module's own)
  put16(b, 0x184, 0);
  put16(b, 0x186, 0x8000);
  b[0x188] = 4;
  b[0x189] = 2;
  put16(b, 0x18A, 3);
  return b;
}

// ---- tests ------------------------------------------------------------------

static void testSfo() {
  Sfo sfo = Sfo::parse(buildSfo({{"DISC_ID", "ULUS00000"}, {"TITLE", "Sample"}}, {{"PARENTAL_LEVEL", 3}}));
  CHECK(sfo.str("DISC_ID") == "ULUS00000");
  CHECK(sfo.str("TITLE") == "Sample");
  CHECK(sfo.get("PARENTAL_LEVEL") && sfo.get("PARENTAL_LEVEL")->num == 3);
  CHECK(sfo.str("MISSING").empty());

  bool threw = false;
  try {
    Sfo::parse(Bytes(8, 0));
  } catch (const Error&) {
    threw = true;
  }
  CHECK(threw);
}

static void testIso() {
  Node root;
  Node game{"PSP_GAME", true, {}, {}, 0};
  Node sysdir{"SYSDIR", true, {}, {}, 0};
  sysdir.kids.push_back({"EBOOT.BIN", false, Bytes(5000, 0xAB), {}, 0});
  game.kids.push_back({"PARAM.SFO", false, buildSfo({{"DISC_ID", "ULUS00000"}}, {}), {}, 0});
  game.kids.push_back(sysdir);
  root.kids.push_back(game);
  root.kids.push_back({"UMD_DATA.BIN", false, Bytes{'U', 'L'}, {}, 0});
  Bytes img = buildIso(root);

  fs::path tmp = fs::temp_directory_path() / "mgspo_test_iso";
  fs::create_directories(tmp);
  fs::path isoPath = tmp / "t.iso";
  writeFile(isoPath, img);

  {
  IsoImage iso(isoPath.string());
  CHECK(iso.volumeId() == "TESTDISC");
  const IsoEntry* eb = iso.find("psp_game/sysdir/eboot.bin");
  CHECK(eb != nullptr);
  if (eb) {
    CHECK(eb->size == 5000);
    Bytes b = iso.read(*eb);
    CHECK(b.size() == 5000 && b[0] == 0xAB && b[4999] == 0xAB);
    iso.extract(*eb, tmp / "out" / "EBOOT.BIN");
    CHECK(fs::file_size(tmp / "out" / "EBOOT.BIN") == 5000);
  }
  const IsoEntry* p = iso.find("/PSP_GAME/PARAM.SFO");
  CHECK(p != nullptr);
  if (p) CHECK(Sfo::parse(iso.read(*p)).str("DISC_ID") == "ULUS00000");
  CHECK(iso.find("nope") == nullptr);
  }

  writeFile(tmp / "junk.iso", Bytes(40000, 0));
  bool threw = false;
  try {
    IsoImage bad((tmp / "junk.iso").string());
  } catch (const Error&) {
    threw = true;
  }
  CHECK(threw);
  fs::remove_all(tmp);
}

static void testElf() {
  Bytes prx = buildPrx();
  CHECK(classifyBoot(prx) == BootKind::Elf);
  ElfInfo e = parseElf(prx);
  CHECK(e.isPrx);
  CHECK(e.machine == 8);
  CHECK(e.entry == 0x40);
  CHECK(e.module.present && e.module.name == "TESTMOD");
  CHECK(e.imports.size() == 1);
  if (e.imports.size() == 1) {
    CHECK(e.imports[0].name == "TestLib");
    CHECK(e.imports[0].funcs.size() == 2);
    CHECK(e.imports[0].funcs[0].nid == 0xAABBCCDD);
    CHECK(e.imports[0].funcs[1].nid == 0x11223344);
    CHECK(e.imports[0].funcs[1].stubAddr == 0x158);
  }
  CHECK(e.exports.size() == 1 && e.exports[0].funcCount == 3 && e.exports[0].varCount == 2);

  bool threw = false;
  try {
    parseElf(Bytes(16, 0));
  } catch (const Error&) {
    threw = true;
  }
  CHECK(threw);
}

static void testBootfile() {
  Bytes psp(0x200, 0);
  std::memcpy(psp.data(), "~PSP", 4);
  put16(psp, 4, 0x1006);
  std::memcpy(&psp[0x0A], "mymod", 5);
  psp[0x27] = 2;
  put32(psp, 0x28, 1234);
  CHECK(classifyBoot(psp) == BootKind::EncryptedPsp);
  PspHeader h = parsePspHeader(psp);
  CHECK(h.moduleName == "mymod" && h.segmentCount == 2 && h.elfSize == 1234 && h.attribute == 0x1006);

  Bytes pbp(0x30 + 10, 0);
  std::memcpy(pbp.data(), "\0PBP", 4);
  for (int i = 0; i < 8; ++i) put32(pbp, 8 + i * 4, i < 6 ? 0x28 : 0x30);
  put32(pbp, 8 + 7 * 4, 0x34);
  for (int i = 0; i < 10; ++i) pbp[0x30 + i] = static_cast<uint8_t>(i);
  CHECK(classifyBoot(pbp) == BootKind::Pbp);
  Bytes data = pbpSection(pbp, 6);
  CHECK(data.size() == 4 && data[0] == 0);
  CHECK(pbpSection(pbp, 7).size() == pbp.size() - 0x34);
}

int main() {
  try {
    testSfo();
    testIso();
    testElf();
    testBootfile();
  } catch (const std::exception& ex) {
    std::printf("unexpected exception: %s\n", ex.what());
    return 1;
  }
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("all tests passed");
  return 0;
}

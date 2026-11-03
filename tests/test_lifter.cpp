// Tests for the decoder, function discovery, emitter output and runtime helpers.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <psp/runtime.h>

#include "emit.h"
#include "functions.h"
#include "mips.h"
#include "prx.h"

using namespace mgspo;

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

static void testDecode() {
  Insn j = decode(0x03E00008, 0x1000);
  CHECK(j.op == Op::Jr && j.rs == 31 && hasDelaySlot(j.op));

  Insn b = decode(0x14800002, 0x1000);  // bne a0, zero, +2
  CHECK(b.op == Op::Bne && b.target == 0x1000 + 4 + 8);
  Insn back = decode(0x1000FFFF, 0x2000);  // b -1
  CHECK(back.op == Op::Beq && back.target == 0x2000);

  Insn jal = decode(0x0C000100, 0x08804000);
  CHECK(jal.op == Op::Jal && jal.target == 0x400);

  CHECK(decode(0x24420001, 0).op == Op::Addiu);
  CHECK(decode(0x24420001, 0).imm == 1);
  CHECK(decode(0x2442FFFF, 0).imm == -1);
  CHECK(decode(0x7C000000 | (7 << 11) | (3 << 6), 0).op == Op::Ext);
  CHECK(isLikely(Op::Beql) && !isLikely(Op::Beq));
  CHECK(decode(0x00000000, 0).op == Op::Sll);
}

// Builds an image by hand: two functions and a nop pad.
static PrxImage makeImage() {
  PrxImage img;
  img.base = 0x08804000;
  img.mem.assign(0x40, 0);
  auto put = [&](uint32_t off, uint32_t v) { std::memcpy(&img.mem[off], &v, 4); };
  put(0x00, 0x0C000000 | ((img.base + 0x20) >> 2 & 0x03FFFFFF));  // jal B
  put(0x04, 0);
  put(0x08, 0x14800002);  // bne a0, zero, +2 -> 0x14
  put(0x0C, 0x24420001);  // addiu v0, v0, 1
  put(0x10, 0x24420002);
  put(0x14, 0x03E00008);  // jr ra
  put(0x18, 0);
  put(0x20, 0x03E00008);  // B: jr ra
  put(0x24, 0x24020007);  // addiu v0, zero, 7
  img.entry = img.base;
  img.code.push_back({img.base, img.base + 0x28, ".text"});
  return img;
}

static void testDiscovery() {
  PrxImage img = makeImage();
  Program p = discover(img);
  CHECK(p.funcs.size() == 2);
  CHECK(p.find(img.base) != nullptr);
  const Function* b = p.find(img.base + 0x20);
  CHECK(b && b->end == img.base + 0x28);
  const Function* a = p.find(img.base);
  CHECK(a && a->end == img.base + 0x20);
  CHECK(p.containing(img.base + 0x10) == a);
  CHECK(p.stats.callTargets == 1);
}

static void testEmit() {
  PrxImage img = makeImage();
  Program p = discover(img);
  size_t unimpl = 0;
  std::string a = emitFunction(p, *p.find(img.base), &unimpl);
  CHECK(unimpl == 0);
  CHECK(a.find("void f_08804000(psp::Cpu& c)") != std::string::npos);
  CHECK(a.find("f_08804020(c);") != std::string::npos);
  CHECK(a.find("goto L_08804014;") != std::string::npos);
  CHECK(a.find("L_08804014:;") != std::string::npos);
  CHECK(a.find("return;") != std::string::npos);
  // The delay slot of the conditional branch runs on both paths.
  size_t first = a.find("+ 0x00000001u");
  CHECK(first != std::string::npos && a.find("+ 0x00000001u", first + 1) != std::string::npos);

  std::string b = emitFunction(p, *p.find(img.base + 0x20));
  CHECK(b.find("c.r[2] = 0u + 0x00000007u;") != std::string::npos);
}

static void testPromptHooks() {
  PrxImage img;
  img.base = 0x0884C000;
  img.mem.resize(0x500);
  auto put = [&](uint32_t a, uint32_t w) { std::memcpy(&img.mem[a - img.base], &w, 4); };
  for (auto signature : {std::pair<uint32_t, uint32_t>{0x0884C0F8, 0x27BDFFD0},
                        {0x0884C330, 0x8E621C48}, {0x0884C334, 0x50500009},
                        {0x0884C350, 0x02004021}, {0x0884C438, 0x8E621C48},
                        {0x0884C43C, 0x1050FFC6}})
    put(signature.first, signature.second);
  Program p;
  p.img = &img;
  for (uint32_t a : {0x0884C330u, 0x0884C438u}) {
    auto emitted = emitFunction(p, {a, a + 4});
    CHECK(emitted.find("prompts_texture(c.r[17], c.r[16])") < emitted.find("psp::lw"));
    CHECK(emitFunction(p, {a, a + 4}, nullptr, true).find("prompts_texture") == std::string::npos);
  }
  put(0x0884C0F8, 0);
  CHECK(emitFunction(p, {0x0884C330, 0x0884C334}).find("prompts_texture") == std::string::npos);
}

static void testControlHooks() {
  PrxImage img;
  img.base = 0x08804000;
  img.mem.resize(0x00111000);
  auto put = [&](uint32_t address, uint32_t word) {
    std::memcpy(&img.mem[address - img.base], &word, 4);
  };
  for (auto signature : {std::pair<uint32_t, uint32_t>{0x0884072C, 0x27BDFFF0},
                        {0x08840738, 0x00808021}, {0x0889814C, 0x27BDFFE0},
                        {0x08898158, 0x2623CB64}, {0x0889742C, 0x27BDFFF0},
                        {0x089142E0, 0x27BDFFF0}, {0x089142EC, 0x240500EE},
                        {0x088B52F8, 0x27BDFFC0}})
    put(signature.first, signature.second);
  Program p;
  p.img = &img;
  CHECK(emitFunction(p, {0x0884072C, 0x08840730}).find("controls_guest_sample") != std::string::npos);
  CHECK(emitFunction(p, {0x0889814C, 0x08898150}).find("controls_equipment") != std::string::npos);
  for (auto entry : {std::pair<uint32_t, const char*>{0x08897440, "Equipment"},
                     {0x089144C0, "Camera"}, {0x088B5BD0, "Gameplay"}}) {
    put(entry.first, 0x8C820000);  // lw v0, 0(a0)
    std::string body = emitFunction(p, {entry.first, entry.first + 4});
    CHECK(body.find(std::string("ControlConsumer::") + entry.second) != std::string::npos);
    CHECK(emitFunction(p, {entry.first, entry.first + 4}, nullptr, true).find("controls_") == std::string::npos);
  }
  put(0x08850000, 0x8C820000);
  CHECK(emitFunction(p, {0x08850000, 0x08850004}).find("controls_") == std::string::npos);
  for (uint32_t end : {0x088975CCu, 0x0889875Cu, 0x08914518u, 0x088B5BFCu}) {
    put(end, 0x8C820000);
    CHECK(emitFunction(p, {end, end + 4}).find("controls_") == std::string::npos);
  }
  put(0x0884072C, 0);
  CHECK(emitFunction(p, {0x0889814C, 0x08898150}).find("controls_") == std::string::npos);
  CHECK(emitFunction(p, {0x089144C0, 0x089144C4}).find("controls_") == std::string::npos);
}

static void testOps() {
  psp::Cpu c;
  psp::div(c, 7, 0);
  CHECK(c.lo == 0xFFFFFFFFu && c.hi == 7);
  psp::div(c, static_cast<uint32_t>(-7), 2);
  CHECK(static_cast<int32_t>(c.lo) == -3 && static_cast<int32_t>(c.hi) == -1);
  psp::multu(c, 0xFFFFFFFFu, 2);
  CHECK(c.hi == 1 && c.lo == 0xFFFFFFFEu);
  psp::madd(c, 3, 4);
  CHECK(c.hi == 2 && c.lo == 10);  // carries into hi
  CHECK(psp::ext(0xABCD1234, 8, 8) == 0x12);
  CHECK(psp::ins(0xFFFFFFFF, 0, 4, 8) == 0xFFFFF00Fu);
  CHECK(psp::clz(1) == 31 && psp::clz(0) == 32 && psp::clo(0xFFFF0000u) == 16);
  CHECK(psp::wsbh(0x11223344) == 0x22114433u);
  CHECK(psp::wsbw(0x11223344) == 0x44332211u);
  CHECK(psp::seb(0x80) == 0xFFFFFF80u && psp::seh(0x8000) == 0xFFFF8000u);
  CHECK(psp::bitrev(1) == 0x80000000u);
  CHECK(psp::rotr(0x12345678, 8) == 0x78123456u);
  CHECK(psp::trunc_w(1e20f) == 0x7FFFFFFFu && psp::trunc_w(-2.9f) == static_cast<uint32_t>(-2));
  CHECK(psp::fcmp(1.f, 2.f, 4) && !psp::fcmp(2.f, 1.f, 4) && psp::fcmp(1.f, 1.f, 2));
}

static void testMemory() {
  psp::init_memory();
  psp::sw(0x08800000, 0x11223344);
  CHECK(psp::lbu(0x08800000) == 0x44);
  CHECK(psp::lw(0x48800000) == 0x11223344);  // uncached mirror
  CHECK(psp::lw(0x88800000) == 0x11223344);  // kernel mirror
  // unaligned read of 0x08800001..4 via lwl/lwr
  psp::sw(0x08800004, 0x88776655);
  uint32_t v = psp::lwl(0x08800004, psp::lwr(0x08800001, 0));
  CHECK(v == 0x55112233u);
  psp::sw(0x04000000, 0xCAFEBABE);  // VRAM
  CHECK(psp::lw(0x04000000) == 0xCAFEBABE);
  bool threw = false;
  try {
    psp::lw(0x20000000);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  psp::free_memory();
}

static int g_hleHits = 0;
static void fakeHle(psp::Cpu& c) {
  ++g_hleHits;
  c.r[2] = 42;
}
static void fakeFn(psp::Cpu& c) { c.r[2] = 99; }

static void testDispatch() {
  psp::register_hle("TestLib", 0xAABBCCDD, fakeHle);
  static const psp::FuncEntry funcs[] = {{0x08804000, fakeFn}};
  static const psp::ImportEntry imports[] = {{0x08805000, "TestLib", 0xAABBCCDD},
                                             {0x08805008, "TestLib", 0x11111111}};
  psp::bind_tables(funcs, 1, imports, 2);
  psp::Cpu c;
  psp::hle(c, 0);
  CHECK(g_hleHits == 1 && c.r[2] == 42);
  psp::call(c, 0x08804000);
  CHECK(c.r[2] == 99);
  psp::call(c, 0x08805000);
  CHECK(g_hleHits == 2);
  psp::hle(c, 1);  // unregistered: returns zero and is counted
  CHECK(c.r[2] == 0 && psp::unresolved_calls() == 1);
  bool threw = false;
  try {
    psp::call(c, 0x08809999);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

// Two modules emitted together: import indices stay unique and the module table lists both exports.
static void testMultiModule() {
  PrxImage a = makeImage();
  PrxImage b = makeImage();
  b.base = 0x08900000;
  b.entry = b.base;
  b.code[0] = {b.base, b.base + 0x28, ".text"};
  a.info.module.name = "main";
  b.info.module.name = "helper";
  a.exports.push_back({"", kNidModuleStart, a.base});
  b.exports.push_back({"lib", 0x11223344, b.base + 0x20});
  b.exportedFuncs.push_back(b.base + 0x20);
  b.info.imports.push_back({"other", 0, 0, 0, {{0xAABBCCDD, 0x30}}});

  Program pa = discover(a);
  Program pb = discover(b, 5);
  CHECK(pb.imports.size() == 1 && pb.imports[0].index == 5);
  CHECK(pb.stubIndex.count(b.base + 0x30) == 1);

  std::string dir = "mgspo_multi_test";
  EmitOptions opts;
  opts.outDir = dir;
  EmitResult r = emitAll({{&pa, "main", ""}, {&pb, "helper", "helper.prx"}}, opts);
  CHECK(r.functions == 4);

  auto slurp = [&](const char* f) {
    std::FILE* fp = std::fopen((dir + "/" + f).c_str(), "rb");
    std::string s;
    if (!fp) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, n);
    std::fclose(fp);
    return s;
  };
  std::string t = slurp("tables.cpp");
  CHECK(t.find("{\"lib\", 0x11223344u, 0x08900020u}") != std::string::npos);
  CHECK(t.find("\"helper.prx\", 0x08900000u") != std::string::npos);
  CHECK(t.find("0x08900030u, \"other\", 0xAABBCCDDu") != std::string::npos);
  CHECK(slurp("tables.h").find("kModuleCount") != std::string::npos);
  std::filesystem::remove_all(dir);
}

int main() {
  testDecode();
  testDiscovery();
  testMultiModule();
  testEmit();
  testControlHooks();
  testPromptHooks();
  testOps();
  testMemory();
  testDispatch();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("all lifter tests passed");
  return 0;
}

// mgspo-lift: turn a PSP PRX/ELF into C++ sources for the runtime to link against.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#include "bytes.h"
#include "emit.h"
#include "functions.h"
#include "mips.h"
#include "prx.h"
#include "zar.h"
#include "vfpu.h"
#include "../stage_inventory.h"

using namespace mgspo;

static void usage() {
  std::cerr << "usage: mgspo-lift <module.elf> <outdir> [--base 0xADDR] [--per-file N] [--helper <module.prx>]... [--helper-dir <dir>]\n"
               "                                    [--stage <module.prx>]... [--stage-dir <dir>]\n"
               "       mgspo-lift <module.elf> --stats [--base 0xADDR]\n"
               "       mgspo-lift <module.elf> --vfpu\n"
               "       mgspo-lift <module.elf> --disasm <start> <count>\n";
}

static void printStats(const Program& p, const PrxImage& img) {
  const DiscoveryStats& s = p.stats;
  uint64_t covered = 0, total = 0;
  for (const auto& f : p.funcs) covered += (f.end - f.start) / 4;
  for (const auto& r : img.code) total += (r.end - r.start) / 4;
  std::printf("module base       0x%08X\n", img.base);
  std::printf("entry             0x%08X\n", img.entry);
  std::printf("gp                0x%08X\n", img.gp);
  std::printf("imports           %zu\n", p.imports.size());
  std::printf("functions         %zu (call targets %u, jump roots %u, pointer roots %u)\n", p.funcs.size(),
              s.callTargets, s.jumpRoots, s.pointerRoots);
  std::printf("code words        %llu (covered by functions %llu)\n", static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(covered));
  std::printf("vfpu insns        %u (undecoded %u, cc branches %u)\n", s.vfpuInsns, s.vfpuUndecoded, s.vfpuBranches);
  std::printf("cop0 insns        %u\n", s.cop0Insns);
  std::printf("unknown insns     %u\n", s.unknownInsns);
  std::printf("calls outside code %u\n", s.callsOutsideCode);
  std::printf("switch targets    %zu\n", img.jumpTargets.size());
  std::printf("unhandled relocs  %u\n", img.unknownRelocs);
}

static void printVfpuHistogram(const PrxImage& img) {
  std::map<std::string, unsigned> hist;
  std::map<uint32_t, unsigned> bad;
  for (const auto& r : img.code)
    for (uint32_t a = r.start; a + 4 <= r.end; a += 4) {
      Insn i = decode(img.word(a), a);
      if (i.op != Op::Vfpu) continue;
      psp::vfpu::Op v = decodeVfpu(i.raw);
      if (v == psp::vfpu::Op::Invalid) ++bad[i.raw];
      else ++hist[psp::vfpu::op_name(v)];
    }
  for (const auto& h : hist) std::printf("%-8s %u\n", h.first.c_str(), h.second);
  for (const auto& b : bad) std::printf("undecoded %08X x%u\n", b.first, b.second);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  try {
    std::string in = argv[1];
    std::string out;
    uint32_t base = kDefaultBase;
    size_t perFile = 150;
    bool stats = false, disasm = false, vfpuHist = false;
    std::vector<std::string> helpers, stages;
    uint32_t dStart = 0, dCount = 0;
    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--base" && i + 1 < argc) base = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
      else if (a == "--per-file" && i + 1 < argc) perFile = std::stoul(argv[++i]);
      else if (a == "--stats") stats = true;
      else if (a == "--helper" && i + 1 < argc) helpers.push_back(argv[++i]);
      else if (a == "--helper-dir" && i + 1 < argc) {
        // every plain PRX in the folder; encrypted (~SCE/~PSP) files are skipped
        std::vector<std::string> found;
        for (const auto& e : fs::directory_iterator(argv[++i])) {
          if (!e.is_regular_file()) continue;
          std::string ext = e.path().extension().string();
          for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          if (ext != ".prx" && ext != ".elf") continue;
          std::vector<uint8_t> head = readFile(e.path().string());
          if (head.size() < 4 || head[0] != 0x7F || head[1] != 'E' || head[2] != 'L' || head[3] != 'F') {
            std::printf("skipping %s (not a plain ELF, decrypt it first)\n", e.path().filename().string().c_str());
            continue;
          }
          found.push_back(e.path().string());
        }
        std::sort(found.begin(), found.end());
        helpers.insert(helpers.end(), found.begin(), found.end());
      }
      else if (a == "--stage" && i + 1 < argc) stages.push_back(argv[++i]);
      else if (a == "--stage-dir" && i + 1 < argc) {
        for (const auto& path : mgspo::stages::enumerate(argv[++i])) stages.push_back(path.string());
      }
      else if (a == "--vfpu") vfpuHist = true;
      else if (a == "--disasm" && i + 2 < argc) {
        disasm = true;
        dStart = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
        dCount = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
      } else if (a[0] != '-' && out.empty()) out = a;
      else {
        usage();
        return 2;
      }
    }

    std::vector<fs::path> stagePaths;
    for (const auto& path : stages) stagePaths.emplace_back(path);
    stagePaths = mgspo::stages::normalize(std::move(stagePaths));
    stages.clear();
    for (const auto& path : stagePaths) stages.push_back(path.string());
    const std::string stageManifest = mgspo::stages::manifest(stagePaths);
    PrxImage img = loadPrx(readFile(in), base);
    if (vfpuHist) {
      printVfpuHistogram(img);
      return 0;
    }
    if (disasm) {
      for (uint32_t k = 0; k < dCount; ++k) {
        uint32_t a = dStart + k * 4;
        if (!img.contains(a)) break;
        Insn i = decode(img.word(a), a);
        std::printf("%08X  %08X  %s\n", a, i.raw, disassemble(i).c_str());
      }
      return 0;
    }

    // Stage modules call straight into the main executable through their link tables. Some of those
    // targets are never reached from main itself, so they have to be seeded as entry points.
    if (!stages.empty()) {
      std::vector<uint32_t> segBases;
      for (const auto& s : img.info.segments)
        if (s.type == 1) segBases.push_back(img.base + s.vaddr);
      std::set<uint32_t> seen;
      std::printf("validating and linking %zu stage modules\n", stages.size());
      std::fflush(stdout);
      for (const auto& path : stages) {
        fs::path zarPath = fs::path(path).parent_path() / "_zar";
        if (!fs::exists(zarPath)) continue;
        Bytes table = findLinkTable(readFile(zarPath.string()));
        if (table.empty()) continue;
        PrxImage si = loadPrx(readFile(path), 0x10000000);
        std::vector<uint32_t> t;
        applyLinkTable(si, table, segBases, &t);
        for (uint32_t a : t)
          if (img.inCode(a) && seen.insert(a).second) img.extraRoots.push_back(a);
      }
      std::printf("seeded %zu entry points referenced from stage modules\n", img.extraRoots.size());
    }

    Program prog = discover(img);
    if (stats || out.empty()) {
      printStats(prog, img);
      if (out.empty()) return 0;
    }

    EmitOptions opts;
    opts.outDir = out;
    opts.funcsPerFile = perFile;
    fs::create_directories(out);
    fs::remove(fs::path(out) / "stages.manifest");

    // Helper modules go right after the main image, each on its own page boundary, so the runtime can
    // reserve them up front and the generated code can use absolute addresses.
    std::deque<PrxImage> helperImgs;
    std::deque<Program> helperProgs;
    std::vector<ModuleUnit> units;
    {
      ModuleUnit m;
      m.prog = &prog;
      m.name = img.info.module.name;
      units.push_back(m);
    }
    uint32_t next = img.base + static_cast<uint32_t>(img.mem.size());
    uint32_t importBase = static_cast<uint32_t>(prog.imports.size());
    for (const auto& path : helpers) {
      next = (next + 0xFFF) & ~0xFFFu;
      next += 0x1000;
      helperImgs.push_back(loadPrx(readFile(path), next));
      const PrxImage& h = helperImgs.back();
      helperProgs.push_back(discover(h, importBase));
      const Program& hp = helperProgs.back();
      importBase += static_cast<uint32_t>(hp.imports.size());
      next += static_cast<uint32_t>(h.mem.size());
      ModuleUnit u;
      u.prog = &hp;
      u.name = h.info.module.name;
      u.file = fs::path(path).filename().string();
      {
        Bytes raw = readFile(path);
        u.hash = hashBytes(raw.data(), raw.size());
      }
      std::printf("helper %-12s base 0x%08X size 0x%X functions %zu imports %zu (unknown insns %u)\n",
                  u.name.c_str(), h.base, static_cast<unsigned>(h.mem.size()), hp.funcs.size(), hp.imports.size(),
                  hp.stats.unknownInsns);
      units.push_back(u);
    }

    // Stage modules are overlays: they all sit in one range after the helpers. Small ones (the tiny
    // loader/root modules) get a slot of their own so they can stay loaded next to a big stage.
    std::deque<PrxImage> stageImgs;
    std::deque<Program> stageProgs;
    if (!stages.empty()) {
      constexpr uint32_t kSmall = 0x10000;
      struct Item {
        std::string path;
        Bytes raw;
        uint32_t size = 0;
      };
      std::vector<uint32_t> mainSegBases;
      for (const auto& s : img.info.segments)
        if (s.type == 1) mainSegBases.push_back(img.base + s.vaddr);
      std::vector<Item> items;
      uint32_t smallMax = 0, bigMax = 0;
      for (const auto& path : stages) {
        Item it;
        it.path = path;
        it.raw = readFile(path);
        it.size = static_cast<uint32_t>(loadPrx(it.raw, 0x10000000).mem.size());
        (it.size <= kSmall ? smallMax : bigMax) = std::max(it.size <= kSmall ? smallMax : bigMax, it.size);
        items.push_back(std::move(it));
      }
      next = (next + 0xFFF) & ~0xFFFu;
      next += 0x1000;
      uint32_t smallBase = next;
      uint32_t bigBase = (smallBase + ((smallMax + 0xFFF) & ~0xFFFu) + 0xFFF) & ~0xFFFu;
      std::map<std::pair<std::string, uint32_t>, uint32_t> interned;
      size_t stageIndex = 0;
      for (const auto& it : items) {
        std::printf("lifting stage %zu/%zu: %s\n", ++stageIndex, items.size(), it.path.c_str());
        std::fflush(stdout);
        uint32_t slotBase = it.size <= kSmall ? smallBase : bigBase;
        stageImgs.push_back(loadPrx(it.raw, slotBase));
        PrxImage& si = stageImgs.back();
        // the game links the stage against the main executable using a table from the stage's _zar file
        fs::path zarPath = fs::path(it.path).parent_path() / "_zar";
        if (fs::exists(zarPath)) {
          Bytes table = findLinkTable(readFile(zarPath.string()));
          if (table.empty()) std::printf("note: no link table in %s\n", zarPath.string().c_str());
          applyLinkTable(si, table, mainSegBases);
        } else {
          std::printf("note: no _zar next to %s, its references to the main module stay unlinked\n", it.path.c_str());
        }
        stageProgs.push_back(discover(si));
        Program& sp = stageProgs.back();
        // one import table for all overlays, keyed by library and NID
        sp.stubIndex.clear();
        for (auto& im : sp.imports) {
          auto ins = interned.emplace(std::make_pair(im.module, im.nid), importBase + static_cast<uint32_t>(interned.size()));
          im.index = ins.first->second;
          sp.stubIndex[im.stub] = im.index;
        }
        ModuleUnit u;
        u.prog = &sp;
        u.name = si.info.module.name;
        u.file = fs::path(it.path).filename().string();
        u.overlay = true;
        u.hash = hashBytes(it.raw.data(), it.raw.size());
        if (sp.stats.unknownInsns) std::printf("note: %s has %u unknown instructions\n", it.path.c_str(), sp.stats.unknownInsns);
        units.push_back(u);
      }
      std::printf("stages: %zu modules, small slot 0x%08X (0x%X), big slot 0x%08X (0x%X)\n", items.size(), smallBase,
                  smallMax, bigBase, bigMax);
    }

    EmitResult r = emitAll(units, opts);
    mgspo::stages::writeManifest(fs::path(out) / "stages.manifest", stageManifest);
    if (r.overlayFunctions)
      std::printf("overlay functions: %zu, distinct after de-duplication: %zu\n", r.overlayFunctions, r.overlayUnique);
    std::printf("wrote %zu files, %zu functions, %.1f MB, %zu untranslated instructions\n", r.files,
                r.functions, r.bytes / 1048576.0, r.unimplemented);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}

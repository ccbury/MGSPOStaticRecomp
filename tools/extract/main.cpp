#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "bootfile.h"
#include "elf.h"
#include "iso.h"
#include "sfo.h"

using namespace mgspo;
namespace fs = std::filesystem;

namespace {

const char* kParamPath = "PSP_GAME/PARAM.SFO";
const char* kEbootPath = "PSP_GAME/SYSDIR/EBOOT.BIN";

void usage() {
  std::puts(
      "usage: mgspo-extract <command> [args]\n"
      "\n"
      "  info <iso>                      volume id and PARAM.SFO metadata\n"
      "  ls <iso>                        list every file in the image\n"
      "  extract <iso> <outdir> [prefix] extract files (optionally only under prefix)\n"
      "  eboot <iso> <outdir>            pull PARAM.SFO and the boot binary; unpack if possible\n"
      "  analyze <file>                  dump ELF/PRX layout, entry point and imports\n"
      "                                  (<file> may be a plain ELF, a PBP or an extracted EBOOT)");
}

void printSfo(const Sfo& sfo) {
  for (const auto& v : sfo.values()) {
    if (v.isInt)
      std::printf("  %-22s = %u (0x%08X)\n", v.key.c_str(), v.num, v.num);
    else
      std::printf("  %-22s = %s\n", v.key.c_str(), v.str.c_str());
  }
}

int cmdInfo(const std::string& isoPath) {
  IsoImage iso(isoPath);
  std::printf("volume id : %s\n", iso.volumeId().c_str());
  std::printf("entries   : %zu\n", iso.entries().size());
  if (const IsoEntry* p = iso.find(kParamPath)) {
    Sfo sfo = Sfo::parse(iso.read(*p));
    std::printf("game id   : %s\n", sfo.str("DISC_ID").c_str());
    std::printf("title     : %s\n", sfo.str("TITLE").c_str());
    std::puts("PARAM.SFO:");
    printSfo(sfo);
  } else {
    std::puts("no PSP_GAME/PARAM.SFO found");
  }
  if (const IsoEntry* e = iso.find(kEbootPath)) {
    Bytes b = iso.read(*e);
    std::printf("EBOOT.BIN : %u bytes, %s\n", e->size, bootKindName(classifyBoot(b)));
  }
  return 0;
}

int cmdLs(const std::string& isoPath) {
  IsoImage iso(isoPath);
  for (const auto& e : iso.entries())
    std::printf("%c %10u  lba %-8u %s\n", e.isDir ? 'd' : '-', e.size, e.lba, e.path.c_str());
  return 0;
}

int cmdExtract(const std::string& isoPath, const fs::path& out, const std::string& prefix) {
  IsoImage iso(isoPath);
  size_t n = 0;
  for (const auto& e : iso.entries()) {
    if (!prefix.empty() && e.path.compare(0, prefix.size(), prefix) != 0) continue;
    iso.extract(e, out / fs::path(e.path));
    if (!e.isDir) ++n;
  }
  std::printf("extracted %zu files to %s\n", n, out.string().c_str());
  return 0;
}

void printElf(const ElfInfo& e) {
  std::printf("type      : 0x%04X (%s)\n", e.type, e.isPrx ? "PRX" : (e.type == 2 ? "executable" : "other"));
  std::printf("machine   : %u (%s)\n", e.machine, e.machine == 8 ? "MIPS" : "unexpected");
  std::printf("entry     : 0x%08X\n", e.entry);
  std::printf("flags     : 0x%08X\n", e.flags);

  std::puts("segments:");
  for (const auto& s : e.segments)
    std::printf("  type %u off 0x%06X vaddr 0x%08X filesz 0x%06X memsz 0x%06X flags %u\n", s.type, s.offset, s.vaddr,
                s.fileSize, s.memSize, s.flags);

  std::puts("sections:");
  for (const auto& s : e.sections)
    std::printf("  %-24s type %-2u addr 0x%08X off 0x%06X size 0x%06X flags 0x%X\n", s.name.c_str(), s.type, s.addr,
                s.offset, s.size, s.flags);

  if (!e.module.present) {
    std::puts("no module info found");
    return;
  }
  const ModuleInfo& m = e.module;
  std::printf("module    : %s (attr 0x%04X, version 0x%04X, gp 0x%08X)\n", m.name.c_str(), m.attr, m.version, m.gp);

  std::printf("exports (%zu libraries):\n", e.exports.size());
  for (const auto& x : e.exports)
    std::printf("  %-24s funcs %u vars %u attr 0x%04X\n", x.name.empty() ? "(module)" : x.name.c_str(), x.funcCount,
                x.varCount, x.attr);

  size_t total = 0;
  for (const auto& i : e.imports) total += i.funcs.size();
  std::printf("imports (%zu modules, %zu functions):\n", e.imports.size(), total);
  for (const auto& i : e.imports) {
    std::printf("  %s  (v0x%04X, %zu funcs, %u vars)\n", i.name.c_str(), i.version, i.funcs.size(), i.varCount);
    for (const auto& f : i.funcs) std::printf("    NID 0x%08X  stub 0x%08X\n", f.nid, f.stubAddr);
  }
}

// Resolves a boot file to something we can describe; returns false if no plain ELF is available.
bool unpackBoot(const Bytes& data, Bytes& elf) {
  switch (classifyBoot(data)) {
    case BootKind::Elf:
      elf = data;
      return true;
    case BootKind::Pbp: {
      Bytes psp = pbpSection(data, 6);
      return unpackBoot(psp, elf);
    }
    case BootKind::EncryptedPsp: {
      PspHeader h = parsePspHeader(data);
      std::printf("~PSP container: module '%s', attr 0x%04X, comp 0x%04X, segments %u\n", h.moduleName.c_str(),
                  h.attribute, h.compAttribute, h.segmentCount);
      std::printf("  elf size %u, psp size %u, boot entry 0x%08X, modinfo off 0x%X, bss field 0x%08X\n", h.elfSize, h.pspSize,
                  h.bootEntry, h.modinfoOffset, h.bssSize);
      std::puts("  this binary is encrypted; supply an already-decrypted ELF to analyze it.");
      return false;
    }
    default:
      std::puts("unrecognised boot file format");
      return false;
  }
}

int cmdEboot(const std::string& isoPath, const fs::path& out) {
  IsoImage iso(isoPath);
  if (const IsoEntry* p = iso.find(kParamPath)) {
    Bytes b = iso.read(*p);
    writeFile(out / "PARAM.SFO", b);
    Sfo sfo = Sfo::parse(b);
    std::printf("game id: %s  title: %s\n", sfo.str("DISC_ID").c_str(), sfo.str("TITLE").c_str());
  }
  const IsoEntry* e = iso.find(kEbootPath);
  if (!e) throw Error("PSP_GAME/SYSDIR/EBOOT.BIN not found in image");
  Bytes b = iso.read(*e);
  writeFile(out / "EBOOT.BIN", b);
  std::printf("EBOOT.BIN: %zu bytes, %s\n", b.size(), bootKindName(classifyBoot(b)));
  Bytes elf;
  if (!unpackBoot(b, elf)) {
    // Some discs ship a plain copy of the module next to the packed EBOOT.
    if (const IsoEntry* alt = iso.find("PSP_GAME/SYSDIR/BOOT.BIN")) {
      Bytes ab = iso.read(*alt);
      if (classifyBoot(ab) == BootKind::Elf) {
        std::printf("SYSDIR/BOOT.BIN is plain ELF (%zu bytes), using it\n", ab.size());
        elf = ab;
      }
    }
  }
  if (!elf.empty()) {
    writeFile(out / "boot.elf", elf);
    std::puts("wrote boot.elf");
  }
  return 0;
}

int cmdAnalyze(const std::string& file) {
  Bytes data = readFile(file);
  Bytes elf;
  if (!unpackBoot(data, elf)) return 2;
  printElf(parseElf(elf));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 1;
  }
  std::string cmd = argv[1];
  try {
    if (cmd == "info") return cmdInfo(argv[2]);
    if (cmd == "ls") return cmdLs(argv[2]);
    if (cmd == "extract" && argc >= 4) return cmdExtract(argv[2], argv[3], argc > 4 ? argv[4] : "");
    if (cmd == "eboot" && argc >= 4) return cmdEboot(argv[2], argv[3]);
    if (cmd == "analyze") return cmdAnalyze(argv[2]);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "error: %s\n", ex.what());
    return 1;
  }
  usage();
  return 1;
}

// ModuleMgrForUser. Modules are lifted ahead of time and sit at fixed guest addresses; loading one
// reads its image from the disc, applies the relocations and maps it there, so the lifted code (which
// has those addresses baked in) can run. Exports of loaded modules are what imports from other
// modules resolve to.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "kernel_internal.h"
#include "prx.h"

namespace psp {
namespace k {

namespace {

constexpr int32_t kMainModuleId = 0x2000;
constexpr uint32_t E_NO_FILE = 0x80020146;
constexpr uint32_t E_ERROR_LOAD = 0x80020148;
constexpr uint32_t E_ALREADY_STARTED = 0x80020135;
constexpr uint32_t E_NOT_STARTED = 0x80020136;

struct Module {
  std::string path;
  const ModuleDesc* desc = nullptr;  // null when the module was never lifted
  bool started = false;
};

const ModuleDesc* g_descs = nullptr;
size_t g_descCount = 0;
std::map<int32_t, Module> g_modules;  // by module id
std::map<const ModuleDesc*, int32_t> g_loaded;

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::string base_name(const std::string& p) {
  size_t i = p.find_last_of("/\\:");
  return lower(i == std::string::npos ? p : p.substr(i + 1));
}

const ModuleDesc* desc_for_path(const std::string& path) {
  std::string name = base_name(path);
  for (size_t i = 1; i < g_descCount; ++i)
    if (g_descs[i].file[0] && lower(g_descs[i].file) == name) return &g_descs[i];
  return nullptr;
}

uint32_t find_export(const char* lib, uint32_t nid) {
  for (const auto& kv : g_loaded) {
    const ModuleDesc* d = kv.first;
    for (size_t i = 0; i < d->exportCount; ++i) {
      const ModuleExport& e = d->exports[i];
      if (e.nid == nid && e.addr && std::strcmp(e.lib, lib) == 0) return e.addr;
    }
  }
  return 0;
}

// Reads a module from the user-supplied decrypted module directory, if there is one.
bool read_user_module(const std::string& path, std::vector<uint8_t>& out) {
  std::string dir = config().moduleDir;
  if (dir.empty()) return false;
  if (dir.back() != '/' && dir.back() != '\\') dir += '/';
  size_t i = path.find_last_of("/\\:");
  std::ifstream in(dir + (i == std::string::npos ? path : path.substr(i + 1)), std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool is_encrypted(const std::vector<uint8_t>& f) {
  return f.size() >= 4 && f[0] == '~' && f[1] == 'S' && f[2] == 'C';
}

uint32_t load_module(const std::string& path) {
  const ModuleDesc* d = desc_for_path(path);
  if (!d) {
    int32_t id = alloc_uid();
    g_modules[id] = {path, nullptr, false};
    std::fprintf(stderr, "[module] load %s -> %X (not lifted, exports unavailable)\n", path.c_str(), id);
    return static_cast<uint32_t>(id);
  }
  auto already = g_loaded.find(d);
  if (already != g_loaded.end()) return static_cast<uint32_t>(already->second);

  std::vector<uint8_t> file;
  bool fromUser = read_user_module(path, file);
  if (!fromUser && !io_read_file(path, file)) return E_NO_FILE;
  if (is_encrypted(file)) {
    // Nothing to map; the runtime's replacement library services this module's imports.
    int32_t id = alloc_uid();
    g_modules[id] = {path, nullptr, false};
    std::fprintf(stderr, "[module] load %s -> %X (encrypted, runtime replacement is used)\n", path.c_str(), id);
    return static_cast<uint32_t>(id);
  }
  try {
    mgspo::PrxImage img = mgspo::loadPrx(file, d->base);
    if (img.mem.size() != d->size) {
      std::fprintf(stderr, "[module] %s does not match the lifted build of %s (size %zX, expected %X)\n",
                   path.c_str(), d->name, img.mem.size(), d->size);
      return E_ERROR_LOAD;
    }
    mem_write(img.base, img.mem.data(), img.mem.size());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[module] cannot load %s: %s\n", path.c_str(), e.what());
    return E_ERROR_LOAD;
  }
  int32_t id = alloc_uid();
  g_modules[id] = {path, d, false};
  g_loaded[d] = id;
  std::fprintf(stderr, "[module] load %s -> %X (%s at %08X%s)\n", path.c_str(), id, d->name, d->base,
               fromUser ? ", user supplied" : "");
  return static_cast<uint32_t>(id);
}

void h_load_module(Cpu& c) { ret(c, load_module(cstr(arg(c, 0)))); }

void h_load_module_by_id(Cpu& c) {
  std::string path = io_path_of_fd(static_cast<int>(arg(c, 0)));
  if (path.empty()) return ret(c, E_IO_BADF);
  ret(c, load_module(path));
}

void h_start_module(Cpu& c) {
  int32_t id = static_cast<int32_t>(arg(c, 0));
  auto it = g_modules.find(id);
  if (it == g_modules.end()) return ret(c, E_UNKNOWN_UID);
  Module& m = it->second;
  if (m.started) return ret(c, E_ALREADY_STARTED);
  m.started = true;
  uint32_t status = 0;
  if (m.desc && m.desc->start) {
    status = call_guest(c, m.desc->start, arg(c, 1), arg(c, 2));
    std::fprintf(stderr, "[module] started %s, module_start returned %d\n", m.desc->name,
                 static_cast<int32_t>(status));
  }
  put32(arg(c, 3), status);
  ret(c, static_cast<uint32_t>(id));
}

void h_stop_module(Cpu& c) {
  int32_t id = static_cast<int32_t>(arg(c, 0));
  auto it = g_modules.find(id);
  if (it == g_modules.end()) return ret(c, E_UNKNOWN_UID);
  Module& m = it->second;
  if (!m.started) return ret(c, E_NOT_STARTED);
  m.started = false;
  uint32_t status = 0;
  if (m.desc && m.desc->stop) status = call_guest(c, m.desc->stop, arg(c, 1), arg(c, 2));
  put32(arg(c, 3), status);
  ret(c, 0);
}

void h_unload_module(Cpu& c) {
  auto it = g_modules.find(static_cast<int32_t>(arg(c, 0)));
  if (it == g_modules.end()) return ret(c, E_UNKNOWN_UID);
  if (it->second.desc) g_loaded.erase(it->second.desc);
  g_modules.erase(it);
  ret(c, 0);
}

void h_get_module_id(Cpu& c) { ret(c, kMainModuleId); }

void h_stop_unload_self(Cpu&) { throw std::runtime_error("module called sceKernelStopUnloadSelfModule"); }

}  // namespace

void modules_init(const ModuleDesc* mods, size_t count) {
  g_descs = mods;
  g_descCount = count;
  g_modules.clear();
  g_loaded.clear();
  if (count) {
    g_modules[kMainModuleId] = {"", &mods[0], true};
    g_loaded[&mods[0]] = kMainModuleId;
  }
  set_import_linker(find_export);
}

void register_modulemgr() {
  const char* m = "ModuleMgrForUser";
  register_hle(m, 0x977DE386, "sceKernelLoadModule", h_load_module);
  register_hle(m, 0xB7F46618, "sceKernelLoadModuleByID", h_load_module_by_id);
  register_hle(m, 0x50F0C1EC, "sceKernelStartModule", h_start_module);
  register_hle(m, 0xD1FF982A, "sceKernelStopModule", h_stop_module);
  register_hle(m, 0x2E0911AA, "sceKernelUnloadModule", h_unload_module);
  register_hle(m, 0xF0A26395, "sceKernelGetModuleId", h_get_module_id);
  register_hle(m, 0xD8B73127, "sceKernelGetModuleIdByAddress", h_get_module_id);
  register_hle(m, 0x8F2DF740, "sceKernelStopUnloadSelfModuleWithStatus", h_stop_unload_self);
}

}  // namespace k
}  // namespace psp

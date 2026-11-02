// IoFileMgrForUser. disc0:/ maps to the extracted disc, ms0:/ to a local save folder.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

#include "iso.h"
#include "kernel_internal.h"

namespace fs = std::filesystem;

namespace psp {
namespace k {

namespace {

constexpr uint32_t E_NOASYNC = 0x80020323;

struct File {
  FILE* f = nullptr;
  std::string vpath;
  bool append = false;
  bool canWrite = false;
  int64_t offset = 0;  // window into the disc image for sce_lbn paths
  int64_t limit = -1;
};

struct Dir {
  std::vector<std::string> names;
  size_t pos = 0;
  fs::path base;
};

std::map<int, File> g_files;
std::map<int, Dir> g_dirs;
std::map<int, int64_t> g_async;
int g_nextFd = 3;
std::string g_cwd = "disc0:/PSP_GAME/USRDIR/";

#ifdef _WIN32
#define SEEK64 _fseeki64
#define TELL64 _ftelli64
#else
#define SEEK64 fseeko
#define TELL64 ftello
#endif

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// Maps a guest path to a host path. Returns false for unknown devices or escapes from the root.
bool resolve(std::string p, fs::path& out, bool& writable) {
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  if (p.find(':') == std::string::npos) p = g_cwd + p;
  size_t colon = p.find(':');
  std::string dev = lower(p.substr(0, colon));
  std::string rest = p.substr(colon + 1);
  fs::path root;
  if (dev == "disc0" || dev == "umd0" || dev == "disc" || dev == "umd") {
    root = config().discRoot;
    writable = false;
  } else if (dev == "ms0" || dev == "fatms0" || dev == "ms") {
    root = config().saveRoot;
    writable = true;
  } else {
    return false;
  }
  if (root.empty()) return false;
  fs::path result = root;
  size_t i = 0;
  while (i <= rest.size()) {
    size_t j = rest.find('/', i);
    if (j == std::string::npos) j = rest.size();
    std::string part = rest.substr(i, j - i);
    if (part == "..") return false;
    if (!part.empty() && part != ".") result /= part;
    i = j + 1;
  }
  out = result;
  return true;
}

uint32_t err_from(const std::error_code& ec) {
  if (ec == std::errc::no_such_file_or_directory) return E_IO_NOENT;
  if (ec == std::errc::file_exists) return E_IO_EXIST;
  if (ec == std::errc::not_a_directory) return E_IO_NOTDIR;
  if (ec == std::errc::is_a_directory) return E_IO_ISDIR;
  if (ec == std::errc::permission_denied) return E_IO_ACCES;
  return E_IO_IO;
}

void fill_date(uint32_t p, time_t t) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  sh(p, static_cast<uint32_t>(tm.tm_year + 1900));
  sh(p + 2, static_cast<uint32_t>(tm.tm_mon + 1));
  sh(p + 4, static_cast<uint32_t>(tm.tm_mday));
  sh(p + 6, static_cast<uint32_t>(tm.tm_hour));
  sh(p + 8, static_cast<uint32_t>(tm.tm_min));
  sh(p + 10, static_cast<uint32_t>(tm.tm_sec));
  sw(p + 12, 0);
}

// The game addresses disc files by sector (disc0:/sce_lbn...), taking the start sector from
// st_private[0] of the stat/dirent. Look it up in the disc image, if one was given.
uint32_t disc_lba(const fs::path& path) {
  static std::unique_ptr<mgspo::IsoImage> iso;
  static bool tried = false;
  if (!tried) {
    tried = true;
    if (!config().discImage.empty()) {
      try {
        iso = std::make_unique<mgspo::IsoImage>(config().discImage);
      } catch (const std::exception&) {
      }
    }
  }
  if (!iso) return 0;
  std::error_code ec;
  std::string rel = fs::relative(path, config().discRoot, ec).generic_string();
  if (ec) return 0;
  const mgspo::IsoEntry* e = iso->find(rel);
  return e ? e->lba : 0;
}

// SceIoStat is 88 bytes: mode, attr, size(8), ctime/atime/mtime (16 each), 6 reserved words.
bool fill_stat(uint32_t p, const fs::path& path, bool writable) {
#ifdef _WIN32
  struct _stat64 st;
  if (_wstat64(path.c_str(), &st) != 0) return false;
#else
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
#endif
  bool dir = (st.st_mode & S_IFDIR) != 0;
  std::memset(ptr(p), 0, 88);
  uint32_t perm = writable ? 0x1FF : 0x16D;
  sw(p, (dir ? 0x1000u : 0x2000u) | perm);
  sw(p + 4, dir ? 0x10u : 0x20u);
  put64(p + 8, dir ? 0 : static_cast<uint64_t>(st.st_size));
  fill_date(p + 16, static_cast<time_t>(st.st_ctime));
  fill_date(p + 32, static_cast<time_t>(st.st_atime));
  fill_date(p + 48, static_cast<time_t>(st.st_mtime));
  if (!writable) sw(p + 64, disc_lba(path));
  return true;
}

// disc0:/sce_lbn0x<lba>_size0x<bytes> addresses a raw range of the disc image.
bool parse_lbn_path(const std::string& vpath, uint32_t& lba, uint32_t& sectors) {
  std::string s = lower(vpath);
  size_t colon = s.find(':');
  if (colon == std::string::npos || s.compare(0, colon, "disc0") != 0) return false;
  size_t i = colon + 1;
  while (i < s.size() && s[i] == '/') ++i;
  return std::sscanf(s.c_str() + i, "sce_lbn0x%x_size0x%x", &lba, &sectors) == 2;
}

int open_file(const std::string& vpath, uint32_t flags, uint32_t& err) {
  uint32_t lba = 0, sectors = 0;
  if (parse_lbn_path(vpath, lba, sectors)) {
    if (config().discImage.empty()) {
      err = E_IO_NOENT;
      return -1;
    }
    FILE* f = std::fopen(config().discImage.c_str(), "rb");
    if (!f) {
      err = E_IO_NOENT;
      return -1;
    }
    int fd = g_nextFd++;
    File& file = g_files[fd];
    file.f = f;
    file.vpath = vpath;
    file.offset = static_cast<int64_t>(lba) * 2048;
    file.limit = static_cast<int64_t>(sectors);
    SEEK64(f, file.offset, SEEK_SET);
    return fd;
  }  fs::path path;
  bool writable = false;
  if (!resolve(vpath, path, writable)) {
    err = E_IO_NODEV;
    return -1;
  }
  uint32_t acc = flags & 3;
  bool wantWrite = (acc & 2) != 0;
  std::error_code ec;
  bool exists = fs::exists(path, ec);
  if (exists && fs::is_directory(path, ec)) {
    err = E_IO_ISDIR;
    return -1;
  }
  if (wantWrite && !writable) {
    err = E_IO_ROFS;
    return -1;
  }
  if (!exists && !(flags & 0x200)) {
    err = E_IO_NOENT;
    return -1;
  }
  if (exists && (flags & 0x200) && (flags & 0x800)) {
    err = E_IO_EXIST;
    return -1;
  }
  const char* mode = "rb";
  if (wantWrite) {
    if (!exists && writable) fs::create_directories(path.parent_path(), ec);
    mode = (!exists || (flags & 0x400)) ? "w+b" : "r+b";
  }
  FILE* f = std::fopen(path.string().c_str(), mode);
  if (!f) {
    err = errno == EACCES ? E_IO_ACCES : E_IO_NOENT;
    return -1;
  }
  int fd = g_nextFd++;
  File& file = g_files[fd];
  file.f = f;
  file.vpath = vpath;
  file.append = (flags & 0x100) != 0;
  file.canWrite = wantWrite;
  return fd;
}

int64_t do_read(int fd, uint32_t buf, uint32_t size) {
  if (fd == 0) return 0;
  auto it = g_files.find(fd);
  if (it == g_files.end()) return static_cast<int32_t>(E_IO_BADF);
  if (!size) return 0;
  ptr(buf + size - 1);
  File& file = it->second;
  if (file.limit >= 0) {
    int64_t pos = TELL64(file.f) - file.offset;
    int64_t left = std::max<int64_t>(0, file.limit - pos);
    size = static_cast<uint32_t>(std::min<int64_t>(size, left));
    if (!size) return 0;
  }
  return static_cast<int64_t>(std::fread(ptr(buf), 1, size, file.f));
}

int64_t do_write(int fd, uint32_t buf, uint32_t size) {
  if (fd == 1 || fd == 2) {
    std::string s;
    for (uint32_t i = 0; i < size; ++i) s.push_back(static_cast<char>(lbu(buf + i)));
    std::fprintf(stderr, "[guest] %s", s.c_str());
    return size;
  }
  auto it = g_files.find(fd);
  if (it == g_files.end()) return static_cast<int32_t>(E_IO_BADF);
  if (!it->second.canWrite) return static_cast<int32_t>(E_IO_BADF);
  if (!size) return 0;
  ptr(buf + size - 1);
  if (it->second.append) SEEK64(it->second.f, 0, SEEK_END);
  return static_cast<int64_t>(std::fwrite(ptr(buf), 1, size, it->second.f));
}

int64_t do_seek(int fd, int64_t off, int whence) {
  auto it = g_files.find(fd);
  if (it == g_files.end()) return static_cast<int32_t>(E_IO_BADF);
  File& file = it->second;
  if (file.limit >= 0) {
    int64_t cur = TELL64(file.f) - file.offset;
    int64_t pos = whence == 0 ? off : whence == 1 ? cur + off : file.limit + off;
    if (pos < 0) return static_cast<int32_t>(E_IO_INVAL);
    SEEK64(file.f, file.offset + pos, SEEK_SET);
    return pos;
  }
  int w = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
  if (SEEK64(file.f, off, w) != 0) return static_cast<int32_t>(E_IO_INVAL);
  return TELL64(file.f);
}

int do_close(int fd) {
  auto it = g_files.find(fd);
  if (it == g_files.end()) return static_cast<int32_t>(E_IO_BADF);
  std::fclose(it->second.f);
  g_files.erase(it);
  return 0;
}

void set_async(Cpu& c, int fd, int64_t res) {
  g_async[fd] = res;
  ret(c, 0);
}

// ---- handlers ----

void h_open(Cpu& c) {
  uint32_t err = 0;
  std::string vpath = cstr(arg(c, 0));
  int fd = open_file(vpath, arg(c, 1), err);
  if (config().trace) std::fprintf(stderr, "[io] open %s -> %d\n", vpath.c_str(), fd);
  if (fd < 0) {
    static std::set<std::string> reported;
    if (reported.insert(vpath).second)
      std::fprintf(stderr, "[io] sceIoOpen(%s, flags %X) failed: %08X\n", vpath.c_str(), arg(c, 1), err);
  }
  ret(c, fd < 0 ? err : static_cast<uint32_t>(fd));
}

void h_open_async(Cpu& c) {
  uint32_t err = 0;
  int fd = open_file(cstr(arg(c, 0)), arg(c, 1), err);
  if (fd < 0) {
    // the error is delivered through the async result of a pseudo descriptor
    fd = g_nextFd++;
    g_async[fd] = static_cast<int32_t>(err);
  } else {
    g_async[fd] = 0;
  }
  ret(c, static_cast<uint32_t>(fd));
}

void h_close(Cpu& c) { ret(c, static_cast<uint32_t>(do_close(static_cast<int>(arg(c, 0))))); }
void h_close_async(Cpu& c) {
  int fd = static_cast<int>(arg(c, 0));
  set_async(c, fd, do_close(fd));
}

void h_read(Cpu& c) { ret(c, static_cast<uint32_t>(do_read(static_cast<int>(arg(c, 0)), arg(c, 1), arg(c, 2)))); }
void h_read_async(Cpu& c) {
  int fd = static_cast<int>(arg(c, 0));
  set_async(c, fd, do_read(fd, arg(c, 1), arg(c, 2)));
}

void h_write(Cpu& c) { ret(c, static_cast<uint32_t>(do_write(static_cast<int>(arg(c, 0)), arg(c, 1), arg(c, 2)))); }
void h_write_async(Cpu& c) {
  int fd = static_cast<int>(arg(c, 0));
  set_async(c, fd, do_write(fd, arg(c, 1), arg(c, 2)));
}

// 64-bit offset sits in a2:a3, whence in t0
void h_lseek(Cpu& c) {
  int64_t r = do_seek(static_cast<int>(arg(c, 0)), static_cast<int64_t>(arg64(c, 2)), static_cast<int>(arg(c, 4)));
  ret64(c, static_cast<uint64_t>(r));
}
void h_lseek_async(Cpu& c) {
  int fd = static_cast<int>(arg(c, 0));
  set_async(c, fd, do_seek(fd, static_cast<int64_t>(arg64(c, 2)), static_cast<int>(arg(c, 4))));
}
void h_lseek32(Cpu& c) {
  int64_t r = do_seek(static_cast<int>(arg(c, 0)), static_cast<int32_t>(arg(c, 1)), static_cast<int>(arg(c, 2)));
  ret(c, static_cast<uint32_t>(r));
}
void h_lseek32_async(Cpu& c) {
  int fd = static_cast<int>(arg(c, 0));
  set_async(c, fd, do_seek(fd, static_cast<int32_t>(arg(c, 1)), static_cast<int>(arg(c, 2))));
}

// UMD ioctls (file position, sector lookups) have nothing to do on a host file system
void h_ioctl_async(Cpu& c) { set_async(c, static_cast<int>(arg(c, 0)), 0); }

void async_result(Cpu& c, int fd, uint32_t resPtr, bool poll) {
  (void)poll;
  auto it = g_async.find(fd);
  if (it == g_async.end()) return ret(c, E_NOASYNC);
  put64(resPtr, static_cast<uint64_t>(it->second));
  g_async.erase(it);
  ret(c, 0);
}
void h_poll_async(Cpu& c) { async_result(c, static_cast<int>(arg(c, 0)), arg(c, 1), true); }
void h_wait_async(Cpu& c) { async_result(c, static_cast<int>(arg(c, 0)), arg(c, 1), false); }
void h_wait_async_cb(Cpu& c) {
  run_callbacks(c);
  async_result(c, static_cast<int>(arg(c, 0)), arg(c, 1), false);
}
void h_zero(Cpu& c) { ret(c, 0); }

void h_getstat(Cpu& c) {
  fs::path p;
  bool w;
  if (!resolve(cstr(arg(c, 0)), p, w)) return ret(c, E_IO_NODEV);
  ret(c, fill_stat(arg(c, 1), p, w) ? 0 : E_IO_NOENT);
}

void h_mkdir(Cpu& c) {
  fs::path p;
  bool w;
  if (!resolve(cstr(arg(c, 0)), p, w)) return ret(c, E_IO_NODEV);
  if (!w) return ret(c, E_IO_ROFS);
  std::error_code ec;
  if (fs::exists(p, ec)) return ret(c, E_IO_EXIST);
  ret(c, fs::create_directories(p, ec) ? 0 : err_from(ec));
}

void h_rmdir(Cpu& c) {
  fs::path p;
  bool w;
  if (!resolve(cstr(arg(c, 0)), p, w)) return ret(c, E_IO_NODEV);
  if (!w) return ret(c, E_IO_ROFS);
  std::error_code ec;
  if (!fs::is_directory(p, ec)) return ret(c, E_IO_NOENT);
  ret(c, fs::remove(p, ec) ? 0 : err_from(ec));
}

void h_remove(Cpu& c) {
  fs::path p;
  bool w;
  if (!resolve(cstr(arg(c, 0)), p, w)) return ret(c, E_IO_NODEV);
  if (!w) return ret(c, E_IO_ROFS);
  std::error_code ec;
  if (!fs::exists(p, ec) || fs::is_directory(p, ec)) return ret(c, E_IO_NOENT);
  ret(c, fs::remove(p, ec) ? 0 : err_from(ec));
}

void h_rename(Cpu& c) {
  fs::path a, b;
  bool wa, wb;
  if (!resolve(cstr(arg(c, 0)), a, wa) || !resolve(cstr(arg(c, 1)), b, wb)) return ret(c, E_IO_NODEV);
  if (!wa || !wb) return ret(c, E_IO_ROFS);
  std::error_code ec;
  fs::rename(a, b, ec);
  ret(c, ec ? err_from(ec) : 0);
}

void h_chdir(Cpu& c) {
  std::string p = cstr(arg(c, 0));
  fs::path hp;
  bool w;
  if (!resolve(p, hp, w)) return ret(c, E_IO_NODEV);
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  if (p.find(':') == std::string::npos) p = g_cwd + p;
  if (p.back() != '/') p.push_back('/');
  g_cwd = p;
  ret(c, 0);
}

void h_dopen(Cpu& c) {
  fs::path p;
  bool w;
  if (!resolve(cstr(arg(c, 0)), p, w)) return ret(c, E_IO_NODEV);
  std::error_code ec;
  if (!fs::is_directory(p, ec)) return ret(c, E_IO_NOENT);
  Dir d;
  d.base = p;
  d.names = {".", ".."};
  for (auto& e : fs::directory_iterator(p, ec)) d.names.push_back(e.path().filename().string());
  int fd = g_nextFd++;
  g_dirs[fd] = std::move(d);
  ret(c, static_cast<uint32_t>(fd));
}

// SceIoDirent: SceIoStat (88), name[256], private pointer, dummy
void h_dread(Cpu& c) {
  auto it = g_dirs.find(static_cast<int>(arg(c, 0)));
  if (it == g_dirs.end()) return ret(c, E_IO_BADF);
  Dir& d = it->second;
  if (d.pos >= d.names.size()) return ret(c, 0);
  uint32_t p = arg(c, 1);
  const std::string& n = d.names[d.pos++];
  if (!fill_stat(p, d.base / n, config().discRoot.empty() ? true : false)) {
    std::memset(ptr(p), 0, 88);
    sw(p, 0x2000 | 0x16D);
    sw(p + 4, 0x20);
  }
  std::memset(ptr(p + 88), 0, 264);
  write_cstr(p + 88, n, 256);
  ret(c, 1);
}

void h_dclose(Cpu& c) { ret(c, g_dirs.erase(static_cast<int>(arg(c, 0))) ? 0 : E_IO_BADF); }

// Devctl commands the game is known to be able to issue are answered; the rest report success.
void h_devctl(Cpu& c) {
  std::string dev = lower(cstr(arg(c, 0)));
  uint32_t cmd = arg(c, 1), in = arg(c, 2), out = arg(c, 4);
  (void)in;
  if (dev.rfind("fatms", 0) == 0 || dev.rfind("ms", 0) == 0) {
    switch (cmd) {
      case 0x02425823:  // is the memory stick inserted
        put32(out, 1);
        return ret(c, 0);
      case 0x02425818: {  // free space: pointer to {maxClusters, freeClusters, maxSectors, sectorSize, sectorCount}
        uint32_t info = lw(out);
        if (info) {
          sw(info, 0x8000);
          sw(info + 4, 0x7000);
          sw(info + 8, 0x40000);
          sw(info + 12, 0x200);
          sw(info + 16, 8);
        }
        return ret(c, 0);
      }
      default: break;
    }
  }
  std::fprintf(stderr, "[io] devctl %s cmd=%08X\n", dev.c_str(), cmd);
  ret(c, 0);
}

}  // namespace

void io_init() {
  for (auto& kv : g_files)
    if (kv.second.f) std::fclose(kv.second.f);
  g_files.clear();
  g_dirs.clear();
  g_async.clear();
  g_nextFd = 3;
  std::error_code ec;
  if (!config().saveRoot.empty()) fs::create_directories(config().saveRoot, ec);
}

std::string io_path_of_fd(int fd) {
  auto it = g_files.find(fd);
  return it == g_files.end() ? std::string() : it->second.vpath;
}

bool io_read_file(const std::string& vpath, std::vector<uint8_t>& out) {
  uint32_t lba = 0, bytes = 0;
  if (parse_lbn_path(vpath, lba, bytes)) {
    if (config().discImage.empty()) return false;
    FILE* f = std::fopen(config().discImage.c_str(), "rb");
    if (!f) return false;
    SEEK64(f, static_cast<int64_t>(lba) * 2048, SEEK_SET);
    out.resize(bytes);
    bool ok = bytes == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
  }
  fs::path p;
  bool w = false;
  if (!resolve(vpath, p, w)) return false;
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? static_cast<size_t>(n) : 0);
  bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

void register_io() {
  const char* m = "IoFileMgrForUser";
  register_hle(m, 0x109F50BC, "sceIoOpen", h_open);
  register_hle(m, 0x89AA9906, "sceIoOpenAsync", h_open_async);
  register_hle(m, 0x810C4BC3, "sceIoClose", h_close);
  register_hle(m, 0xFF5940B6, "sceIoCloseAsync", h_close_async);
  register_hle(m, 0x6A638D83, "sceIoRead", h_read);
  register_hle(m, 0xA0B5A7C2, "sceIoReadAsync", h_read_async);
  register_hle(m, 0x42EC03AC, "sceIoWrite", h_write);
  register_hle(m, 0x0FACAB19, "sceIoWriteAsync", h_write_async);
  register_hle(m, 0x27EB27B8, "sceIoLseek", h_lseek);
  register_hle(m, 0x71B19E77, "sceIoLseekAsync", h_lseek_async);
  register_hle(m, 0x68963324, "sceIoLseek32", h_lseek32);
  register_hle(m, 0x1B385D8F, "sceIoLseek32Async", h_lseek32_async);
  register_hle(m, 0x3251EA56, "sceIoPollAsync", h_poll_async);
  register_hle(m, 0xE23EEC33, "sceIoWaitAsync", h_wait_async);
  register_hle(m, 0x35DBD746, "sceIoWaitAsyncCB", h_wait_async_cb);
  register_hle(m, 0xCB05F8D6, "sceIoGetAsyncStat", h_poll_async);
  register_hle(m, 0xA12A0514, "sceIoSetAsyncCallback", h_zero);
  register_hle(m, 0xACE946E8, "sceIoGetstat", h_getstat);
  register_hle(m, 0x06A70004, "sceIoMkdir", h_mkdir);
  register_hle(m, 0x1117C65F, "sceIoRmdir", h_rmdir);
  register_hle(m, 0xF27A9C51, "sceIoRemove", h_remove);
  register_hle(m, 0x779103A0, "sceIoRename", h_rename);
  register_hle(m, 0x55F4717D, "sceIoChdir", h_chdir);
  register_hle(m, 0xB29DDF9C, "sceIoDopen", h_dopen);
  register_hle(m, 0xE3EB004C, "sceIoDread", h_dread);
  register_hle(m, 0xEB092469, "sceIoDclose", h_dclose);
  register_hle(m, 0x54F5FB11, "sceIoDevctl", h_devctl);
  register_hle(m, 0x63632449, "sceIoIoctl", h_zero);
  register_hle(m, 0xE95A012B, "sceIoIoctlAsync", h_ioctl_async);
}

}  // namespace k
}  // namespace psp

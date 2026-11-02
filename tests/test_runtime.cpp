// Drives a few firmware handlers directly through the import dispatcher, without a game attached.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <psp/kernel.h>
#include <psp/runtime.h>

using namespace psp;

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

enum Import : uint32_t { CreateSema, PollSema, SignalSema, AllocMem, BlockHead, IoOpen, IoWrite, IoRead, IoClose, kCount };

static const ImportEntry kImports[kCount] = {
    {0x08800000, "ThreadManForUser", 0xD6DA4BA1},
    {0x08800008, "ThreadManForUser", 0x58B1F937},
    {0x08800010, "ThreadManForUser", 0x3F53E640},
    {0x08800018, "SysMemUserForUser", 0x237DBD4F},
    {0x08800020, "SysMemUserForUser", 0x9D9A5BA1},
    {0x08800028, "IoFileMgrForUser", 0x109F50BC},
    {0x08800030, "IoFileMgrForUser", 0x42EC03AC},
    {0x08800038, "IoFileMgrForUser", 0x6A638D83},
    {0x08800040, "IoFileMgrForUser", 0x810C4BC3},
};

static uint32_t call_import(Import i, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0,
                            uint32_t t0 = 0) {
  Cpu c;
  c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3; c.r[8] = t0;
  hle(c, i);
  return c.r[2];
}

static void put_str(uint32_t addr, const char* s) { mem_write(addr, s, std::strlen(s) + 1); }

int main() {
  namespace fs = std::filesystem;
  fs::path tmp = fs::temp_directory_path() / "mgspo_rt_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "disc");

  init_memory();
  KernelConfig cfg;
  cfg.discRoot = (tmp / "disc").string();
  cfg.saveRoot = (tmp / "save").string();
  cfg.headless = true;
  kernel_init(cfg, 0x08900000, 0);
  register_all_hle();
  bind_tables(nullptr, 0, kImports, kCount);

  // semaphores
  const uint32_t scratch = 0x08A00000;
  put_str(scratch, "test");
  uint32_t sema = call_import(CreateSema, scratch, 0, 1, 1, 0);
  CHECK(static_cast<int32_t>(sema) > 0);
  CHECK(call_import(PollSema, sema, 1) == 0);
  CHECK(call_import(PollSema, sema, 1) != 0);  // empty now
  CHECK(call_import(SignalSema, sema, 1) == 0);
  CHECK(call_import(PollSema, sema, 1) == 0);

  // user memory allocation
  uint32_t blk = call_import(AllocMem, 2, scratch, 0, 0x1000, 0);
  CHECK(static_cast<int32_t>(blk) > 0);
  uint32_t head = call_import(BlockHead, blk);
  CHECK(head >= 0x08800000 && head < 0x0A000000);

  // memory stick files land in the save folder
  uint32_t path = scratch + 0x100, buf = scratch + 0x200;
  put_str(path, "ms0:/SAVEDATA/test.bin");
  put_str(buf, "hello");
  uint32_t fd = call_import(IoOpen, path, 0x0602 /* WRONLY | CREAT | TRUNC */, 0777);
  CHECK(static_cast<int32_t>(fd) >= 0);
  CHECK(call_import(IoWrite, fd, buf, 5) == 5);
  CHECK(call_import(IoClose, fd) == 0);
  CHECK(fs::exists(tmp / "save" / "SAVEDATA" / "test.bin"));

  fd = call_import(IoOpen, path, 1 /* RDONLY */, 0);
  CHECK(static_cast<int32_t>(fd) >= 0);
  std::memset(ptr(buf), 0, 8);
  CHECK(call_import(IoRead, fd, buf, 8) == 5);
  CHECK(std::memcmp(ptr(buf), "hello", 5) == 0);
  call_import(IoClose, fd);

  // the disc is read only and does not allow escaping its folder
  put_str(path, "disc0:/new.bin");
  CHECK(static_cast<int32_t>(call_import(IoOpen, path, 0x0602, 0777)) < 0);
  put_str(path, "disc0:/../secret");
  CHECK(static_cast<int32_t>(call_import(IoOpen, path, 1, 0)) < 0);

  fs::remove_all(tmp);
  if (g_failures == 0) std::printf("runtime tests passed\n");
  return g_failures ? 1 : 0;
}

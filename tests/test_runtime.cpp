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

enum Import : uint32_t {
  CreateSema, PollSema, SignalSema, AllocMem, BlockHead, IoOpen, IoWrite, IoRead, IoClose,
  IoOpenAsync, IoReadAsync, IoPollAsync, IoWaitAsync, IoWaitAsyncCB, IoGetAsyncStat, IoCloseAsync,
  CreateThread, StartThread, WaitThreadEnd, ExitGame, kCount
};

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
    {0x08800048, "IoFileMgrForUser", 0x89AA9906},
    {0x08800050, "IoFileMgrForUser", 0xA0B5A7C2},
    {0x08800058, "IoFileMgrForUser", 0x3251EA56},
    {0x08800060, "IoFileMgrForUser", 0xE23EEC33},
    {0x08800068, "IoFileMgrForUser", 0x35DBD746},
    {0x08800070, "IoFileMgrForUser", 0xCB05F8D6},
    {0x08800078, "IoFileMgrForUser", 0xFF5940B6},
    {0x08800080, "ThreadManForUser", 0x446D8DE6},
    {0x08800088, "ThreadManForUser", 0xF475845D},
    {0x08800090, "ThreadManForUser", 0x278C0DF5},
    {0x08800098, "LoadExecForUser", 0x05572A5F},
};

static uint32_t call_import(Import i, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0,
                            uint32_t t0 = 0) {
  Cpu c;
  c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3; c.r[8] = t0;
  hle(c, i);
  return c.r[2];
}

static void put_str(uint32_t addr, const char* s) { mem_write(addr, s, std::strlen(s) + 1); }

static constexpr uint32_t asyncPath = 0x08A00100, asyncBuf = 0x08A00200, asyncResult = 0x08A00300;
static uint32_t asyncFd;
static bool cacheActivated;

static void asyncReader(Cpu&) {
  unsigned pending = 0;
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    CHECK(call_import(IoReadAsync, asyncFd, asyncBuf, 5) == 0);
    CHECK(call_import(IoReadAsync, asyncFd, asyncBuf, 5) == 0x80020329);
    CHECK(call_import(IoRead, asyncFd, asyncBuf, 5) == 0x80020329);
    CHECK(call_import(IoClose, asyncFd) == 0x80020329);
    sw(asyncResult, 0xDEADBEEF);
    sw(asyncResult + 4, 0x12345678);
    const uint32_t polled = call_import(IoPollAsync, asyncFd, asyncResult);
    CHECK(polled <= 1);
    if (polled == 1) {
      ++pending;
      CHECK(lw(asyncResult) == 0xDEADBEEF && lw(asyncResult + 4) == 0x12345678);
      CHECK(call_import(IoWaitAsync, asyncFd, asyncResult) == 0);
      CHECK(cacheActivated);  // Waiting for I/O lets the lower-priority owner activate its current bank.
    }
    CHECK(lw(asyncResult) == (attempt == 0 ? 5u : 0u) && lw(asyncResult + 4) == 0);
    CHECK(std::memcmp(ptr(asyncBuf), "hello", 5) == 0);
    CHECK(call_import(IoPollAsync, asyncFd, asyncResult) == 0x8002032A);
  }
  CHECK(pending > 0);  // Tolerate an occasional host scheduling pause, not always-synchronous completion.
  CHECK(call_import(IoReadAsync, asyncFd, asyncBuf, 5) == 0);
  CHECK(call_import(IoWaitAsyncCB, asyncFd, asyncResult) == 0);
  CHECK(lw(asyncResult) == 0 && lw(asyncResult + 4) == 0);
}

static void asyncDriver(Cpu&) {
  asyncFd = call_import(IoOpenAsync, asyncPath, 1, 0);
  CHECK(static_cast<int32_t>(asyncFd) > 0);
  CHECK(call_import(IoGetAsyncStat, asyncFd, 0, asyncResult) == 0);
  CHECK(lw(asyncResult) == asyncFd && lw(asyncResult + 4) == 0);
  put_str(0x08A00400, "async-reader");
  const uint32_t reader = call_import(CreateThread, 0x08A00400, 0x08805000, 0x10, 0x4000);
  CHECK(static_cast<int32_t>(reader) > 0);
  CHECK(call_import(StartThread, reader) == 0);
  cacheActivated = true;
  CHECK(call_import(WaitThreadEnd, reader) == 0);
  CHECK(call_import(IoReadAsync, asyncFd, asyncBuf, 5) == 0);
  sw(asyncResult, 0xAABBCCDD);
  const uint32_t polled = call_import(IoGetAsyncStat, asyncFd, 1, asyncResult);
  CHECK(polled <= 1);
  if (polled == 1) {
    CHECK(lw(asyncResult) == 0xAABBCCDD);
    CHECK(call_import(IoGetAsyncStat, asyncFd, 0, asyncResult) == 0);
  }
  CHECK(lw(asyncResult) == 0);
  CHECK(call_import(IoCloseAsync, asyncFd) == 0);
  CHECK(call_import(IoWaitAsync, asyncFd, asyncResult) == 0);
  CHECK(lw(asyncResult) == 0 && lw(asyncResult + 4) == 0);
  CHECK(call_import(IoReadAsync, asyncFd, asyncBuf, 5) == 0x80010009);
  CHECK(call_import(IoPollAsync, asyncFd, asyncResult) == 0x80010009);
  CHECK(call_import(IoReadAsync, UINT32_MAX, asyncBuf, 5) == 0x80010009);
  CHECK(call_import(IoPollAsync, UINT32_MAX, asyncResult) == 0x80010009);
  call_import(ExitGame);
}

static void placeholder(Cpu& c) { c.r[2] = 0xBAD; }
static void linked_export(Cpu& c) { c.r[2] = c.r[4] + 7; }
static void firmware_import(Cpu& c) { c.r[2] = 123; }
static uint32_t module_linker(const char* module, uint32_t nid) {
  return std::strcmp(module, "test-sound") == 0 && nid == 1 ? 0x08802000 : 0;
}

static void test_import_stub_precedence() {
  const FuncEntry funcs[] = {{0x08801000, placeholder}, {0x08801004, placeholder},
                            {0x08801008, placeholder}, {0x08802000, linked_export}};
  const ImportEntry imports[] = {{UINT32_MAX, "test-sound", 2}, {0x08801008, "test-firmware", 3},
                                {0x08801000, "test-sound", 1}};
  register_hle("test-firmware", 3, firmware_import);
  bind_tables(funcs, 4, imports, 3);
  set_import_linker(module_linker);
  Cpu c;
  c.r[4] = 35;
  call(c, 0x08801000);
  CHECK(c.r[2] == 42);  // cross-module calls must not execute a generated empty import placeholder
  c.r[2] = 0;
  call(c, 0x08801004);
  CHECK(c.r[2] == 42);  // the delay-slot address belongs to the same eight-byte stub
  call(c, 0x08801008);
  CHECK(c.r[2] == 123);  // registered firmware handlers also override empty placeholders
  c.r[4] = 1;
  call(c, 0x08802000);
  CHECK(c.r[2] == 8);  // ordinary lifted functions remain reachable
  set_import_linker(nullptr);
  bind_tables(funcs, 4, nullptr, 0);
  call(c, 0x08801000);
  CHECK(c.r[2] == 0xBAD);  // rebinding must remove stale import ranges
}

int main() {
  namespace fs = std::filesystem;
  fs::path tmp = fs::temp_directory_path() / "mgspo_rt_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "disc");

  init_memory();
  test_import_stub_precedence();
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

  put_str(asyncPath, "ms0:/SAVEDATA/test.bin");
  const FuncEntry asyncFunctions[] = {{0x08804000, asyncDriver}, {0x08805000, asyncReader}};
  bind_tables(asyncFunctions, 2, kImports, kCount);
  CHECK(kernel_run_main(0x08804000) == 0);

  fs::remove_all(tmp);
  if (g_failures == 0) std::printf("runtime tests passed\n");
  return g_failures ? 1 : 0;
}

// Small libraries: loadexec, rtc, utils, power, umd, utility, net stubs.
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "kernel_internal.h"

namespace psp {
namespace k {

namespace {

void h_zero(Cpu& c) { ret(c, 0); }

// ---- LoadExecForUser ----

void h_exit_game(Cpu&) { game_exit(); }

void h_register_exit_callback(Cpu& c) {
  set_exit_callback(static_cast<int32_t>(arg(c, 0)));
  ret(c, 0);
}

// ---- sceRtc ----

constexpr uint64_t kUnixToRtcUs = 62135596800ULL * 1000000ULL;

uint64_t unix_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void h_rtc_resolution(Cpu& c) { ret(c, 1000000); }

void h_rtc_current_tick(Cpu& c) {
  put64(arg(c, 0), unix_us() + kUnixToRtcUs);
  ret(c, 0);
}

void h_rtc_local_time(Cpu& c) {
  uint64_t us = unix_us();
  std::time_t t = static_cast<std::time_t>(us / 1000000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  uint32_t p = arg(c, 0);
  if (!p) return ret(c, E_ERROR);
  sh(p, static_cast<uint32_t>(tm.tm_year + 1900));
  sh(p + 2, static_cast<uint32_t>(tm.tm_mon + 1));
  sh(p + 4, static_cast<uint32_t>(tm.tm_mday));
  sh(p + 6, static_cast<uint32_t>(tm.tm_hour));
  sh(p + 8, static_cast<uint32_t>(tm.tm_min));
  sh(p + 10, static_cast<uint32_t>(tm.tm_sec));
  sw(p + 12, static_cast<uint32_t>(us % 1000000));
  ret(c, 0);
}

// ---- UtilsForUser ----

void h_gettimeofday(Cpu& c) {
  uint64_t us = unix_us();
  put32(arg(c, 0), static_cast<uint32_t>(us / 1000000));
  if (arg(c, 0)) sw(arg(c, 0) + 4, static_cast<uint32_t>(us % 1000000));
  ret(c, 0);
}

void h_libc_clock(Cpu& c) { ret(c, static_cast<uint32_t>(now_us())); }

void h_libc_time(Cpu& c) {
  uint32_t t = static_cast<uint32_t>(unix_us() / 1000000);
  put32(arg(c, 0), t);
  ret(c, t);
}

// ---- sceSuspendForUser ----

constexpr uint32_t kVolatileBase = 0x08400000;
constexpr uint32_t kVolatileSize = 0x00400000;

void h_volatile_lock(Cpu& c) {
  put32(arg(c, 1), kVolatileBase);
  put32(arg(c, 2), kVolatileSize);
  ret(c, 0);
}

// ---- sceUmdUser ----

void h_umd_drive_stat(Cpu& c) { ret(c, 0x32); }  // present, initialised, ready

// ---- sceUtility ----

void h_sysparam_int(Cpu& c) {
  uint32_t id = arg(c, 0);
  uint32_t v = 0;
  switch (id) {
    case 1: v = 11; break;  // ad-hoc channel
    case 3: v = 1; break;   // date format
    case 4: v = 0; break;   // time format 24h
    case 7: v = 1; break;   // language: English
    case 8: v = 1; break;   // button assignment: cross to confirm
    default: break;
  }
  put32(arg(c, 1), v);
  ret(c, 0);
}

void h_sysparam_string(Cpu& c) {
  if (arg(c, 0) == 1) write_cstr(arg(c, 1), "PSP", arg(c, 2));
  else if (arg(c, 2)) sb(arg(c, 1), 0);
  ret(c, 0);
}

// ---- sceUtility dialogs ----

// The msgdialog and on-screen keyboard share one lifecycle: initialise, run for a few
// updates, finish, then shut down. Both resolve immediately with a canned answer.
struct UtilityDialog {
  int state = 0;  // 0 none, 1 initialised, 2 running, 3 finished, 4 shut down
  int updates = 0;
  uint32_t params = 0;
};

UtilityDialog g_msgDialog, g_osk;

int dialog_status(UtilityDialog& d) {
  int s = d.state;
  if (s == 1) d.state = 2;
  else if (s == 4) d.state = 0;
  return s;
}

void dialog_update(UtilityDialog& d) {
  if (d.state == 2 && ++d.updates >= 3) d.state = 3;
}

void dialog_start(UtilityDialog& d, uint32_t params) {
  d.state = 1;
  d.updates = 0;
  d.params = params;
}

void h_msgdialog_init(Cpu& c) {
  uint32_t p = arg(c, 0);
  dialog_start(g_msgDialog, p);
  // layout: mode +0x34, error +0x38, message[512] +0x3C, options +0x23C, buttonPressed +0x240
  if (p && lw(p) >= 0x244) {
    sw(p + 0x1C, 0);   // result: ok
    sw(p + 0x240, 1);  // button pressed: yes
  }
  ret(c, 0);
}

void h_msgdialog_status(Cpu& c) { ret(c, static_cast<uint32_t>(dialog_status(g_msgDialog))); }

void h_msgdialog_update(Cpu& c) {
  dialog_update(g_msgDialog);
  ret(c, 0);
}

void h_msgdialog_shutdown(Cpu& c) {
  g_msgDialog.state = 4;
  ret(c, 0);
}

void osk_fill(uint32_t p) {
  if (p) {
    sw(p + 0x1C, 0);
    uint32_t data = lw(p + 0x34);
    if (data) {
      static const char16_t kName[] = u"JACK";
      // data: outtextlength +0x24, outtext +0x28, result +0x2C, outtextlimit +0x30
      uint32_t out = lw(data + 0x28), limit = lw(data + 0x30);
      uint32_t n = 0;
      if (out) {
        for (; kName[n] && (!limit || n < limit); ++n) sh(out + n * 2, kName[n]);
        sh(out + n * 2, 0);
      }
      sw(data + 0x24, n);
      sw(data + 0x2C, 2);  // result: text changed
    }
  }
}

void h_osk_init(Cpu& c) {
  uint32_t p = arg(c, 0);
  dialog_start(g_osk, p);
  osk_fill(p);
  ret(c, 0);
}

void h_osk_status(Cpu& c) { ret(c, static_cast<uint32_t>(dialog_status(g_osk))); }

void h_osk_update(Cpu& c) {
  dialog_update(g_osk);
  if (g_osk.state == 3) osk_fill(g_osk.params);
  ret(c, 0);
}

void h_osk_shutdown(Cpu& c) {
  g_osk.state = 4;
  ret(c, 0);
}

// ---- sceUtility savedata ----

constexpr uint32_t kSaveLoadNoData = 0x80110306;
constexpr uint32_t kSaveLoadAccess = 0x80110305;
constexpr uint32_t kSaveDeleteNoData = 0x80110345;

UtilityDialog g_savedata;

// Layout (SceUtilitySavedataParam): mode +0x30, gameName[13] +0x3C, saveName[20] +0x4C,
// saveNameList +0x60, fileName[13] +0x64, dataBuf +0x74, dataBufSize +0x78, dataSize +0x7C.
void savedata_run(uint32_t p) {
  if (!p) return;
  uint32_t mode = lw(p + 0x30);
  std::string game = cstr(p + 0x3C, 13);
  std::string name = cstr(p + 0x4C, 20);
  uint32_t list = lw(p + 0x60);
  if ((name.empty() || name[0] == '<') && list) name = cstr(lw(list), 20);
  std::string file = cstr(p + 0x64, 13);
  uint32_t buf = lw(p + 0x74), bufSize = lw(p + 0x78), dataSize = lw(p + 0x7C);
  namespace fs = std::filesystem;
  fs::path dir = fs::path(config().saveRoot) / "PSP" / "SAVEDATA" / (game + name);
  fs::path path = dir / file;
  std::error_code ec;
  uint32_t result = 0;
  switch (mode) {
    case 0: case 2: case 4: {  // load
      std::ifstream in(path, std::ios::binary);
      if (!in || !buf) {
        result = kSaveLoadNoData;
        break;
      }
      std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      uint32_t n = static_cast<uint32_t>(std::min<size_t>(data.size(), bufSize ? bufSize : data.size()));
      for (uint32_t i = 0; i < n; ++i) sb(buf + i, static_cast<uint8_t>(data[i]));
      sw(p + 0x7C, n);
      break;
    }
    case 1: case 3: case 5: {  // save
      fs::create_directories(dir, ec);
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out) {
        result = kSaveLoadAccess;
        break;
      }
      for (uint32_t i = 0; buf && i < dataSize; ++i) out.put(static_cast<char>(lb(buf + i)));
      break;
    }
    case 6: case 7:  // delete
      if (!fs::remove_all(dir, ec)) result = kSaveDeleteNoData;
      break;
    default:  // sizes, file lists, ...: report success without data
      break;
  }
  sw(p + 0x1C, result);
}

void h_save_init(Cpu& c) {
  uint32_t p = arg(c, 0);
  dialog_start(g_savedata, p);
  savedata_run(p);
  ret(c, 0);
}

void h_save_status(Cpu& c) { ret(c, static_cast<uint32_t>(dialog_status(g_savedata))); }

void h_save_update(Cpu& c) {
  dialog_update(g_savedata);
  ret(c, 0);
}

void h_save_shutdown(Cpu& c) {
  g_savedata.state = 4;
  ret(c, 0);
}

// ---- sceOpenPSID ----

void h_openpsid(Cpu& c) {
  for (uint32_t i = 0; i < 16; ++i) sb(arg(c, 0) + i, static_cast<uint8_t>(0x30 + i));
  ret(c, 0);
}

// ---- StdioForUser ----

void h_stdin(Cpu& c) { ret(c, 0); }
void h_stdout(Cpu& c) { ret(c, 1); }
void h_stderr(Cpu& c) { ret(c, 2); }

void h_net_unavailable(Cpu& c) { ret(c, E_NET_UNAVAILABLE); }

}  // namespace

void register_misc() {
  const char* le = "LoadExecForUser";
  register_hle(le, 0x05572A5F, "sceKernelExitGame", h_exit_game);
  register_hle(le, 0x4AC57943, "sceKernelRegisterExitCallback", h_register_exit_callback);

  const char* rtc = "sceRtc";
  register_hle(rtc, 0x011F03C1, "sceRtcGetTickResolution", h_rtc_resolution);
  register_hle(rtc, 0x3F7AD767, "sceRtcGetCurrentTick", h_rtc_current_tick);
  register_hle(rtc, 0xE7C27D1B, "sceRtcGetCurrentClockLocalTime", h_rtc_local_time);

  const char* u = "UtilsForUser";
  register_hle(u, 0x79D1C3FA, "sceKernelDcacheWritebackAll", h_zero);
  register_hle(u, 0x34B9FA9E, "sceKernelDcacheWritebackInvalidateRange", h_zero);
  register_hle(u, 0x3EE30821, "sceKernelDcacheWritebackRange", h_zero);
  register_hle(u, 0xBFA98062, "sceKernelDcacheInvalidateRange", h_zero);
  register_hle(u, 0x920F104A, "sceKernelIcacheInvalidateAll", h_zero);
  register_hle(u, 0x37FB5C42, "sceKernelGetGPI", h_zero);
  register_hle(u, 0x71EC4271, "sceKernelLibcGettimeofday", h_gettimeofday);
  register_hle(u, 0x91E4F6A7, "sceKernelLibcClock", h_libc_clock);
  register_hle(u, 0x27CC57F0, "sceKernelLibcTime", h_libc_time);

  const char* s = "sceSuspendForUser";
  register_hle(s, 0x090CCB3F, "sceKernelPowerTick", h_zero);
  register_hle(s, 0x3E0271D3, "sceKernelVolatileMemLock", h_volatile_lock);
  register_hle(s, 0xA569E425, "sceKernelVolatileMemUnlock", h_zero);

  register_hle("sceUmdUser", 0x6B4A146C, "sceUmdGetDriveStat", h_umd_drive_stat);
  register_hle("sceUmdUser", 0x4A9E5E29, "sceUmdWaitDriveStatCB", h_zero);
  register_hle("sceUmdUser", 0xC6183D47, "sceUmdActivate", h_zero);

  register_hle("sceUtility", 0xA5DA2406, "sceUtilityGetSystemParamInt", h_sysparam_int);
  register_hle("sceUtility", 0x34B78343, "sceUtilityGetSystemParamString", h_sysparam_string);
  register_hle("sceUtility", 0xC629AF26, "sceUtilityLoadAvModule", h_zero);
  register_hle("sceUtility", 0x9A1C91D7, "sceUtilityMsgDialogGetStatus", h_msgdialog_status);
  register_hle("sceUtility", 0x2AD8E239, "sceUtilityMsgDialogInitStart", h_msgdialog_init);
  register_hle("sceUtility", 0x95FC253B, "sceUtilityMsgDialogUpdate", h_msgdialog_update);
  register_hle("sceUtility", 0x67AF3428, "sceUtilityMsgDialogShutdownStart", h_msgdialog_shutdown);
  register_hle("sceUtility", 0xF6269B82, "sceUtilityOskInitStart", h_osk_init);
  register_hle("sceUtility", 0xF3F76017, "sceUtilityOskGetStatus", h_osk_status);
  register_hle("sceUtility", 0x4B85C861, "sceUtilityOskUpdate", h_osk_update);
  register_hle("sceUtility", 0x3DFAEBA9, "sceUtilityOskShutdownStart", h_osk_shutdown);
  register_hle("sceUtility", 0x50C4CD57, "sceUtilitySavedataInitStart", h_save_init);
  register_hle("sceUtility", 0x8874DBE0, "sceUtilitySavedataGetStatus", h_save_status);
  register_hle("sceUtility", 0xD4B95FFB, "sceUtilitySavedataUpdate", h_save_update);
  register_hle("sceUtility", 0x9790B33C, "sceUtilitySavedataShutdownStart", h_save_shutdown);

  register_hle("scePower", 0x04B7766E, "scePowerRegisterCallback", h_zero);
  register_hle("sceImpose", 0x36AA6E91, "sceImposeSetLanguageMode", h_zero);
  register_hle("sceOpenPSID", 0xC69BEBCE, "sceOpenPSIDGetOpenPSID", h_openpsid);

  register_hle("StdioForUser", 0x172D316E, "sceKernelStdin", h_stdin);
  register_hle("StdioForUser", 0xA6BAB2E9, "sceKernelStdout", h_stdout);
  register_hle("StdioForUser", 0xF78BA90A, "sceKernelStderr", h_stderr);

  // Multiplayer and online features are not supported; report the network as unavailable.
  for (const char* m : {"sceNet", "sceNetInet", "sceNetResolver", "sceNetAdhoc", "sceNetAdhocctl", "sceNetApctl",
                        "sceHttp", "sceSsl"})
    register_hle_fallback(m, h_net_unavailable);
  register_hle_fallback("sceWlanDrv", h_zero);
}

}  // namespace k
}  // namespace psp

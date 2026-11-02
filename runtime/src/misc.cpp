// Small libraries: loadexec, rtc, utils, power, umd, utility, net stubs.
#include <chrono>
#include <ctime>
#include <map>
#include <string>

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
void h_mpeg_query_mem(Cpu& c) { ret(c, 0x10000); }
void h_mpeg_rb_mem(Cpu& c) { ret(c, arg(c, 0) * 2104); }
void h_mpeg_stream(Cpu& c) { ret(c, 1); }
void h_mpeg_atrac_size(Cpu& c) {
  put32(arg(c, 1), 0x2000);
  put32(arg(c, 2), 0x2000);
  ret(c, 0);
}
void h_mpeg_stream_offset(Cpu& c) {
  put32(arg(c, 2), 0);
  ret(c, 0);
}
// No video is ever produced, so streams report that they have run dry.
void h_mpeg_no_data(Cpu& c) { ret(c, 0x80618001); }
// sceMpegCreate(mpeg*, data, size, ringbuffer, ...): the handle is the work area itself.
void h_mpeg_create(Cpu& c) {
  put32(arg(c, 0), arg(c, 1));
  ret(c, 0);
}

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
  register_hle("sceUtility", 0x9A1C91D7, "sceUtilityMsgDialogGetStatus", h_zero);

  // The intro movies are not decoded yet; these only let the game set up and tear down its player.
  register_hle("sceMpeg", 0x682A619B, "sceMpegInit", h_zero);
  register_hle("sceMpeg", 0x874624D6, "sceMpegFinish", h_zero);
  register_hle("sceMpeg", 0xC132E22F, "sceMpegQueryMemSize", h_mpeg_query_mem);
  register_hle("sceMpeg", 0xD7A29F46, "sceMpegRingbufferQueryMemSize", h_mpeg_rb_mem);
  register_hle("sceMpeg", 0x37295ED8, "sceMpegRingbufferConstruct", h_zero);
  register_hle("sceMpeg", 0x13407F13, "sceMpegRingbufferDestruct", h_zero);
  register_hle("sceMpeg", 0xD8C5F121, "sceMpegCreate", h_mpeg_create);
  register_hle("sceMpeg", 0x167AFD9E, "sceMpegInitAu", h_zero);
  register_hle("sceMpeg", 0xF8DCB679, "sceMpegQueryAtracEsSize", h_mpeg_atrac_size);
  register_hle("sceMpeg", 0xCEB870B1, "sceMpegFreeAvcEsBuf", h_zero);
  register_hle("sceMpeg", 0x21FF80E4, "sceMpegQueryStreamOffset", h_mpeg_stream_offset);
  register_hle("sceMpeg", 0x611E9E11, "sceMpegQueryStreamSize", h_zero);
  register_hle("sceMpeg", 0xB5F6DC87, "sceMpegRingbufferPut", h_zero);
  register_hle("sceMpeg", 0xFE246728, "sceMpegGetAvcAu", h_mpeg_no_data);
  register_hle("sceMpeg", 0x0E3C2E9D, "sceMpegAvcDecode", h_mpeg_no_data);
  register_hle("sceMpeg", 0xE1CE83A7, "sceMpegGetAtracAu", h_mpeg_no_data);
  register_hle("sceMpeg", 0x800C44DF, "sceMpegAtracDecode", h_mpeg_no_data);
  register_hle("sceMpeg", 0x606A4649, "sceMpegDelete", h_zero);
  register_hle("sceMpeg", 0x42560F23, "sceMpegRegistStream", h_mpeg_stream);
  register_hle("sceMpeg", 0x591A4AA2, "sceMpegUnRegistStream", h_zero);
  register_hle("sceMpeg", 0xA780CF7E, "sceMpegMallocAvcEsBuf", h_mpeg_stream);
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

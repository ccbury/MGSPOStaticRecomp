// mgspo-launcher: drives the whole extract -> lift -> build -> run pipeline from the user's own ISO.
// Nothing derived from the game is ever written next to the sources; everything goes to a work
// directory (default %LOCALAPPDATA%\MGSPO\work) and is rebuilt locally.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using std::string;
using std::wstring;

#ifndef MGSPO_SOURCE_DIR
#define MGSPO_SOURCE_DIR ""
#endif
#ifndef MGSPO_TOOL_REVISION
#define MGSPO_TOOL_REVISION "unversioned"
#endif

namespace {

std::function<void(const wstring&)> g_output;
std::atomic<bool> g_cancel{false};
bool g_background = false;

// ---- strings and console ----------------------------------------------------------------------

string toUtf8(const wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

wstring toWide(const string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

void out(const wstring& w, bool err = false) {
  if (g_output) g_output(w);
  HANDLE h = GetStdHandle(err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  DWORD written = 0;
  if (GetConsoleMode(h, &mode)) {
    WriteConsoleW(h, w.data(), static_cast<DWORD>(w.size()), &written, nullptr);
  } else {
    string s = toUtf8(w);
    WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
  }
}

void say(const wstring& s) { out(s + L"\n"); }
void warn(const wstring& s) { out(L"warning: " + s + L"\n", true); }

struct Fatal {
  wstring msg;
};
[[noreturn]] void die(const wstring& msg) { throw Fatal{msg}; }

wstring lower(wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return s;
}

// 8.3 names sidestep quoting trouble in older build scripts when the path has spaces.
fs::path shortPath(const fs::path& p) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetShortPathNameW(p.c_str(), buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return p;
  return fs::path(buf);
}

// ---- simple key=value store -------------------------------------------------------------------

struct Ini {
  fs::path file;
  std::map<string, string> kv;

  void load(const fs::path& f) {
    file = f;
    kv.clear();
    std::ifstream in(f, std::ios::binary);
    if (!in && fs::exists(f)) die(L"cannot read settings: " + f.wstring());
    string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto eq = line.find('=');
      if (eq == string::npos || line.empty() || line[0] == '#') continue;
      kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    if (in.bad()) die(L"error reading settings: " + f.wstring());
  }
  void save() const {
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    if (ec) die(L"cannot create settings directory: " + toWide(ec.message()));
    fs::path staging = file.wstring() + L".pending-" + std::to_wstring(GetCurrentProcessId());
    std::ofstream o(staging, std::ios::binary | std::ios::trunc);
    for (auto& e : kv) o << e.first << '=' << e.second << "\n";
    o.close();
    if (!o) die(L"cannot save settings: " + file.wstring());
    if (!MoveFileExW(staging.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      fs::remove(staging, ec);
      die(L"cannot replace settings; previous file was kept: " + file.wstring());
    }
  }
  string get(const string& k) const {
    auto it = kv.find(k);
    return it == kv.end() ? string() : it->second;
  }
  void set(const string& k, const string& v) { kv[k] = v; }
  void erase(const string& k) { kv.erase(k); }
};

// ---- processes --------------------------------------------------------------------------------

wstring quoteArg(const wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
  wstring r = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++slashes;
    } else if (c == L'"') {
      r.append(slashes * 2 + 1, L'\\');
      r += L'"';
      slashes = 0;
    } else {
      r.append(slashes, L'\\');
      slashes = 0;
      r += c;
    }
  }
  r.append(slashes * 2, L'\\');
  r += L'"';
  return r;
}

using LineFn = std::function<void(const string&)>;

// Runs a program with stdout+stderr merged, echoing to the console and optionally a log file.
// If rawCmdLine is set it is used verbatim as the command line (needed for cmd.exe /s /c).
int runProcess(const wstring& exe, const std::vector<wstring>& args, const fs::path& cwd, const fs::path& logFile,
               const LineFn& onLine, bool echo = true, const wstring& rawCmdLine = L"") {
  if (g_cancel) die(L"Operation cancelled. Incomplete build steps will be retried next time.");
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) die(L"could not create a pipe");
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  wstring cmd = rawCmdLine.empty() ? quoteArg(exe) : rawCmdLine;
  if (rawCmdLine.empty())
    for (auto& a : args) cmd += L" " + quoteArg(a);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  if (input == INVALID_HANDLE_VALUE) {
    CloseHandle(rd); CloseHandle(wr);
    die(L"could not open null input for child process");
  }
  si.hStdInput = input;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  const wchar_t* app = (rawCmdLine.empty() && exe.find_first_of(L"\\/") != wstring::npos) ? exe.c_str() : nullptr;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    if (job) CloseHandle(job);
    CloseHandle(input);
    CloseHandle(rd); CloseHandle(wr);
    die(L"could not create process cancellation job");
  }
  struct Cleanup {
    HANDLE pipe, job;
    PROCESS_INFORMATION& process;
    ~Cleanup() {
      CloseHandle(job);  // Also stops descendants if a callback or file operation throws.
      CloseHandle(pipe);
      if (process.hThread) CloseHandle(process.hThread);
      if (process.hProcess) CloseHandle(process.hProcess);
    }
  } cleanup{rd, job, pi};
  BOOL ok = CreateProcessW(app, buf.data(), nullptr, nullptr, TRUE,
                           CREATE_SUSPENDED | (g_background ? CREATE_NO_WINDOW : 0), nullptr,
                           cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  CloseHandle(wr);
  CloseHandle(input);
  if (!ok) {
    die(L"could not start " + (exe.empty() ? cmd : exe) + L" (error " + std::to_wstring(GetLastError()) + L")");
  }
  if (!AssignProcessToJobObject(job, pi.hProcess)) {
    TerminateProcess(pi.hProcess, 1);
    die(L"could not contain child process for safe cancellation");
  }
  if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) die(L"could not resume owned child process");

  std::ofstream log;
  if (!logFile.empty()) {
    std::error_code ec;
    fs::create_directories(logFile.parent_path(), ec);
    log.open(logFile, std::ios::binary | std::ios::app);
    if (!log) die(L"cannot open operation log: " + logFile.wstring());
  }
  string pending;
  char chunk[8192];
  DWORD got = 0;
  for (;;) {
    if (g_cancel) { TerminateJobObject(job, ERROR_CANCELLED); break; }
    DWORD available = 0;
    if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &available, nullptr)) break;
    if (!available) {
      if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &available, nullptr) || !available) break;
      }
      continue;
    }
    if (!ReadFile(rd, chunk, std::min<DWORD>(sizeof(chunk), available), &got, nullptr) || !got) break;
    if (echo) {
      DWORD w = 0;
      WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), chunk, got, &w, nullptr);
      if (g_output) g_output(toWide(string(chunk, got)));
    }
    if (log.is_open()) {
      log.write(chunk, got);
      if (!log) die(L"cannot write operation log: " + logFile.wstring());
    }
    if (onLine) pending.append(chunk, got);
    if (pending.size() > 1024 * 1024) die(L"Child process produced an oversized output line; operation stopped.");
    size_t nl;
    while ((nl = pending.find('\n')) != string::npos) {
      string line = pending.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (onLine) onLine(line);
      pending.erase(0, nl + 1);
    }
  }
  if (!pending.empty() && onLine) onLine(pending);
  while (WaitForSingleObject(pi.hProcess, 30) == WAIT_TIMEOUT)
    if (g_cancel) TerminateJobObject(job, ERROR_CANCELLED);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  if (g_cancel) die(L"Operation cancelled. Saves/settings were kept; incomplete steps will be retried.");
  return static_cast<int>(code);
}

// ---- state ------------------------------------------------------------------------------------

struct Options {
  string cmd = "run";
  wstring iso;
  wstring work;
  wstring source;
  wstring configPath;
  string controls;
  std::vector<wstring> gameArgs;
  bool noRelaunch = false;
  bool saves = false;
  bool all = false;
};

struct Ctx {
  Options opt;
  Ini config;  // user level: iso + work dir
  Ini state;   // inside the work dir: stamps for each cached step
  fs::path exeDir, source, work, iso;
  fs::path disc, boot, gen, build, save, logs;
};

struct OperationLock {
  HANDLE handle = INVALID_HANDLE_VALUE;
  OperationLock() = default;
  OperationLock(const OperationLock&) = delete;
  OperationLock& operator=(const OperationLock&) = delete;
  void release() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
  }
  void acquire(const fs::path& work) {
    handle = CreateFileW((work / L".launcher-operation").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      die(L"Work directory is busy in another launcher/game, or is not writable. Wait for it to finish.");
  }
  ~OperationLock() { release(); }
};

// Stage modules lifted by default, relative to PSP_GAME/USRDIR/stage.
const char* kDefaultStages[] = {"init/init.prx",       "r_main01/r_main01.prx", "r_main02/r_main02.prx",
                                "r_net01/r_net01.prx", "gsmenu/gsmenu.prx",     "title/title.prx",
                                "flashdemo/flashdemo.prx", "st04vo/st04vo.prx"};
const char* kHelpers[] = {"zlibdec.prx", "kjfs.prx", "sound2.prx"};

string fileStamp(const fs::path& p) {
  std::error_code ec;
  auto sz = fs::file_size(p, ec);
  if (ec) return "missing";
  auto t = fs::last_write_time(p, ec).time_since_epoch().count();
  return std::to_string(sz) + ":" + std::to_string(static_cast<long long>(t));
}

fs::path exePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return fs::path(wstring(buf, n));
}

fs::path localAppData() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
  if (n > 0 && n < std::size(buf)) return fs::path(wstring(buf, n));
  return exePath().parent_path();
}

fs::path defaultWorkDir() { return localAppData() / L"MGSPO" / L"work"; }
fs::path configFile() { return localAppData() / L"MGSPO" / L"launcher.ini"; }

// ---- paths and ISO selection ------------------------------------------------------------------

wstring pickIso() {
  wchar_t file[MAX_PATH * 2] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = L"PSP disc image (*.iso)\0*.iso\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = static_cast<DWORD>(std::size(file));
  ofn.lpstrTitle = L"Select your Metal Gear Solid: Portable Ops disc image";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) return {};
  return file;
}

void resolvePaths(Ctx& c) {
  c.exeDir = exePath().parent_path();
  c.config.load(c.opt.configPath.empty() ? configFile() : fs::path(c.opt.configPath));

  wstring workStr = c.opt.work;
  if (workStr.empty()) workStr = toWide(c.config.get("work"));
  c.work = workStr.empty() ? defaultWorkDir() : fs::path(workStr);
  std::error_code ec;
  c.work = fs::absolute(c.work, ec);
  fs::create_directories(c.work, ec);
  if (!fs::is_directory(c.work)) die(L"cannot create work directory " + c.work.wstring());
  if (!c.opt.work.empty() && toUtf8(c.work.wstring()) != c.config.get("work")) {
    c.config.set("work", toUtf8(c.work.wstring()));
  }
  c.work = shortPath(c.work);

  c.disc = c.work / L"disc";
  c.boot = c.work / L"boot";
  c.gen = c.work / L"gen";
  c.build = c.work / L"build";
  c.save = c.work / L"save";
  c.logs = c.work / L"logs";
  c.state.load(c.work / L"state.ini");

  // An explicit/remembered source must never silently fall back to another checkout.
  std::vector<fs::path> cands;
  if (!c.opt.source.empty()) cands.emplace_back(c.opt.source);
  else if (!c.config.get("source").empty()) cands.emplace_back(toWide(c.config.get("source")));
  if (cands.empty()) {
    cands.emplace_back(toWide(MGSPO_SOURCE_DIR));
    for (fs::path p = c.exeDir; !p.empty() && p != p.parent_path(); p = p.parent_path()) cands.push_back(p);
  }
  for (auto& p : cands) {
    if (!p.empty() && fs::exists(p / L"CMakeLists.txt", ec) && fs::exists(p / L"runtime" / L"src" / L"game_main.cpp", ec)) {
      c.source = shortPath(fs::absolute(p, ec));
      break;
    }
  }
}

void resolveIso(Ctx& c) {
  wstring iso = c.opt.iso;
  if (iso.empty()) iso = toWide(c.config.get("iso"));
  if (iso.empty()) {
    say(L"No disc image given, opening a file picker...");
    iso = pickIso();
    if (iso.empty())
      die(L"no ISO selected. Run again with the path, e.g.  mgspo-launcher run \"D:\\games\\PortableOps.iso\"");
  }
  std::error_code ec;
  c.iso = fs::absolute(fs::path(iso), ec);
  if (!fs::is_regular_file(c.iso, ec))
    die(L"ISO not found: " + c.iso.wstring() + L"\nChoose another image in the desktop UI, or pass --iso <file>.");
  if (toUtf8(c.iso.wstring()) != c.config.get("iso")) {
    c.config.set("iso", toUtf8(c.iso.wstring()));
    c.config.save();
  }
}

// ---- toolchain --------------------------------------------------------------------------------

wstring searchExe(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = SearchPathW(nullptr, name, L".exe", static_cast<DWORD>(std::size(buf)), buf, nullptr);
  return n ? wstring(buf, n) : wstring();
}

void prependPath(const fs::path& dir) {
  std::vector<wchar_t> buf(32768);
  DWORD n = GetEnvironmentVariableW(L"PATH", buf.data(), static_cast<DWORD>(buf.size()));
  wstring cur(buf.data(), n);
  SetEnvironmentVariableW(L"PATH", (dir.wstring() + L";" + cur).c_str());
}

wstring findVsInstall() {
  wchar_t pf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, static_cast<DWORD>(std::size(pf)));
  if (!n) return {};
  fs::path vswhere = fs::path(pf) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
  if (!fs::exists(vswhere)) return {};
  wstring found;
  auto grab = [&](const string& l) {
    if (found.empty() && !l.empty()) found = toWide(l);
  };
  runProcess(vswhere.wstring(),
             {L"-latest", L"-products", L"*", L"-requires", L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
              L"-property", L"installationPath", L"-utf8"},
             {}, {}, grab, false);
  return found;
}

bool haveX64Toolchain() {
  wchar_t target[32] = {};
  DWORD n = GetEnvironmentVariableW(L"VSCMD_ARG_TGT_ARCH", target, static_cast<DWORD>(std::size(target)));
  if (!n || n >= std::size(target) || lower(target) != L"x64") return false;
  fs::path compiler(searchExe(L"cl.exe"));
  return !compiler.empty() && lower(compiler.parent_path().filename().wstring()) == L"x64" &&
         GetEnvironmentVariableW(L"VCINSTALLDIR", nullptr, 0) > 0;
}

void setupToolchain() {
  say(L"Looking for the C++ toolchain...");
  wstring vs;
  if (!haveX64Toolchain()) {
    vs = findVsInstall();
    if (vs.empty()) {
      die(L"Could not find Visual Studio or Build Tools with the C++ x64 tools.\n"
          L"Install \"Build Tools for Visual Studio 2022\" (https://aka.ms/vs/17/release/vs_BuildTools.exe) and tick\n"
          L"\"Desktop development with C++\" (it includes CMake and Ninja), then run this again.");
    }
    fs::path vcvars = fs::path(vs) / L"VC" / L"Auxiliary" / L"Build" / L"vcvars64.bat";
    if (!fs::exists(vcvars)) die(L"vcvars64.bat is missing from " + vs + L"; repair the C++ workload in the VS installer.");
    say(L"  selecting x64 tools from " + vs);
    wstring cmdline = L"cmd.exe /s /c \"call \"" + vcvars.wstring() + L"\" >nul 2>&1 && set\"";
    std::vector<std::pair<wstring, wstring>> env;
    int rc = runProcess(L"", {}, {}, {}, [&](const string& l) {
      wstring w = toWide(l);
      auto eq = w.find(L'=');
      if (eq != wstring::npos && eq > 0) env.emplace_back(w.substr(0, eq), w.substr(eq + 1));
    }, false, cmdline);
    if (rc != 0 || env.empty()) die(L"vcvars64.bat failed; open the Visual Studio Installer and repair the C++ workload.");
    for (auto& e : env) SetEnvironmentVariableW(e.first.c_str(), e.second.c_str());
    if (!haveX64Toolchain())
      die(L"An x64 compiler is not available after running vcvars64.bat. Repair the C++ x64 tools in the VS installer.");
  } else {
    say(L"  already inside an x64 developer prompt");
  }

  // VS ships its own CMake and Ninja; use them if the system doesn't have its own.
  if (vs.empty()) vs = findVsInstall();
  fs::path cmakeDir = fs::path(vs) / L"Common7" / L"IDE" / L"CommonExtensions" / L"Microsoft" / L"CMake";
  if (searchExe(L"cmake.exe").empty() && !vs.empty()) prependPath(cmakeDir / L"CMake" / L"bin");
  if (searchExe(L"ninja.exe").empty() && !vs.empty()) prependPath(cmakeDir / L"Ninja");
  if (searchExe(L"cmake.exe").empty())
    die(L"CMake was not found. Install CMake 3.16+ from https://cmake.org/download/ (tick \"add to PATH\"),\n"
        L"or add \"C++ CMake tools for Windows\" in the Visual Studio Installer.");
  if (searchExe(L"ninja.exe").empty())
    die(L"Ninja was not found. Install it with  winget install Ninja-build.Ninja  or add\n"
        L"\"C++ CMake tools for Windows\" in the Visual Studio Installer.");
}

// ---- pipeline steps ---------------------------------------------------------------------------

fs::path toolExe(const Ctx& c, const wchar_t* name) {
  fs::path p = c.exeDir / name;
  if (!fs::exists(p)) die(wstring(name) + L" not found next to the launcher (" + c.exeDir.wstring() + L"). Build the whole project first.");
  return p;
}

std::vector<string> splitList(const string& s) {
  std::vector<string> r;
  std::stringstream ss(s);
  string item;
  while (std::getline(ss, item, ';'))
    if (!item.empty()) r.push_back(item);
  return r;
}

string joinList(const std::vector<string>& v) {
  string r;
  for (auto& s : v) r += (r.empty() ? "" : ";") + s;
  return r;
}

// Default stages plus any the game asked for in earlier runs.
std::vector<string> stageList(const Ctx& c) {
  std::vector<string> r;
  for (auto s : kDefaultStages) r.push_back(s);
  for (auto& s : splitList(c.state.get("extra_stages")))
    if (std::find(r.begin(), r.end(), s) == r.end()) r.push_back(s);
  return r;
}

string isoId(const Ctx& c) { return toUtf8(c.iso.wstring()) + "|" + fileStamp(c.iso); }

string sourceRevision(const Ctx& c) {
  if (!fs::exists(c.source / L".git")) return "unversioned";
  string revision;
  if (runProcess(L"git.exe", {L"rev-parse", L"HEAD"}, c.source, {}, [&](const string& l) { revision = l; }, false) != 0)
    die(L"Cannot read source revision. Install Git for Windows and check this checkout.");
  if (revision.size() != 40 && revision.size() != 64) die(L"Invalid source revision.");
  return revision;
}

string liftStamp(const Ctx& c) {
  return isoId(c) + "|" + joinList(stageList(c)) + "|" + fileStamp(toolExe(c, L"mgspo-lift.exe")) +
         "|" + sourceRevision(c);
}

string buildStamp(const Ctx& c) {
  return liftStamp(c) + "|pipeline-2|" + fileStamp(c.exeDir / L"mgspo-launcher.exe") +
         "|" + toUtf8(c.source.wstring());
}

bool cachedCompilerIsX64(const Ctx& c) {
  fs::path files = c.build / L"CMakeFiles";
  if (!fs::is_directory(files)) return false;
  bool found = false;
  for (const auto& entry : fs::directory_iterator(files)) {
    if (!entry.is_directory()) continue;
    fs::path file = entry.path() / L"CMakeCXXCompiler.cmake";
    if (!fs::exists(file)) continue;
    std::ifstream in(file, std::ios::binary);
    if (!in) die(L"cannot read " + file.wstring());
    bool x64 = false, pointer64 = false;
    string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line == "set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)") x64 = true;
      if (line == "set(CMAKE_CXX_SIZEOF_DATA_PTR \"8\")") pointer64 = true;
    }
    if (in.bad()) die(L"error reading " + file.wstring());
    if (!x64 || !pointer64) return false;
    found = true;
  }
  return found;
}

bool buildCacheMatches(const Ctx& c) {
  fs::path file = c.build / L"CMakeCache.txt";
  if (!fs::exists(file)) return false;
  std::ifstream in(file, std::ios::binary);
  if (!in) die(L"cannot read " + file.wstring());
  std::map<string, string> entries;
  string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto eq = line.find('=');
    if (eq != string::npos) entries[line.substr(0, eq)] = line.substr(eq + 1);
  }
  if (in.bad()) die(L"error reading " + file.wstring());
  auto sameDirectory = [&](const char* key, const fs::path& expected) {
    auto it = entries.find(key);
    if (it == entries.end() || it->second.empty() || expected.empty()) return false;
    std::error_code ec;
    return fs::equivalent(fs::path(toWide(it->second)), expected, ec) && !ec;
  };
  // equivalent() also recognises the 8.3 paths used by the launcher.
  return sameDirectory("CMAKE_HOME_DIRECTORY:INTERNAL", c.source) &&
         sameDirectory("CMAKE_CACHEFILE_DIR:INTERNAL", c.build) &&
         entries["CMAKE_GENERATOR:INTERNAL"] == "Ninja" && cachedCompilerIsX64(c);
}

void resetBuildCache(const Ctx& c) {
  // Keep compiled objects, extracted files, generated sources and saves.
  for (const auto& name : {L"CMakeCache.txt", L"CMakeFiles"}) {
    fs::path path = c.build / name;
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) die(L"cannot reset " + path.wstring() + L": " + toWide(ec.message()));
  }
}

void validateIso(Ctx& c) {
  say(L"[1/4] Checking the disc image");
  std::ifstream f(c.iso, std::ios::binary);
  char magic[4] = {};
  f.read(magic, 4);
  if (string(magic, 4) == "CISO" || string(magic, 4) == "ZISO")
    die(L"This looks like a compressed CSO/ZSO image. Convert it to a plain .iso first (e.g. with maxcso --decompress).");

  string id;
  int rc = runProcess(toolExe(c, L"mgspo-extract.exe").wstring(), {L"info", c.iso.wstring()}, {}, {}, [&](const string& l) {
    auto p = l.find("game id");
    if (p == string::npos) return;
    auto colon = l.find(':', p);
    if (colon == string::npos) return;
    id = l.substr(colon + 1);
    id.erase(0, id.find_first_not_of(" \t"));
    id.erase(id.find_last_not_of(" \t\r\n") + 1);
  }, false);
  if (rc != 0) die(L"mgspo-extract could not read " + c.iso.wstring() + L" (is it a plain, uncompressed PSP .iso?)");
  if (id != "ULUS10202")
    die(L"This disc is \"" + toWide(id.empty() ? "unknown" : id) +
        L"\", but the recompiler targets Metal Gear Solid: Portable Ops (US, ULUS10202).");
  say(L"  ULUS10202 - Metal Gear Solid: Portable Ops, ok");
}

void stepExtract(Ctx& c) {
  if (c.state.get("extracted") == isoId(c) && fs::exists(c.boot / L"boot.elf") &&
      fs::exists(c.disc / L"PSP_GAME" / L"USRDIR"))
    return say(L"[1-2/4] Disc already extracted (cached)");
  validateIso(c);
  say(L"[2/4] Extracting disc files (about 1.3 GB, this takes a minute)");
  c.state.erase("extracted");
  c.state.erase("lifted");
  c.state.erase("built");
  c.state.save();
  std::error_code ec;
  fs::remove_all(c.disc, ec);
  fs::remove_all(c.boot, ec);
  fs::path log = c.logs / L"extract.log";
  fs::remove(log, ec);
  fs::path ex = toolExe(c, L"mgspo-extract.exe");
  if (runProcess(ex.wstring(), {L"extract", c.iso.wstring(), c.disc.wstring(), L"PSP_GAME"}, {}, log, nullptr) != 0)
    die(L"extracting the disc failed, see " + log.wstring());
  if (runProcess(ex.wstring(), {L"eboot", c.iso.wstring(), c.boot.wstring()}, {}, log, nullptr) != 0)
    die(L"extracting boot.elf failed, see " + log.wstring());
  if (!fs::exists(c.boot / L"boot.elf")) die(L"boot.elf was not produced; see " + log.wstring());
  c.state.set("extracted", isoId(c));
  c.state.save();
}

void stepLift(Ctx& c, bool force) {
  if (!force && c.state.get("lifted") == liftStamp(c) && fs::exists(c.gen))
    return say(L"[3/4] Recompiled code is up to date (cached)");
  auto stages = stageList(c);
  say(L"[3/4] Lifting boot.elf, " + std::to_wstring(std::size(kHelpers)) + L" helper modules and " +
      std::to_wstring(stages.size()) + L" stage modules to C++");
  c.state.erase("lifted");
  c.state.erase("built");
  c.state.save();
  std::error_code ec;
  fs::remove_all(c.gen, ec);

  fs::path usr = c.disc / L"PSP_GAME" / L"USRDIR";
  std::vector<wstring> args = {(c.boot / L"boot.elf").wstring(), c.gen.wstring()};
  for (auto h : kHelpers) {
    args.push_back(L"--helper");
    args.push_back((usr / L"modules" / h).wstring());
  }
  for (auto& s : stages) {
    fs::path p = usr / L"stage" / fs::path(toWide(s));
    if (!fs::exists(p)) {
      warn(L"stage module not on disc, skipping: " + p.wstring());
      continue;
    }
    args.push_back(L"--stage");
    args.push_back(p.wstring());
  }
  fs::path log = c.logs / L"lift.log";
  fs::remove(log, ec);
  if (runProcess(toolExe(c, L"mgspo-lift.exe").wstring(), args, {}, log, nullptr) != 0)
    die(L"lifting failed, see " + log.wstring());
  c.state.set("lifted", liftStamp(c));
  c.state.save();
}

void stepBuild(Ctx& c, bool force) {
  fs::path game = c.build / L"mgspo-game.exe";
  bool cacheMatches = buildCacheMatches(c);
  if (!force && cacheMatches && c.state.get("built") == buildStamp(c) && fs::exists(game))
    return say(L"[4/4] Game executable is up to date (cached)");
  say(L"[4/4] Building the game (the first build compiles a lot of generated code and can take a long while)");
  c.state.erase("built");
  c.state.save();
  setupToolchain();
  if (!cacheMatches) {
    if (fs::exists(c.build / L"CMakeCache.txt"))
      say(L"  Source checkout, build directory or compiler architecture changed; resetting CMake configuration (saves are kept).");
    resetBuildCache(c);
  }
  fs::path log = c.logs / L"build.log";
  std::error_code ec;
  fs::remove(log, ec);
  int rc = runProcess(L"cmake.exe",
                      {L"-S", c.source.wstring(), L"-B", c.build.wstring(), L"-G", L"Ninja", L"-DCMAKE_BUILD_TYPE=Release",
                       L"-DMGSPO_GENERATED_DIR=" + c.gen.generic_wstring(), L"-DMGSPO_BUILD_TESTS=OFF",
                       L"-DCMAKE_CXX_COMPILER=" + searchExe(L"cl.exe")},
                      {}, log, nullptr);
  if (rc != 0) die(L"cmake configure failed, see " + log.wstring());
  rc = runProcess(L"cmake.exe", {L"--build", c.build.wstring(), L"--target", L"mgspo-game", L"--parallel", L"1"}, {}, log, nullptr);
  if (rc != 0) die(L"build failed, see " + log.wstring());
  if (!fs::exists(game)) die(L"build finished but " + game.wstring() + L" is missing");
  c.state.set("built", buildStamp(c));
  c.state.save();
}

// Maps a "not lifted" log entry (disc path and file size) to a stage path relative to USRDIR/stage.
string stageFromLog(const Ctx& c, const string& path, uintmax_t size) {
  string p = path;
  std::replace(p.begin(), p.end(), '\\', '/');
  string low = p;
  std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  const string key = "/usrdir/stage/";
  auto pos = low.find(key);
  fs::path stageRoot = c.disc / L"PSP_GAME" / L"USRDIR" / L"stage";
  std::error_code ec;
  if (pos != string::npos) {
    string rel = p.substr(pos + key.size());
    if (fs::exists(stageRoot / fs::path(toWide(rel)), ec)) return rel;
  }
  if (size) {
    for (auto it = fs::recursive_directory_iterator(stageRoot, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (it->is_regular_file() && lower(it->path().extension().wstring()) == L".prx" && it->file_size() == size)
        return toUtf8(fs::relative(it->path(), stageRoot).generic_wstring());
    }
  }
  return {};
}

void ensureBuilt(Ctx& c, bool forceLift) {
  if (fs::exists(c.source / L".git" / L"mgspo-owner") && sourceRevision(c) != MGSPO_TOOL_REVISION)
    die(L"These launcher/tools were built for a different source revision. Use the updated UI, "
        L"or Check updates then Update / rebuild before running the managed source.");
  stepExtract(c);
  stepLift(c, forceLift);
  stepBuild(c, forceLift);
}

std::vector<wstring> gameArguments(const Ctx& c) {
  std::vector<wstring> args = {(c.boot / L"boot.elf").wstring(), L"--disc", c.disc.wstring(), L"--iso", c.iso.wstring(),
                               L"--save", c.save.wstring()};
  string controls = c.opt.controls.empty() ? c.config.get("controls") : c.opt.controls;
  if (controls.empty()) controls = "modern";
  if (controls != "modern" && controls != "legacy") die(L"Controls must be modern or legacy.");
  args.insert(args.end(), {L"--controls", toWide(controls)});
  for (auto& a : c.opt.gameArgs) args.push_back(a);
  return args;
}

int launchGame(Ctx& c, std::set<string>& unlifted) {
  std::error_code ec;
  fs::create_directories(c.save, ec);
  auto args = gameArguments(c);
  fs::path log = c.logs / L"game.log";
  fs::remove(log, ec);
  return runProcess((c.build / L"mgspo-game.exe").wstring(), args, c.work, log, [&](const string& l) {
    // "[module] load <path> -> <id> (not lifted: <n> bytes, hash ...)"
    const string head = "[module] load ";
    auto p = l.find("(not lifted:");
    if (p == string::npos || l.compare(0, head.size(), head) != 0) return;
    auto arrow = l.find(" -> ");
    string path = arrow == string::npos ? string() : l.substr(head.size(), arrow - head.size());
    uintmax_t size = std::strtoull(l.c_str() + p + 12, nullptr, 10);
    string rel = stageFromLog(c, path, size);
    if (!rel.empty()) unlifted.insert(rel);
    else warn(L"game loaded an unlifted module that is not a stage: " + toWide(path));
  });
}

int cmdRun(Ctx& c) {
  resolveIso(c);
  ensureBuilt(c, false);
  for (int round = 0;; ++round) {
    say(L"Starting the game (saves are kept in " + c.save.wstring() + L")");
    std::set<string> missing;
    int rc = launchGame(c, missing);
    auto extra = splitList(c.state.get("extra_stages"));
    auto all = stageList(c);
    bool added = false;
    for (auto& m : missing) {
      if (std::find(all.begin(), all.end(), m) != all.end()) continue;
      say(L"The game asked for a stage that is not lifted yet: " + toWide(m));
      extra.push_back(m);
      added = true;
    }
    if (!added) {
      say(rc == 0 ? wstring(L"Game exited.")
                  : L"Game exited with code " + std::to_wstring(rc) + L". Log: " + (c.logs / L"game.log").wstring());
      return rc;
    }
    c.state.set("extra_stages", joinList(extra));
    c.state.save();
    if (rc == 0 || c.opt.noRelaunch || round >= 5) {
      say(L"Added to the stage list; the next run will lift and rebuild automatically.");
      return rc;
    }
    say(L"Lifting the new stage(s), rebuilding and relaunching...");
    ensureBuilt(c, false);
  }
}

int cmdSetup(Ctx& c) {
  resolveIso(c);
  ensureBuilt(c, false);
  say(L"Setup complete. Start the game with:  mgspo-launcher run");
  return 0;
}

int cmdBuild(Ctx& c) {
  resolveIso(c);
  ensureBuilt(c, false);
  return 0;
}

int cmdRebuild(Ctx& c) {
  resolveIso(c);
  std::error_code ec;
  fs::remove_all(c.build, ec);
  stepExtract(c);
  stepLift(c, true);
  stepBuild(c, true);
  return 0;
}

int cmdClean(Ctx& c) {
  std::error_code ec;
  say(L"Removing generated code, build output and extracted disc files from " + c.work.wstring());
  fs::remove_all(c.gen, ec);
  fs::remove_all(c.build, ec);
  fs::remove_all(c.disc, ec);
  fs::remove_all(c.boot, ec);
  c.state.erase("extracted");
  c.state.erase("lifted");
  c.state.erase("built");
  c.state.save();
  if (c.opt.saves || c.opt.all) {
    fs::remove_all(c.save, ec);
    say(L"Removed saves");
  } else {
    say(L"Saves were kept (use --saves to remove them too)");
  }
  if (c.opt.all) {
    fs::remove_all(c.logs, ec);
    fs::remove(c.work / L"state.ini", ec);
    fs::remove(c.config.file, ec);
    say(L"Removed logs and remembered settings");
  }
  return 0;
}

int cmdStatus(Ctx& c) {
  say(L"work dir : " + c.work.wstring());
  say(L"source   : " + (c.source.empty() ? wstring(L"(not found)") : c.source.wstring()));
  string iso = c.opt.iso.empty() ? c.config.get("iso") : toUtf8(c.opt.iso);
  say(L"iso      : " + (iso.empty() ? wstring(L"(not set)") : toWide(iso)));
  if (!iso.empty() && !c.exeDir.empty()) {
    c.iso = fs::path(toWide(iso));
    say(L"extracted: " + wstring(c.state.get("extracted") == isoId(c) ? L"yes" : L"no"));
    say(L"lifted   : " + wstring(c.state.get("lifted") == liftStamp(c) ? L"yes" : L"no"));
    say(L"built    : " + wstring(buildCacheMatches(c) && c.state.get("built") == buildStamp(c) ? L"yes" : L"no"));
  }
  string extra = c.state.get("extra_stages");
  if (!extra.empty()) say(L"extra stages: " + toWide(extra));
  return 0;
}

void usage() {
  say(L"mgspo-launcher - set up and run Metal Gear Solid: Portable Ops from your own ISO\n"
      L"\n"
      L"usage: mgspo-launcher [command] [iso] [options] [-- game args]\n"
      L"\n"
      L"commands:\n"
      L"  run       set up whatever is missing, then start the game (default)\n"
      L"  setup     extract, lift and build, but do not start the game\n"
      L"  build     same as setup (cached steps are skipped)\n"
      L"  rebuild   throw away generated code and build output, then lift and build again\n"
      L"  clean     delete generated code, build output and extracted files (--saves, --all)\n"
      L"  status    show what is cached\n"
      L"\n"
      L"options:\n"
      L"  --iso <file>      disc image (remembered for next time; a file picker opens if none is known)\n"
      L"  --work <dir>      work directory (default %LOCALAPPDATA%\\MGSPO\\work; needs about 6 GB free)\n"
      L"  --source <dir>    project source tree (normally detected)\n"
      L"  --config <file>   alternate settings file\n"
      L"  --controls <modern|legacy>  input style (otherwise remembered setting)\n"
      L"  --no-relaunch     do not rebuild and restart when the game needs another stage\n"
      L"  --saves / --all   extra things for clean\n"
      L"  -- <args>         everything after -- is passed to mgspo-game (for example --headless --timeout 60)\n"
      L"\n"
      L"You can also drag an .iso onto the launcher.");
}

bool parse(int argc, wchar_t** argv, Options& o) {
  static const std::set<wstring> cmds = {L"run", L"setup", L"build", L"rebuild", L"clean", L"status", L"help"};
  bool gotCmd = false;
  for (int i = 1; i < argc; ++i) {
    wstring a = argv[i];
    bool more = i + 1 < argc;
    if (a == L"--") {
      for (++i; i < argc; ++i) o.gameArgs.push_back(argv[i]);
    } else if (a == L"--iso" && more) o.iso = argv[++i];
    else if (a == L"--work" && more) o.work = argv[++i];
    else if (a == L"--source" && more) o.source = argv[++i];
    else if (a == L"--config" && more) o.configPath = argv[++i];
    else if (a == L"--controls" && more) {
      o.controls = toUtf8(argv[++i]);
      if (o.controls != "modern" && o.controls != "legacy") { warn(L"Controls must be modern or legacy."); return false; }
    }
    else if (a == L"--no-relaunch") o.noRelaunch = true;
    else if (a == L"--saves") o.saves = true;
    else if (a == L"--all") o.all = true;
    else if (a == L"-h" || a == L"--help") o.cmd = "help", gotCmd = true;
    else if (!gotCmd && cmds.count(a)) o.cmd = toUtf8(a), gotCmd = true;
    else if (!a.empty() && a[0] != L'-' && o.iso.empty()) o.iso = a;
    else {
      warn(L"unknown argument: " + a);
      return false;
    }
  }
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  g_background = GetConsoleWindow() == nullptr;
  Ctx c;
  int rc = 0;
  bool explicitCmd = argc > 1;
  try {
    if (!parse(argc, argv, c.opt)) {
      usage();
      return 2;
    }
    if (c.opt.cmd == "help") {
      usage();
      return 0;
    }
    resolvePaths(c);
    OperationLock operation;
    operation.acquire(c.work);
    if (!c.opt.work.empty()) c.config.save();
    const string& m = c.opt.cmd;
    if (m != "clean" && m != "status" && c.source.empty())
      die(L"Could not find the project sources (CMakeLists.txt). Pass --source <repo dir>.");
    rc = m == "run" ? cmdRun(c) : m == "setup" ? cmdSetup(c) : m == "build" ? cmdBuild(c) : m == "rebuild" ? cmdRebuild(c)
         : m == "clean" ? cmdClean(c) : cmdStatus(c);
  } catch (const Fatal& f) {
    out(L"\nerror: " + f.msg + L"\n", true);
    rc = 1;
  } catch (const std::exception& error) {
    out(L"\nerror: " + toWide(error.what()) + L"\n", true);
    rc = 1;
  }
  // when started by double click the console would vanish before the message can be read
  DWORD pids[2];
  if (!explicitCmd || rc != 0) {
    if (GetConsoleProcessList(pids, 2) <= 1) {
      out(L"\nPress Enter to close...");
      getchar();
    }
  }
  return rc;
}

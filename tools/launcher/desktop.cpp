// Native Windows front end; the command-line launcher remains available for automation.
#define wmain launcher_cli_entry
#include "main.cpp"
#undef wmain
#include "update.h"
#include <shellapi.h>
#include <thread>
#include <memory>

namespace {
constexpr UINT kLog = WM_APP + 1, kDone = WM_APP + 2;
enum { Iso = 101, Browse, Controls, SaveSettings, Launch, Build, Check, Update, Cancel, Restart,
       Status, Log, Revision, Prompts, RenderScale, WindowSize, Renderer, CheckRenderer };
HWND g_window, g_iso, g_controls, g_prompts, g_scale, g_windowSize, g_renderer, g_status, g_log, g_revision;
constexpr const char* kRenderers[] = {"software", "d3d11", "d3d12", "vulkan"};
constexpr const char* kWindowSizes[] = {"480x272", "960x544", "1440x816", "1920x1088"};
std::thread g_worker;
bool g_busy = false, g_closing = false;
Ctx g_context;
UpdateInfo g_update;
fs::path g_restart;
string g_checkedSource;

struct Completion {
  Ctx context;
  UpdateInfo update;
  fs::path restart;
  wstring status;
  bool checked = false;
};

wstring windowText(HWND window) {
  int length = GetWindowTextLengthW(window);
  std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
  GetWindowTextW(window, text.data(), length + 1);
  return text.data();
}

wstring windowsText(const wstring& text) {
  wstring normalized;
  for (wchar_t ch : text) {
    if (ch == L'\n' && (normalized.empty() || normalized.back() != L'\r')) normalized += L'\r';
    normalized += ch;
  }
  return normalized;
}

void appendLog(const wstring& text) {
  // Bounded live view; full pipeline logs remain on disk.
  if (GetWindowTextLengthW(g_log) > 160000) SetWindowTextW(g_log, L"(Earlier output is in the work directory logs.)\r\n");
  wstring normalized = windowsText(text);
  SendMessageW(g_log, EM_SETSEL, static_cast<WPARAM>(-1), -1);
  SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(normalized.c_str()));
}

void buttons() {
  for (int id : {Iso, Browse, Controls, Prompts, RenderScale, WindowSize, Renderer, CheckRenderer, SaveSettings, Launch, Build, Check})
    EnableWindow(GetDlgItem(g_window, id), !g_busy && g_restart.empty());
  EnableWindow(GetDlgItem(g_window, Update), !g_busy && g_restart.empty() && !g_update.available.empty());
  EnableWindow(GetDlgItem(g_window, Restart), !g_busy && !g_restart.empty());
  EnableWindow(GetDlgItem(g_window, Cancel), g_busy);
}

void readSettings() {
  wstring iso = windowText(g_iso);
  if (iso.find_first_of(L"\r\n") != wstring::npos) die(L"ISO path contains a newline.");
  g_context.opt.iso = iso;
  g_context.opt.controls = SendMessageW(g_controls, CB_GETCURSEL, 0, 0) == 1 ? "legacy" : "modern";
  g_context.opt.prompts = SendMessageW(g_prompts, CB_GETCURSEL, 0, 0) == 1 ? "modern" : "original";
  LRESULT scale = SendMessageW(g_scale, CB_GETCURSEL, 0, 0);
  LRESULT size = SendMessageW(g_windowSize, CB_GETCURSEL, 0, 0);
  if (scale < 0 || scale > 7 || size < 0 || size >= 4) die(L"Select an internal render scale and window size.");
  g_context.opt.renderScale = std::to_string(scale + 1);
  g_context.opt.windowSize = kWindowSizes[size];
  LRESULT renderer = SendMessageW(g_renderer, CB_GETCURSEL, 0, 0);
  if (renderer < 0 || renderer >= 4) die(L"Select a renderer.");
  g_context.opt.renderer = kRenderers[renderer];
}

void persistSettings(Ctx& context) {
  context.config.load(context.config.file);
  context.config.set("iso", toUtf8(context.opt.iso));
  context.config.set("controls", context.opt.controls);
  context.config.set("prompts", context.opt.prompts);
  context.config.set("render_scale", context.opt.renderScale);
  context.config.set("window_size", context.opt.windowSize);
  context.config.set("renderer", context.opt.renderer);
  context.config.set("work", toUtf8(context.work.wstring()));
  context.config.save();
}

void startOperation(int id) {
  if (g_busy || !g_restart.empty()) return;
  if (id == Update && g_update.available.empty()) die(L"Check for updates first.");
  readSettings();
  if (id == Update && MessageBoxW(g_window,
      L"Fetch GitHub main again, update a dedicated clone under this work directory, and rebuild "
      L"the launcher/tools and game using one compiler job?\n\n"
      L"Saves/settings and your original source checkout are kept. Local edits/divergent history "
      L"are refused. A failed build may leave the managed source updated; rollback is not promised. "
      L"Restart is offered only after all builds succeed.",
      L"Update and rebuild locally", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  Ctx context = g_context;
  UpdateInfo expected = g_update;
  if (id == Update && g_checkedSource != toUtf8(context.source.wstring()))
    die(L"The selected source changed. Check for updates again.");
  g_busy = true;
  g_cancel = false;
  SetWindowTextW(g_status, id == Launch ? L"Launching / game running - other operations are locked" :
                      id == CheckRenderer ? L"Checking selected renderer, shader and GPU driver..." :
                      id == Check ? L"Checking GitHub main..." :
                      id == Update ? L"Updating and rebuilding locally..." : L"Building from ISO...");
  buttons();
  struct StartGuard {
    bool started = false;
    ~StartGuard() { if (!started) { g_busy = false; buttons(); } }
  } guard;
  g_worker = std::thread([context, expected, id]() mutable {
    auto result = std::make_unique<Completion>();
    result->context = context;
    OperationLock lock;
    try {
      lock.acquire(context.work);
      persistSettings(context);
      context.state.load(context.work / L"state.ini");
      if (id == CheckRenderer) {
        result->status = L"Renderer available: " +
            toWide(psp::ge::hardware::probe(psp::ge::hardware::parse(rendererOption(context))));
        out(result->status + L"\n");
      } else if (id == Check) {
        result->update = checkUpdates(context.source);
        result->checked = true;
        result->status = result->update.newer ? L"Update available. Review revisions, then choose Update / rebuild." :
                                             L"Source matches GitHub main. Update / rebuild can repair tools if needed.";
      } else if (id == Update) {
        // Verify prerequisites before changing even the managed source.
        if (context.opt.iso.empty()) die(L"Choose an ISO path first.");
        resolveIso(context);
        setupToolchain();
        fs::path tools = performUpdate(context, expected, lock);
        result->restart = tools / L"mgspo-desktop.exe";
        result->status = L"Update and both builds succeeded. Choose Restart updated UI.";
      } else {
        // No file picker is opened from a worker thread.
        if (context.opt.iso.empty()) die(L"Choose an ISO path first.");
        if (context.source.empty()) die(L"Project sources not found. Start with --source <dedicated clone>.");
        int rc = id == Launch ? cmdRun(context) : cmdBuild(context);
        if (rc) die(L"Game/build exited with code " + std::to_wstring(rc) + L". See the log below.");
        result->status = id == Launch ? L"Game exited. Ready to launch." : L"Build complete. Ready to launch.";
      }
      result->context = context;
    } catch (const Fatal& error) {
      result->status = L"Error: " + error.msg;
      out(result->status + L"\n", true);
    } catch (const std::exception& error) {
      result->status = L"Error: " + toWide(error.what());
      out(result->status + L"\n", true);
    }
    lock.release();
    PostMessageW(g_window, kDone, 0, reinterpret_cast<LPARAM>(result.release()));
  });
  guard.started = true;
}

void startDesktop(const fs::path& desktop, const Ctx& context) {
  wstring cmd = quoteArg(desktop.wstring()) + L" --config " + quoteArg(context.config.file.wstring()) +
                L" --work " + quoteArg(context.work.wstring());
  auto video = videoOptions(context);
  cmd += L" --render-scale " + toWide(video.first) + L" --window-size " + toWide(video.second);
  cmd += L" --renderer " + toWide(rendererOption(context));
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> text(cmd.begin(), cmd.end());
  text.push_back(0);
  if (!CreateProcessW(desktop.c_str(), text.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    die(L"Cannot start updated UI. This launcher remains open; retry Restart.");
  CloseHandle(pi.hThread);
  DWORD code = 0;
  bool ready = WaitForInputIdle(pi.hProcess, 10000) == 0 &&
               GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE;
  bool windowReady = false;
  // Keep this window class stable: staged launchers use it to verify startup.
  for (HWND window = nullptr; ready && (window = FindWindowExW(nullptr, window, L"MgspoDesktop", nullptr));) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == pi.dwProcessId && IsWindowVisible(window)) windowReady = true;
  }
  ready = ready && windowReady;
  if (!ready) TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hProcess);
  if (!ready) die(L"Updated UI did not become ready. This launcher is kept; check updates and rebuild.");
}

void restartUpdated() {
  startDesktop(g_restart, g_context);
  DestroyWindow(g_window);
}

HWND control(const wchar_t* type, const wchar_t* label, DWORD style, int id,
             int x, int y, int width, int height) {
  HWND item = CreateWindowExW(type == wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, label,
      WS_CHILD | WS_VISIBLE | style, x, y, width, height, g_window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
  return item;
}

LRESULT CALLBACK desktopProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  try {
    if (message == WM_COMMAND) {
      switch (LOWORD(wparam)) {
        case Browse: {
          wstring iso = pickIso();
          if (!iso.empty()) SetWindowTextW(g_iso, iso.c_str());
          return 0;
        }
        case SaveSettings:
        {
          readSettings();
          OperationLock lock;
          lock.acquire(g_context.work);
          persistSettings(g_context);
          SetWindowTextW(g_status, L"Settings saved. Launch checks source/build freshness before running.");
          return 0;
        }
        case Launch: case Build: case Check: case Update: case CheckRenderer: startOperation(LOWORD(wparam)); return 0;
        case Cancel:
          if (MessageBoxW(window, L"Stop the active operation and its child processes? "
              L"This also stops a running game. Saves already written are kept.",
              L"Cancel operation", MB_YESNO | MB_ICONWARNING) == IDYES) g_cancel = true;
          return 0;
        case Restart: restartUpdated(); return 0;
      }
    } else if (message == kLog) {
      std::unique_ptr<wstring> text(reinterpret_cast<wstring*>(lparam));
      appendLog(*text);
      return 0;
    } else if (message == kDone) {
      std::unique_ptr<Completion> result(reinterpret_cast<Completion*>(lparam));
      if (g_worker.joinable()) g_worker.join();
      g_busy = false;
      g_context = result->context;
      g_restart = result->restart;
      if (result->checked) {
        g_update = result->update;
        g_checkedSource = toUtf8(g_context.source.wstring());
        SetWindowTextW(g_revision, (L"Current: " + toWide(g_update.current) +
            L"\r\nGitHub main: " + toWide(g_update.available)).c_str());
      } else if (result->status.rfind(L"Error:", 0) == 0) {
        g_update = {};
      }
      SetWindowTextW(g_status, windowsText(result->status).c_str());
      buttons();
      if (g_closing) DestroyWindow(window);
      return 0;
    } else if (message == WM_CLOSE) {
      if (g_busy) {
        if (MessageBoxW(window, L"An operation/game is running. Cancel it and close once its processes stop?",
                        L"Close launcher", MB_YESNO | MB_ICONWARNING) == IDYES) {
          g_closing = true;
          g_cancel = true;
          SetWindowTextW(g_status, L"Stopping owned processes before closing...");
        }
      } else DestroyWindow(window);
      return 0;
    } else if (message == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
  } catch (const Fatal& error) {
    MessageBoxW(window, error.msg.c_str(), L"MGSPO Launcher", MB_OK | MB_ICONERROR);
  } catch (const std::exception& error) {
    MessageBoxW(window, toWide(error.what()).c_str(), L"MGSPO Launcher", MB_OK | MB_ICONERROR);
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int desktopMain(int argc, wchar_t** argv, int show) {
  if (!parse(argc, argv, g_context.opt)) die(L"Invalid launcher arguments. Use --config, --work or --source.");
  resolvePaths(g_context);
  wstring startupWarning;
  try {
    fs::path active = g_context.opt.source.empty() ? activatedDesktop(g_context) : fs::path{};
    if (!active.empty() && !fs::equivalent(active, exePath())) {
      startDesktop(active, g_context);
      return 0;
    }
  } catch (const Fatal& error) { startupWarning = error.msg; }
  catch (const std::exception& error) { startupWarning = L"Cannot start activated UI: " + toWide(error.what()); }
  SetEnvironmentVariableW(L"MGSPO_GE_THREADS", L"2");
  g_background = true;
  WNDCLASSW cls{};
  cls.lpfnWndProc = desktopProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  cls.lpszClassName = L"MgspoDesktop";
  RegisterClassW(&cls);
  g_window = CreateWindowW(cls.lpszClassName, L"MGSPO - Portable Ops Launcher",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
      900, 786, nullptr, nullptr, cls.hInstance, nullptr);
  if (!g_window) die(L"Could not create launcher window.");
  control(L"STATIC", L"Metal Gear Solid: Portable Ops  |  Local build from your own US ISO", 0, 0, 20, 16, 850, 24);
  control(L"STATIC", L"ISO image", 0, 0, 20, 51, 90, 24);
  wstring iso = g_context.opt.iso.empty() ? toWide(g_context.config.get("iso")) : g_context.opt.iso;
  g_iso = control(L"EDIT", iso.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, Iso, 115, 48, 635, 27);
  control(L"BUTTON", L"Browse...", WS_TABSTOP, Browse, 760, 48, 105, 28);
  control(L"STATIC", L"Control style", 0, 0, 20, 90, 95, 24);
  g_controls = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Controls, 115, 86, 220, 100);
  SendMessageW(g_controls, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Modern gamepad"));
  SendMessageW(g_controls, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Legacy PSP mapping"));
  string controls = g_context.opt.controls.empty() ? g_context.config.get("controls") : g_context.opt.controls;
  if (!controls.empty() && controls != "modern" && controls != "legacy") die(L"Invalid controls setting in launcher.ini.");
  SendMessageW(g_controls, CB_SETCURSEL, controls == "legacy" ? 1 : 0, 0);
  control(L"STATIC", L"Icons", 0, 0, 350, 90, 45, 24);
  g_prompts = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Prompts, 400, 86, 300, 100);
  SendMessageW(g_prompts, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Original PSP"));
  SendMessageW(g_prompts, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Modern Xbox-style (menu X/Y)"));
  string prompts = g_context.opt.prompts.empty() ? g_context.config.get("prompts") : g_context.opt.prompts;
  if (!prompts.empty() && prompts != "original" && prompts != "modern") die(L"Invalid prompts setting in launcher.ini.");
  SendMessageW(g_prompts, CB_SETCURSEL, prompts == "modern" ? 1 : 0, 0);
  control(L"BUTTON", L"Save settings", WS_TABSTOP, SaveSettings, 720, 86, 145, 28);
  control(L"STATIC", L"Render scale", 0, 0, 20, 130, 95, 24);
  g_scale = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, RenderScale, 115, 126, 220, 240);
  for (int scale = 1; scale <= 8; ++scale) {
    auto label = std::to_wstring(scale) + L"x - " + std::to_wstring(480 * scale) + L"x" +
                 std::to_wstring(272 * scale) + (scale == 1 ? L" (native)" : L" (software)");
    SendMessageW(g_scale, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  }
  control(L"STATIC", L"Window", 0, 0, 350, 130, 60, 24);
  g_windowSize = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, WindowSize, 415, 126, 180, 130);
  auto video = videoOptions(g_context);
  SendMessageW(g_scale, CB_SETCURSEL, psp::video::render_scale(video.first) - 1, 0);
  for (int i = 0; i < 4; ++i) {
    auto label = toWide(kWindowSizes[i]);
    SendMessageW(g_windowSize, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (video.second == kWindowSizes[i]) SendMessageW(g_windowSize, CB_SETCURSEL, i, 0);
  }
  control(L"STATIC", L"Next launch; cost grows with scale squared.", 0, 0, 610, 130, 255, 40);
  control(L"STATIC", L"Renderer", 0, 0, 20, 178, 95, 24);
  g_renderer = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Renderer, 115, 174, 280, 150);
  auto selectedRenderer = rendererOption(g_context);
  const wchar_t* labels[] = {L"Software (default / reference)", L"Direct3D 11 (compute raster)", L"Direct3D 12 (compute raster)", L"Vulkan (compute raster)"};
  for (int i = 0; i < 4; ++i) {
    wstring label = labels[i];
    if (!psp::ge::hardware::built(psp::ge::hardware::parse(kRenderers[i]))) label += L" - unavailable in build";
    SendMessageW(g_renderer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (selectedRenderer == kRenderers[i]) SendMessageW(g_renderer, CB_SETCURSEL, i, 0);
  }
  control(L"BUTTON", L"Check GPU / driver", WS_TABSTOP, CheckRenderer, 415, 174, 180, 28);
  control(L"STATIC", L"Hardware still retains the CPU native pass.", 0, 0, 610, 178, 255, 40);
  control(L"STATIC", (L"Work / saves: " + g_context.work.wstring()).c_str(), SS_PATHELLIPSIS, 0, 20, 222, 845, 23);
  control(L"STATIC", (L"Source: " + g_context.source.wstring()).c_str(), SS_PATHELLIPSIS, 0, 20, 247, 845, 23);
  control(L"BUTTON", L"Launch game", WS_TABSTOP | BS_DEFPUSHBUTTON, Launch, 20, 284, 145, 35);
  control(L"BUTTON", L"Build / setup ISO", WS_TABSTOP, Build, 175, 284, 155, 35);
  control(L"BUTTON", L"Check updates", WS_TABSTOP, Check, 340, 284, 140, 35);
  control(L"BUTTON", L"Update / rebuild", WS_TABSTOP, Update, 490, 284, 155, 35);
  control(L"BUTTON", L"Cancel", WS_TABSTOP, Cancel, 655, 284, 90, 35);
  control(L"BUTTON", L"Restart UI", WS_TABSTOP, Restart, 755, 284, 110, 35);
  g_revision = control(L"STATIC", L"Updates are manual. Check fetches main; Update stages a dedicated clone and rebuilds locally.",
                       0, Revision, 20, 332, 845, 44);
  bool cached = fs::exists(g_context.build / L"mgspo-game.exe") && !g_context.state.get("built").empty();
  g_status = control(L"EDIT", cached ? L"Cached game found. Launch validates freshness before running." :
      iso.empty() ? L"First run: choose an ISO. Build includes all stages; reserve 20 GiB and time for a long first build." :
      L"Build needed: Build / setup ISO includes all stages. One compiler job; the first build can take tens of minutes or longer.",
      ES_READONLY | ES_MULTILINE, Status, 20, 382, 845, 54);
  g_log = control(L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, Log, 20, 446, 845, 290);
  g_output = [](const wstring& text) {
    auto line = std::make_unique<wstring>(text);
    if (PostMessageW(g_window, kLog, 0, reinterpret_cast<LPARAM>(line.get()))) line.release();
  };
  buttons();
  if (!startupWarning.empty()) {
    appendLog(startupWarning + L"\n");
    SetWindowTextW(g_status, startupWarning.c_str());
  }
  ShowWindow(g_window, show);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(g_window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  g_output = {};
  return 0;
}
}  // namespace

#ifndef MGSPO_DESKTOP_TEST
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  try {
    int result = desktopMain(argc, argv, show);
    LocalFree(argv);
    return result;
  } catch (const Fatal& error) {
    MessageBoxW(nullptr, error.msg.c_str(), L"MGSPO Launcher", MB_OK | MB_ICONERROR);
  } catch (const std::exception& error) {
    MessageBoxW(nullptr, toWide(error.what()).c_str(), L"MGSPO Launcher", MB_OK | MB_ICONERROR);
  }
  LocalFree(argv);
  return 1;
}
#endif

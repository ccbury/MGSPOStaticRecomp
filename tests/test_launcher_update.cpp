#define wmain launcher_entry
#include "../tools/launcher/main.cpp"
#undef wmain
#include "../tools/launcher/update.h"
#include <thread>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void write(const fs::path& path, const string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  stream << text;
  check(static_cast<bool>(stream), "fixture write failed");
}
template<class Fn> void refused(Fn fn, const wchar_t* expected) {
  try { fn(); }
  catch (const Fatal& error) {
    check(error.msg.find(expected) != wstring::npos, "unexpected refusal reason");
    return;
  }
  throw std::runtime_error("unsafe operation was not refused");
}
struct Fixture {
  fs::path root = fs::temp_directory_path() / (L"mgspo-update-tests-" + std::to_wstring(GetCurrentProcessId()) +
                                              L"-" + std::to_wstring(GetTickCount64()));
  ~Fixture() {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_regular_file()) fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(root, ec);
    if (ec) std::fprintf(stderr, "fixture cleanup failed: %s\n", ec.message().c_str());
  }
};
void commit(const fs::path& source) {
  gitOutput(source, {L"add", L"."});
  gitOutput(source, {L"-c", L"user.name=Launcher Fixture", L"-c", L"user.email=fixture@example.invalid",
                     L"commit", L"-m", L"fixture"});
}
void cancellation() {
  std::vector<HANDLE> children;
  std::atomic<bool> ready{false};
  std::thread cancel([&] {
    for (int i = 0; i < 50 && !ready; ++i) Sleep(100);
    g_cancel = true;
  });
  bool cancelled = false;
  try {
    runProcess(exePath().wstring(), {L"--wait-child"}, {}, {}, [&](const string& line) {
      children.push_back(OpenProcess(SYNCHRONIZE, FALSE, std::stoul(line)));
      ready = children.size() == 2;
    }, false);
  } catch (const Fatal& error) { cancelled = error.msg.find(L"cancelled") != wstring::npos; }
  cancel.join();
  g_cancel = false;
  check(cancelled, "cancellation must be explicit");
  bool stopped = children.size() == 2;
  for (HANDLE child : children) {
    stopped = child && WaitForSingleObject(child, 5000) == WAIT_OBJECT_0 && stopped;
    if (child) CloseHandle(child);
  }
  check(stopped, "owned parent and grandchild must both stop on cancellation");
}

void fixtures(bool rebuild) {
  Fixture fixture;
  fs::path seed = fixture.root / L"seed with spaces";
  fs::path remote = fixture.root / L"origin.git";
  Ctx c;
  c.source = fixture.root / L"source with spaces";
  c.work = fixture.root / L"work with spaces";
  c.logs = c.work / L"logs";
  c.iso = fixture.root / L"test only.iso";
  c.opt.controls = "legacy";
  c.config.file = fixture.root / L"settings with spaces" / L"launcher.ini";
  c.config.set("custom", "kept");
  c.config.set("prompts", "modern");
  c.config.set("render_scale", "8");
  c.config.set("window_size", "1440x816");
  c.config.set("renderer", "d3d12");
  c.config.save();
  write(c.work / L"save" / L"sentinel", "save must survive");
  fs::create_directories(seed);
  gitOutput(seed, {L"init", L"--initial-branch=main"});
  write(seed / L"CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\nproject(Fixture LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\n"
        "add_executable(mgspo-launcher dummy.cpp)\nadd_executable(mgspo-desktop dummy.cpp)\n"
        "add_dependencies(mgspo-desktop mgspo-launcher)\n");
  write(seed / L"dummy.cpp", R"CPP(
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
int wmain(int argc, wchar_t** argv) {
#ifdef FAIL_GAME
  return 9;
#else
  if (argc < 2 || std::wstring(argv[1]) != L"build") return 7;
  bool modernPrompts = false;
  bool highRes = false, windowSize = false, renderer = false;
  for (int i=1;i+1<argc;++i)
    if (std::wstring(argv[i]) == L"--prompts") modernPrompts = std::wstring(argv[i+1]) == L"modern";
    else if (std::wstring(argv[i]) == L"--render-scale") highRes = std::wstring(argv[i+1]) == L"8";
    else if (std::wstring(argv[i]) == L"--window-size") windowSize = std::wstring(argv[i+1]) == L"1440x816";
    else if (std::wstring(argv[i]) == L"--renderer") renderer = std::wstring(argv[i+1]) == L"d3d12";
  if (!modernPrompts || !highRes || !windowSize || !renderer) return 8;
  for (int i=1;i+1<argc;++i) if (std::wstring(argv[i]) == L"--work") {
    std::filesystem::path work(argv[i+1]);
    HANDLE lock = CreateFileW((work / L".launcher-operation").c_str(), GENERIC_WRITE, 0,
                             nullptr, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return 8;
    std::ofstream(work / L"fixture-game-built") << "built";
    CloseHandle(lock);
  }
  return 0;
#endif
}
)CPP");
  commit(seed);
  gitOutput(seed, {L"init", L"--bare", L"--initial-branch=main", remote.wstring()});
  gitOutput(seed, {L"remote", L"add", L"origin", remote.wstring()});
  gitOutput(seed, {L"push", L"origin", L"main"});
  gitOutput(seed, {L"clone", remote.wstring(), c.source.wstring()});
  UpdatePolicy policy{true, remote.wstring()};
  auto same = checkUpdates(c.source, policy);
  check(!same.newer, "current revision should be current");
  check(!githubOrigin(toUtf8(remote.wstring())), "local remotes must not be allowed in production");
  refused([&] { checkUpdates(c.source); }, L"Unexpected origin");
  write(c.source / L"local.txt", "untracked");
  refused([&] { checkUpdates(c.source, policy); }, L"local edits");
  fs::remove(c.source / L"local.txt");
  write(c.source / L"dummy.cpp", "dirty");
  refused([&] { checkUpdates(c.source, policy); }, L"local edits");
  // Restore only the disposable test file.
  gitOutput(c.source, {L"restore", L"dummy.cpp"});
  gitOutput(c.source, {L"switch", L"-c", L"feature"});
  refused([&] { checkUpdates(c.source, policy); }, L"require main");
  gitOutput(c.source, {L"switch", L"main"});
  write(c.source / L"local.txt", "local commit");
  commit(c.source);
  refused([&] { checkUpdates(c.source, policy); }, L"ahead of or divergent");
  write(seed / L"remote.txt", "remote commit");
  commit(seed);
  gitOutput(seed, {L"push", L"origin", L"main"});
  refused([&] { checkUpdates(c.source, policy); }, L"ahead of or divergent");
  // Replace only our own fixture checkout; no user's repository is touched.
  c.source = fixture.root / L"clean source";
  gitOutput(seed, {L"clone", remote.wstring(), c.source.wstring()});
  gitOutput(c.source, {L"remote", L"set-url", L"origin", (fixture.root / L"offline.git").wstring()});
  UpdatePolicy offline{true, (fixture.root / L"offline.git").wstring()};
  refused([&] { checkUpdates(c.source, offline); }, L"Git operation failed");
  gitOutput(c.source, {L"remote", L"set-url", L"origin", remote.wstring()});
  fs::path linked = fixture.root / L"linked";
  gitOutput(c.source, {L"worktree", L"add", L"--detach", linked.wstring()});
  refused([&] { checkUpdates(linked, policy); }, L"standalone Git clone");
  refused([&] { requireOwnedSource(c); }, L"not launcher-owned");
  auto old = checkUpdates(c.source, policy);
  write(seed / L"next.txt", "new");
  commit(seed);
  gitOutput(seed, {L"push", L"origin", L"main"});
  refused([&] { prepareUpdateSource(c, old, policy); }, L"changed since Check");
  auto next = checkUpdates(c.source, policy);
  check(next.newer, "must find update after fetching");
  check(sourceRevision(c) == old.current, "check must not change source revision");
  if (rebuild) {
    OperationLock operation;
    operation.acquire(c.work);
    OperationLock conflict;
    refused([&] { conflict.acquire(c.work); }, L"busy");
    fs::path tools = performUpdate(c, next, operation, policy);
    check(fs::exists(tools / L"mgspo-desktop.exe"), "new desktop must exist");
    check(fs::exists(c.work / L"fixture-game-built"), "new CLI must complete fixture game build");
    check(c.config.get("source_revision") == next.available, "settings activate only after full success");
    check(c.config.get("custom") == "kept", "custom settings must survive");
    check(c.config.get("prompts") == "modern", "icon selection must survive update activation");
    check(c.config.get("render_scale") == "8" && c.config.get("window_size") == "1440x816",
          "independent graphics preferences must survive update activation");
    check(c.config.get("renderer") == "d3d12", "renderer must survive update activation");
    check(activatedDesktop(c) == tools / L"mgspo-desktop.exe", "activation must select new staged UI");
    string desktop = c.config.get("desktop");
    c.config.set("desktop", toUtf8((fixture.root / L"mgspo-desktop.exe").wstring()));
    refused([&] { activatedDesktop(c); }, L"unavailable or outside");
    c.config.set("desktop", desktop);
    check(fs::exists(c.work / L"save" / L"sentinel"), "save must survive");
    check(sourceRevision(c) == old.current, "original checkout must remain unchanged");
    write(seed / L"broken.cpp", "not C++");
    write(seed / L"CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(Fixture LANGUAGES CXX)\n"
          "add_executable(mgspo-desktop broken.cpp)\n");
    commit(seed);
    gitOutput(seed, {L"push", L"origin", L"main"});
    refused([&] { performUpdate(c, checkUpdates(c.source, policy), operation, policy); }, L"tools failed");
    Ini persisted;
    persisted.load(c.config.file);
    check(persisted.get("source_revision") == next.available, "failed tool build must not activate settings");
    write(seed / L"CMakeLists.txt",
          "cmake_minimum_required(VERSION 3.20)\nproject(Fixture LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\n"
          "add_executable(mgspo-launcher dummy.cpp)\nadd_executable(mgspo-desktop dummy.cpp)\n"
          "add_dependencies(mgspo-desktop mgspo-launcher)\nadd_compile_definitions(FAIL_GAME=1)\n");
    commit(seed);
    gitOutput(seed, {L"push", L"origin", L"main"});
    refused([&] { performUpdate(c, checkUpdates(c.source, policy), operation, policy); }, L"game build failed");
    persisted.load(c.config.file);
    check(persisted.get("source_revision") == next.available, "failed game build must not activate settings");
    check(fs::exists(c.work / L"save" / L"sentinel"), "failed updates must preserve saves");
  } else {
    fs::path managed = prepareUpdateSource(c, next, policy);
    check(gitOutput(managed, {L"rev-parse", L"HEAD"}) == next.available, "managed clone must reach fetched main");
    Ctx stale = c;
    stale.source = managed;
    refused([&] { ensureBuilt(stale, false); }, L"different source revision");
    stale.source = fixture.root / L"revision source";
    stale.exeDir = fixture.root;
    gitOutput(seed, {L"clone", remote.wstring(), stale.source.wstring()});
    write(fixture.root / L"mgspo-lift.exe", "");
    stale.disc = c.work / L"disc";
    write(stale.disc / L"PSP_GAME" / L"USRDIR" / L"stage" / L"init.prx", "\x7F" "ELF-init");
    string before = buildStamp(stale);
    write(stale.source / L"revision.txt", "new source revision");
    commit(stale.source);
    check(buildStamp(stale) != before, "source revision alone must invalidate build cache");
    write(managed / L"dirty", "preserve");
    refused([&] { prepareUpdateSource(c, next, policy); }, L"local edits");
    check(fs::exists(managed / L"dirty"), "managed edits must survive refusal");
    fs::remove(managed / L"dirty");
    write(managed / L".git" / L"mgspo-owner", "another work directory");
    refused([&] { prepareUpdateSource(c, next, policy); }, L"different work directory");
  }
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc == 6 && wstring(argv[1]) == L"--activate-ui-fixture") {
    try {
      Ctx c;
      c.opt.configPath = argv[2];
      c.opt.work = argv[3];
      c.opt.source = argv[4];
      resolvePaths(c);
      OperationLock lock;
      lock.acquire(c.work);
      activateUpdate(c, c.source, argv[5], string(40, '1'));
      return 0;
    } catch (const Fatal& error) { out(error.msg + L"\n", true); return 1; }
  }
  if (argc > 1 && wstring(argv[1]) == L"--wait-grandchild") { Sleep(30000); return 0; }
  if (argc > 1 && wstring(argv[1]) == L"--wait-child") {
    std::printf("%lu\n", GetCurrentProcessId());
    std::fflush(stdout);
    wstring command = quoteArg(exePath().wstring()) + L" --wait-grandchild";
    std::vector<wchar_t> text(command.begin(), command.end());
    text.push_back(0);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exePath().c_str(), text.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 2;
    std::printf("%lu\n", process.dwProcessId);
    std::fflush(stdout);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return 0;
  }
  try {
    g_background = true;
    cancellation();
    fixtures(argc > 1 && wstring(argv[1]) == L"--rebuild");
    std::puts("Launcher updater fixture tests passed.");
    return 0;
  } catch (const Fatal& error) { out(error.msg + L"\n", true); }
  catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); }
  return 1;
}

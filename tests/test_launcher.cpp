#define wmain launcher_entry
#include "../tools/launcher/main.cpp"
#undef wmain

#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void writeText(const fs::path& path, const string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  check(static_cast<bool>(out), "could not write test file");
}

void writeCache(const Ctx& c, const fs::path& source, const fs::path& build, const string& generator = "Ninja",
                const string& arch = "x64", const string& pointerSize = "8") {
  fs::path metadata = c.build / L"CMakeFiles" / L"3.31.6";
  fs::create_directories(metadata);
  writeText(metadata / L"CMakeCXXCompiler.cmake", "set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID " + arch + ")\r\n" +
            "set(CMAKE_CXX_SIZEOF_DATA_PTR \"" + pointerSize + "\")\r\n");
  writeText(c.build / L"CMakeCache.txt",
            "CMAKE_HOME_DIRECTORY:INTERNAL=" + toUtf8(source.generic_wstring()) + "\r\n" +
            "CMAKE_CACHEFILE_DIR:INTERNAL=" + toUtf8(build.generic_wstring()) + "\r\n" +
            "CMAKE_GENERATOR:INTERNAL=" + generator + "\r\n");
}

struct Fixture {
  fs::path root = fs::temp_directory_path() /
                  (L"mgspo-launcher-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(GetTickCount64()));
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(root, ec);
    if (ec) std::fprintf(stderr, "test cleanup failed: %s\n", ec.message().c_str());
  }
};

}  // namespace

int wmain() {
  try {
    Fixture fixture;
    Ctx c;
    c.source = fixture.root / L"source with spaces";
    c.work = fixture.root / L"work with spaces";
    c.build = c.work / L"build";
    c.save = c.work / L"save";
    c.disc = c.work / L"disc";
    c.gen = c.work / L"gen";
    c.exeDir = fixture.root;
    fs::path otherSource = fixture.root / L"other source";
    fs::path otherBuild = fixture.root / L"other build";
    for (const auto& dir : {c.source, c.build, c.save, c.disc, c.gen, otherSource, otherBuild})
      fs::create_directories(dir);
    check(!buildCacheMatches(c), "missing cache should require configuration");
    writeCache(c, c.source, c.build);
    check(buildCacheMatches(c), "matching source and build directories should reuse cache");
    writeCache(c, shortPath(c.source), shortPath(c.build));
    check(buildCacheMatches(c), "short and long paths should match");
    writeCache(c, otherSource, c.build);
    check(!buildCacheMatches(c), "different source must invalidate cache");
    writeCache(c, fixture.root / L"removed source", c.build);
    check(!buildCacheMatches(c), "removed source must invalidate cache");
    writeCache(c, c.source, otherBuild);
    check(!buildCacheMatches(c), "moved build directory must invalidate cache");
    writeCache(c, c.source, c.build, "Visual Studio 17 2022");
    check(!buildCacheMatches(c), "different generator must invalidate cache");
    writeCache(c, c.source, c.build, "Ninja", "X86", "4");
    check(!buildCacheMatches(c), "x86 compiler must invalidate cache");
    writeCache(c, c.source, c.build, "Ninja", "ARM64", "8");
    check(!buildCacheMatches(c), "ARM64 compiler must invalidate cache");
    writeCache(c, c.source, c.build, "Ninja", "x64", "4");
    check(!buildCacheMatches(c), "32-bit pointers must invalidate cache");
    writeCache(c, c.source, c.build);
    fs::remove(c.build / L"CMakeFiles" / L"3.31.6" / L"CMakeCXXCompiler.cmake");
    check(!buildCacheMatches(c), "missing compiler metadata must invalidate cache");
    writeText(c.build / L"CMakeCache.txt", "incomplete cache");
    check(!buildCacheMatches(c), "incomplete cache must invalidate cache");

    fs::create_directories(c.build / L"CMakeFiles");
    writeText(c.build / L"CMakeFiles" / L"stale", "stale");
    writeText(c.build / L"compiled.obj", "object");
    writeText(c.save / L"save.dat", "save");
    writeText(c.disc / L"disc.dat", "disc");
    writeText(c.gen / L"generated.cpp", "code");
    resetBuildCache(c);
    check(!fs::exists(c.build / L"CMakeCache.txt"), "cache should be removed");
    check(!fs::exists(c.build / L"CMakeFiles"), "CMake metadata should be removed");
    for (const auto& file : {c.build / L"compiled.obj", c.save / L"save.dat",
                             c.disc / L"disc.dat", c.gen / L"generated.cpp"})
      check(fs::exists(file), "cache reset must preserve objects and game data");
    resetBuildCache(c);
    writeCache(c, c.source, c.build);
    check(buildCacheMatches(c), "reconfigured cache should be reusable");
    writeText(c.exeDir / L"mgspo-lift.exe", "");
    fs::path stages = c.disc / L"PSP_GAME" / L"USRDIR" / L"stage";
    fs::create_directories(stages / L"nested");
    writeText(stages / L"nested" / L"extra.prx", "\x7F" "ELF-extra");
    writeText(stages / L"init.prx", "\x7F" "ELF-init");
    c.state.set("extra_stages", "removed/removed.prx");
    check(stageList(c).size() == 2, "default must include every nested disc stage, ignoring obsolete partial lists");
    const string inventory = mgspo::stages::manifest(stageList(c));
    check(!liftOutputsMatch(c, inventory), "old partial lift without completed inventory must be invalidated");
    writeText(c.gen / L"tables.h", "header");
    writeText(c.gen / L"tables.cpp", "table");
    writeText(c.gen / L"stages.manifest", inventory);
    check(liftOutputsMatch(c, inventory), "complete inventory must validate generated outputs");
    const string allStamp = liftStamp(c);
    check(allStamp.find("all-stages-v1:") != string::npos, "partial policy must not share all-stage cache stamp");
    writeText(stages / L"new.prx", "\x7F" "ELF-new");
    check(liftStamp(c) != allStamp && !liftOutputsMatch(c, mgspo::stages::manifest(stageList(c))),
          "added stage must invalidate stamp and completed manifest");
    fs::remove(stages / L"new.prx");
    fs::remove(c.gen / L"tables.cpp");
    check(!liftOutputsMatch(c, inventory), "missing generated table must invalidate completed lift");
    writeText(c.gen / L"tables.cpp", "table");
    string firstStamp = buildStamp(c);
    c.source = otherSource;
    check(firstStamp != buildStamp(c), "build stamp must include source checkout");
    check(sourceRevision(c) == "unversioned", "source archives must remain supported for CLI builds");
    auto args = gameArguments(c);
    check(args[args.size() - 3] == L"modern", "default control profile must be modern");
    check(args.back() == L"original", "default prompts must preserve original rendering");
    c.config.set("controls", "legacy");
    check(gameArguments(c)[args.size() - 3] == L"legacy", "remembered control profile must be used");
    c.opt.controls = "modern";
    check(gameArguments(c)[args.size() - 3] == L"modern", "explicit profile must override config");
    c.config.set("prompts", "modern");
    check(gameArguments(c).back() == L"modern", "remembered icon mode must be forwarded");
    c.opt.prompts = "original";
    check(gameArguments(c).back() == L"original", "explicit icons must override config");
    c.opt.prompts = "bad";
    bool invalidPrompts = false;
    try { gameArguments(c); } catch (const Fatal&) { invalidPrompts = true; }
    check(invalidPrompts, "invalid icons must fail explicitly");
    c.opt.prompts.clear();
    check(videoOptions(c) == std::make_pair(string("1"), string("960x544")), "old config must use native scale and existing window size");
    check(rendererOption(c) == "software", "old config must default to software renderer");
    const string graphicsStamp = buildStamp(c);
    c.config.set("render_scale", "8");
    c.config.set("window_size", "1440x816");
    c.config.set("renderer", "d3d12");
    args = gameArguments(c);
    auto hasPair = [&](const wchar_t* flag, const wchar_t* value) {
      auto it = std::find(args.begin(), args.end(), flag);
      return it != args.end() && ++it != args.end() && *it == value;
    };
    check(hasPair(L"--render-scale", L"8") && hasPair(L"--window-size", L"1440x816"), "runtime graphics arguments must use remembered values");
    check(hasPair(L"--renderer", L"d3d12"), "renderer must be forwarded independently");
    c.opt.renderer = "d3d11";
    check(rendererOption(c) == "d3d11", "explicit renderer must override config");
    c.opt.renderer = "invalid";
    bool invalidRenderer = false;
    try { gameArguments(c); } catch (const Fatal&) { invalidRenderer = true; }
    check(invalidRenderer, "invalid renderer must fail explicitly");
    c.opt.renderer.clear();
    c.opt.windowSize = "480x272";
    check(videoOptions(c) == std::make_pair(string("8"), string("480x272")), "window size must not change internal scale");
    c.opt.renderScale = "1";
    check(videoOptions(c) == std::make_pair(string("1"), string("480x272")), "explicit scale must not change window size");
    check(buildStamp(c) == graphicsStamp, "graphics settings must not require regenerating or compiling stages");
    c.opt.renderScale = "999999999999";
    bool invalidVideo = false;
    try { gameArguments(c); } catch (const Fatal&) { invalidVideo = true; }
    check(invalidVideo, "extreme render scale must fail explicitly");
    c.opt.renderScale.clear(); c.opt.windowSize.clear();
    c.opt.gameArgs = {L"--controls", L"legacy"};
    check(gameArguments(c).back() == L"legacy", "raw game arguments must remain compatible");
    c.opt.controls = "invalid";
    bool invalid = false;
    try { gameArguments(c); } catch (const Fatal&) { invalid = true; }
    check(invalid, "invalid profiles must fail explicitly");
    c.config.file = fixture.root / L"settings with spaces" / L"launcher.ini";
    c.config.set("custom", "preserved");
    c.config.save();
    Ini loaded;
    loaded.load(c.config.file);
    check(loaded.get("controls") == "legacy" && loaded.get("prompts") == "modern" && loaded.get("custom") == "preserved",
          "settings must round trip with custom keys");
    check(loaded.get("render_scale") == "8" && loaded.get("window_size") == "1440x816", "graphics preferences must round trip independently");
    check(loaded.get("renderer") == "d3d12", "renderer preference must round trip");
    HANDLE settings = CreateFileW(c.config.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    check(settings != INVALID_HANDLE_VALUE, "cannot lock fixture settings");
    c.config.set("custom", "should not replace");
    bool locked = false;
    try { c.config.save(); } catch (const Fatal&) { locked = true; }
    CloseHandle(settings);
    check(locked, "locked settings must report failure");
    loaded.load(c.config.file);
    check(loaded.get("custom") == "preserved", "failed settings write must preserve original");
    wchar_t a0[] = L"launcher", a1[] = L"--controls", a2[] = L"legacy", a3[] = L"--config",
            a4[] = L"C:\\folder with spaces\\settings.ini", a5[] = L"game image.iso";
    wchar_t* command[] = {a0, a1, a2, a3, a4, a5};
    Options parsed;
    check(parse(6, command, parsed) && parsed.controls == "legacy" && parsed.configPath == a4 && parsed.iso == a5,
          "config/profile/ISO arguments must parse without losing spaces");
    wchar_t p1[] = L"--prompts", p2[] = L"modern", p3[] = L"invalid";
    wchar_t* promptCommand[] = {a0, p1, p2};
    check(parse(3, promptCommand, parsed) && parsed.prompts == "modern", "icon mode must parse");
    promptCommand[2] = p3;
    check(!parse(3, promptCommand, parsed), "invalid icon option must be rejected");
    wchar_t v1[] = L"--render-scale", v2[] = L"8", v3[] = L"--window-size", v4[] = L"1920x1088";
    wchar_t* graphicsCommand[] = {a0, v1, v2, v3, v4};
    check(parse(5, graphicsCommand, parsed) && parsed.renderScale == "8" && parsed.windowSize == "1920x1088", "graphics arguments must parse independently");
    graphicsCommand[2] = p3;
    check(!parse(5, graphicsCommand, parsed), "invalid graphics option must be rejected");
    wchar_t zero[] = L"0", nine[] = L"9", extreme[] = L"999999999999";
    for (auto invalid : {zero, nine, extreme}) {
      graphicsCommand[2] = invalid;
      check(!parse(5, graphicsCommand, parsed), "out-of-range graphics scale must be rejected");
    }
    wchar_t rendererFlag[] = L"--renderer", rendererValue[] = L"d3d11";
    wchar_t* rendererCommand[] = {a0, rendererFlag, rendererValue};
    check(parse(3, rendererCommand, parsed) && parsed.renderer == "d3d11", "renderer argument must parse");
    rendererCommand[2] = p3;
    check(!parse(3, rendererCommand, parsed), "unknown renderer must be rejected");
    if (GetEnvironmentVariableW(L"MGSPO_TEST_TOOLCHAIN", nullptr, 0) > 0) {
      setupToolchain();
      check(haveX64Toolchain(), "toolchain setup must select x64");
    }
    // Redirect even the default config into the disposable fixture before testing clean.
    SetEnvironmentVariableW(L"LOCALAPPDATA", (fixture.root / L"fake app data").c_str());
    Ini defaults;
    defaults.file = configFile();
    defaults.set("keep", "yes");
    defaults.save();
    c.boot = c.work / L"boot";
    c.logs = c.work / L"logs";
    c.state.file = c.work / L"state.ini";
    c.opt.all = true;
    cmdClean(c);
    check(!fs::exists(c.config.file), "clean --all must remove the selected alternate config");
    check(fs::exists(defaults.file), "clean with alternate config must not remove default settings");
    std::puts("Launcher cache regression tests passed.");
    return 0;
  } catch (const Fatal& error) {
    out(error.msg + L"\n", true);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
  }
  return 1;
}

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
    string firstStamp = buildStamp(c);
    c.source = otherSource;
    check(firstStamp != buildStamp(c), "build stamp must include source checkout");
    if (GetEnvironmentVariableW(L"MGSPO_TEST_TOOLCHAIN", nullptr, 0) > 0) {
      setupToolchain();
      check(haveX64Toolchain(), "toolchain setup must select x64");
    }
    std::puts("Launcher cache regression tests passed.");
    return 0;
  } catch (const Fatal& error) {
    out(error.msg + L"\n", true);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
  }
  return 1;
}

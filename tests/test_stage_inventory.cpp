#include "../tools/stage_inventory.h"

#include <chrono>
#include <iostream>

namespace fs = std::filesystem;
namespace stages = mgspo::stages;

namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void write(const fs::path& path, const std::string& data) {
  fs::create_directories(path.parent_path());
  stages::writeManifest(path, data);
}
template<class Fn> void rejected(Fn fn) {
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error("invalid stage input was accepted");
}
struct Fixture {
  fs::path root = fs::temp_directory_path() / ("mgspo-stage-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(root, ec);
    if (ec) std::cerr << "fixture cleanup: " << ec.message() << "\n";
  }
};
}

int main() {
  try {
    Fixture fixture;
    const auto root = fixture.root / "stages with spaces";
    fs::create_directories(root);
    rejected([&] { stages::enumerate(root); });
    rejected([&] { stages::enumerate(root / "missing"); });
    const auto a = root / "a" / "a.prx", z = root / "deep" / "z" / "z.PRX";
    write(z, "\x7F" "ELF-z");
    write(a, "\x7F" "ELF-a");
    write(root / "ignored.bin", "not a module");
    write(a.parent_path() / "_zar", "link-a");
    auto inventory = stages::enumerate(root);
    check(inventory == std::vector<fs::path>{fs::canonical(a), fs::canonical(z)},
          "nested/case-insensitive enumeration must be complete and sorted");
    auto duplicate = stages::normalize({z, a, root / "a" / "." / "a.prx", z});
    check(duplicate == inventory, "explicit and directory entries must deduplicate deterministically");
    const auto first = stages::manifest(inventory);
    const auto file = fixture.root / "stages.manifest";
    check(!stages::matches(file, first), "missing completed manifest must not match");
    stages::writeManifest(file, first);
    check(stages::matches(file, first), "complete manifest must round trip");
    write(file, first.substr(0, first.size() / 2));
    check(!stages::matches(file, first), "partial manifest must not match");
    auto mtime = fs::last_write_time(a);
    write(a, "\x7F" "ELF-b");
    fs::last_write_time(a, mtime);
    check(stages::manifest(stages::enumerate(root)) != first,
          "same-size content changes with preserved timestamps must invalidate inventory");
    write(a, "\x7F" "ELF-a");
    const auto zarTime = fs::last_write_time(a.parent_path() / "_zar");
    write(a.parent_path() / "_zar", "link-b");
    fs::last_write_time(a.parent_path() / "_zar", zarTime);
    check(stages::manifest(inventory) != first, "stage relocation archives must participate in inventory");
    write(a.parent_path() / "_zar", "link-a");
    write(root / "new.prx", "\x7F" "ELF-new");
    check(stages::manifest(stages::enumerate(root)) != first, "added modules must invalidate inventory");
    fs::remove(root / "new.prx");
    fs::remove(z);
    check(stages::manifest(stages::enumerate(root)) != first, "removed modules must invalidate inventory");
    write(z, "~SCE encrypted");
    rejected([&] { stages::enumerate(root); });
    write(z, "x");
    rejected([&] { stages::enumerate(root); });
    write(z, "bad magic");
    rejected([&] { stages::normalize({z}); });
    fs::remove(z);
    fs::create_directory(z);
    rejected([&] { stages::normalize({z}); });
    std::cout << "Stage inventory checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}

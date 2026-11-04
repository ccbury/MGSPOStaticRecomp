#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psp::ge::surface {

// Internal GE/display access must not be mistaken for an external CPU/HLE read or write.
struct NativeScope {
  NativeScope();
  ~NativeScope();
  NativeScope(const NativeScope&) = delete;
  NativeScope& operator=(const NativeScope&) = delete;
};

struct Plane {
  uint32_t addr, stride;
  int format, bpp, rows, scale;
  uint64_t accessEpoch = 0, used = 0;
  bool dirty = false;
  std::vector<uint8_t> native, pixels;
  void refresh();
  void reseed();
  void accept_native();
};

void set_scale(int scale);
int scale();
void reset();
void native_access();
// A caller retaining a writable VRAM pointer across GE execution must notify even same-value writes.
void written(uint32_t addr, size_t bytes);
void diagnostic(const char* reason);
std::shared_ptr<Plane> acquire(uint32_t addr, uint32_t stride, int format, int rows);
bool overlaps(uint32_t a, size_t an, uint32_t b, size_t bn);
bool is_target(uint32_t addr, size_t bytes);
// Returns packed guest-format pixels at the selected scale, not a resampled native image.
std::shared_ptr<Plane> display(uint32_t addr, uint32_t stride, int format);
size_t allocated_bytes();
size_t budget_bytes();

}  // namespace psp::ge::surface

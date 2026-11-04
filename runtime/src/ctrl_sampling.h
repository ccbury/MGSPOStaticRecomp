#pragma once
#include <cstdint>

namespace psp {
namespace k {

struct CtrlReadCycle {
  uint64_t consumed = UINT64_MAX;

  bool take(uint64_t cycle) {
    if (consumed == cycle) return false;
    consumed = cycle;
    return true;
  }
};

}  // namespace k
}  // namespace psp

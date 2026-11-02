#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "bytes.h"

namespace mgspo {

struct SfoValue {
  std::string key;
  bool isInt = false;
  std::string str;
  uint32_t num = 0;
};

class Sfo {
public:
  static Sfo parse(const Bytes& data);

  const std::vector<SfoValue>& values() const { return values_; }
  const SfoValue* get(const std::string& key) const;
  std::string str(const std::string& key) const;

private:
  std::vector<SfoValue> values_;
};

}  // namespace mgspo

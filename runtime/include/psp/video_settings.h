#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

namespace psp::video {
struct Size { int width, height; };
inline int render_scale(const std::string& value) {
  if (value.size() == 1 && value[0] >= '1' && value[0] <= '8') return value[0] - '0';
  throw std::invalid_argument("Internal render scale must be an integer from 1 through 8 (480x272 through 3840x2176).");
}
inline Size window_size(const std::string& value) {
  if (value == "480x272") return {480, 272};
  if (value == "960x544") return {960, 544};
  if (value == "1440x816") return {1440, 816};
  if (value == "1920x1088") return {1920, 1088};
  throw std::invalid_argument("Window size must be 480x272, 960x544, 1440x816 or 1920x1088. You can resize it after launch.");
}
// Fits the source aspect ratio without changing simulation or input coordinates.
inline Size fit(Size client, Size source) {
  if (client.width <= 0 || client.height <= 0 || source.width <= 0 || source.height <= 0 ||
      client.width > 65536 || client.height > 65536 || source.width > 3840 || source.height > 2176)
    return {0, 0};
  if (int64_t(client.width) * source.height > int64_t(client.height) * source.width)
    client.width = client.height * source.width / source.height;
  else client.height = client.width * source.height / source.width;
  return client;
}
}

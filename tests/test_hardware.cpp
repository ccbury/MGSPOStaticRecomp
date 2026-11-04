#include "hardware_renderer.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace h = psp::ge::hardware;
static void check(bool result, const char* message) {
  if (!result) throw std::runtime_error(message);
}
static h::Command triangle(int scale) {
  h::Command c;
  c.sizes[0] = c.sizes[1] = 8 * scale; c.sizes[2] = 3;
  c.clip[2] = c.clip[3] = 8 * scale - 1;
  c.bounds[2] = c.bounds[3] = 8 * scale;
  const float positions[3][2] = {{2,2},{2.75f,2},{2,2.75f}};
  for (int i = 0; i < 3; ++i) {
    auto& v = c.vertices[i];
    v.position[0] = positions[i][0] * scale; v.position[1] = positions[i][1] * scale;
    v.position[2] = 100; v.position[3] = 1;
    std::fill(v.colour, v.colour + 4, 1.0f);
  }
  return c;
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("Specify d3d11, d3d12 or vulkan.");
    auto backend = h::parse(argv[1]);
    if (!h::built(h::Backend::Vulkan)) {
      bool rejected = false;
      try { h::select(h::Backend::Vulkan); } catch (const std::runtime_error&) { rejected = true; }
      check(rejected && h::selected() == h::Backend::Software, "Unavailable backend silently selected or fell back.");
    }
    std::printf("%s\n", h::probe(backend).c_str());
    h::select(backend);
    {
      h::Job invalid;
      invalid.colour.resize(64); invalid.depth.resize(64);
      auto command = triangle(1);
      command.bounds[0] = UINT32_MAX;
      invalid.commands = {command};
      const auto count = h::counters().submissions;
      bool rejected = false;
      try { h::run(invalid); } catch (const std::runtime_error&) { rejected = true; }
      check(rejected && h::counters().submissions == count, "Invalid GPU bounds were not rejected before submission.");
    }
    for (int scale = 1; scale <= 8; ++scale) {
      for (uint32_t format = 0; format < 4; ++format) {
        h::Job job;
        job.colour.resize(size_t(8 * scale) * 8 * scale);
        job.depth.resize(job.colour.size(), 65535);
        auto command = triangle(scale);
        command.sizes[2] = format;
        command.features[0] = h::DepthTest;
        command.registers[0xDE] = 4;
        job.commands.push_back(command);
        h::run(job);
        const size_t sample = size_t(2 * scale) * (8 * scale) + 2 * scale;
        check(job.colour[sample] == (scale == 1 ? 0u : format == 3 ? UINT32_MAX : 65535u),
              "Subpixel GPU triangle/packed format coverage mismatch.");
        check(job.depth[sample] == (scale == 1 ? 65535u : 100u), "GPU independent depth mismatch.");
        command.modes[3] = 1;
        command.vertices[0].position[0] = command.vertices[0].position[1] = static_cast<float>(scale);
        command.vertices[1].position[0] = command.vertices[1].position[1] = static_cast<float>(4 * scale);
        command.vertices[1].position[2] = 200;
        command.vertices[1].colour[1] = command.vertices[1].colour[2] = 0;
        job.commands = {command};
        h::run(job);
        check(job.colour[sample] == (scale == 1 ? 0xFF0000FFu : format == 3 ? UINT32_MAX : 65535u) ||
              (scale == 1 && format != 3 && job.colour[sample] != 0), "GPU depth occlusion failed.");
        // Alpha reject and packed write masks must not modify the target.
        auto before = job.colour;
        command.features[0] = h::AlphaTest;
        command.registers[0xDB] = 0;  // never
        job.commands = {command};
        h::run(job);
        check(job.colour == before, "GPU alpha rejection modified pixels.");
        command.features[0] = 0; command.sizes[3] = UINT32_MAX;
        job.commands = {command}; h::run(job);
        check(job.colour == before, "GPU write mask modified pixels.");
      }
    }
    check(h::counters().dispatches > 0 && h::counters().primitives > 0, "No actual GPU geometry submitted.");
    {
      h::Job ordered;
      ordered.colour.resize(64); ordered.depth.resize(1);
      auto command = triangle(1);
      command.modes[3] = 1;
      command.vertices[0].position[0] = command.vertices[0].position[1] = 0;
      command.vertices[1].position[0] = command.vertices[1].position[1] = 8;
      for (int i = 0; i < 150; ++i) {
        command.vertices[1].colour[0] = i / 255.0f;
        command.vertices[1].colour[1] = command.vertices[1].colour[2] = 0;
        ordered.commands.push_back(command);
      }
      const auto before = h::counters();
      h::run(ordered);
      check(ordered.colour[0] == 0xFF000095 && h::counters().submissions == before.submissions + 3 &&
            h::counters().primitives == before.primitives + 150, "Bounded submissions dropped or reordered primitives.");
    }
    std::printf("GPU submissions=%llu dispatches=%llu primitives=%llu; scales1..8 packed/depth/alpha/mask passed.\n",
                 (unsigned long long)h::counters().submissions, (unsigned long long)h::counters().dispatches,
                 (unsigned long long)h::counters().primitives);
    h::reset();
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}

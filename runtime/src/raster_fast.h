// Fast span rasteriser used by the GE for the common draw states.
//
// Raster (gpu_draw.cpp) keeps the generic per-fragment pipeline as the reference implementation. When the
// state of a draw is simple enough (no fog, colour test, stencil, logic op or secondary colour) it fills in
// a Ctx and feeds triangles and sprites to a Batch instead. The batch snaps vertices to the 1/16 pixel grid,
// finds the covered span of every row analytically (no per-pixel coverage test), interpolates with plane
// equations and shades with integer maths. Large batches are split into bands of rows that run on worker
// threads; the bands never overlap so the result does not depend on the thread count.
#pragma once
#include <cstdint>
#include <vector>

namespace psp {
namespace ge {
namespace fast {

struct Ctx {
  // framebuffer
  uint8_t* fb = nullptr;
  uint32_t fbStride = 0;
  int fbFmt = 3;       // 0 565, 1 5551, 2 4444, 3 8888
  uint32_t keep = 0;   // write mask in the framebuffer's packed format (bits set are kept)
  int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;

  // depth
  uint8_t* zb = nullptr;
  uint32_t zStride = 0;
  bool zTest = false, zWrite = true;
  int zFunc = 0;
  bool clipZ = false;
  float zlo = 0, zhi = 0;

  // texture: either decoded RGBA (texRgba) or 8-bit indices plus a resolved palette
  bool tex = false;
  const uint32_t* texRgba = nullptr;
  const uint8_t* texIdx = nullptr;
  const uint32_t* pal = nullptr;
  int tW = 1, tH = 1;
  bool clampU = false, clampV = false, linear = false;
  int tFunc = 0;
  bool tAlpha = true, tDouble = false;
  uint32_t env = 0;

  bool blend = false;
  int sfac = 0, dfac = 0, bfunc = 0;
  uint32_t fixA = 0, fixB = 0;

  bool atest = false;
  bool fog = false;
  uint32_t fogCol = 0;
  int aFunc = 0, aRef = 0, aMask = 0;
};

struct Vert {
  float x, y, z, invw, u, v;
  float fog = 1;
  float col[4];  // 0..1
};

// Texture data that has been decoded for sampling. Owned by the cache; valid until the next lookup that evicts it.
struct Texture {
  const uint32_t* rgba = nullptr;
  const uint8_t* idx = nullptr;
};

struct TexSource {
  const uint8_t* mem = nullptr;
  size_t avail = 0;   // readable bytes at mem
  uint32_t addr = 0;  // guest address, part of the cache key
  int fmt = 0;        // GE texture format 0..5
  bool swizzled = false;
  int bufW = 0, w = 0, h = 0;
  bool inVram = false;  // pending draws may still be writing to the source
};

// Decodes (or finds in the cache) the texture. Formats 0-5 only. The source bytes are hashed on every call so
// stale data is never used. Returns false for unsupported formats or sources outside the cache budget.
bool texture_lookup(const TexSource& src, Texture& out);

// Primitives are not rendered right away. Each draw call appends its triangles and sprites to a global queue
// together with a copy of the draw state; sync() renders everything that is queued, split into bands of rows
// that run on worker threads. Draws that follow each other in the queue stay in order inside every band, and
// bands never overlap, so the result does not depend on the thread count. The queue must be synced before
// anything else reads or writes VRAM (end of a list, block transfers, reference-path pixels, textures that
// live in VRAM). Capacity limits also drain the queue synchronously; no draws are discarded.
class Batch {
 public:
  struct Prim;
  Batch() = default;
  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;

  // pal: resolved palette (256 entries) when the texture is indexed. cat: stats category 1..3.
  void begin(const Ctx* cx, const uint32_t* pal, int cat);
  // Return false if the primitive cannot be handled (coordinates out of range); nothing is queued then.
  bool add_tri(const Vert& a, const Vert& b, const Vert& c);
  bool add_rect(const Vert& a, const Vert& b);

 private:
  void open();
  const Ctx* cx_ = nullptr;
  const uint32_t* pal_ = nullptr;
  int cat_ = 1;
  uint64_t epoch_ = ~0ull;
};

// Renders everything that is queued. Cheap when nothing is.
void sync();

// Where sync() reports fragment counts and per category raster time (4 entries). May be null.
void set_stats_sink(uint64_t* fragments, uint64_t* catNs);
// Clears a rectangle in clear mode. Colour is 8888 RGBA, z is the 16-bit depth value.
void clear_rect(const Ctx& cx, int x0, int y0, int x1, int y1, bool clearColour, bool clearAlpha, bool clearDepth, uint32_t rgba,
                uint16_t z);

// Total rendering threads, including the caller, clamped to 1..6 (0 or 1 = single threaded).
void set_threads(int n);

// MGSPO_GE_REF=1 forces the reference rasteriser.
enum class Mode { Fast, Reference };
Mode mode();
void set_mode(Mode m);
int threads();

}  // namespace fast
}  // namespace ge
}  // namespace psp

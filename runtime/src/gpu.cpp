// GE command interpreter: list control flow, state registers, matrices and block transfers.
#include <cstring>
#include <stdexcept>

#include <psp/memory.h>

#include "gpu.h"

namespace psp {
namespace ge {

namespace {

float f24(uint32_t arg) {
  uint32_t bits = arg << 8;
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

bool mem_ok(uint32_t a) {
  uint32_t p = a & 0x3FFFFFFF;
  return (p - kRamBase < kRamSize) || (p - kVramBase < kVramSize) || (p - kScratchBase < kScratchSize);
}

struct Name {
  uint8_t op;
  const char* name;
};

const Name kNames[] = {
    {0x00, "NOP"},         {0x01, "VADDR"},       {0x02, "IADDR"},        {0x04, "PRIM"},
    {0x05, "BEZIER"},      {0x06, "SPLINE"},      {0x07, "BBOX"},         {0x08, "JUMP"},
    {0x09, "BJUMP"},       {0x0A, "CALL"},        {0x0B, "RET"},          {0x0C, "END"},
    {0x0E, "SIGNAL"},      {0x0F, "FINISH"},      {0x10, "BASE"},         {0x12, "VERTEXTYPE"},
    {0x13, "OFFSETADDR"},  {0x14, "ORIGIN"},      {0x15, "REGION1"},      {0x16, "REGION2"},
    {0x17, "LIGHTINGENABLE"}, {0x18, "LIGHTENABLE0"}, {0x19, "LIGHTENABLE1"}, {0x1A, "LIGHTENABLE2"},
    {0x1B, "LIGHTENABLE3"}, {0x1C, "CLIPENABLE"}, {0x1D, "CULLFACEENABLE"}, {0x1E, "TEXTUREMAPENABLE"},
    {0x1F, "FOGENABLE"},   {0x20, "DITHERENABLE"}, {0x21, "ALPHABLENDENABLE"}, {0x22, "ALPHATESTENABLE"},
    {0x23, "ZTESTENABLE"}, {0x24, "STENCILTESTENABLE"}, {0x25, "ANTIALIASENABLE"}, {0x26, "PATCHCULLENABLE"},
    {0x27, "COLORTESTENABLE"}, {0x28, "LOGICOPENABLE"}, {0x2A, "BONEMATRIXNUMBER"}, {0x2B, "BONEMATRIXDATA"},
    {0x2C, "MORPHWEIGHT0"}, {0x2D, "MORPHWEIGHT1"}, {0x2E, "MORPHWEIGHT2"}, {0x2F, "MORPHWEIGHT3"},
    {0x30, "MORPHWEIGHT4"}, {0x31, "MORPHWEIGHT5"}, {0x32, "MORPHWEIGHT6"}, {0x33, "MORPHWEIGHT7"},
    {0x36, "PATCHDIVISION"}, {0x37, "PATCHPRIMITIVE"}, {0x38, "PATCHFACING"},
    {0x3A, "WORLDMATRIXNUMBER"}, {0x3B, "WORLDMATRIXDATA"}, {0x3C, "VIEWMATRIXNUMBER"}, {0x3D, "VIEWMATRIXDATA"},
    {0x3E, "PROJMATRIXNUMBER"}, {0x3F, "PROJMATRIXDATA"}, {0x40, "TGENMATRIXNUMBER"}, {0x41, "TGENMATRIXDATA"},
    {0x42, "VIEWPORTXSCALE"}, {0x43, "VIEWPORTYSCALE"}, {0x44, "VIEWPORTZSCALE"}, {0x45, "VIEWPORTXCENTER"},
    {0x46, "VIEWPORTYCENTER"}, {0x47, "VIEWPORTZCENTER"}, {0x48, "TEXSCALEU"}, {0x49, "TEXSCALEV"},
    {0x4A, "TEXOFFSETU"},  {0x4B, "TEXOFFSETV"},  {0x4C, "OFFSETX"},      {0x4D, "OFFSETY"},
    {0x50, "SHADEMODE"},   {0x51, "REVERSENORMAL"}, {0x53, "MATERIALUPDATE"}, {0x54, "MATERIALEMISSIVE"},
    {0x55, "MATERIALAMBIENT"}, {0x56, "MATERIALDIFFUSE"}, {0x57, "MATERIALSPECULAR"}, {0x58, "MATERIALALPHA"},
    {0x5B, "MATERIALSPECULARCOEF"}, {0x5C, "AMBIENTCOLOR"}, {0x5D, "AMBIENTALPHA"}, {0x5E, "LIGHTMODE"},
    {0x5F, "LIGHTTYPE0"},  {0x60, "LIGHTTYPE1"},  {0x61, "LIGHTTYPE2"},  {0x62, "LIGHTTYPE3"},
    {0x9B, "CULL"},        {0x9C, "FRAMEBUFPTR"}, {0x9D, "FRAMEBUFWIDTH"}, {0x9E, "ZBUFPTR"},
    {0x9F, "ZBUFWIDTH"},   {0xA0, "TEXADDR0"},    {0xA8, "TEXBUFWIDTH0"}, {0xB0, "CLUTADDR"},
    {0xB1, "CLUTADDRUPPER"}, {0xB2, "TRANSFERSRC"}, {0xB3, "TRANSFERSRCW"}, {0xB4, "TRANSFERDST"},
    {0xB5, "TRANSFERDSTW"}, {0xB8, "TEXSIZE0"},   {0xC0, "TEXMAPMODE"},   {0xC1, "TEXSHADELS"},
    {0xC2, "TEXMODE"},     {0xC3, "TEXFORMAT"},   {0xC4, "LOADCLUT"},     {0xC5, "CLUTFORMAT"},
    {0xC6, "TEXFILTER"},   {0xC7, "TEXWRAP"},     {0xC8, "TEXLEVEL"},     {0xC9, "TEXFUNC"},
    {0xCA, "TEXENVCOLOR"}, {0xCB, "TEXFLUSH"},    {0xCC, "TEXSYNC"},      {0xCD, "FOG1"},
    {0xCE, "FOG2"},        {0xCF, "FOGCOLOR"},    {0xD0, "TEXLODSLOPE"},  {0xD2, "FRAMEBUFPIXFORMAT"},
    {0xD3, "CLEARMODE"},   {0xD4, "SCISSOR1"},    {0xD5, "SCISSOR2"},     {0xD6, "MINZ"},
    {0xD7, "MAXZ"},        {0xD8, "COLORTEST"},   {0xD9, "COLORREF"},     {0xDA, "COLORTESTMASK"},
    {0xDB, "ALPHATEST"},   {0xDC, "STENCILTEST"}, {0xDD, "STENCILOP"},    {0xDE, "ZTEST"},
    {0xDF, "BLENDMODE"},   {0xE0, "BLENDFIXEDA"}, {0xE1, "BLENDFIXEDB"},  {0xE2, "DITH0"},
    {0xE3, "DITH1"},       {0xE4, "DITH2"},       {0xE5, "DITH3"},        {0xE6, "LOGICOP"},
    {0xE7, "ZWRITEDISABLE"}, {0xE8, "MASKRGB"},   {0xE9, "MASKALPHA"},    {0xEA, "TRANSFERSTART"},
    {0xEB, "TRANSFERSRCPOS"}, {0xEC, "TRANSFERDSTPOS"}, {0xEE, "TRANSFERSIZE"},
};

}  // namespace

const char* Gpu::command_name(uint32_t op) {
  op &= 0xFF;
  if (op >= 0x63 && op <= 0x9A) {
    if (op < 0x6F) return "LIGHTPOS";
    if (op < 0x7B) return "LIGHTDIR";
    if (op < 0x87) return "LIGHTATT";
    if (op < 0x8B) return "LIGHTSPOTEXP";
    if (op < 0x8F) return "LIGHTSPOTCUT";
    return "LIGHTCOLOR";
  }
  if (op >= 0xA0 && op <= 0xA7) return "TEXADDR";
  if (op >= 0xA8 && op <= 0xAF) return "TEXBUFWIDTH";
  if (op >= 0xB8 && op <= 0xBF) return "TEXSIZE";
  for (const Name& n : kNames)
    if (n.op == op) return n.name;
  return "?";
}

Gpu::Gpu() { reset(); }

void Gpu::reset() {
  std::memset(r_, 0, sizeof r_);
  std::memset(bone_, 0, sizeof bone_);
  std::memset(world_, 0, sizeof world_);
  std::memset(view_, 0, sizeof view_);
  std::memset(tgen_, 0, sizeof tgen_);
  std::memset(proj_, 0, sizeof proj_);
  std::memset(clut_, 0, sizeof clut_);
  clutBytes_ = 0;
  boneIdx_ = worldIdx_ = viewIdx_ = projIdx_ = tgenIdx_ = 0;
  vaddr_ = iaddr_ = base_ = offset_ = 0;
  bboxVisible_ = 1;
  fault_ = false;
  // identity matrices
  for (float* m : {world_, view_, tgen_}) m[0] = m[4] = m[8] = 1.0f;
  proj_[0] = proj_[5] = proj_[10] = proj_[15] = 1.0f;
  // defaults the hardware has after reset that games rarely bother to set
  r_[0x50] = 1;           // gouraud
  r_[0x5B] = 0;           // specular coefficient
  r_[0xD6] = 0;           // min z
  r_[0xD7] = 0xFFFF;      // max z
  r_[0xE8] = 0;
  r_[0xE9] = 0;
}

bool Gpu::get_matrix(int type, float* out) const {
  if (type >= 0 && type < 8) std::memcpy(out, &bone_[type * 12], 12 * sizeof(float));
  else if (type == 8) std::memcpy(out, world_, sizeof world_);
  else if (type == 9) std::memcpy(out, view_, sizeof view_);
  else if (type == 10) std::memcpy(out, proj_, sizeof proj_);
  else if (type == 11) std::memcpy(out, tgen_, sizeof tgen_);
  else return false;
  return true;
}

void Gpu::save_context(uint32_t* out) const {
  std::memcpy(out, r_, sizeof r_);
  out += 256;
  std::memcpy(out, bone_, sizeof bone_);
  out += 96;
  std::memcpy(out, world_, sizeof world_);
  out += 12;
  std::memcpy(out, view_, sizeof view_);
  out += 12;
  std::memcpy(out, proj_, sizeof proj_);
  out += 16;
  std::memcpy(out, tgen_, sizeof tgen_);
}

void Gpu::restore_context(const uint32_t* in) {
  std::memcpy(r_, in, sizeof r_);
  in += 256;
  std::memcpy(bone_, in, sizeof bone_);
  in += 96;
  std::memcpy(world_, in, sizeof world_);
  in += 12;
  std::memcpy(view_, in, sizeof view_);
  in += 12;
  std::memcpy(proj_, in, sizeof proj_);
  in += 16;
  std::memcpy(tgen_, in, sizeof tgen_);
  base_ = (r_[0x10] & 0xFF0000) << 8;
  offset_ = (r_[0x13] & 0xFFFFFF) << 8;
}

void Gpu::exec_word(uint32_t word) {
  Stop s = Stop::Stalled;
  execute(word, nullptr, nullptr, &s);
}

Stop Gpu::run(ListContext& l, Host* host) {
  int guard = 0;
  try {
    while (true) {
      if (l.stall && (l.pc & 0x0FFFFFFF) == (l.stall & 0x0FFFFFFF)) return Stop::Stalled;
      if (!mem_ok(l.pc) || ++guard > 16000000) return Stop::Fault;
      uint32_t w = lw(l.pc);
      l.pc += 4;
      Stop stop = Stop::Stalled;
      execute(w, &l, host, &stop);
      if (stop != Stop::Stalled) return stop;
    }
  } catch (const std::runtime_error&) {
    return Stop::Fault;
  }
}

void Gpu::execute(uint32_t word, ListContext* l, Host* host, Stop* stop) {
  const uint32_t op = word >> 24, arg = word & 0xFFFFFF;
  if (stats_) stats_->commands[op]++;
  if (dump_) std::fprintf(dump_, "%08X: %02X %-18s %06X\n", l ? l->pc - 4 : 0u, op, command_name(op), arg);
  r_[op] = arg;

  switch (op) {
    case 0x01: vaddr_ = rel_addr(arg); break;
    case 0x02: iaddr_ = rel_addr(arg); break;
    case 0x04: draw_prim(arg); break;
    case 0x05: draw_patch(arg, false); break;
    case 0x06: draw_patch(arg, true); break;
    case 0x07: bbox(arg); break;
    case 0x08:
      if (l) l->pc = rel_addr(arg) & ~3u;
      break;
    case 0x09:
      if (l && !bboxVisible_) l->pc = rel_addr(arg) & ~3u;
      break;
    case 0x0A:
      if (l && l->stack.size() < 64) {
        l->stack.push_back({l->pc, base_, offset_});
        l->pc = rel_addr(arg) & ~3u;
      }
      break;
    case 0x0B:
      if (l && !l->stack.empty()) {
        ListContext::Frame f = l->stack.back();
        l->stack.pop_back();
        l->pc = f.pc;
        base_ = f.base;
        offset_ = f.offset;
      }
      break;
    case 0x0C: {
      if (!l) break;
      uint32_t prev = mem_ok(l->pc - 8) ? lw(l->pc - 8) : 0;
      uint32_t pk = prev >> 24;
      if (pk == 0x0E) {
        uint32_t behaviour = (prev >> 16) & 0xFF, sig = prev & 0xFFFF;
        if (host) host->on_signal((behaviour << 16) | sig);
        uint32_t target = ((sig << 16) | (arg & 0xFFFF)) & 0x0FFFFFFC;
        switch (behaviour) {
          case 0x03: *stop = Stop::Paused; break;
          case 0x10: l->pc = target; break;
          case 0x11:
            l->stack.push_back({l->pc, base_, offset_});
            l->pc = target;
            break;
          case 0x12:
            if (!l->stack.empty()) {
              l->pc = l->stack.back().pc;
              l->stack.pop_back();
            }
            break;
          case 0x13: l->pc = (l->pc - 8 + target) & 0x0FFFFFFC; break;
          case 0x14:
            l->stack.push_back({l->pc, base_, offset_});
            l->pc = (l->pc - 8 + target) & 0x0FFFFFFC;
            break;
          case 0x15: l->pc = (l->origin + target) & 0x0FFFFFFC; break;
          case 0x16:
            l->stack.push_back({l->pc, base_, offset_});
            l->pc = (l->origin + target) & 0x0FFFFFFC;
            break;
          default: break;  // sync / continue: the list carries on
        }
      } else if (pk == 0x0F) {
        if (host) host->on_finish(prev & 0xFFFF);
        *stop = Stop::Ended;
      } else {
        *stop = Stop::Ended;
      }
      break;
    }
    case 0x10: base_ = (arg & 0xFF0000) << 8; break;
    case 0x13: offset_ = arg << 8; break;
    case 0x14:
      if (l) l->origin = l->pc - 4;
      break;
    case 0x2A: boneIdx_ = arg & 0x7F; break;
    case 0x2B:
      if (boneIdx_ < 96) bone_[boneIdx_] = f24(arg);
      ++boneIdx_;
      break;
    case 0x3A: worldIdx_ = arg & 0xF; break;
    case 0x3B:
      if (worldIdx_ < 12) world_[worldIdx_] = f24(arg);
      ++worldIdx_;
      break;
    case 0x3C: viewIdx_ = arg & 0xF; break;
    case 0x3D:
      if (viewIdx_ < 12) view_[viewIdx_] = f24(arg);
      ++viewIdx_;
      break;
    case 0x3E: projIdx_ = arg & 0x1F; break;
    case 0x3F:
      if (projIdx_ < 16) proj_[projIdx_] = f24(arg);
      ++projIdx_;
      break;
    case 0x40: tgenIdx_ = arg & 0xF; break;
    case 0x41:
      if (tgenIdx_ < 12) tgen_[tgenIdx_] = f24(arg);
      ++tgenIdx_;
      break;
    case 0xC4: load_clut(arg); break;
    case 0xEA: transfer(); break;
    default: break;
  }
}

void Gpu::load_clut(uint32_t arg) {
  uint32_t addr = (r_[0xB0] & 0xFFFFFF) | ((r_[0xB1] & 0x0F0000) << 8);
  uint32_t bytes = (arg & 0x3F) * 32;
  if (!bytes) return;
  if (bytes > sizeof clut_) bytes = sizeof clut_;
  std::memcpy(clut_, ptr(addr), bytes);
  clutBytes_ = bytes;
  if (stats_) stats_->clutLoads++;
}

void Gpu::transfer() {
  uint32_t src = (r_[0xB2] & 0xFFFFFF) | ((r_[0xB3] & 0xFF0000) << 8);
  uint32_t dst = (r_[0xB4] & 0xFFFFFF) | ((r_[0xB5] & 0xFF0000) << 8);
  uint32_t sw = r_[0xB3] & 0x7FF, dw = r_[0xB5] & 0x7FF;
  uint32_t sx = r_[0xEB] & 0x3FF, sy = (r_[0xEB] >> 10) & 0x3FF;
  uint32_t dx = r_[0xEC] & 0x3FF, dy = (r_[0xEC] >> 10) & 0x3FF;
  uint32_t w = (r_[0xEE] & 0x3FF) + 1, h = ((r_[0xEE] >> 10) & 0x3FF) + 1;
  uint32_t bpp = (r_[0xEA] & 1) ? 4 : 2;
  for (uint32_t y = 0; y < h; ++y) {
    uint32_t s = src + ((sy + y) * sw + sx) * bpp;
    uint32_t d = dst + ((dy + y) * dw + dx) * bpp;
    std::memmove(ptr(d), ptr(s), w * bpp);
  }
  if (stats_) {
    stats_->transforms++;
    stats_->transformBytes += static_cast<uint64_t>(w) * h * bpp;
  }
}

}  // namespace ge
}  // namespace psp

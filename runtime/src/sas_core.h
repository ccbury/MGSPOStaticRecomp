// Software model of the PSP's SAS (sound) unit: 32 voices of ADPCM/PCM/noise, an ADSR envelope per voice,
// pitch resampling, a dry mix and a simple reverb send. Sample data is read from guest memory.
#pragma once
#include <cstdint>
#include <vector>

namespace psp {
namespace sas {

constexpr int kMaxVoices = 32;
constexpr int kPitchBase = 0x1000;
constexpr int kPitchMax = 0x4000;
constexpr int kVolMax = 0x1000;
constexpr int32_t kEnvMax = 0x3FFFFFFF;

enum CurveMode { LinearInc = 0, LinearDec = 1, LinearBent = 2, ExpDec = 3, ExpInc = 4, Direct = 5 };
enum OutputMode { Mixed = 0, Raw = 1 };

// Decoders for the bitfields taken by sceSasSetSimpleADSR.
struct SimpleAdsr {
  int attackRate, decayRate, sustainRate, releaseRate;
  int attackType, decayType, sustainType, releaseType;
  int sustainLevel;
};
SimpleAdsr decode_simple_adsr(uint32_t env1, uint32_t env2);

class Envelope {
 public:
  enum State { Off, Attack, Decay, Sustain, Release };

  void key_on();
  void key_off();
  void kill();
  void step();
  int32_t height() const { return height_; }
  State state() const { return state_; }
  bool active() const { return state_ != Off; }

  void set_rates(int flag, int a, int d, int s, int r);
  void set_modes(int flag, int a, int d, int s, int r);
  void set_simple(uint32_t env1, uint32_t env2);
  void set_sustain_level(int level);

  int attackRate = 0x7FFFFFFF, decayRate = 0x7FFFFFFF, sustainRate = 0, releaseRate = 0x40000000;
  int attackType = LinearInc, decayType = ExpDec, sustainType = Direct, releaseType = LinearDec;
  int sustainLevel = kEnvMax;

 private:
  State state_ = Off;
  int32_t height_ = 0;
};

// Decodes the PSX/PSP ADPCM ("VAG") block format, 28 samples per 16 byte block.
class VagDecoder {
 public:
  void start(uint32_t addr, uint32_t size, bool loop);
  // Returns false once the stream has run out.
  bool next(int16_t& out);

 private:
  bool decode_block();
  uint32_t addr_ = 0;
  int numBlocks_ = 0;
  int block_ = 0;
  int loopStart_ = 0;
  bool loop_ = false, loopAtEnd_ = false, ended_ = true;
  int pos_ = 28;
  int s1_ = 0, s2_ = 0;
  int16_t samples_[28] = {};
};

enum class VoiceType { None, Vag, Pcm, Noise };

struct Voice {
  VoiceType type = VoiceType::None;
  uint32_t vagAddr = 0, vagSize = 0;
  uint32_t pcmAddr = 0, pcmSize = 0, pcmPos = 0, pcmLoopPos = 0;
  bool loop = false;
  int noiseFreq = 0;
  uint32_t lfsr = 0x1234567;
  int pitch = kPitchBase;
  int volL = 0, volR = 0, effL = 0, effR = 0;
  bool on = false, playing = false, paused = false;
  Envelope env;
  VagDecoder vag;

  uint32_t frac = 0;
  int cur = 0, nxt = 0;
  bool primed = false;
  bool srcEnded = false;

  void key_on();
  void key_off();
  void reset_source();
  bool read_source(int16_t& s);
};

// Small feedback delay network standing in for the hardware reverb.
class Reverb {
 public:
  void set_type(int type);
  void set_params(int delay, int feedback);
  void process(const int32_t* send, int frames, int32_t* outL, int32_t* outR, int volL, int volR);
  int type = -1;

 private:
  struct Line {
    std::vector<int32_t> buf;
    size_t pos = 0;
  };
  void configure();
  int delay_ = 0, feedback_ = 0;
  Line l_[2], r_[2];
  int fb_ = 0;  // 0..4096
  int lenL_[2] = {0, 0}, lenR_[2] = {0, 0};
};

class Core {
 public:
  void init(int grain, int maxVoices, int outputMode, int sampleRate);
  bool initialised() const { return grain_ != 0; }
  int grain() const { return grain_; }
  void set_grain(int g) {
    grain_ = g;
    dry_.assign(static_cast<size_t>(g) * 2, 0);
    send_.assign(static_cast<size_t>(g) * 2, 0);
  }
  int output_mode() const { return outputMode_; }
  void set_output_mode(int m) { outputMode_ = m; }
  int max_voices() const { return maxVoices_; }

  Voice voices[kMaxVoices];
  Reverb reverb;
  bool dryOn = true, wetOn = false;
  int revVolL = 0, revVolR = 0;

  // Mixes one grain. Mixed mode writes grain*2 interleaved frames to out; raw mode writes four planes
  // (dry L, dry R, send L, send R). When `in` is set it is added to the output scaled by inL/inR (0x1000 = 1.0).
  void mix(int16_t* out, const int16_t* in, int inL, int inR);
  uint32_t end_flags() const;
  uint32_t pause_flags() const;

 private:
  void mix_voice(Voice& v);
  int grain_ = 0, maxVoices_ = kMaxVoices, outputMode_ = Mixed, sampleRate_ = 44100;
  std::vector<int32_t> dry_, send_;
};

}  // namespace sas
}  // namespace psp

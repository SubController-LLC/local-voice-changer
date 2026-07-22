#pragma once

#include <cstddef>
#include <span>

namespace vvc::dsp {

struct SafetyChainSettings {
  float high_pass_hz = 80.0F;
  float gate_threshold_db = -52.0F;
  float gate_attack_ms = 5.0F;
  float gate_hold_ms = 50.0F;
  float gate_release_ms = 120.0F;
  float compressor_threshold_db = -18.0F;
  float compressor_ratio = 3.0F;
  float compressor_attack_ms = 8.0F;
  float compressor_release_ms = 80.0F;
  float limiter_ceiling_db = -1.0F;
};

class HighPassFilter {
public:
  HighPassFilter(float sampleRate, float cutoffHz);
  [[nodiscard]] float process(float input);
  void reset();

private:
  float coefficient_ = 0.0F;
  float previous_input_ = 0.0F;
  float previous_output_ = 0.0F;
};

class NoiseGate {
public:
  NoiseGate(float sampleRate, const SafetyChainSettings& settings);
  [[nodiscard]] float process(float input);
  void reset();

private:
  float threshold_ = 0.0F;
  float attack_coefficient_ = 0.0F;
  float release_coefficient_ = 0.0F;
  std::size_t hold_samples_ = 0U;
  std::size_t hold_remaining_ = 0U;
  float envelope_ = 0.0F;
  float gain_ = 0.0F;
};

class Compressor {
public:
  Compressor(float sampleRate, const SafetyChainSettings& settings);
  [[nodiscard]] float process(float input);
  void reset();

private:
  float threshold_db_ = 0.0F;
  float ratio_ = 1.0F;
  float attack_coefficient_ = 0.0F;
  float release_coefficient_ = 0.0F;
  float gain_ = 1.0F;
};

class SoftLimiter {
public:
  explicit SoftLimiter(float ceilingDb = -1.0F);
  [[nodiscard]] float process(float input) const;
  [[nodiscard]] float ceilingLinear() const;

private:
  float ceiling_ = 1.0F;
};

class SafetyChain {
public:
  explicit SafetyChain(float sampleRate = 48'000.0F,
                       SafetyChainSettings settings = SafetyChainSettings{});

  void reset();
  void process(std::span<float> samples);
  [[nodiscard]] float limiterCeilingLinear() const;

private:
  HighPassFilter high_pass_;
  NoiseGate gate_;
  Compressor compressor_;
  SoftLimiter limiter_;
};

} // namespace vvc::dsp

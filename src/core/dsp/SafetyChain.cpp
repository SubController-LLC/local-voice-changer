#include "core/dsp/SafetyChain.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vvc::dsp {
namespace {

[[nodiscard]] float dbToLinear(const float decibels) { return std::pow(10.0F, decibels / 20.0F); }

[[nodiscard]] float smoothingCoefficient(const float milliseconds, const float sampleRate) {
  const float seconds = std::max(milliseconds, 0.01F) / 1'000.0F;
  return std::exp(-1.0F / (seconds * sampleRate));
}

} // namespace

HighPassFilter::HighPassFilter(const float sampleRate, const float cutoffHz) {
  const float timeConstant = 1.0F / (2.0F * std::numbers::pi_v<float> * cutoffHz);
  const float sampleInterval = 1.0F / sampleRate;
  coefficient_ = timeConstant / (timeConstant + sampleInterval);
}

float HighPassFilter::process(const float input) {
  const float output = coefficient_ * (previous_output_ + input - previous_input_);
  previous_input_ = input;
  previous_output_ = output;
  return output;
}

void HighPassFilter::reset() {
  previous_input_ = 0.0F;
  previous_output_ = 0.0F;
}

NoiseGate::NoiseGate(const float sampleRate, const SafetyChainSettings& settings)
    : threshold_(dbToLinear(settings.gate_threshold_db)),
      attack_coefficient_(smoothingCoefficient(settings.gate_attack_ms, sampleRate)),
      release_coefficient_(smoothingCoefficient(settings.gate_release_ms, sampleRate)),
      hold_samples_(
          static_cast<std::size_t>(std::lround(settings.gate_hold_ms * sampleRate / 1'000.0F))) {}

float NoiseGate::process(const float input) {
  const float magnitude = std::abs(input);
  const float envelopeCoefficient =
      magnitude > envelope_ ? attack_coefficient_ : release_coefficient_;
  envelope_ = envelopeCoefficient * envelope_ + (1.0F - envelopeCoefficient) * magnitude;

  float targetGain = 0.0F;
  if (envelope_ >= threshold_) {
    hold_remaining_ = hold_samples_;
    targetGain = 1.0F;
  } else if (hold_remaining_ > 0U) {
    --hold_remaining_;
    targetGain = 1.0F;
  }

  const float gainCoefficient = targetGain > gain_ ? attack_coefficient_ : release_coefficient_;
  gain_ = gainCoefficient * gain_ + (1.0F - gainCoefficient) * targetGain;
  return input * gain_;
}

void NoiseGate::reset() {
  hold_remaining_ = 0U;
  envelope_ = 0.0F;
  gain_ = 0.0F;
}

Compressor::Compressor(const float sampleRate, const SafetyChainSettings& settings)
    : threshold_db_(settings.compressor_threshold_db),
      ratio_(std::max(settings.compressor_ratio, 1.0F)),
      attack_coefficient_(smoothingCoefficient(settings.compressor_attack_ms, sampleRate)),
      release_coefficient_(smoothingCoefficient(settings.compressor_release_ms, sampleRate)) {}

float Compressor::process(const float input) {
  const float magnitude = std::max(std::abs(input), 1.0e-9F);
  const float inputDb = 20.0F * std::log10(magnitude);
  const float outputDb =
      inputDb > threshold_db_ ? threshold_db_ + (inputDb - threshold_db_) / ratio_ : inputDb;
  const float targetGain = dbToLinear(outputDb - inputDb);
  const float coefficient = targetGain < gain_ ? attack_coefficient_ : release_coefficient_;
  gain_ = coefficient * gain_ + (1.0F - coefficient) * targetGain;
  return input * gain_;
}

void Compressor::reset() { gain_ = 1.0F; }

SoftLimiter::SoftLimiter(const float ceilingDb) : ceiling_(dbToLinear(ceilingDb)) {}

float SoftLimiter::process(const float input) const {
  return ceiling_ * std::tanh(input / ceiling_);
}

float SoftLimiter::ceilingLinear() const { return ceiling_; }

SafetyChain::SafetyChain(const float sampleRate, SafetyChainSettings settings)
    : high_pass_(sampleRate, settings.high_pass_hz), gate_(sampleRate, settings),
      compressor_(sampleRate, settings), limiter_(settings.limiter_ceiling_db) {}

void SafetyChain::reset() {
  high_pass_.reset();
  gate_.reset();
  compressor_.reset();
}

void SafetyChain::process(const std::span<float> samples) {
  for (float& sample : samples) {
    const float finiteInput = std::isfinite(sample) ? sample : 0.0F;
    sample = limiter_.process(compressor_.process(gate_.process(high_pass_.process(finiteInput))));
  }
}

float SafetyChain::limiterCeilingLinear() const { return limiter_.ceilingLinear(); }

} // namespace vvc::dsp

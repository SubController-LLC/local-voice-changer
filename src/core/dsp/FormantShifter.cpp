#include "core/dsp/FormantShifter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace vvc::dsp {
namespace {

constexpr std::array<float, 4> kReferenceFormants{500.0F, 1'500.0F, 2'500.0F, 3'500.0F};
constexpr std::array<float, 4> kQualities{1.1F, 1.4F, 1.7F, 1.9F};

} // namespace

FormantShifter::FormantShifter(const float sampleRate) : sample_rate_(sampleRate) {}

void FormantShifter::PeakingFilter::configure(const float sampleRate, const float frequency,
                                              const float quality, const float gainDb) {
  const float clampedFrequency = std::clamp(frequency, 40.0F, sampleRate * 0.45F);
  const double amplitude = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
  const double omega = 2.0 * std::numbers::pi * static_cast<double>(clampedFrequency) /
                       static_cast<double>(sampleRate);
  const double alpha = std::sin(omega) / (2.0 * static_cast<double>(quality));
  const double cosine = std::cos(omega);
  const double a0 = 1.0 + alpha / amplitude;

  b0 = (1.0 + alpha * amplitude) / a0;
  b1 = (-2.0 * cosine) / a0;
  b2 = (1.0 - alpha * amplitude) / a0;
  a1 = (-2.0 * cosine) / a0;
  a2 = (1.0 - alpha / amplitude) / a0;
}

float FormantShifter::PeakingFilter::process(const float input) {
  const double output = b0 * static_cast<double>(input) + z1;
  z1 = b1 * static_cast<double>(input) - a1 * output + z2;
  z2 = b2 * static_cast<double>(input) - a2 * output;
  return static_cast<float>(output);
}

void FormantShifter::PeakingFilter::reset() {
  z1 = 0.0;
  z2 = 0.0;
}

void FormantShifter::reset() {
  for (auto& filter : cuts_) {
    filter.reset();
  }
  for (auto& filter : boosts_) {
    filter.reset();
  }
}

void FormantShifter::updateFilters(const float semitones) {
  const float shiftFactor = std::pow(2.0F, semitones / 12.0F);
  const float strength = std::clamp(std::abs(semitones) / 6.0F, 0.0F, 1.0F);
  const float cutDb = -4.5F * strength;
  const float boostDb = 4.5F * strength;
  for (std::size_t band = 0; band < kReferenceFormants.size(); ++band) {
    cuts_[band].configure(sample_rate_, kReferenceFormants[band], kQualities[band], cutDb);
    boosts_[band].configure(sample_rate_, kReferenceFormants[band] * shiftFactor, kQualities[band],
                            boostDb);
  }
  configured_semitones_ = semitones;
  configured_ = true;
}

void FormantShifter::process(const std::span<float> samples, const float semitones) {
  if (samples.empty() || std::abs(semitones) < 0.001F) {
    return;
  }

  if (!configured_ || std::abs(semitones - configured_semitones_) >= 0.001F) {
    updateFilters(semitones);
  }

  // Paired cuts and boosts move broad vocal resonances without changing the harmonic pitch.
  for (float& sample : samples) {
    for (std::size_t band = 0; band < kReferenceFormants.size(); ++band) {
      sample = boosts_[band].process(cuts_[band].process(sample));
    }
  }
}

} // namespace vvc::dsp

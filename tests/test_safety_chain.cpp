#include "core/audio/SignalMetrics.h"
#include "core/dsp/SafetyChain.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

[[nodiscard]] double tailRms(const std::vector<float>& samples) {
  const auto start = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2U);
  return vvc::audio::measureSignal(std::span<const float>(start, samples.end())).rms;
}

[[nodiscard]] std::vector<float> sine(const double frequency, const float amplitude = 0.5F) {
  std::vector<float> samples(48'000U);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const double time = static_cast<double>(index) / 48'000.0;
    samples[index] = static_cast<float>(static_cast<double>(amplitude) *
                                        std::sin(2.0 * std::numbers::pi * frequency * time));
  }
  return samples;
}

} // namespace

TEST_CASE("80 Hz high-pass attenuates low frequency more than speech-band frequency") {
  auto low = sine(40.0);
  auto speech = sine(1'000.0);
  vvc::dsp::HighPassFilter lowFilter(48'000.0F, 80.0F);
  vvc::dsp::HighPassFilter speechFilter(48'000.0F, 80.0F);
  for (float& sample : low) {
    sample = lowFilter.process(sample);
  }
  for (float& sample : speech) {
    sample = speechFilter.process(sample);
  }
  CHECK(tailRms(low) < tailRms(speech) * 0.6);
}

TEST_CASE("noise gate closes for a signal below threshold") {
  const vvc::dsp::SafetyChainSettings settings;
  vvc::dsp::NoiseGate gate(48'000.0F, settings);
  float peak = 0.0F;
  for (std::size_t index = 0; index < 48'000U; ++index) {
    peak = std::max(peak, std::abs(gate.process(0.0001F)));
  }
  CHECK(peak < 0.00001F);
}

TEST_CASE("compressor reduces sustained signal above threshold") {
  const vvc::dsp::SafetyChainSettings settings;
  vvc::dsp::Compressor compressor(48'000.0F, settings);
  float output = 0.0F;
  for (std::size_t index = 0; index < 48'000U; ++index) {
    output = compressor.process(1.0F);
  }
  CHECK(output < 0.4F);
  CHECK(output > 0.1F);
}

TEST_CASE("soft limiter never exceeds its ceiling") {
  const vvc::dsp::SoftLimiter limiter(-1.0F);
  for (const float input : {-100.0F, -2.0F, -1.0F, 0.0F, 1.0F, 2.0F, 100.0F}) {
    CHECK(std::abs(limiter.process(input)) <= limiter.ceilingLinear());
  }
  CHECK(limiter.ceilingLinear() == Catch::Approx(0.8912509F).margin(0.000001F));
}

TEST_CASE("safety chain sanitizes non-finite and excessive inputs") {
  std::vector<float> samples{std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(), -10.0F, 10.0F};
  vvc::dsp::SafetyChain chain;
  chain.process(samples);
  const auto metrics = vvc::audio::measureSignal(samples, chain.limiterCeilingLinear());
  CHECK(metrics.non_finite_samples == 0U);
  CHECK(metrics.clipped_samples == 0U);
  CHECK(metrics.peak <= static_cast<double>(chain.limiterCeilingLinear()));
}

#include "core/audio/SignalMetrics.h"
#include "core/dsp/VoiceProcessor.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

[[nodiscard]] std::vector<float> makeInput() {
  std::vector<float> samples(48'000U);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const double time = static_cast<double>(index) / 48'000.0;
    samples[index] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * 190.0 * time) +
                                        0.12 * std::sin(2.0 * std::numbers::pi * 570.0 * time) +
                                        0.06 * std::sin(2.0 * std::numbers::pi * 1'520.0 * time));
  }
  return samples;
}

[[nodiscard]] double difference(const std::vector<float>& left, const std::vector<float>& right) {
  double total = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    total += std::abs(static_cast<double>(left[index] - right[index]));
  }
  return total;
}

} // namespace

TEST_CASE("voice settings enforce validation ranges") {
  CHECK(vvc::dsp::VoiceProcessor::isValid({3.0F, 2.0F}));
  CHECK_FALSE(vvc::dsp::VoiceProcessor::isValid({-0.1F, 2.0F}));
  CHECK_FALSE(vvc::dsp::VoiceProcessor::isValid({3.0F, 6.1F}));
}

TEST_CASE("comparison outputs are aligned, finite, and limited") {
  const auto input = makeInput();
  const vvc::dsp::VoiceProcessor processor;
  const auto output = processor.processComparison(input, {3.0F, 2.0F});

  REQUIRE(output.dry.size() == input.size());
  REQUIRE(output.pitch_only.size() == input.size());
  REQUIRE(output.natural.size() == input.size());
  for (const auto* samples : {&output.dry, &output.pitch_only, &output.natural}) {
    const auto metrics = vvc::audio::measureSignal(*samples);
    CHECK(metrics.non_finite_samples == 0U);
    CHECK(metrics.clipped_samples == 0U);
    CHECK(metrics.peak <= 0.891251);
  }
  CHECK(output.natural_diagnostics.signalsmith_input_latency_samples > 0);
  CHECK(output.natural_diagnostics.signalsmith_output_latency_samples > 0);
}

TEST_CASE("pitch and formant controls affect output independently") {
  const auto input = makeInput();
  const vvc::dsp::VoiceProcessor processor;
  const auto base = processor.processComparison(input, {2.0F, 0.0F});
  const auto formantChanged = processor.processComparison(input, {2.0F, 2.0F});
  const auto pitchChanged = processor.processComparison(input, {4.0F, 0.0F});

  CHECK(difference(base.natural, formantChanged.natural) > 1.0);
  CHECK(difference(base.pitch_only, formantChanged.pitch_only) < 0.000001);
  CHECK(difference(base.pitch_only, pitchChanged.pitch_only) > 1.0);
}

TEST_CASE("silence and excessive input remain numerically safe") {
  const vvc::dsp::VoiceProcessor processor;
  for (const float value : {0.0F, 10.0F}) {
    const std::vector<float> input(12'000U, value);
    const auto output = processor.processComparison(input, {3.0F, 2.0F});
    const auto metrics = vvc::audio::measureSignal(output.natural);
    CHECK(metrics.non_finite_samples == 0U);
    CHECK(metrics.clipped_samples == 0U);
    CHECK(metrics.peak <= 0.891251);
  }
}

TEST_CASE("non-finite input is sanitized before spectral processing") {
  auto input = makeInput();
  input[100] = std::numeric_limits<float>::quiet_NaN();
  input[200] = std::numeric_limits<float>::infinity();
  const vvc::dsp::VoiceProcessor processor;
  const auto output = processor.processComparison(input, {3.0F, 2.0F});
  CHECK(vvc::audio::measureSignal(output.pitch_only).non_finite_samples == 0U);
  CHECK(vvc::audio::measureSignal(output.natural).non_finite_samples == 0U);
}

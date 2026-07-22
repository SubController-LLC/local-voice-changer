#include "realtime/RealtimeVoiceProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("realtime processor keeps output finite and within the limiter") {
  vvc::realtime::RealtimeVoiceProcessor processor;
  std::array<float, 480> input{};
  std::array<float, 480> output{};
  for (int block = 0; block < 20; ++block) {
    for (std::size_t index = 0U; index < input.size(); ++index) {
      const float phase =
          static_cast<float>(block * static_cast<int>(input.size()) + static_cast<int>(index));
      input[index] = 1.5F * std::sin(2.0F * std::numbers::pi_v<float> * 220.0F * phase / 48'000.0F);
    }
    processor.processBlock(input, output);
    REQUIRE(std::ranges::all_of(output, [](const float sample) { return std::isfinite(sample); }));
    REQUIRE(*std::ranges::max_element(output) <= 0.892F);
    REQUIRE(*std::ranges::min_element(output) >= -0.892F);
  }
}

TEST_CASE("realtime processor validates block sizes") {
  vvc::realtime::RealtimeVoiceProcessor processor;
  std::array<float, 8> input{};
  std::array<float, 7> output{};
  REQUIRE_THROWS(processor.processBlock(input, output));
}

TEST_CASE("bypass uses the full spectral latency for sample-aligned switching") {
  constexpr std::size_t kFrames = 480U;
  vvc::realtime::RealtimeVoiceProcessor processor;
  processor.setBypassed(true);
  processor.reset();

  const int latency = processor.algorithmLatencySamples();
  REQUIRE(latency == 3'000);

  std::array<float, kFrames> input{};
  std::array<float, kFrames> output{};
  input[0] = 0.5F;
  bool foundImpulse = false;
  for (int block = 0; block <= latency / static_cast<int>(kFrames); ++block) {
    processor.processBlock(input, output);
    input.fill(0.0F);
    for (std::size_t index = 0U; index < output.size(); ++index) {
      const int absoluteIndex = block * static_cast<int>(kFrames) + static_cast<int>(index);
      if (absoluteIndex < latency) {
        REQUIRE(std::abs(output[index]) < 1.0e-6F);
      } else if (absoluteIndex == latency) {
        REQUIRE(std::abs(output[index] - 0.5F) < 1.0e-6F);
        foundImpulse = true;
      }
    }
  }
  REQUIRE(foundImpulse);
}

TEST_CASE("reported latency matches the wet signal path") {
  constexpr std::size_t kFrames = 480U;
  constexpr std::size_t kBlocks = 48U;
  constexpr int kExpectedLatency = 3'000;
  vvc::realtime::RealtimeVoiceProcessor processor;
  processor.setVoiceSettings({0.0F, 0.0F});
  processor.reset();

  std::vector<float> input(kFrames * kBlocks, 0.0F);
  std::vector<float> output(input.size(), 0.0F);
  std::uint32_t state = 0x51A7E123U;
  for (float& sample : input) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    const float unit = static_cast<float>(state & 0x00FF'FFFFU) / 8'388'608.0F - 1.0F;
    sample = unit * 0.18F;
  }
  for (std::size_t block = 0U; block < kBlocks; ++block) {
    const auto offset = block * kFrames;
    processor.processBlock(std::span<const float>(input).subspan(offset, kFrames),
                           std::span<float>(output).subspan(offset, kFrames));
  }

  int bestLag = 0;
  double bestCorrelation = -1.0;
  for (int lag = kExpectedLatency - 5; lag <= kExpectedLatency + 5; ++lag) {
    double cross = 0.0;
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    for (std::size_t index = 0U; index + static_cast<std::size_t>(lag) < input.size(); ++index) {
      const double dry = input[index];
      const double wet = output[index + static_cast<std::size_t>(lag)];
      cross += dry * wet;
      inputEnergy += dry * dry;
      outputEnergy += wet * wet;
    }
    const double correlation = cross / std::sqrt(inputEnergy * outputEnergy);
    if (correlation > bestCorrelation) {
      bestCorrelation = correlation;
      bestLag = lag;
    }
  }

  REQUIRE(processor.algorithmLatencySamples() == kExpectedLatency);
  REQUIRE(std::abs(bestLag - kExpectedLatency) <= 1);
  REQUIRE(bestCorrelation > 0.9);
}

TEST_CASE("bypass mute and transitions remain finite and limited") {
  vvc::realtime::RealtimeVoiceProcessor processor;
  processor.setBypassed(true);
  processor.reset();
  std::array<float, 480U> input{};
  std::array<float, 480U> output{};
  input.fill(4.0F);

  for (int block = 0; block < 20; ++block) {
    if (block == 8) {
      processor.setMuted(true);
    }
    if (block == 12) {
      processor.setBypassed(false);
    }
    processor.processBlock(input, output);
    REQUIRE(std::ranges::all_of(output, [](const float sample) {
      return std::isfinite(sample) && sample <= 0.8912509F && sample >= -0.8912509F;
    }));
  }
}

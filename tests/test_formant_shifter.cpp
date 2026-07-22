#include "core/dsp/FormantShifter.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

[[nodiscard]] std::vector<float> makeVoiceLikeSignal() {
  constexpr std::size_t kSamples = 48'000U;
  std::vector<float> signal(kSamples);
  for (std::size_t index = 0; index < signal.size(); ++index) {
    const double time = static_cast<double>(index) / 48'000.0;
    signal[index] = static_cast<float>(0.22 * std::sin(2.0 * std::numbers::pi * 180.0 * time) +
                                       0.14 * std::sin(2.0 * std::numbers::pi * 540.0 * time) +
                                       0.08 * std::sin(2.0 * std::numbers::pi * 1'440.0 * time) +
                                       0.05 * std::sin(2.0 * std::numbers::pi * 2'520.0 * time));
  }
  return signal;
}

} // namespace

TEST_CASE("zero formant shift is bit-identical") {
  auto signal = makeVoiceLikeSignal();
  const auto original = signal;
  vvc::dsp::FormantShifter shifter;
  shifter.process(signal, 0.0F);
  CHECK(signal == original);
}

TEST_CASE("formant control changes spectral envelope without non-finite samples") {
  auto signal = makeVoiceLikeSignal();
  const auto original = signal;
  vvc::dsp::FormantShifter shifter;
  shifter.process(signal, 2.0F);

  REQUIRE(signal.size() == original.size());
  CHECK(std::all_of(signal.begin(), signal.end(),
                    [](const float sample) { return std::isfinite(sample); }));

  double absoluteDifference = 0.0;
  for (std::size_t index = 0; index < signal.size(); ++index) {
    absoluteDifference += std::abs(static_cast<double>(signal[index] - original[index]));
  }
  CHECK(absoluteDifference > 1.0);
}

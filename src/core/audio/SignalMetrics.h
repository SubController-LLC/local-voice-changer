#pragma once

#include <cstddef>
#include <span>

namespace vvc::audio {

struct SignalMetrics {
  double peak = 0.0;
  double rms = 0.0;
  std::size_t clipped_samples = 0;
  std::size_t non_finite_samples = 0;
};

[[nodiscard]] SignalMetrics measureSignal(std::span<const float> samples,
                                          float clipThreshold = 1.0F);

} // namespace vvc::audio

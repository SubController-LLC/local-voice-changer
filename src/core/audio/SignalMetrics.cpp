#include "core/audio/SignalMetrics.h"

#include <algorithm>
#include <cmath>

namespace vvc::audio {

SignalMetrics measureSignal(const std::span<const float> samples, const float clipThreshold) {
  SignalMetrics metrics;
  double sumSquares = 0.0;
  std::size_t finiteSamples = 0;

  for (const float sample : samples) {
    if (!std::isfinite(sample)) {
      ++metrics.non_finite_samples;
      continue;
    }

    const double magnitude = std::abs(static_cast<double>(sample));
    metrics.peak = std::max(metrics.peak, magnitude);
    sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    ++finiteSamples;
    if (magnitude > static_cast<double>(clipThreshold)) {
      ++metrics.clipped_samples;
    }
  }

  if (finiteSamples != 0U) {
    metrics.rms = std::sqrt(sumSquares / static_cast<double>(finiteSamples));
  }
  return metrics;
}

} // namespace vvc::audio

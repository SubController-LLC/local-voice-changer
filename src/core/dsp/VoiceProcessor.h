#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vvc::dsp {

struct VoiceSettings {
  float pitch_semitones = 3.0F;
  float formant_semitones = 2.0F;
};

struct ProcessingDiagnostics {
  int signalsmith_input_latency_samples = 0;
  int signalsmith_output_latency_samples = 0;
  double processing_milliseconds = 0.0;
};

struct ComparisonAudio {
  std::vector<float> dry;
  std::vector<float> pitch_only;
  std::vector<float> natural;
  ProcessingDiagnostics pitch_only_diagnostics;
  ProcessingDiagnostics natural_diagnostics;
};

class VoiceProcessor {
public:
  explicit VoiceProcessor(std::uint32_t sampleRate = 48'000U);

  [[nodiscard]] ComparisonAudio processComparison(std::span<const float> input,
                                                  const VoiceSettings& settings) const;

  [[nodiscard]] static bool isValid(const VoiceSettings& settings);

private:
  std::uint32_t sample_rate_;
};

} // namespace vvc::dsp

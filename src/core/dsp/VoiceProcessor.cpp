#include "core/dsp/VoiceProcessor.h"

#include "core/audio/SignalMetrics.h"
#include "core/dsp/SafetyChain.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <signalsmith-stretch.h>

namespace vvc::dsp {
namespace {

constexpr float kOutputCeiling = 0.8912509F; // -1 dBFS

struct StretchedAudio {
  std::vector<float> samples;
  ProcessingDiagnostics diagnostics;
};

[[nodiscard]] StretchedAudio pitchShift(const std::span<const float> input,
                                        const float pitchSemitones, const float formantSemitones,
                                        const std::uint32_t sampleRate) {
  const auto started = std::chrono::steady_clock::now();
  signalsmith::stretch::SignalsmithStretch<float> stretch(0x5EEDL);
  stretch.presetCheaper(1, static_cast<float>(sampleRate));
  stretch.setTransposeSemitones(pitchSemitones, 8'000.0F / static_cast<float>(sampleRate));
  stretch.setFormantSemitones(formantSemitones);

  const int inputLatency = stretch.inputLatency();
  const int outputLatency = stretch.outputLatency();
  const std::size_t inputLatencySize = static_cast<std::size_t>(inputLatency);
  const std::size_t outputLatencySize = static_cast<std::size_t>(outputLatency);

  std::vector<float> seekInput(inputLatencySize, 0.0F);
  const std::size_t copied = std::min(input.size(), inputLatencySize);
  std::copy_n(input.begin(), copied, seekInput.begin());
  std::array<const float*, 1> seekPointers{seekInput.data()};
  stretch.seek(seekPointers.data(), inputLatency, 1.0);

  const std::size_t remainingCount =
      input.size() > inputLatencySize ? input.size() - inputLatencySize : 0U;
  const std::size_t processCount = std::max(input.size(), inputLatencySize);
  std::vector<float> processInput(processCount, 0.0F);
  if (remainingCount != 0U) {
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(inputLatencySize), remainingCount,
                processInput.begin());
  }

  std::vector<float> processOutput(processCount, 0.0F);
  std::array<const float*, 1> inputPointers{processInput.data()};
  std::array<float*, 1> outputPointers{processOutput.data()};
  stretch.process(inputPointers.data(), static_cast<int>(processCount), outputPointers.data(),
                  static_cast<int>(processCount));

  std::vector<float> tail(outputLatencySize, 0.0F);
  std::array<float*, 1> tailPointers{tail.data()};
  stretch.flush(tailPointers.data(), outputLatency);

  processOutput.insert(processOutput.end(), tail.begin(), tail.end());
  std::vector<float> aligned;
  aligned.reserve(input.size());
  const std::size_t cropBegin = std::min(outputLatencySize, processOutput.size());
  const std::size_t available = processOutput.size() - cropBegin;
  const std::size_t toCopy = std::min(input.size(), available);
  aligned.insert(aligned.end(), processOutput.begin() + static_cast<std::ptrdiff_t>(cropBegin),
                 processOutput.begin() + static_cast<std::ptrdiff_t>(cropBegin + toCopy));
  aligned.resize(input.size(), 0.0F);

  const auto ended = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double, std::milli>(ended - started).count();
  return {std::move(aligned), {inputLatency, outputLatency, elapsed}};
}

void sanitizeAndProtect(std::vector<float>& samples) {
  for (float& sample : samples) {
    if (!std::isfinite(sample)) {
      sample = 0.0F;
    }
  }
  const auto metrics = audio::measureSignal(samples);
  if (metrics.peak > static_cast<double>(kOutputCeiling)) {
    const float gain = kOutputCeiling / static_cast<float>(metrics.peak);
    for (float& sample : samples) {
      sample *= gain;
    }
  }
}

void matchRmsAndProtect(std::vector<float>& samples, const double targetRms) {
  const auto current = audio::measureSignal(samples);
  if (current.rms > 1.0e-9 && targetRms > 1.0e-9) {
    const double desiredGain = targetRms / current.rms;
    const double peakLimitedGain =
        current.peak > 1.0e-9 ? static_cast<double>(kOutputCeiling) / current.peak : desiredGain;
    const float gain = static_cast<float>(std::min({desiredGain, peakLimitedGain, 2.0}));
    for (float& sample : samples) {
      sample *= gain;
    }
  }
  sanitizeAndProtect(samples);
}

} // namespace

VoiceProcessor::VoiceProcessor(const std::uint32_t sampleRate) : sample_rate_(sampleRate) {
  if (sampleRate != 48'000U) {
    throw std::invalid_argument("VoiceProcessorは48 kHz専用です。");
  }
}

ComparisonAudio VoiceProcessor::processComparison(const std::span<const float> input,
                                                  const VoiceSettings& settings) const {
  if (input.empty()) {
    throw std::invalid_argument("入力音声が空です。");
  }
  if (!isValid(settings)) {
    throw std::invalid_argument("Pitchは0〜+7、Formantは-2〜+6 semitonesで指定してください。");
  }
  if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("入力音声が長すぎます。");
  }

  std::vector<float> sanitizedInput(input.begin(), input.end());
  for (float& sample : sanitizedInput) {
    if (!std::isfinite(sample)) {
      sample = 0.0F;
    }
  }

  ComparisonAudio result;
  result.dry = sanitizedInput;
  sanitizeAndProtect(result.dry);
  const double referenceRms = audio::measureSignal(result.dry).rms;

  auto pitchOnly = pitchShift(sanitizedInput, settings.pitch_semitones, 0.0F, sample_rate_);
  result.pitch_only = std::move(pitchOnly.samples);
  result.pitch_only_diagnostics = pitchOnly.diagnostics;
  matchRmsAndProtect(result.pitch_only, referenceRms);

  auto natural = pitchShift(sanitizedInput, settings.pitch_semitones, settings.formant_semitones,
                            sample_rate_);
  result.natural = std::move(natural.samples);
  result.natural_diagnostics = natural.diagnostics;
  SafetyChain safety(static_cast<float>(sample_rate_));
  safety.process(result.natural);
  matchRmsAndProtect(result.natural, referenceRms);

  return result;
}

bool VoiceProcessor::isValid(const VoiceSettings& settings) {
  return std::isfinite(settings.pitch_semitones) && std::isfinite(settings.formant_semitones) &&
         settings.pitch_semitones >= 0.0F && settings.pitch_semitones <= 7.0F &&
         settings.formant_semitones >= -2.0F && settings.formant_semitones <= 6.0F;
}

} // namespace vvc::dsp

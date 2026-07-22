#include "realtime/RealtimeVoiceProcessor.h"

#include "core/dsp/SafetyChain.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <signalsmith-stretch.h>

namespace vvc::realtime {
namespace {

constexpr float kPitchSmoothingPerBlock = 0.18F;
constexpr float kFormantSmoothingPerBlock = 0.12F;
constexpr float kMixRampSamples = 960.0F; // 20 ms at 48 kHz
constexpr float kAnalysisWindowSeconds = 0.05F;
constexpr float kAnalysisIntervalSeconds = 0.0125F;
constexpr float kOutputCeiling = 0.8912509F; // -1 dBFS

[[nodiscard]] float moveTowards(const float current, const float target, const float step) {
  if (current < target) {
    return std::min(current + step, target);
  }
  return std::max(current - step, target);
}

} // namespace

struct RealtimeVoiceProcessor::Impl {
  explicit Impl(const float rate)
      : sample_rate(rate), stretch(0x5EEDL), safety(rate),
        input_scratch(kMaximumBlockFrames, 0.0F) {
    if (std::abs(sample_rate - 48'000.0F) > 0.5F) {
      throw std::invalid_argument("RealtimeVoiceProcessorは48 kHz専用です。");
    }
    // A 50 ms speech window with 4x overlap keeps the spectral algorithm's total input+output
    // latency at 62.5 ms. Split computation spreads FFT work across intervals.
    stretch.configure(1, static_cast<int>(sample_rate * kAnalysisWindowSeconds),
                      static_cast<int>(sample_rate * kAnalysisIntervalSeconds), true);
    stretch.setTransposeSemitones(current_pitch, 8'000.0F / sample_rate);
    stretch.setFormantSemitones(current_formant);
    const int totalLatency = stretch.inputLatency() + stretch.outputLatency();
    dry_delay.assign(std::max<std::size_t>(static_cast<std::size_t>(totalLatency), 1U), 0.0F);
  }

  void reset() {
    stretch.reset();
    safety.reset();
    std::fill(input_scratch.begin(), input_scratch.end(), 0.0F);
    std::fill(dry_delay.begin(), dry_delay.end(), 0.0F);
    dry_delay_index = 0U;
    processed_mix = bypassed.load(std::memory_order_relaxed) ? 0.0F : 1.0F;
    mute_gain = muted.load(std::memory_order_relaxed) ? 0.0F : 1.0F;
    current_pitch = pitch.load(std::memory_order_relaxed);
    current_formant = formant_shift.load(std::memory_order_relaxed);
    stretch.setTransposeSemitones(current_pitch, 8'000.0F / sample_rate);
    stretch.setFormantSemitones(current_formant);
  }

  float sample_rate;
  signalsmith::stretch::SignalsmithStretch<float> stretch;
  dsp::SafetyChain safety;
  std::vector<float> input_scratch;
  std::vector<float> dry_delay;
  std::size_t dry_delay_index = 0U;
  std::atomic<float> pitch{3.0F};
  std::atomic<float> formant_shift{2.0F};
  std::atomic<bool> bypassed{false};
  std::atomic<bool> muted{false};
  float current_pitch = 3.0F;
  float current_formant = 2.0F;
  float processed_mix = 1.0F;
  float mute_gain = 1.0F;
};

RealtimeVoiceProcessor::RealtimeVoiceProcessor(const float sampleRate)
    : impl_(std::make_unique<Impl>(sampleRate)) {}

RealtimeVoiceProcessor::~RealtimeVoiceProcessor() = default;
RealtimeVoiceProcessor::RealtimeVoiceProcessor(RealtimeVoiceProcessor&&) noexcept = default;
RealtimeVoiceProcessor&
RealtimeVoiceProcessor::operator=(RealtimeVoiceProcessor&&) noexcept = default;

void RealtimeVoiceProcessor::setVoiceSettings(const dsp::VoiceSettings settings) {
  if (!dsp::VoiceProcessor::isValid(settings)) {
    return;
  }
  impl_->pitch.store(settings.pitch_semitones, std::memory_order_relaxed);
  impl_->formant_shift.store(settings.formant_semitones, std::memory_order_relaxed);
}

void RealtimeVoiceProcessor::setBypassed(const bool bypassed) {
  impl_->bypassed.store(bypassed, std::memory_order_relaxed);
}

void RealtimeVoiceProcessor::setMuted(const bool muted) {
  impl_->muted.store(muted, std::memory_order_relaxed);
}

void RealtimeVoiceProcessor::reset() { impl_->reset(); }

void RealtimeVoiceProcessor::processBlock(const std::span<const float> input,
                                          const std::span<float> output) {
  if (input.size() != output.size()) {
    throw std::invalid_argument("リアルタイム入出力のフレーム数が異なります。");
  }
  if (input.size() > kMaximumBlockFrames) {
    throw std::invalid_argument("リアルタイム処理ブロックが大きすぎます。");
  }
  if (input.empty()) {
    return;
  }

  const float targetPitch = impl_->pitch.load(std::memory_order_relaxed);
  impl_->current_pitch = moveTowards(impl_->current_pitch, targetPitch, kPitchSmoothingPerBlock);
  const float targetFormant = impl_->formant_shift.load(std::memory_order_relaxed);
  impl_->current_formant =
      moveTowards(impl_->current_formant, targetFormant, kFormantSmoothingPerBlock);
  impl_->stretch.setTransposeSemitones(impl_->current_pitch, 8'000.0F / impl_->sample_rate);
  impl_->stretch.setFormantSemitones(impl_->current_formant);

  auto scratch = std::span<float>(impl_->input_scratch).first(input.size());
  std::transform(input.begin(), input.end(), scratch.begin(),
                 [](const float sample) { return std::isfinite(sample) ? sample : 0.0F; });

  std::array<const float*, 1> inputPointers{scratch.data()};
  std::array<float*, 1> outputPointers{output.data()};
  impl_->stretch.process(inputPointers.data(), static_cast<int>(input.size()),
                         outputPointers.data(), static_cast<int>(output.size()));
  impl_->safety.process(output);

  const float bypassTarget = impl_->bypassed.load(std::memory_order_relaxed) ? 0.0F : 1.0F;
  const float muteTarget = impl_->muted.load(std::memory_order_relaxed) ? 0.0F : 1.0F;
  const float mixStep = 1.0F / kMixRampSamples;
  for (std::size_t index = 0U; index < output.size(); ++index) {
    const float delayedDry = impl_->dry_delay[impl_->dry_delay_index];
    impl_->dry_delay[impl_->dry_delay_index] = scratch[index];
    impl_->dry_delay_index = (impl_->dry_delay_index + 1U) % impl_->dry_delay.size();

    impl_->processed_mix = moveTowards(impl_->processed_mix, bypassTarget, mixStep);
    impl_->mute_gain = moveTowards(impl_->mute_gain, muteTarget, mixStep);
    const float mixed =
        (output[index] * impl_->processed_mix + delayedDry * (1.0F - impl_->processed_mix)) *
        impl_->mute_gain;
    output[index] =
        std::isfinite(mixed) ? std::clamp(mixed, -kOutputCeiling, kOutputCeiling) : 0.0F;
  }
}

int RealtimeVoiceProcessor::algorithmLatencySamples() const {
  return impl_->stretch.inputLatency() + impl_->stretch.outputLatency();
}

} // namespace vvc::realtime

#pragma once

#include "core/dsp/VoiceProcessor.h"

#include <cstddef>
#include <memory>
#include <span>

namespace vvc::realtime {

class RealtimeVoiceProcessor {
public:
  static constexpr std::size_t kMaximumBlockFrames = 2'048U;

  explicit RealtimeVoiceProcessor(float sampleRate = 48'000.0F);
  ~RealtimeVoiceProcessor();

  RealtimeVoiceProcessor(const RealtimeVoiceProcessor&) = delete;
  RealtimeVoiceProcessor& operator=(const RealtimeVoiceProcessor&) = delete;
  RealtimeVoiceProcessor(RealtimeVoiceProcessor&&) noexcept;
  RealtimeVoiceProcessor& operator=(RealtimeVoiceProcessor&&) noexcept;

  void setVoiceSettings(dsp::VoiceSettings settings);
  void setBypassed(bool bypassed);
  void setMuted(bool muted);
  void reset();
  void processBlock(std::span<const float> input, std::span<float> output);

  [[nodiscard]] int algorithmLatencySamples() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vvc::realtime

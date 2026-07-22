#pragma once

#include "core/dsp/VoiceProcessor.h"
#include "realtime/AudioLifecycle.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vvc::windows {

struct AudioDeviceInfo {
  std::wstring name;
  bool is_default = false;
};

struct AudioStats {
  float input_peak = 0.0F;
  float output_peak = 0.0F;
  std::uint64_t input_overruns = 0U;
  std::uint64_t output_underruns = 0U;
  std::uint64_t deadline_misses = 0U;
  std::uint64_t resynchronizations = 0U;
  std::uint64_t fatal_dsp_failures = 0U;
  std::size_t input_queue_frames = 0U;
  std::size_t output_queue_frames = 0U;
  std::size_t max_input_queue_frames = 0U;
  std::size_t max_output_queue_frames = 0U;
  std::size_t avoidable_backlog_frames = 0U;
  std::size_t max_avoidable_backlog_frames = 0U;
  int algorithm_latency_ms = 0;
};

struct AudioStartResult {
  bool succeeded = false;
  std::wstring message;
};

struct AudioServiceResult {
  realtime::AudioLifecycleState state = realtime::AudioLifecycleState::stopped;
  realtime::AudioFailureReason failure = realtime::AudioFailureReason::none;
  bool cleanup_performed = false;
};

class AudioEngine {
public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  [[nodiscard]] bool refreshDevices(std::wstring& errorMessage);
  [[nodiscard]] const std::vector<AudioDeviceInfo>& captureDevices() const;
  [[nodiscard]] const std::vector<AudioDeviceInfo>& playbackDevices() const;
  [[nodiscard]] AudioStartResult start(std::size_t captureIndex, std::size_t playbackIndex);
  void stop() noexcept;
  [[nodiscard]] AudioServiceResult service();

  void setVoiceSettings(dsp::VoiceSettings settings);
  void setBypassed(bool bypassed);
  void setMuted(bool muted);

  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] AudioStats takeStats();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vvc::windows

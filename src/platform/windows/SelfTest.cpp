#include "platform/windows/SelfTest.h"

#include "platform/windows/AppSettings.h"
#include "platform/windows/AudioEngine.h"
#include "realtime/AudioLifecycle.h"
#include "realtime/AudioRingBuffer.h"
#include "realtime/DeviceRouting.h"
#include "realtime/RealtimeAudioPipeline.h"
#include "realtime/RealtimeVoiceProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace vvc::windows {
namespace {

[[nodiscard]] bool testRingBuffer() {
  realtime::AudioRingBuffer ring(8U);
  const std::array first{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  if (ring.push(first) != first.size()) {
    return false;
  }
  std::array<float, 4U> discarded{};
  if (ring.pop(discarded) != discarded.size()) {
    return false;
  }
  const std::array second{7.0F, 8.0F, 9.0F, 10.0F, 11.0F, 12.0F};
  if (ring.push(second) != second.size()) {
    return false;
  }
  std::array<float, 8U> output{};
  const std::array expected{5.0F, 6.0F, 7.0F, 8.0F, 9.0F, 10.0F, 11.0F, 12.0F};
  return ring.pop(output) == output.size() && output == expected;
}

[[nodiscard]] bool testRealtimeProcessor() {
  constexpr std::size_t kFrames = 480U;
  constexpr int kBlocks = 120;
  realtime::RealtimeVoiceProcessor processor;
  std::array<float, kFrames> input{};
  std::array<float, kFrames> output{};
  double outputEnergy = 0.0;

  for (int block = 0; block < kBlocks; ++block) {
    for (std::size_t index = 0U; index < input.size(); ++index) {
      const auto sampleIndex =
          static_cast<float>(block * static_cast<int>(kFrames) + static_cast<int>(index));
      input[index] =
          0.18F *
          (std::sin(2.0F * std::numbers::pi_v<float> * 180.0F * sampleIndex / 48'000.0F) +
           0.5F * std::sin(2.0F * std::numbers::pi_v<float> * 347.0F * sampleIndex / 48'000.0F));
    }
    if (block == 0) {
      input[0] = std::numeric_limits<float>::quiet_NaN();
      input[1] = std::numeric_limits<float>::infinity();
    }
    processor.processBlock(input, output);
    for (const float sample : output) {
      if (!std::isfinite(sample) || std::abs(sample) > 0.892F) {
        return false;
      }
      if (block > 20) {
        outputEnergy += static_cast<double>(sample) * static_cast<double>(sample);
      }
    }
  }

  const int latency = processor.algorithmLatencySamples();
  return outputEnergy > 0.01 && latency == 3'000 && latency <= 3'120;
}

[[nodiscard]] bool testBoundedPipeline() {
  realtime::RealtimeAudioPipeline pipeline;
  pipeline.prepare();
  std::array<float, realtime::RealtimeAudioPipeline::kBlockFrames> block{};
  for (int tick = 0; tick < 8; ++tick) {
    (void)pipeline.pushInput(block);
  }
  if (pipeline.stats().phase != realtime::PipelinePhase::reset_pending ||
      pipeline.stats().max_avoidable_backlog >
          realtime::RealtimeAudioPipeline::kMaximumAvoidableBacklogFrames) {
    return false;
  }
  std::array<float, realtime::RealtimeAudioPipeline::kBlockFrames> output{};
  (void)pipeline.popOutput(output);
  const bool reset = pipeline.processOne(
      [](const std::span<const float> input, const std::span<float> processed) {
        std::ranges::copy(input, processed.begin());
      },
      [] {});
  return reset && pipeline.stats().phase == realtime::PipelinePhase::running;
}

[[nodiscard]] bool testLifecycleAndRouting() {
  realtime::AudioLifecycle lifecycle;
  if (!lifecycle.beginStart()) {
    return false;
  }
  const auto generation = lifecycle.snapshot().generation;
  if (!lifecycle.markRunning(generation) ||
      !lifecycle.fail(realtime::AudioFailureReason::device_rerouted, generation)) {
    return false;
  }
  if (!lifecycle.consumeCleanupRequest() || lifecycle.consumeCleanupRequest() ||
      !lifecycle.acknowledgeRecovery()) {
    return false;
  }
  if (!lifecycle.beginStart()) {
    return false;
  }
  const auto restartedGeneration = lifecycle.snapshot().generation;
  if (restartedGeneration == generation || !lifecycle.markRunning(restartedGeneration) ||
      lifecycle.fail(realtime::AudioFailureReason::device_stopped, generation)) {
    return false;
  }
  const auto restarted = lifecycle.snapshot();
  if (restarted.state != realtime::AudioLifecycleState::running ||
      restarted.failure != realtime::AudioFailureReason::none || restarted.cleanup_requested) {
    return false;
  }
  lifecycle.stop();
  const std::array capture{realtime::RoutingDevice{L"CABLE Output", true},
                           realtime::RoutingDevice{L"Physical Microphone", false}};
  const std::array playback{realtime::RoutingDevice{L"Speakers", true},
                            realtime::RoutingDevice{L"CABLE Input", false}};
  return realtime::chooseSafeCapture(capture) == 1U &&
         realtime::chooseCablePlayback(playback) == 1U;
}

[[nodiscard]] bool testWindowsAudioContext() {
  AudioEngine audio;
  std::wstring error;
  if (!audio.refreshDevices(error)) {
    // Hosted Windows runners can have the Audio Service disabled and no endpoints. The GUI must
    // still start and present a useful message; physical streaming remains a hardware QA gate.
    return !error.empty();
  }
  const AudioStats stats = audio.takeStats();
  return std::isfinite(stats.input_peak) && std::isfinite(stats.output_peak) &&
         stats.algorithm_latency_ms > 0;
}

} // namespace

int runExecutableSelfTest() noexcept {
  try {
    if (!testRingBuffer()) {
      return 10;
    }
    if (!testRealtimeProcessor()) {
      return 20;
    }
    if (!testBoundedPipeline()) {
      return 25;
    }
    if (!testLifecycleAndRouting()) {
      return 27;
    }
    if (!testWindowsAudioContext()) {
      return 30;
    }
    (void)loadAppSettings();
    return 0;
  } catch (...) {
    return 99;
  }
}

} // namespace vvc::windows

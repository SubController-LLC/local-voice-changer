#pragma once

#include "realtime/AudioRingBuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>

namespace vvc::realtime {

enum class PipelinePhase { suspended, running, reset_arming, reset_pending, priming, fatal };

struct RealtimePipelineStats {
  std::size_t input_depth = 0U;
  std::size_t output_depth = 0U;
  std::size_t logical_depth = 0U;
  std::size_t max_input_depth = 0U;
  std::size_t max_output_depth = 0U;
  std::size_t max_logical_depth = 0U;
  std::size_t avoidable_backlog = 0U;
  std::size_t max_avoidable_backlog = 0U;
  std::uint64_t deadline_misses = 0U;
  std::uint64_t resynchronizations = 0U;
  std::uint64_t input_overruns = 0U;
  std::uint64_t output_underruns = 0U;
  std::uint64_t fatal_dsp_failures = 0U;
  PipelinePhase phase = PipelinePhase::suspended;
};

class RealtimeAudioPipeline {
public:
  static constexpr std::size_t kBlockFrames = 480U;
  static constexpr std::size_t kNominalPrimingFrames = kBlockFrames;
  static constexpr std::size_t kMaximumAvoidableBacklogFrames = 960U;
  static constexpr std::size_t kMaximumTotalQueueFrames =
      kNominalPrimingFrames + kMaximumAvoidableBacklogFrames;
  static constexpr auto kProcessingDeadline = std::chrono::milliseconds(10);

  RealtimeAudioPipeline();

  // Call only while callbacks and the worker are stopped.
  void prepare();
  void suspend() noexcept;

  // Callback-side operations. They are allocation-free, lock-free, and noexcept.
  std::size_t pushInput(std::span<const float> input) noexcept;
  std::size_t popOutput(std::span<float> output) noexcept;

  // Worker-side operation. Exactly one worker may call this method.
  template <class Process, class Reset> bool processOne(Process&& process, Reset&& reset) noexcept {
    return processOneImpl(std::forward<Process>(process), std::forward<Reset>(reset),
                          []() noexcept {});
  }

  // Exposes the otherwise zero-overhead point immediately after the final phase check so tests
  // can deterministically exercise a reset racing the output publication.
  template <class Process, class Reset, class BeforePublish>
  bool processOneWithPublishHookForTesting(Process&& process, Reset&& reset,
                                           BeforePublish&& beforePublish) noexcept {
    return processOneImpl(std::forward<Process>(process), std::forward<Reset>(reset),
                          std::forward<BeforePublish>(beforePublish));
  }

  void markFatalFailure() noexcept;
  [[nodiscard]] bool hasFatalFailure() const noexcept;
  [[nodiscard]] RealtimePipelineStats stats() const noexcept;

private:
  template <class Process, class Reset, class BeforePublish>
  bool processOneImpl(Process&& process, Reset&& reset, BeforePublish&& beforePublish) noexcept {
    if (phase_.load(std::memory_order_acquire) == PipelinePhase::reset_pending) {
      const std::uint64_t epoch = reset_epoch_.load(std::memory_order_acquire);
      if (drained_epoch_.load(std::memory_order_acquire) != epoch) {
        return false;
      }
      PipelinePhase expected = PipelinePhase::reset_pending;
      if (!phase_.compare_exchange_strong(expected, PipelinePhase::priming,
                                          std::memory_order_acq_rel)) {
        return false;
      }
      const std::size_t discarded = input_ring_.discardOldest(input_ring_.availableToRead());
      releaseLogicalFrames(discarded);
      try {
        std::invoke(std::forward<Reset>(reset));
      } catch (...) {
        markFatalFailure();
        return false;
      }
      std::array<float, kBlockFrames> silence{};
      if (!reserveLogicalFrames(silence.size())) {
        markFatalFailure();
        return false;
      }
      const std::size_t primed = output_ring_.push(silence);
      if (primed != silence.size()) {
        releaseLogicalFrames(silence.size() - primed);
        markFatalFailure();
        return false;
      }
      phase_.store(PipelinePhase::running, std::memory_order_release);
      updateDepthMetrics();
      return true;
    }

    if (phase_.load(std::memory_order_acquire) != PipelinePhase::running ||
        input_ring_.availableToRead() < kBlockFrames ||
        output_ring_.availableToWrite() < kBlockFrames) {
      return false;
    }

    // This flag and the second phase read form a seq_cst publication gate with the callback's
    // reset transition. Either this worker observes the reset, or the callback observes an active
    // worker and withholds its drain acknowledgement until a later callback.
    worker_in_flight_.store(true, std::memory_order_seq_cst);
    const auto finishWorkerAttempt = [this] {
      worker_in_flight_.store(false, std::memory_order_seq_cst);
    };
    if (phase_.load(std::memory_order_seq_cst) != PipelinePhase::running) {
      finishWorkerAttempt();
      return false;
    }

    std::array<float, kBlockFrames> input{};
    std::array<float, kBlockFrames> output{};
    const std::size_t popped = input_ring_.pop(input);
    if (popped != input.size()) {
      releaseLogicalFrames(popped);
      finishWorkerAttempt();
      return false;
    }

    const auto started = std::chrono::steady_clock::now();
    try {
      std::invoke(std::forward<Process>(process), std::span<const float>(input),
                  std::span<float>(output));
    } catch (...) {
      releaseLogicalFrames(input.size());
      markFatalFailure();
      finishWorkerAttempt();
      return false;
    }
    if (std::chrono::steady_clock::now() - started > kProcessingDeadline) {
      deadline_misses_.fetch_add(1U, std::memory_order_relaxed);
    }

    constexpr float kMinusOneDbfs = 0.891'250'9F;
    for (float& sample : output) {
      sample = std::isfinite(sample) ? std::clamp(sample, -kMinusOneDbfs, kMinusOneDbfs) : 0.0F;
    }
    // A callback can request a reset while DSP is running. Its input remains included in the
    // logical frame reservation until this point, but output from that old generation must not
    // be published into the newly-reset stream.
    if (phase_.load(std::memory_order_acquire) != PipelinePhase::running) {
      releaseLogicalFrames(input.size());
      finishWorkerAttempt();
      updateDepthMetrics();
      return false;
    }

    try {
      std::invoke(std::forward<BeforePublish>(beforePublish));
    } catch (...) {
      releaseLogicalFrames(input.size());
      markFatalFailure();
      finishWorkerAttempt();
      return false;
    }

    const std::size_t pushed = output_ring_.push(output);
    if (pushed != output.size()) {
      releaseLogicalFrames(output.size() - pushed);
      requestResynchronization(false);
      finishWorkerAttempt();
      return false;
    }
    finishWorkerAttempt();
    updateDepthMetrics();
    return true;
  }
  bool reserveLogicalFrames(std::size_t count) noexcept;
  void releaseLogicalFrames(std::size_t count) noexcept;
  void requestResynchronization(bool inputOverrun) noexcept;
  void updateDepthMetrics() noexcept;
  static void updateMaximum(std::atomic<std::size_t>& destination, std::size_t value) noexcept;

  AudioRingBuffer input_ring_;
  AudioRingBuffer output_ring_;
  std::atomic<PipelinePhase> phase_{PipelinePhase::suspended};
  std::atomic<bool> worker_in_flight_{false};
  std::atomic<std::uint64_t> reset_epoch_{0U};
  std::atomic<std::uint64_t> drained_epoch_{0U};
  // Counts queued, callback-reserved, and worker-in-flight frames. Moving a block from the input
  // ring through DSP to the output ring does not change this value.
  std::atomic<std::size_t> logical_frames_{0U};
  std::atomic<std::size_t> max_input_depth_{0U};
  std::atomic<std::size_t> max_output_depth_{0U};
  std::atomic<std::size_t> max_logical_depth_{0U};
  std::atomic<std::size_t> max_avoidable_backlog_{0U};
  std::atomic<std::uint64_t> deadline_misses_{0U};
  std::atomic<std::uint64_t> resynchronizations_{0U};
  std::atomic<std::uint64_t> input_overruns_{0U};
  std::atomic<std::uint64_t> output_underruns_{0U};
  std::atomic<std::uint64_t> fatal_dsp_failures_{0U};
};

} // namespace vvc::realtime

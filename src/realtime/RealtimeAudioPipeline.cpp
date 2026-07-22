#include "realtime/RealtimeAudioPipeline.h"

#include <algorithm>
#include <array>

namespace vvc::realtime {
namespace {

constexpr std::size_t kRingCapacity = 2'048U;

} // namespace

RealtimeAudioPipeline::RealtimeAudioPipeline()
    : input_ring_(kRingCapacity), output_ring_(kRingCapacity) {}

void RealtimeAudioPipeline::prepare() {
  phase_.store(PipelinePhase::suspended, std::memory_order_release);
  input_ring_.clear();
  output_ring_.clear();
  worker_in_flight_.store(false, std::memory_order_relaxed);
  reset_epoch_.store(0U, std::memory_order_relaxed);
  drained_epoch_.store(0U, std::memory_order_relaxed);
  logical_frames_.store(0U, std::memory_order_relaxed);
  max_input_depth_.store(0U, std::memory_order_relaxed);
  max_output_depth_.store(0U, std::memory_order_relaxed);
  max_logical_depth_.store(0U, std::memory_order_relaxed);
  max_avoidable_backlog_.store(0U, std::memory_order_relaxed);
  deadline_misses_.store(0U, std::memory_order_relaxed);
  resynchronizations_.store(0U, std::memory_order_relaxed);
  input_overruns_.store(0U, std::memory_order_relaxed);
  output_underruns_.store(0U, std::memory_order_relaxed);
  fatal_dsp_failures_.store(0U, std::memory_order_relaxed);
  std::array<float, kNominalPrimingFrames> silence{};
  const std::size_t primed = output_ring_.push(silence);
  logical_frames_.store(primed, std::memory_order_relaxed);
  phase_.store(PipelinePhase::running, std::memory_order_release);
  updateDepthMetrics();
}

void RealtimeAudioPipeline::suspend() noexcept {
  phase_.store(PipelinePhase::suspended, std::memory_order_release);
}

std::size_t RealtimeAudioPipeline::pushInput(const std::span<const float> input) noexcept {
  if (phase_.load(std::memory_order_acquire) != PipelinePhase::running || input.empty()) {
    return 0U;
  }
  if (!reserveLogicalFrames(input.size())) {
    requestResynchronization(true);
    return 0U;
  }
  const std::size_t pushed = input_ring_.push(input);
  if (pushed != input.size()) {
    releaseLogicalFrames(input.size() - pushed);
    requestResynchronization(true);
  }
  updateDepthMetrics();
  return pushed;
}

std::size_t RealtimeAudioPipeline::popOutput(const std::span<float> output) noexcept {
  if (output.empty()) {
    return 0U;
  }
  std::fill(output.begin(), output.end(), 0.0F);
  const PipelinePhase phase = phase_.load(std::memory_order_acquire);
  if (phase == PipelinePhase::reset_pending) {
    const std::uint64_t epoch = reset_epoch_.load(std::memory_order_acquire);
    if (drained_epoch_.load(std::memory_order_acquire) == epoch) {
      return 0U;
    }
    const bool workerInFlight = worker_in_flight_.load(std::memory_order_seq_cst);
    const std::size_t discarded = output_ring_.discardOldest(output_ring_.availableToRead());
    releaseLogicalFrames(discarded);
    if (!workerInFlight && phase_.load(std::memory_order_acquire) == PipelinePhase::reset_pending &&
        reset_epoch_.load(std::memory_order_acquire) == epoch) {
      drained_epoch_.store(epoch, std::memory_order_release);
    }
    updateDepthMetrics();
    return 0U;
  }
  if (phase != PipelinePhase::running) {
    return 0U;
  }

  const std::size_t popped = output_ring_.pop(output);
  releaseLogicalFrames(popped);
  if (popped < output.size()) {
    output_underruns_.fetch_add(1U, std::memory_order_relaxed);
    requestResynchronization(false);
  }
  updateDepthMetrics();
  return popped;
}

void RealtimeAudioPipeline::markFatalFailure() noexcept {
  const PipelinePhase previous = phase_.exchange(PipelinePhase::fatal, std::memory_order_acq_rel);
  if (previous != PipelinePhase::fatal) {
    fatal_dsp_failures_.fetch_add(1U, std::memory_order_relaxed);
  }
}

bool RealtimeAudioPipeline::hasFatalFailure() const noexcept {
  return phase_.load(std::memory_order_acquire) == PipelinePhase::fatal;
}

RealtimePipelineStats RealtimeAudioPipeline::stats() const noexcept {
  const std::size_t inputDepth = input_ring_.availableToRead();
  const std::size_t outputDepth = output_ring_.availableToRead();
  const std::size_t logicalDepth = logical_frames_.load(std::memory_order_relaxed);
  return {inputDepth,
          outputDepth,
          logicalDepth,
          max_input_depth_.load(std::memory_order_relaxed),
          max_output_depth_.load(std::memory_order_relaxed),
          max_logical_depth_.load(std::memory_order_relaxed),
          logicalDepth > kNominalPrimingFrames ? logicalDepth - kNominalPrimingFrames : 0U,
          max_avoidable_backlog_.load(std::memory_order_relaxed),
          deadline_misses_.load(std::memory_order_relaxed),
          resynchronizations_.load(std::memory_order_relaxed),
          input_overruns_.load(std::memory_order_relaxed),
          output_underruns_.load(std::memory_order_relaxed),
          fatal_dsp_failures_.load(std::memory_order_relaxed),
          phase_.load(std::memory_order_acquire)};
}

bool RealtimeAudioPipeline::reserveLogicalFrames(const std::size_t count) noexcept {
  if (count > kMaximumTotalQueueFrames) {
    return false;
  }
  std::size_t current = logical_frames_.load(std::memory_order_relaxed);
  while (current <= kMaximumTotalQueueFrames - count) {
    const std::size_t desired = current + count;
    if (logical_frames_.compare_exchange_weak(current, desired, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
      updateMaximum(max_logical_depth_, desired);
      updateMaximum(max_avoidable_backlog_,
                    desired > kNominalPrimingFrames ? desired - kNominalPrimingFrames : 0U);
      return true;
    }
  }
  return false;
}

void RealtimeAudioPipeline::releaseLogicalFrames(const std::size_t count) noexcept {
  std::size_t current = logical_frames_.load(std::memory_order_relaxed);
  while (true) {
    const std::size_t desired = count < current ? current - count : 0U;
    if (logical_frames_.compare_exchange_weak(current, desired, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
      return;
    }
  }
}

void RealtimeAudioPipeline::requestResynchronization(const bool inputOverrun) noexcept {
  PipelinePhase expected = PipelinePhase::running;
  if (phase_.compare_exchange_strong(expected, PipelinePhase::reset_arming,
                                     std::memory_order_seq_cst)) {
    (void)reset_epoch_.fetch_add(1U, std::memory_order_acq_rel);
    phase_.store(PipelinePhase::reset_pending, std::memory_order_release);
    resynchronizations_.fetch_add(1U, std::memory_order_relaxed);
  }
  if (inputOverrun) {
    input_overruns_.fetch_add(1U, std::memory_order_relaxed);
  }
}

void RealtimeAudioPipeline::updateDepthMetrics() noexcept {
  const std::size_t inputDepth = input_ring_.availableToRead();
  const std::size_t outputDepth = output_ring_.availableToRead();
  const std::size_t logicalDepth = logical_frames_.load(std::memory_order_relaxed);
  updateMaximum(max_input_depth_, inputDepth);
  updateMaximum(max_output_depth_, outputDepth);
  updateMaximum(max_logical_depth_, logicalDepth);
  updateMaximum(max_avoidable_backlog_,
                logicalDepth > kNominalPrimingFrames ? logicalDepth - kNominalPrimingFrames : 0U);
}

void RealtimeAudioPipeline::updateMaximum(std::atomic<std::size_t>& destination,
                                          const std::size_t value) noexcept {
  std::size_t previous = destination.load(std::memory_order_relaxed);
  while (value > previous &&
         !destination.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
  }
}

} // namespace vvc::realtime

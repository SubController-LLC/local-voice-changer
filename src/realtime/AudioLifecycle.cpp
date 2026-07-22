#include "realtime/AudioLifecycle.h"

namespace vvc::realtime {
namespace {

constexpr std::uint64_t kCleanupRequestedMask = std::uint64_t{1} << 16U;

[[nodiscard]] AudioLifecycleState decodeState(const std::uint64_t value) noexcept {
  return static_cast<AudioLifecycleState>(value & 0xFFU);
}

[[nodiscard]] AudioFailureReason decodeFailure(const std::uint64_t value) noexcept {
  return static_cast<AudioFailureReason>((value >> 8U) & 0xFFU);
}

[[nodiscard]] bool decodeCleanupRequested(const std::uint64_t value) noexcept {
  return (value & kCleanupRequestedMask) != 0U;
}

[[nodiscard]] AudioLifecycleGeneration decodeGeneration(const std::uint64_t value) noexcept {
  return static_cast<AudioLifecycleGeneration>(value >> 32U);
}

} // namespace

bool AudioLifecycle::beginStart() noexcept {
  std::uint64_t current = value_.load(std::memory_order_acquire);
  while (decodeState(current) == AudioLifecycleState::stopped &&
         decodeFailure(current) == AudioFailureReason::none && !decodeCleanupRequested(current)) {
    const AudioLifecycleGeneration nextGeneration = decodeGeneration(current) + 1U;
    if (value_.compare_exchange_weak(
            current,
            encode(AudioLifecycleState::starting, AudioFailureReason::none, false, nextGeneration),
            std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

bool AudioLifecycle::markRunning(const AudioLifecycleGeneration expectedGeneration) noexcept {
  std::uint64_t current = value_.load(std::memory_order_acquire);
  while (decodeGeneration(current) == expectedGeneration &&
         decodeState(current) == AudioLifecycleState::starting &&
         decodeFailure(current) == AudioFailureReason::none && !decodeCleanupRequested(current)) {
    if (value_.compare_exchange_weak(current,
                                     encode(AudioLifecycleState::running, AudioFailureReason::none,
                                            false, expectedGeneration),
                                     std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

bool AudioLifecycle::fail(const AudioFailureReason reason,
                          const AudioLifecycleGeneration expectedGeneration) noexcept {
  if (reason == AudioFailureReason::none) {
    return false;
  }
  std::uint64_t current = value_.load(std::memory_order_acquire);
  while (decodeGeneration(current) == expectedGeneration &&
         decodeState(current) != AudioLifecycleState::stopped &&
         decodeState(current) != AudioLifecycleState::recoverable_error) {
    if (value_.compare_exchange_weak(
            current,
            encode(AudioLifecycleState::recoverable_error, reason, true, expectedGeneration),
            std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

bool AudioLifecycle::consumeCleanupRequest() noexcept {
  std::uint64_t current = value_.load(std::memory_order_acquire);
  const AudioLifecycleGeneration observedGeneration = decodeGeneration(current);
  while (decodeGeneration(current) == observedGeneration && decodeCleanupRequested(current)) {
    const std::uint64_t desired = current & ~kCleanupRequestedMask;
    if (value_.compare_exchange_weak(current, desired, std::memory_order_acq_rel)) {
      return true;
    }
  }
  return false;
}

bool AudioLifecycle::acknowledgeRecovery() noexcept {
  std::uint64_t current = value_.load(std::memory_order_acquire);
  const AudioLifecycleGeneration observedGeneration = decodeGeneration(current);
  while (decodeGeneration(current) == observedGeneration &&
         decodeState(current) == AudioLifecycleState::recoverable_error) {
    if (decodeCleanupRequested(current)) {
      return false;
    }
    if (value_.compare_exchange_weak(current,
                                     encode(AudioLifecycleState::stopped, AudioFailureReason::none,
                                            false, observedGeneration),
                                     std::memory_order_acq_rel)) {
      return true;
    }
  }
  return decodeGeneration(current) == observedGeneration &&
         decodeState(current) == AudioLifecycleState::stopped && !decodeCleanupRequested(current);
}

void AudioLifecycle::stop() noexcept {
  std::uint64_t current = value_.load(std::memory_order_acquire);
  while (true) {
    const std::uint64_t stopped = encode(AudioLifecycleState::stopped, AudioFailureReason::none,
                                         false, decodeGeneration(current));
    if (current == stopped ||
        value_.compare_exchange_weak(current, stopped, std::memory_order_acq_rel)) {
      return;
    }
  }
}

AudioLifecycleSnapshot AudioLifecycle::snapshot() const noexcept {
  const std::uint64_t value = value_.load(std::memory_order_acquire);
  return {decodeState(value), decodeFailure(value), decodeCleanupRequested(value),
          decodeGeneration(value)};
}

bool AudioLifecycle::isRunning() const noexcept {
  return decodeState(value_.load(std::memory_order_acquire)) == AudioLifecycleState::running;
}

} // namespace vvc::realtime

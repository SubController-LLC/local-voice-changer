#pragma once

#include <atomic>
#include <cstdint>

namespace vvc::realtime {

enum class AudioLifecycleState { stopped, starting, running, recoverable_error };

enum class AudioFailureReason {
  none,
  device_stopped,
  device_rerouted,
  interruption_began,
  device_open_failed,
  device_start_failed,
  dsp_failure,
};

using AudioLifecycleGeneration = std::uint32_t;

struct AudioLifecycleSnapshot {
  AudioLifecycleState state = AudioLifecycleState::stopped;
  AudioFailureReason failure = AudioFailureReason::none;
  bool cleanup_requested = false;
  AudioLifecycleGeneration generation = 0U;
};

// Thread-safe lifecycle reducer. Audio callbacks may publish failure events, while the UI thread
// consumes the single cleanup request and owns device destruction.
class AudioLifecycle {
public:
  [[nodiscard]] bool beginStart() noexcept;
  // Session-bound transitions reject work captured before a stop/restart. fail() returns true only
  // for the caller that publishes the recoverable error, allowing it to gate related side effects.
  [[nodiscard]] bool markRunning(AudioLifecycleGeneration expectedGeneration) noexcept;
  [[nodiscard]] bool fail(AudioFailureReason reason,
                          AudioLifecycleGeneration expectedGeneration) noexcept;
  [[nodiscard]] bool consumeCleanupRequest() noexcept;
  [[nodiscard]] bool acknowledgeRecovery() noexcept;
  void stop() noexcept;

  [[nodiscard]] AudioLifecycleSnapshot snapshot() const noexcept;
  [[nodiscard]] bool isRunning() const noexcept;

private:
  static constexpr std::uint64_t encode(const AudioLifecycleState state,
                                        const AudioFailureReason failure,
                                        const bool cleanupRequested = false,
                                        const AudioLifecycleGeneration generation = 0U) noexcept {
    return static_cast<std::uint64_t>(state) | (static_cast<std::uint64_t>(failure) << 8U) |
           (cleanupRequested ? (std::uint64_t{1} << 16U) : 0U) |
           (static_cast<std::uint64_t>(generation) << 32U);
  }

  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "Audio lifecycle updates must remain lock-free on the realtime callback path");

  std::atomic<std::uint64_t> value_{encode(AudioLifecycleState::stopped, AudioFailureReason::none)};
};

} // namespace vvc::realtime

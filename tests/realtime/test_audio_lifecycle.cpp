#include "realtime/AudioLifecycle.h"

#include <array>
#include <barrier>
#include <thread>

#include <catch2/catch_test_macros.hpp>

using vvc::realtime::AudioFailureReason;
using vvc::realtime::AudioLifecycle;
using vvc::realtime::AudioLifecycleGeneration;
using vvc::realtime::AudioLifecycleState;

TEST_CASE("audio lifecycle covers start running stop and recovery") {
  AudioLifecycle lifecycle;
  REQUIRE(lifecycle.snapshot().state == AudioLifecycleState::stopped);
  REQUIRE(lifecycle.beginStart());
  const auto firstGeneration = lifecycle.snapshot().generation;
  REQUIRE(firstGeneration != 0U);
  REQUIRE(lifecycle.snapshot().state == AudioLifecycleState::starting);
  REQUIRE(lifecycle.markRunning(firstGeneration));
  REQUIRE(lifecycle.isRunning());

  REQUIRE(lifecycle.fail(AudioFailureReason::device_rerouted, firstGeneration));
  const auto failed = lifecycle.snapshot();
  REQUIRE(failed.state == AudioLifecycleState::recoverable_error);
  REQUIRE(failed.failure == AudioFailureReason::device_rerouted);
  REQUIRE(failed.cleanup_requested);
  REQUIRE(failed.generation == firstGeneration);
  REQUIRE_FALSE(lifecycle.acknowledgeRecovery());
  REQUIRE(lifecycle.consumeCleanupRequest());
  REQUIRE_FALSE(lifecycle.consumeCleanupRequest());
  REQUIRE(lifecycle.acknowledgeRecovery());
  REQUIRE(lifecycle.beginStart());
  const auto secondGeneration = lifecycle.snapshot().generation;
  REQUIRE(secondGeneration == firstGeneration + 1U);
  REQUIRE(lifecycle.markRunning(secondGeneration));
  lifecycle.stop();
  REQUIRE(lifecycle.snapshot().state == AudioLifecycleState::stopped);
  REQUIRE(lifecycle.snapshot().generation == secondGeneration);
}

TEST_CASE("audio lifecycle failure events collapse into one cleanup") {
  AudioLifecycle lifecycle;
  REQUIRE(lifecycle.beginStart());
  const auto generation = lifecycle.snapshot().generation;
  REQUIRE(lifecycle.markRunning(generation));

  const std::array reasons{AudioFailureReason::device_stopped, AudioFailureReason::device_rerouted,
                           AudioFailureReason::interruption_began, AudioFailureReason::dsp_failure};
  std::array<std::thread, reasons.size()> threads;
  for (std::size_t index = 0U; index < reasons.size(); ++index) {
    threads[index] = std::thread([&lifecycle, generation, reason = reasons[index]] {
      for (int iteration = 0; iteration < 100; ++iteration) {
        (void)lifecycle.fail(reason, generation);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(lifecycle.snapshot().state == AudioLifecycleState::recoverable_error);
  REQUIRE(lifecycle.snapshot().failure != AudioFailureReason::none);
  REQUIRE(lifecycle.consumeCleanupRequest());
  REQUIRE_FALSE(lifecycle.consumeCleanupRequest());
}

TEST_CASE("audio lifecycle models every recoverable failure and restart") {
  for (const auto reason :
       {AudioFailureReason::device_stopped, AudioFailureReason::device_rerouted,
        AudioFailureReason::interruption_began, AudioFailureReason::device_open_failed,
        AudioFailureReason::device_start_failed, AudioFailureReason::dsp_failure}) {
    AudioLifecycle lifecycle;
    REQUIRE(lifecycle.beginStart());
    const auto failedGeneration = lifecycle.snapshot().generation;
    REQUIRE(lifecycle.fail(reason, failedGeneration));
    REQUIRE(lifecycle.snapshot().state == AudioLifecycleState::recoverable_error);
    REQUIRE(lifecycle.snapshot().failure == reason);
    REQUIRE(lifecycle.snapshot().generation == failedGeneration);
    REQUIRE(lifecycle.consumeCleanupRequest());
    REQUIRE(lifecycle.acknowledgeRecovery());
    REQUIRE(lifecycle.beginStart());
    const auto restartedGeneration = lifecycle.snapshot().generation;
    REQUIRE(restartedGeneration == failedGeneration + 1U);
    REQUIRE(lifecycle.markRunning(restartedGeneration));
  }
}

TEST_CASE("audio lifecycle ignores late failures after an intentional stop") {
  AudioLifecycle lifecycle;
  REQUIRE_FALSE(lifecycle.fail(AudioFailureReason::dsp_failure, 0U));
  REQUIRE(lifecycle.snapshot().state == AudioLifecycleState::stopped);
  REQUIRE(lifecycle.snapshot().failure == AudioFailureReason::none);

  REQUIRE(lifecycle.beginStart());
  const auto generation = lifecycle.snapshot().generation;
  REQUIRE(lifecycle.markRunning(generation));
  lifecycle.stop();
  REQUIRE_FALSE(lifecycle.fail(AudioFailureReason::device_stopped, generation));
  REQUIRE(lifecycle.snapshot().state == AudioLifecycleState::stopped);
  REQUIRE_FALSE(lifecycle.consumeCleanupRequest());
}

TEST_CASE("audio lifecycle rejects a delayed failure from the previous session") {
  AudioLifecycle lifecycle;
  REQUIRE(lifecycle.beginStart());
  const auto oldGeneration = lifecycle.snapshot().generation;
  REQUIRE(lifecycle.markRunning(oldGeneration));

  // Models a notification that captured the old running snapshot, then resumed only after the UI
  // stopped that device and started its replacement.
  lifecycle.stop();
  REQUIRE(lifecycle.beginStart());
  const auto newGeneration = lifecycle.snapshot().generation;
  REQUIRE(newGeneration == oldGeneration + 1U);
  REQUIRE(lifecycle.markRunning(newGeneration));

  REQUIRE_FALSE(lifecycle.fail(AudioFailureReason::device_rerouted, oldGeneration));
  const auto current = lifecycle.snapshot();
  REQUIRE(current.state == AudioLifecycleState::running);
  REQUIRE(current.failure == AudioFailureReason::none);
  REQUIRE_FALSE(current.cleanup_requested);
  REQUIRE(current.generation == newGeneration);
}

TEST_CASE("audio lifecycle stop and failure cannot leak cleanup into a new session") {
  constexpr int kIterations = 2'000;
  AudioLifecycle lifecycle;
  std::barrier startGate(3);
  std::barrier finishGate(3);
  AudioLifecycleGeneration activeGeneration = 0U;

  std::thread stopper([&] {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      startGate.arrive_and_wait();
      lifecycle.stop();
      finishGate.arrive_and_wait();
    }
  });
  std::thread failer([&] {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      startGate.arrive_and_wait();
      (void)lifecycle.fail(AudioFailureReason::device_stopped, activeGeneration);
      finishGate.arrive_and_wait();
    }
  });

  bool valid = true;
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    valid = lifecycle.beginStart() && valid;
    activeGeneration = lifecycle.snapshot().generation;
    valid = lifecycle.markRunning(activeGeneration) && valid;
    startGate.arrive_and_wait();
    finishGate.arrive_and_wait();

    const auto stopped = lifecycle.snapshot();
    valid = stopped.state == AudioLifecycleState::stopped && valid;
    valid = stopped.failure == AudioFailureReason::none && valid;
    valid = !stopped.cleanup_requested && valid;
    valid = !lifecycle.consumeCleanupRequest() && valid;

    valid = lifecycle.beginStart() && valid;
    const auto replacementGeneration = lifecycle.snapshot().generation;
    valid = replacementGeneration == activeGeneration + 1U && valid;
    valid = lifecycle.markRunning(replacementGeneration) && valid;
    valid = !lifecycle.consumeCleanupRequest() && valid;
    lifecycle.stop();
  }

  stopper.join();
  failer.join();
  REQUIRE(valid);
}

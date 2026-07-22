#include "realtime/AudioRingBuffer.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <span>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("audio ring buffer preserves order across wraparound") {
  vvc::realtime::AudioRingBuffer ring(8U);
  const std::array first{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  REQUIRE(ring.push(first) == first.size());

  std::array<float, 4> head{};
  REQUIRE(ring.pop(head) == head.size());
  REQUIRE(head == std::array{1.0F, 2.0F, 3.0F, 4.0F});

  const std::array second{7.0F, 8.0F, 9.0F, 10.0F, 11.0F, 12.0F};
  REQUIRE(ring.push(second) == second.size());
  std::array<float, 8> output{};
  REQUIRE(ring.pop(output) == output.size());
  REQUIRE(output == std::array{5.0F, 6.0F, 7.0F, 8.0F, 9.0F, 10.0F, 11.0F, 12.0F});
}

TEST_CASE("audio ring buffer reports overflow without overwriting unread audio") {
  vvc::realtime::AudioRingBuffer ring(4U);
  const std::array input{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
  REQUIRE(ring.push(input) == 4U);
  REQUIRE(ring.availableToWrite() == 0U);

  std::array<float, 4> output{};
  REQUIRE(ring.pop(output) == output.size());
  REQUIRE(output == std::array{1.0F, 2.0F, 3.0F, 4.0F});
}

TEST_CASE("audio ring buffer consumer can discard stale frames") {
  vvc::realtime::AudioRingBuffer ring(8U);
  const std::array input{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  REQUIRE(ring.push(input) == input.size());
  REQUIRE(ring.discardOldest(4U) == 4U);
  REQUIRE(ring.availableToRead() == 2U);

  std::array<float, 4U> output{};
  REQUIRE(ring.pop(output) == 2U);
  REQUIRE(output[0] == 5.0F);
  REQUIRE(output[1] == 6.0F);
  REQUIRE(ring.discardOldest(10U) == 0U);
}

TEST_CASE("audio ring buffer preserves a long concurrent SPSC stream") {
  constexpr std::size_t kSampleCount = 100'000U;
  vvc::realtime::AudioRingBuffer ring(1'024U);
  std::vector<float> input(kSampleCount, 0.0F);
  std::vector<float> output(kSampleCount, 0.0F);
  for (std::size_t index = 0U; index < input.size(); ++index) {
    input[index] = static_cast<float>(index);
  }

  std::thread producer([&] {
    std::size_t written = 0U;
    while (written < input.size()) {
      const std::size_t count = ring.push(std::span<const float>(input).subspan(written));
      written += count;
      if (count == 0U) {
        std::this_thread::yield();
      }
    }
  });

  std::size_t read = 0U;
  while (read < output.size()) {
    const std::size_t count = ring.pop(std::span<float>(output).subspan(read));
    read += count;
    if (count == 0U) {
      std::this_thread::yield();
    }
  }
  producer.join();
  REQUIRE(output == input);
}

TEST_CASE("audio ring buffer observer snapshots stay within capacity") {
  constexpr std::size_t kSampleCount = 500'000U;
  vvc::realtime::AudioRingBuffer ring(8U);
  std::vector<float> input(kSampleCount, 0.0F);
  for (std::size_t index = 0U; index < input.size(); ++index) {
    input[index] = static_cast<float>(index);
  }

  std::barrier startGate(4);
  std::atomic<bool> consumerDone{false};
  std::atomic<bool> invalidSnapshot{false};
  std::atomic<std::size_t> observations{0U};

  std::thread producer([&] {
    startGate.arrive_and_wait();
    std::size_t written = 0U;
    while (written < input.size()) {
      written += ring.push(std::span<const float>(input).subspan(written));
      std::this_thread::yield();
    }
  });

  std::thread consumer([&] {
    startGate.arrive_and_wait();
    std::array<float, 7U> block{};
    std::size_t read = 0U;
    while (read < input.size()) {
      const std::size_t count = ring.pop(block);
      for (std::size_t index = 0U; index < count; ++index) {
        if (block[index] != input[read + index]) {
          invalidSnapshot.store(true, std::memory_order_relaxed);
        }
      }
      read += count;
      std::this_thread::yield();
    }
    consumerDone.store(true, std::memory_order_release);
  });

  std::thread observer([&] {
    startGate.arrive_and_wait();
    do {
      const std::size_t readable = ring.availableToRead();
      const std::size_t writable = ring.availableToWrite();
      if (readable > ring.capacity() || writable > ring.capacity()) {
        invalidSnapshot.store(true, std::memory_order_relaxed);
      }
      observations.fetch_add(1U, std::memory_order_relaxed);
      std::this_thread::yield();
    } while (!consumerDone.load(std::memory_order_acquire));
  });

  startGate.arrive_and_wait();
  producer.join();
  consumer.join();
  observer.join();

  REQUIRE_FALSE(invalidSnapshot.load(std::memory_order_relaxed));
  REQUIRE(observations.load(std::memory_order_relaxed) > 0U);
  REQUIRE(ring.availableToRead() == 0U);
  REQUIRE(ring.availableToWrite() == ring.capacity());
}

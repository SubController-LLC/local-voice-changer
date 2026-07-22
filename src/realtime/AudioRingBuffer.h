#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace vvc::realtime {

// Single-producer/single-consumer audio FIFO. Storage is allocated once at construction;
// push/pop perform no locks, allocation, logging, or system calls on the audio callback.
#ifdef _MSC_VER
#pragma warning(push)
// Cache-line separation intentionally pads this type to avoid false sharing between callbacks.
#pragma warning(disable : 4324)
#endif
class AudioRingBuffer {
public:
  explicit AudioRingBuffer(const std::size_t minimumCapacity)
      : capacity_(std::bit_ceil(std::max<std::size_t>(minimumCapacity, 2U))), mask_(capacity_ - 1U),
        samples_(capacity_, 0.0F) {
    if (capacity_ < minimumCapacity) {
      throw std::overflow_error("AudioRingBuffer capacity overflow");
    }
  }

  [[nodiscard]] std::size_t capacity() const { return capacity_; }

  [[nodiscard]] std::size_t availableToRead() const {
    // Reading the consumer position first creates a transitive happens-before chain from the
    // producer position observed by that consumer. The cap also keeps a third-party diagnostics
    // observer safe across counter wraparound or a deliberately stale mixed snapshot.
    const std::size_t read = read_position_.load(std::memory_order_acquire);
    const std::size_t write = write_position_.load(std::memory_order_acquire);
    return std::min(write - read, capacity_);
  }

  [[nodiscard]] std::size_t availableToWrite() const { return capacity_ - availableToRead(); }

  std::size_t push(const std::span<const float> input) {
    const std::size_t write = write_position_.load(std::memory_order_relaxed);
    const std::size_t read = read_position_.load(std::memory_order_acquire);
    const std::size_t count = std::min(input.size(), capacity_ - (write - read));
    copyInto(write, input.first(count));
    write_position_.store(write + count, std::memory_order_release);
    return count;
  }

  std::size_t pop(const std::span<float> output) {
    const std::size_t read = read_position_.load(std::memory_order_relaxed);
    const std::size_t write = write_position_.load(std::memory_order_acquire);
    const std::size_t count = std::min(output.size(), write - read);
    copyOut(read, output.first(count));
    read_position_.store(read + count, std::memory_order_release);
    return count;
  }

  // Advances only the consumer-owned read position. This is safe while the producer is active,
  // but it must be called by the same thread that normally calls pop().
  std::size_t discardOldest(const std::size_t requestedCount) {
    const std::size_t read = read_position_.load(std::memory_order_relaxed);
    const std::size_t write = write_position_.load(std::memory_order_acquire);
    const std::size_t count = std::min(requestedCount, write - read);
    read_position_.store(read + count, std::memory_order_release);
    return count;
  }

  // Only call while producer and consumer are stopped.
  void clear() {
    read_position_.store(0U, std::memory_order_relaxed);
    write_position_.store(0U, std::memory_order_relaxed);
    std::fill(samples_.begin(), samples_.end(), 0.0F);
  }

private:
  void copyInto(const std::size_t position, const std::span<const float> input) {
    const std::size_t start = position & mask_;
    const std::size_t first = std::min(input.size(), capacity_ - start);
    if (first != 0U) {
      std::memcpy(samples_.data() + start, input.data(), first * sizeof(float));
    }
    if (input.size() > first) {
      std::memcpy(samples_.data(), input.data() + first, (input.size() - first) * sizeof(float));
    }
  }

  void copyOut(const std::size_t position, const std::span<float> output) const {
    const std::size_t start = position & mask_;
    const std::size_t first = std::min(output.size(), capacity_ - start);
    if (first != 0U) {
      std::memcpy(output.data(), samples_.data() + start, first * sizeof(float));
    }
    if (output.size() > first) {
      std::memcpy(output.data() + first, samples_.data(), (output.size() - first) * sizeof(float));
    }
  }

  std::size_t capacity_;
  std::size_t mask_;
  std::vector<float> samples_;
  alignas(64) std::atomic<std::size_t> read_position_{0U};
  alignas(64) std::atomic<std::size_t> write_position_{0U};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace vvc::realtime

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mpmc {

namespace detail {

inline constexpr std::size_t kCacheLineSize = 64;

inline bool is_power_of_two(const std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

}  // namespace detail

template <typename T>
class BoundedMPMCQueue {
 public:
  explicit BoundedMPMCQueue(const std::size_t capacity) : capacity_(capacity), mask_(capacity - 1), buffer_(capacity) {
    if (!detail::is_power_of_two(capacity_)) {
      throw std::invalid_argument("capacity must be power of two");
    }

    for (std::size_t i = 0; i < capacity_; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  BoundedMPMCQueue(const BoundedMPMCQueue&) = delete;
  BoundedMPMCQueue& operator=(const BoundedMPMCQueue&) = delete;

  ~BoundedMPMCQueue() {
    drain_remaining();
  }

  bool try_enqueue(const T& value) {
    return emplace_impl(value);
  }

  bool try_enqueue(T&& value) {
    return emplace_impl(std::move(value));
  }

  template <typename... Args>
  bool try_emplace(Args&&... args) {
    return emplace_impl(std::forward<Args>(args)...);
  }

  bool try_dequeue(T& out) {
    std::size_t pos = dequeue_pos_.value.load(std::memory_order_relaxed);
    Backoff backoff{};

    for (;;) {
      Cell& cell = buffer_[pos & mask_];
      const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
      const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

      if (diff == 0) {
        if (dequeue_pos_.value.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
          move_out_cell(cell, out);
          cell.sequence.store(pos + capacity_, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = dequeue_pos_.value.load(std::memory_order_relaxed);
      }

      backoff.pause();
    }
  }

  std::size_t capacity() const noexcept {
    return capacity_;
  }

  std::size_t approx_size() const noexcept {
    const std::size_t enq = enqueue_pos_.value.load(std::memory_order_relaxed);
    const std::size_t deq = dequeue_pos_.value.load(std::memory_order_relaxed);
    return enq - deq;
  }

 private:
  struct Backoff {
    void pause() noexcept {
      if (count < 16) {
        detail::cpu_relax();
        ++count;
      }
    }
    std::uint32_t count{0};
  };

  struct alignas(detail::kCacheLineSize) Cell {
    std::atomic<std::size_t> sequence{};
    alignas(T) std::byte storage[sizeof(T)]{};
  };

  struct alignas(detail::kCacheLineSize) AlignedAtomicSizeT {
    std::atomic<std::size_t> value{0};
    std::byte padding[detail::kCacheLineSize - sizeof(std::atomic<std::size_t>)]{};
  };

  template <typename... Args>
  bool emplace_impl(Args&&... args) {
    std::size_t pos = enqueue_pos_.value.load(std::memory_order_relaxed);
    Backoff backoff{};

    for (;;) {
      Cell& cell = buffer_[pos & mask_];
      const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
      const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

      if (diff == 0) {
        if (enqueue_pos_.value.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
          construct_in_cell(cell, std::forward<Args>(args)...);
          cell.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = enqueue_pos_.value.load(std::memory_order_relaxed);
      }

      backoff.pause();
    }
  }

  template <typename... Args>
  static void construct_in_cell(Cell& cell, Args&&... args) {
    new (cell.storage) T(std::forward<Args>(args)...);
  }

  static void move_out_cell(Cell& cell, T& out) {
    T* ptr = std::launder(reinterpret_cast<T*>(cell.storage));
    out = std::move(*ptr);

    if constexpr (!std::is_trivially_destructible_v<T>) {
      ptr->~T();
    }
  }

  void drain_remaining() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      const std::size_t deq = dequeue_pos_.value.load(std::memory_order_relaxed);
      const std::size_t enq = enqueue_pos_.value.load(std::memory_order_relaxed);

      for (std::size_t pos = deq; pos < enq; ++pos) {
        Cell& cell = buffer_[pos & mask_];
        const std::size_t seq = cell.sequence.load(std::memory_order_relaxed);
        if (seq == pos + 1) {
          T* ptr = std::launder(reinterpret_cast<T*>(cell.storage));
          ptr->~T();
        }
      }
    }
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::vector<Cell> buffer_;
  AlignedAtomicSizeT enqueue_pos_{};
  AlignedAtomicSizeT dequeue_pos_{};
};

}  // namespace mpmc

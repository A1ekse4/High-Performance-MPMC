#include "mpmc_bounded_queue.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void test_single_thread_fifo() {
  mpmc::BoundedMPMCQueue<int> queue(1024);

  for (int i = 0; i < 1000; ++i) {
    const bool ok = queue.try_enqueue(i);
    assert(ok);
  }

  int value = -1;
  for (int i = 0; i < 1000; ++i) {
    const bool ok = queue.try_dequeue(value);
    assert(ok);
    assert(value == i);
  }

  assert(!queue.try_dequeue(value));
}

void test_mpmc_no_loss_no_duplicates() {
  constexpr std::size_t producers = 4;
  constexpr std::size_t consumers = 4;
  constexpr std::size_t items_per_producer = 100'000;
  constexpr std::size_t total_items = producers * items_per_producer;

  mpmc::BoundedMPMCQueue<std::uint64_t> queue(1u << 15u);
  std::atomic<std::size_t> consumed{0};
  std::vector<std::atomic<std::uint8_t>> seen(total_items);
  for (auto& s : seen) {
    s.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> producer_threads;
  producer_threads.reserve(producers);

  for (std::size_t p = 0; p < producers; ++p) {
    producer_threads.emplace_back([p, &queue]() {
      const std::uint64_t base = static_cast<std::uint64_t>(p) * items_per_producer;
      for (std::uint64_t i = 0; i < items_per_producer; ++i) {
        const std::uint64_t id = base + i;
        while (!queue.try_enqueue(id)) {
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::thread> consumer_threads;
  consumer_threads.reserve(consumers);
  for (std::size_t c = 0; c < consumers; ++c) {
    consumer_threads.emplace_back([&queue, &seen, &consumed]() {
      std::uint64_t item = 0;
      while (consumed.load(std::memory_order_relaxed) < total_items) {
        if (queue.try_dequeue(item)) {
          const auto previous = seen[item].fetch_add(1, std::memory_order_relaxed);
          assert(previous == 0);
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& t : producer_threads) {
    t.join();
  }
  for (auto& t : consumer_threads) {
    t.join();
  }

  assert(consumed.load(std::memory_order_relaxed) == total_items);
  for (const auto& s : seen) {
    assert(s.load(std::memory_order_relaxed) == 1);
  }
}

void test_progress_under_contention() {
  constexpr std::size_t producers = 8;
  constexpr std::size_t consumers = 8;

  mpmc::BoundedMPMCQueue<std::uint64_t> queue(1u << 12u);
  std::atomic<bool> run{true};
  std::atomic<std::uint64_t> pushed{0};
  std::atomic<std::uint64_t> popped{0};

  std::vector<std::thread> threads;
  threads.reserve(producers + consumers);

  for (std::size_t i = 0; i < producers; ++i) {
    threads.emplace_back([&]() {
      std::uint64_t local = 0;
      while (run.load(std::memory_order_relaxed)) {
        if (queue.try_enqueue(local)) {
          ++local;
          pushed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (std::size_t i = 0; i < consumers; ++i) {
    threads.emplace_back([&]() {
      std::uint64_t value = 0;
      while (run.load(std::memory_order_relaxed)) {
        if (queue.try_dequeue(value)) {
          popped.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  run.store(false, std::memory_order_relaxed);

  for (auto& t : threads) {
    t.join();
  }

  assert(pushed.load(std::memory_order_relaxed) > 1000);
  assert(popped.load(std::memory_order_relaxed) > 1000);
}

}  // namespace

int main() {
  test_single_thread_fifo();
  test_mpmc_no_loss_no_duplicates();
  test_progress_under_contention();

  std::cout << "All MPMC queue tests passed\n";
  return 0;
}

#include "mpmc_bounded_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Result {
  std::size_t producers{};
  std::size_t consumers{};
  std::size_t capacity{};
  double duration_seconds{};
  std::uint64_t enq_ops{};
  std::uint64_t deq_ops{};
};

Result run_case(const std::size_t producers, const std::size_t consumers, const std::size_t capacity,
                const std::chrono::seconds duration) {
  // создаем очередь
  mpmc::BoundedMPMCQueue<std::uint64_t> queue(capacity);
  // атомарные флаги для старта и остановки
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};

  // создаем вектор потоков
  std::vector<std::thread> threads;
  // резервируем общую память для потоков
  threads.reserve(producers + consumers);

  // вектор для подсчета успешных операций записи и чтения
  std::vector<std::uint64_t> producer_ops(producers, 0);
  std::vector<std::uint64_t> consumer_ops(consumers, 0);

  // создаем producers потоков для записи
  for (std::size_t p = 0; p < producers; ++p) {
    // создаем поток для записи
    threads.emplace_back([&, p]() {
      // ждем старта
      // acquire - синхронизирует с другими потоками, которые могут писать в эту ячейку
      while (!start.load(std::memory_order_acquire)) {
        // уступаем квант времени
        std::this_thread::yield();
      }

      // значение для записи
      std::uint64_t value = 0;
      // счетчик успешных операций записи
      std::uint64_t count = 0;
      // пока stop=false, пытаемся записать значение
      while (!stop.load(std::memory_order_relaxed)) {
        // если удалось записать значение, то увеличиваем счетчик и значение
        if (queue.try_enqueue(value)) {
          ++value;
          ++count;
        } else {
          // уступаем квант времени
          std::this_thread::yield();
        }
      }
      // записываем результат в свой индекс вектора
      producer_ops[p] = count;
    });
  }

  // создаем consumers потоков для чтения
  for (std::size_t c = 0; c < consumers; ++c) {
    // создаем поток для чтения
    threads.emplace_back([&, c]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      std::uint64_t value = 0;
      std::uint64_t count = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        if (queue.try_dequeue(value)) {
          ++count;
        } else {
          std::this_thread::yield();
        }
      }
      consumer_ops[c] = count;
    });
  }

  // начинаем измерение времени
  const auto begin = std::chrono::steady_clock::now();
  // устанавливаем флаг старта
  start.store(true, std::memory_order_release);
  // ждем duration секунд(время теста)
  std::this_thread::sleep_for(duration);
  // устанавливаем флаг остановки
  stop.store(true, std::memory_order_release);

  // ждем завершения всех потоков
  for (auto& t : threads) {
    t.join();
  }
  // завершаем измерение времени
  const auto end = std::chrono::steady_clock::now();

  // подсчитываем общее количество успешных операций записи и чтения
  std::uint64_t enq_total = 0;
  std::uint64_t deq_total = 0;
  for (const auto n : producer_ops) {
    enq_total += n;
  }
  for (const auto n : consumer_ops) {
    deq_total += n;
  }

  return Result{
      .producers = producers,
      .consumers = consumers,
      .capacity = capacity,
      .duration_seconds = std::chrono::duration<double>(end - begin).count(),
      .enq_ops = enq_total,
      .deq_ops = deq_total,
  };
}

std::vector<std::size_t> make_thread_matrix() {
  const std::size_t hw = std::max<std::size_t>(2, std::thread::hardware_concurrency());
  std::vector<std::size_t> matrix;
  for (std::size_t v = 1; v <= hw; v *= 2) {
    matrix.push_back(v);
  }
  if (matrix.back() != hw) {
    matrix.push_back(hw);
  }
  return matrix;
}

}

int main(int argc, char** argv) {
  if (argc == 5) {
    const std::size_t producers = static_cast<std::size_t>(std::stoull(argv[1]));
    const std::size_t consumers = static_cast<std::size_t>(std::stoull(argv[2]));
    const std::size_t capacity = static_cast<std::size_t>(std::stoull(argv[3]));
    const std::size_t seconds = static_cast<std::size_t>(std::stoull(argv[4]));

    const Result r = run_case(producers, consumers, capacity, std::chrono::seconds(seconds));
    const double effective_ops = static_cast<double>(std::min(r.enq_ops, r.deq_ops)) / r.duration_seconds;
    std::cout << "producers,consumers,capacity,enq_ops,deq_ops,effective_ops_per_sec\n";
    std::cout << r.producers << "," << r.consumers << "," << r.capacity << "," << r.enq_ops << "," << r.deq_ops
              << "," << std::fixed << std::setprecision(2) << effective_ops << "\n";
    return 0;
  }

  const auto matrix = make_thread_matrix();
  const std::vector<std::size_t> capacities = {1u << 12u, 1u << 15u, 1u << 18u};
  const auto duration = std::chrono::seconds(2);

  std::cout << "producers,consumers,capacity,enq_ops,deq_ops,effective_ops_per_sec\n";
  for (const auto producers : matrix) {
    for (const auto consumers : matrix) {
      for (const auto capacity : capacities) {
        const Result r = run_case(producers, consumers, capacity, duration);
        const double effective_ops = static_cast<double>(std::min(r.enq_ops, r.deq_ops)) / r.duration_seconds;

        std::cout << r.producers << "," << r.consumers << "," << r.capacity << "," << r.enq_ops << "," << r.deq_ops
                  << "," << std::fixed << std::setprecision(2) << effective_ops << "\n";
      }
    }
  }

  return 0;
}

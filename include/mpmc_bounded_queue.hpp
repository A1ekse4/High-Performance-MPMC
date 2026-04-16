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

// размер cache line в байтах
inline constexpr std::size_t kCacheLineSize = 64;

// проверяет, является ли значение степенью двойки и равно ли оно нулю
inline bool is_power_of_two(const std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

// функция для паузы в spin-loop
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
    // проверяем, что capacity является степенью двойки
    if (!detail::is_power_of_two(capacity_)) {
      throw std::invalid_argument("capacity must be power of two");
    }

    // инициализируем sequence для каждой ячейки
    for (std::size_t i = 0; i < capacity_; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }
  // удаляем копирование и присваивание
  BoundedMPMCQueue(const BoundedMPMCQueue&) = delete;
  BoundedMPMCQueue& operator=(const BoundedMPMCQueue&) = delete;

  ~BoundedMPMCQueue() {
    drain_remaining();
  }

  // перегрузка для const lvalue и rvalue
  //  перенаправляем значение в emplace_impl
  bool try_enqueue(const T& value) {
    return emplace_impl(value);
  }
  // перенаправляем значение в emplace_impl без копирования
  bool try_enqueue(T&& value) {
    return emplace_impl(std::move(value));
  }
  // перенаправляем значение в emplace_impl без копирования
  // сохраняем типы аргументов
  template <typename... Args>
  bool try_emplace(Args&&... args) {
    return emplace_impl(std::forward<Args>(args)...);
  }

  bool try_dequeue(T& out) {
    // загружаем текущую позицию dequeue(глобальный индекс)
    std::size_t pos = dequeue_pos_.value.load(std::memory_order_relaxed);
    // инициализируем backoff для spin-loop
    // цикл не крутится бесконечно, иногда делает pause()
    Backoff backoff{};

    // бесконечный цикл
    for (;;) {
      // получаем текущую позицию в кольце
      Cell& cell = buffer_[pos & mask_];
      // загружаем такущее состояние ячейки
      // acquire - синхронизирует с другими потоками, которые могут писать в эту ячейку
      // получаем гаранию, что объект уже полностю сконструирован
      const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
      // вычисляем разницу между текущей позицией и позицией в ячейке
      const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      // если разница равна 0, то готова к чтению

      if (diff == 0) {
        // проверяем, что текущая позиция не изменилась другим потоком и обновляем её
        if (dequeue_pos_.value.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
          // перемещаем значение из ячейки в out
          move_out_cell(cell, out);
          // обновляем состояние ячейки
          // release - синхронизирует с другими потоками, которые могут читать из этой ячейки
          // оповещаем все потоки,что ячейка снова свободна
          cell.sequence.store(pos + capacity_, std::memory_order_release);
          // возвращаем true, если удалось прочитать значение
          return true;
        }
      } else if (diff < 0) {
        // producer еще не записал значение в ячейку
        return false;
      } else {
        // текщий pos уже неактуален, загружаем новую
        pos = dequeue_pos_.value.load(std::memory_order_relaxed);
      }
      // делаем паузу в spin-loop, чтобы избежать busy waiting
      backoff.pause();
    }
  }

  // возвращаем capacity
  std::size_t capacity() const noexcept {
    return capacity_;
  }

  // возвращаем приблизительный размер очереди
  // approx, потому что нет синхронизации и размеры могут немного отличаться
  std::size_t approx_size() const noexcept {
    const std::size_t enq = enqueue_pos_.value.load(std::memory_order_relaxed);
    const std::size_t deq = dequeue_pos_.value.load(std::memory_order_relaxed);
    return enq - deq;
  }

  // пауза между попытками чтения/записи
  // избавляет от лишней нагрузки на CPU
  private:
  struct Backoff {
    void pause() noexcept {
      if (count < 16) {
        detail::cpu_relax();
        ++count;
      }
    }
    // инициализируем счетчик пауз
    std::uint32_t count{0};
  };

  // alignas - выравнивание на границу объекта T, чтобы не было false sharing
  struct alignas(detail::kCacheLineSize) Cell {
    // атомарный номер состояния ячейки
    std::atomic<std::size_t> sequence{};
    // выравниваем storage на границу объекта T
    alignas(T) std::byte storage[sizeof(T)]{};
  };

  struct alignas(detail::kCacheLineSize) AlignedAtomicSizeT {
    // сам атомарный счетчик(enqueue_pos_ и dequeue_pos_)
    std::atomic<std::size_t> value{0};
    // выравниваем на границу до полного cache line, чтобы атомики en и de не трогали друг друга
    std::byte padding[detail::kCacheLineSize - sizeof(std::atomic<std::size_t>)]{};
  };

  template <typename... Args>
  bool emplace_impl(Args&&... args) {
    // загружаем текущую позицию enqueue
    std::size_t pos = enqueue_pos_.value.load(std::memory_order_relaxed);
    Backoff backoff{};

    for (;;) {
      // получаем текущую ячейку
      Cell& cell = buffer_[pos & mask_];
      // загружаем текущее состояние ячейки
      // acquire - синхронизирует с другими потоками, которые могут писать в эту ячейку
      const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
      // вычисляем разницу между текущей позицией и позицией в ячейке
      const std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      // если разница равна 0, то готова к записи
      if (diff == 0) {
        // проверяем, что текущая позиция не изменилась другим потоком и обновляем её
        if (enqueue_pos_.value.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
          // конструируем объект в ячейке
          construct_in_cell(cell, std::forward<Args>(args)...);
          // обновляем состояние ячейки
          // release - синхронизирует с другими потоками, которые могут читать из этой ячейки
          // оповещаем все потоки, что ячейка снова свободна
          cell.sequence.store(pos + 1, std::memory_order_release);
          // возвращаем true, если удалось записать значение
          return true;
        }
      } else if (diff < 0) {
        // очередь заполнена
        return false;
      } else {
        // кто-то еще записал значение в ячейку, загружаем новую позицию
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

    // сами вызываем деструктор, если объект нетривиально деструируемый
    if constexpr (!std::is_trivially_destructible_v<T>) {
      ptr->~T();
    }
  }

  // освобождаем все ячейки, которые не были прочитаны
  void drain_remaining() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      const std::size_t deq = dequeue_pos_.value.load(std::memory_order_relaxed);
      const std::size_t enq = enqueue_pos_.value.load(std::memory_order_relaxed);

      for (std::size_t pos = deq; pos < enq; ++pos) {
        Cell& cell = buffer_[pos & mask_];
        const std::size_t seq = cell.sequence.load(std::memory_order_relaxed);
        if (seq == pos + 1) {
          // если элемент действтельно живет и готов к чтению, то получаем указатель
          T* ptr = std::launder(reinterpret_cast<T*>(cell.storage));
          // в ручную вызываем деструктор
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

}

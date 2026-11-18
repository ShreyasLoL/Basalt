#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace basalt::common {

// Bounded single-producer/single-consumer queue. Each endpoint must have exactly
// one owning thread; counters are separated to reduce producer/consumer cache traffic.
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(const std::size_t capacity) : slots_(capacity), capacity_(capacity) {
        if (capacity_ == 0) throw std::invalid_argument("SPSC ring capacity must be positive");
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    [[nodiscard]] bool try_push(T&& value) {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        if (head - tail >= capacity_) return false;
        slots_[head % capacity_].emplace(std::move(value));
        head_.value.store(head + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(const T& value) {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        if (head - tail >= capacity_) return false;
        slots_[head % capacity_].emplace(value);
        head_.value.store(head + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        const auto head = head_.value.load(std::memory_order_acquire);
        if (head == tail) return std::nullopt;
        auto& slot = slots_[tail % capacity_];
        std::optional<T> value(std::move(*slot));
        slot.reset();
        tail_.value.store(tail + 1U, std::memory_order_release);
        return value;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.value.load(std::memory_order_acquire) == tail_.value.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const auto head = head_.value.load(std::memory_order_acquire);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        return head - tail >= capacity_;
    }

    // Concurrent observations are approximate; only the endpoints should use this
    // for diagnostics or waiting predicates, never as a substitute for try_push/pop.
    [[nodiscard]] std::size_t size() const noexcept {
        const auto head = head_.value.load(std::memory_order_acquire);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        return (head - tail) > capacity_ ? capacity_ : head - tail;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct alignas(64) Counter {
        std::atomic<std::size_t> value{0};
    };

    std::vector<std::optional<T>> slots_;
    const std::size_t capacity_;
    Counter head_{};
    Counter tail_{};
};

} // namespace basalt::common

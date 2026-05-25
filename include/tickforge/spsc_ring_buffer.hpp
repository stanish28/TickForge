#pragma once

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace tickforge {

namespace detail {
constexpr std::size_t cache_line_size = 64;

struct alignas(cache_line_size) AtomicIndex {
    std::atomic<std::size_t> value{0};
};
}  // namespace detail

template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t capacity)
        : buffer_(capacity + 1), capacity_with_sentinel_(capacity + 1) {
        if (capacity == 0) {
            throw std::invalid_argument("SpscRingBuffer capacity must be greater than zero");
        }
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    bool push(const T& item) {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        const auto next_tail = increment(tail);

        // acquire pairs with the consumer's release store to head_. This is SPSC only:
        // one producer owns tail_ and one consumer owns head_; multiple producers or
        // consumers would race on the same index and need a different algorithm.
        if (next_tail == head_.value.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[tail] = item;
        tail_.value.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const auto head = head_.value.load(std::memory_order_relaxed);

        // acquire observes the producer's release store to tail_, making the written
        // element visible before this consumer reads it.
        if (head == tail_.value.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[head];
        head_.value.store(increment(head), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const {
        return head_.value.load(std::memory_order_acquire) ==
               tail_.value.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const {
        const auto tail = tail_.value.load(std::memory_order_acquire);
        return increment(tail) == head_.value.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_with_sentinel_ - 1;
    }

private:
    [[nodiscard]] std::size_t increment(std::size_t index) const noexcept {
        const auto next = index + 1;
        return next == capacity_with_sentinel_ ? 0 : next;
    }

    std::vector<T> buffer_;
    const std::size_t capacity_with_sentinel_;

    detail::AtomicIndex head_;
    detail::AtomicIndex tail_;
};

}  // namespace tickforge

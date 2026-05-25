#include "tickforge/spsc_ring_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <thread>

#include <gtest/gtest.h>

namespace tickforge {

TEST(SpscRingBufferTest, PreservesFifoOrder) {
    SpscRingBuffer<int> ring(4);

    EXPECT_TRUE(ring.push(1));
    EXPECT_TRUE(ring.push(2));
    EXPECT_TRUE(ring.push(3));

    int value = 0;
    ASSERT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 1);
    ASSERT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 3);
}

TEST(SpscRingBufferTest, HandlesFullAndEmptyStates) {
    SpscRingBuffer<int> ring(2);
    int value = 0;

    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.full());
    EXPECT_FALSE(ring.pop(value));

    EXPECT_TRUE(ring.push(10));
    EXPECT_TRUE(ring.push(20));
    EXPECT_TRUE(ring.full());
    EXPECT_FALSE(ring.push(30));

    EXPECT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(ring.push(30));

    EXPECT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 20);
    EXPECT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 30);
    EXPECT_FALSE(ring.pop(value));
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingBufferTest, HandlesManyEvents) {
    constexpr std::size_t event_count = 100'000;
    SpscRingBuffer<std::size_t> ring(1024);

    for (std::size_t i = 0; i < event_count; ++i) {
        ASSERT_TRUE(ring.push(i));

        std::size_t value = 0;
        ASSERT_TRUE(ring.pop(value));
        ASSERT_EQ(value, i);
    }

    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingBufferTest, ConcurrentProducerConsumer) {
    constexpr std::size_t event_count = 100'000;
    SpscRingBuffer<std::size_t> ring(1024);
    std::atomic<bool> producer_done{false};

    std::size_t received = 0;
    bool fifo_ok = true;

    std::thread producer([&] {
        for (std::size_t i = 0; i < event_count; ++i) {
            while (!ring.push(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::size_t value = 0;
        while (!producer_done.load(std::memory_order_acquire) || !ring.empty()) {
            if (!ring.pop(value)) {
                std::this_thread::yield();
                continue;
            }

            if (value != received) {
                fifo_ok = false;
            }
            ++received;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(fifo_ok);
    EXPECT_EQ(received, event_count);
    EXPECT_TRUE(ring.empty());
}

}  // namespace tickforge

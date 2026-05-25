#include "tickforge/benchmark_runner.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

#include "tickforge/spsc_ring_buffer.hpp"

namespace tickforge::bench {
namespace {

using Clock = std::chrono::steady_clock;
std::atomic<std::uint64_t> ring_sink{0};

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

BenchmarkResult make_result(std::string name, std::size_t operations, std::uint64_t ns) {
    const auto ns_per_op = operations == 0 ? 0.0 : static_cast<double>(ns) / operations;
    const auto ops_per_sec =
        ns == 0 ? 0.0 : static_cast<double>(operations) * 1'000'000'000.0 / ns;
    return BenchmarkResult{
        .name = std::move(name),
        .operations = operations,
        .elapsed_ns = ns,
        .ns_per_op = ns_per_op,
        .ops_per_sec = ops_per_sec,
    };
}

bool same_event(const MarketEvent& lhs, const MarketEvent& rhs) {
    return lhs.timestamp_ns == rhs.timestamp_ns && lhs.type == rhs.type &&
           lhs.order_id == rhs.order_id && lhs.side == rhs.side && lhs.price == rhs.price &&
           lhs.quantity == rhs.quantity;
}

std::uint64_t checksum_event(const MarketEvent& event) {
    auto value = event.timestamp_ns ^ (event.order_id << 1);
    value ^= static_cast<std::uint64_t>(event.price) << 7;
    value ^= static_cast<std::uint64_t>(event.quantity) << 13;
    value ^= static_cast<std::uint64_t>(event.type) << 19;
    value ^= static_cast<std::uint64_t>(event.side) << 23;
    return value;
}

QueueHandoffResult make_queue_result(std::string implementation,
                                     std::size_t events,
                                     std::uint64_t ns) {
    return QueueHandoffResult{
        .implementation = std::move(implementation),
        .events = events,
        .elapsed_ns = ns,
        .events_per_sec =
            ns == 0 ? 0.0 : static_cast<double>(events) * 1'000'000'000.0 / ns,
    };
}

}  // namespace

BenchmarkResult run_ring_buffer_benchmark(std::size_t event_count) {
    const auto events = generate_benchmark_events(event_count);
    SpscRingBuffer<MarketEvent> ring(1024);
    MarketEvent out;
    std::uint64_t checksum = 0;

    const auto start = Clock::now();
    for (const auto& event : events) {
        (void)ring.push(event);
        (void)ring.pop(out);
        checksum += out.order_id;
    }
    const auto end = Clock::now();

    ring_sink.store(checksum, std::memory_order_relaxed);
    return make_result("spsc.push_pop", events.size(), elapsed_ns(start, end));
}

QueueHandoffBenchmark run_queue_handoff_benchmark(std::size_t event_count) {
    const auto events = generate_benchmark_events(event_count);

    // This is a microbenchmark for one producer and one consumer moving already-built
    // MarketEvent objects. It is not a universal performance claim for every queue or
    // workload; it isolates handoff overhead under TickForge-like replay traffic.
    auto run_spsc = [&events] {
        SpscRingBuffer<MarketEvent> ring(65'536);
        std::atomic<bool> producer_done{false};
        std::size_t consumed = 0;
        std::uint64_t checksum = 0;
        bool fifo_ok = true;

        const auto start = Clock::now();
        std::thread producer([&] {
            for (const auto& event : events) {
                while (!ring.push(event)) {
                    std::this_thread::yield();
                }
            }
            producer_done.store(true, std::memory_order_release);
        });

        std::thread consumer([&] {
            MarketEvent event;
            while (!producer_done.load(std::memory_order_acquire) || !ring.empty()) {
                if (!ring.pop(event)) {
                    std::this_thread::yield();
                    continue;
                }

                if (consumed >= events.size() || !same_event(event, events[consumed])) {
                    fifo_ok = false;
                }
                checksum += checksum_event(event);
                ++consumed;
            }
        });

        producer.join();
        consumer.join();
        const auto end = Clock::now();

        if (consumed != events.size() || !fifo_ok) {
            throw std::runtime_error("SPSC handoff benchmark failed FIFO/count validation");
        }

        ring_sink.store(checksum, std::memory_order_relaxed);
        return make_queue_result("SPSC Ring Buffer", events.size(), elapsed_ns(start, end));
    };

    auto run_mutex_queue = [&events] {
        std::queue<MarketEvent> queue;
        std::mutex mutex;
        std::condition_variable cv;
        bool producer_done = false;
        std::size_t consumed = 0;
        std::uint64_t checksum = 0;
        bool fifo_ok = true;

        const auto start = Clock::now();
        std::thread producer([&] {
            for (const auto& event : events) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    queue.push(event);
                }
                cv.notify_one();
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                producer_done = true;
            }
            cv.notify_one();
        });

        std::thread consumer([&] {
            for (;;) {
                MarketEvent event;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&] { return producer_done || !queue.empty(); });
                    if (queue.empty()) {
                        if (producer_done) {
                            break;
                        }
                        continue;
                    }
                    event = queue.front();
                    queue.pop();
                }

                if (consumed >= events.size() || !same_event(event, events[consumed])) {
                    fifo_ok = false;
                }
                checksum += checksum_event(event);
                ++consumed;
            }
        });

        producer.join();
        consumer.join();
        const auto end = Clock::now();

        if (consumed != events.size() || !fifo_ok) {
            throw std::runtime_error("mutex queue benchmark failed FIFO/count validation");
        }

        ring_sink.store(checksum, std::memory_order_relaxed);
        return make_queue_result("Mutex Queue", events.size(), elapsed_ns(start, end));
    };

    auto spsc = run_spsc();
    auto mutex_queue = run_mutex_queue();
    const auto speedup = mutex_queue.events_per_sec == 0.0
                             ? 0.0
                             : spsc.events_per_sec / mutex_queue.events_per_sec;

    return QueueHandoffBenchmark{
        .spsc = std::move(spsc),
        .mutex_queue = std::move(mutex_queue),
        .speedup = speedup,
    };
}

}  // namespace tickforge::bench

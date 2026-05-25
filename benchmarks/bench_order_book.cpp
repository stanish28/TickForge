#include "tickforge/benchmark_runner.hpp"

#include <atomic>
#include <chrono>

#include "tickforge/order_book.hpp"

namespace tickforge::bench {
namespace {

using Clock = std::chrono::steady_clock;
std::atomic<std::int64_t> order_book_sink{0};

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

}  // namespace

BenchmarkResult run_order_book_benchmark(std::size_t event_count) {
    const auto events = generate_benchmark_events(event_count);
    OrderBook book;

    const auto start = Clock::now();
    for (const auto& event : events) {
        (void)book.apply_event(event);
    }
    const auto end = Clock::now();

    order_book_sink.store(book.total_depth(Side::Buy) + book.total_depth(Side::Sell),
                          std::memory_order_relaxed);

    return make_result("order_book.apply_event", events.size(), elapsed_ns(start, end));
}

}  // namespace tickforge::bench

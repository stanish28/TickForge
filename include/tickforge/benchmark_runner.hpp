#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "tickforge/market_event.hpp"

namespace tickforge::bench {

struct BenchmarkResult {
    std::string name;
    std::size_t operations{};
    std::uint64_t elapsed_ns{};
    double ns_per_op{};
    double ops_per_sec{};
};

struct QueueHandoffResult {
    std::string implementation;
    std::size_t events{};
    std::uint64_t elapsed_ns{};
    double events_per_sec{};
};

struct QueueHandoffBenchmark {
    QueueHandoffResult spsc;
    QueueHandoffResult mutex_queue;
    double speedup{};
};

std::vector<MarketEvent> generate_benchmark_events(std::size_t count);

BenchmarkResult run_order_book_benchmark(std::size_t event_count);
BenchmarkResult run_ring_buffer_benchmark(std::size_t event_count);
BenchmarkResult run_replay_benchmark(std::size_t event_count);
QueueHandoffBenchmark run_queue_handoff_benchmark(std::size_t event_count);

void print_benchmark_results(std::ostream& os, const std::vector<BenchmarkResult>& results);
void print_queue_handoff_benchmark(std::ostream& os, const QueueHandoffBenchmark& result);

}  // namespace tickforge::bench

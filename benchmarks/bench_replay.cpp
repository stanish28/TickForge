#include "tickforge/benchmark_runner.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>
#include <vector>

#include "tickforge/replay_engine.hpp"

namespace tickforge::bench {
namespace {

using Clock = std::chrono::steady_clock;
std::atomic<std::int64_t> replay_sink{0};

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

bool parse_size(std::string_view value, std::size_t& out) {
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

}  // namespace

BenchmarkResult run_replay_benchmark(std::size_t event_count) {
    const auto events = generate_benchmark_events(event_count);
    ReplayEngine engine(ReplayConfig{.mode = ReplayMode::Max});

    const auto start = Clock::now();
    const auto summary = engine.replay(events);
    const auto end = Clock::now();

    replay_sink.store(summary.final_total_buy_depth + summary.final_total_sell_depth,
                      std::memory_order_relaxed);

    return make_result("replay.max_pipeline", events.size(), elapsed_ns(start, end));
}

}  // namespace tickforge::bench

int main(int argc, char** argv) {
    std::size_t event_count = 200'000;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--events") {
            if (i + 1 >= argc || !tickforge::bench::parse_size(argv[++i], event_count)) {
                std::cerr << "Usage: ./tickforge_bench [--events <count>]\n";
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ./tickforge_bench [--events <count>]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            std::cerr << "Usage: ./tickforge_bench [--events <count>]\n";
            return 1;
        }
    }

    const std::vector<tickforge::bench::BenchmarkResult> results = {
        tickforge::bench::run_order_book_benchmark(event_count),
        tickforge::bench::run_ring_buffer_benchmark(event_count),
        tickforge::bench::run_replay_benchmark(event_count),
    };

    tickforge::bench::print_benchmark_results(std::cout, results);
    tickforge::bench::print_queue_handoff_benchmark(
        std::cout, tickforge::bench::run_queue_handoff_benchmark(event_count));
    return 0;
}

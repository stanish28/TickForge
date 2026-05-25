#include "tickforge/benchmark_runner.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace tickforge::bench {
namespace {

struct ActiveOrder {
    std::uint64_t order_id{};
    Side side{Side::Buy};
    std::int64_t price{};
    std::int64_t quantity{};
};

}  // namespace

std::vector<MarketEvent> generate_benchmark_events(std::size_t count) {
    std::vector<MarketEvent> events;
    events.reserve(count);

    std::vector<ActiveOrder> active;
    active.reserve(std::min<std::size_t>(count, 100'000));

    std::uint64_t timestamp_ns = 1'000'000'000;
    std::uint64_t next_order_id = 1;

    auto append_add = [&](std::size_t i) {
        const auto side = i % 2 == 0 ? Side::Buy : Side::Sell;
        const auto price = side == Side::Buy
                               ? 10'000 - static_cast<std::int64_t>(i % 100)
                               : 10'005 + static_cast<std::int64_t>(i % 100);
        const auto quantity = 10 + static_cast<std::int64_t>(i % 50);
        const auto order_id = next_order_id++;

        events.push_back(MarketEvent{
            .timestamp_ns = timestamp_ns,
            .type = EventType::Add,
            .order_id = order_id,
            .side = side,
            .price = price,
            .quantity = quantity,
        });
        active.push_back(ActiveOrder{
            .order_id = order_id,
            .side = side,
            .price = price,
            .quantity = quantity,
        });
        timestamp_ns += 10;
    };

    for (std::size_t i = 0; i < count; ++i) {
        if (active.empty() || i % 10 < 5) {
            append_add(i);
            continue;
        }

        const auto index = (i * 17) % active.size();
        auto& order = active[index];

        if (i % 10 < 7) {
            const auto price_delta = static_cast<std::int64_t>(i % 5) - 2;
            const auto candidate_price = std::max<std::int64_t>(1, order.price + price_delta);
            const auto new_price = order.side == Side::Buy
                                       ? std::min<std::int64_t>(10'000, candidate_price)
                                       : std::max<std::int64_t>(10'005, candidate_price);
            const auto new_quantity = 5 + static_cast<std::int64_t>(i % 75);

            events.push_back(MarketEvent{
                .timestamp_ns = timestamp_ns,
                .type = EventType::Modify,
                .order_id = order.order_id,
                .side = order.side,
                .price = new_price,
                .quantity = new_quantity,
            });

            order.price = new_price;
            order.quantity = new_quantity;
        } else if (i % 10 < 9) {
            const auto traded_quantity =
                std::min(order.quantity, 1 + static_cast<std::int64_t>(i % 7));

            events.push_back(MarketEvent{
                .timestamp_ns = timestamp_ns,
                .type = EventType::Trade,
                .order_id = order.order_id,
                .side = order.side,
                .price = order.price,
                .quantity = traded_quantity,
            });

            order.quantity -= traded_quantity;
            if (order.quantity == 0) {
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
            }
        } else {
            events.push_back(MarketEvent{
                .timestamp_ns = timestamp_ns,
                .type = EventType::Cancel,
                .order_id = order.order_id,
                .side = order.side,
                .price = order.price,
                .quantity = 0,
            });

            active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
        }

        timestamp_ns += 10;
    }

    return events;
}

void print_benchmark_results(std::ostream& os, const std::vector<BenchmarkResult>& results) {
    os << "TickForge benchmark results\n";
    os << "===========================\n";
    os << std::left << std::setw(32) << "benchmark" << std::right << std::setw(14)
       << "operations" << std::setw(16) << "elapsed_ms" << std::setw(16) << "ns/op"
       << std::setw(18) << "ops/sec" << '\n';

    for (const auto& result : results) {
        os << std::left << std::setw(32) << result.name << std::right << std::setw(14)
           << result.operations << std::setw(16) << std::fixed << std::setprecision(3)
           << static_cast<double>(result.elapsed_ns) / 1'000'000.0 << std::setw(16)
           << std::fixed << std::setprecision(2) << result.ns_per_op << std::setw(18)
           << std::fixed << std::setprecision(2) << result.ops_per_sec << '\n';
    }
}

void print_queue_handoff_benchmark(std::ostream& os, const QueueHandoffBenchmark& result) {
    os << "\nQueue Handoff Benchmark\n";
    os << "------------------------------------------------------------\n";
    os << std::left << std::setw(22) << "Implementation" << std::right << std::setw(12)
       << "Events" << std::setw(16) << "Time(ms)" << std::setw(18) << "Events/sec"
       << '\n';

    auto print_row = [&](const QueueHandoffResult& row) {
        os << std::left << std::setw(22) << row.implementation << std::right
           << std::setw(12) << row.events << std::setw(16) << std::fixed
           << std::setprecision(2)
           << static_cast<double>(row.elapsed_ns) / 1'000'000.0 << std::setw(18)
           << std::fixed << std::setprecision(0) << row.events_per_sec << '\n';
    };

    print_row(result.spsc);
    print_row(result.mutex_queue);
    os << std::left << std::setw(22) << "Speedup" << std::right << std::setw(46)
       << std::fixed << std::setprecision(2) << result.speedup << "x\n";
}

}  // namespace tickforge::bench

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "tickforge/latency_profiler.hpp"
#include "tickforge/market_event.hpp"
#include "tickforge/order_book.hpp"

namespace tickforge {

enum class ReplayMode {
    Max,
    Realtime,
    Speed,
};

struct ReplayConfig {
    ReplayMode mode{ReplayMode::Max};
    double speed_multiplier{1.0};
    std::size_t ring_capacity{65'536};
    // Forwarded to OrderBookConfig before replay starts. See
    // OrderBookConfig::accept_unknown_order_ops for semantics.
    bool accept_unknown_order_ops{false};
};

struct ReplaySummary {
    std::size_t events_processed{};
    std::size_t rejected_events{};
    std::optional<std::int64_t> final_best_bid;
    std::optional<std::int64_t> final_best_ask;
    std::optional<std::int64_t> final_spread;
    std::int64_t final_total_buy_depth{};
    std::int64_t final_total_sell_depth{};
    std::uint64_t elapsed_ns{};
    LatencyStats latency;
};

class ReplayEngine {
public:
    explicit ReplayEngine(ReplayConfig config = {});

    ReplaySummary replay(const std::vector<MarketEvent>& events);

    [[nodiscard]] const OrderBook& order_book() const noexcept;
    [[nodiscard]] const LatencyProfiler& latency_profiler() const noexcept;

private:
    ReplaySummary replay_max_threaded(const std::vector<MarketEvent>& events);
    ReplaySummary replay_paced_single_threaded(const std::vector<MarketEvent>& events);

    ReplayConfig config_;
    OrderBook book_;
    LatencyProfiler profiler_;
};

}  // namespace tickforge

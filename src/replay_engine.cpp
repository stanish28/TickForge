#include "tickforge/replay_engine.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <atomic>

#include "tickforge/spsc_ring_buffer.hpp"

namespace tickforge {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

void sleep_for_timestamp_gap(const MarketEvent& previous,
                             const MarketEvent& current,
                             const ReplayConfig& config) {
    if (config.mode == ReplayMode::Max || current.timestamp_ns <= previous.timestamp_ns) {
        return;
    }

    const auto raw_gap_ns = current.timestamp_ns - previous.timestamp_ns;
    const auto divisor = config.mode == ReplayMode::Speed ? config.speed_multiplier : 1.0;
    const auto adjusted_gap =
        static_cast<std::uint64_t>(static_cast<long double>(raw_gap_ns) / divisor);

    if (adjusted_gap > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(adjusted_gap));
    }
}

}  // namespace

ReplayEngine::ReplayEngine(ReplayConfig config) : config_(config) {
    if (config_.speed_multiplier <= 0.0 || !std::isfinite(config_.speed_multiplier)) {
        throw std::invalid_argument("speed multiplier must be finite and greater than zero");
    }
}

ReplaySummary ReplayEngine::replay(const std::vector<MarketEvent>& events) {
    if (config_.mode == ReplayMode::Max) {
        return replay_max_threaded(events);
    }

    return replay_paced_single_threaded(events);
}

ReplaySummary ReplayEngine::replay_max_threaded(const std::vector<MarketEvent>& events) {
    book_.clear();
    book_.set_config(OrderBookConfig{
        .accept_unknown_order_ops = config_.accept_unknown_order_ops,
    });
    profiler_.clear();
    profiler_.reserve(events.size());

    SpscRingBuffer<MarketEvent> ring(config_.ring_capacity);
    ReplaySummary summary;
    std::atomic<bool> producer_done{false};

    const auto replay_start = Clock::now();

    // SPSC ownership model: the producer thread is the only writer of tail_ and the
    // consumer thread is the only writer of head_. MarketEvent payloads move in file
    // order through the fixed-capacity ring; producer_done is release/acquire so the
    // consumer cannot exit until it observes completion and drains the queue.
    std::thread producer([&] {
        for (const auto& event : events) {
            while (!ring.push(event)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        MarketEvent queued_event;

        while (!producer_done.load(std::memory_order_acquire) || !ring.empty()) {
            if (!ring.pop(queued_event)) {
                std::this_thread::yield();
                continue;
            }

            // Boundary: CSV parsing, producer enqueue time, and producer waiting are
            // excluded. This samples consumer-side event handling after a successful
            // dequeue, which is the order-book mutation hot path.
            const auto event_start = Clock::now();
            const bool applied = book_.apply_event(queued_event);
            const auto event_end = Clock::now();

            profiler_.record(elapsed_ns(event_start, event_end));
            ++summary.events_processed;
            if (!applied) {
                ++summary.rejected_events;
            }
        }
    });

    producer.join();
    consumer.join();

    const auto replay_end = Clock::now();
    summary.elapsed_ns = elapsed_ns(replay_start, replay_end);
    summary.final_best_bid = book_.best_bid();
    summary.final_best_ask = book_.best_ask();
    summary.final_spread = book_.spread();
    summary.final_total_buy_depth = book_.total_depth(Side::Buy);
    summary.final_total_sell_depth = book_.total_depth(Side::Sell);
    summary.latency = profiler_.stats(summary.elapsed_ns);

    return summary;
}

ReplaySummary ReplayEngine::replay_paced_single_threaded(const std::vector<MarketEvent>& events) {
    book_.clear();
    book_.set_config(OrderBookConfig{
        .accept_unknown_order_ops = config_.accept_unknown_order_ops,
    });
    profiler_.clear();
    profiler_.reserve(events.size());

    SpscRingBuffer<MarketEvent> ring(config_.ring_capacity);
    ReplaySummary summary;

    const auto replay_start = Clock::now();

    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i > 0) {
            sleep_for_timestamp_gap(events[i - 1], events[i], config_);
        }

        // Boundary: this timer intentionally excludes CSV parsing and any replay pacing
        // sleep. It captures the hot path of queue handoff plus order-book mutation.
        const auto event_start = Clock::now();

        if (!ring.push(events[i])) {
            throw std::runtime_error("SPSC ring buffer unexpectedly full in single-thread replay");
        }

        MarketEvent queued_event;
        if (!ring.pop(queued_event)) {
            throw std::runtime_error("SPSC ring buffer unexpectedly empty after push");
        }

        const bool applied = book_.apply_event(queued_event);
        const auto event_end = Clock::now();

        profiler_.record(elapsed_ns(event_start, event_end));
        ++summary.events_processed;
        if (!applied) {
            ++summary.rejected_events;
        }
    }

    const auto replay_end = Clock::now();
    summary.elapsed_ns = elapsed_ns(replay_start, replay_end);
    summary.final_best_bid = book_.best_bid();
    summary.final_best_ask = book_.best_ask();
    summary.final_spread = book_.spread();
    summary.final_total_buy_depth = book_.total_depth(Side::Buy);
    summary.final_total_sell_depth = book_.total_depth(Side::Sell);
    summary.latency = profiler_.stats(summary.elapsed_ns);

    return summary;
}

const OrderBook& ReplayEngine::order_book() const noexcept {
    return book_;
}

const LatencyProfiler& ReplayEngine::latency_profiler() const noexcept {
    return profiler_;
}

}  // namespace tickforge

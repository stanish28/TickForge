#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tickforge/market_event.hpp"

namespace tickforge {

struct Order {
    std::uint64_t order_id{};
    Side side{Side::Buy};
    std::int64_t price{};
    std::int64_t quantity{};
};

using PriceLevel = std::pair<std::int64_t, std::int64_t>;

struct OrderBookConfig {
    // When true, CANCEL and TRADE events that reference an unknown order_id
    // fall back to a level-only depth reduction at the event's (side, price)
    // by event.quantity. This mirrors exchange feeds that report depth
    // changes without an engine-owned order universe -- notably LOBSTER
    // replay, where pre-session orders are referenced by id but never
    // ADDed within the captured message stream. Default behavior (strict
    // L3 order tracking) is unchanged when false.
    bool accept_unknown_order_ops{false};
};

class OrderBook {
public:
    OrderBook() = default;
    explicit OrderBook(OrderBookConfig config) noexcept : config_(config) {}

    void set_config(OrderBookConfig config) noexcept { config_ = config; }
    [[nodiscard]] const OrderBookConfig& config() const noexcept { return config_; }

    bool apply_event(const MarketEvent& event);

    [[nodiscard]] std::optional<std::int64_t> best_bid() const;
    [[nodiscard]] std::optional<std::int64_t> best_ask() const;
    [[nodiscard]] std::optional<std::int64_t> spread() const;
    [[nodiscard]] std::int64_t depth_at_price(Side side, std::int64_t price) const;
    [[nodiscard]] std::vector<PriceLevel> top_n(Side side, std::size_t n) const;
    [[nodiscard]] std::int64_t total_depth(Side side) const;
    [[nodiscard]] std::size_t order_count() const noexcept;

    void clear();

private:
    bool add_order(const MarketEvent& event);
    bool cancel_order(const MarketEvent& event);
    bool modify_order(const MarketEvent& event);
    bool trade_order(const MarketEvent& event);

    void add_depth(Side side, std::int64_t price, std::int64_t quantity);
    bool reduce_depth(Side side, std::int64_t price, std::int64_t quantity);

    OrderBookConfig config_{};
    std::unordered_map<std::uint64_t, Order> orders_;

    // std::map keeps deterministic level ordering and simple correctness.
    // A production HFT book may later replace this with flatter, symbol-aware storage.
    std::map<std::int64_t, std::int64_t, std::greater<>> bids_;
    std::map<std::int64_t, std::int64_t, std::less<>> asks_;
};

}  // namespace tickforge

#include "tickforge/order_book.hpp"

#include <algorithm>

namespace tickforge {

bool OrderBook::apply_event(const MarketEvent& event) {
    switch (event.type) {
        case EventType::Add:
            return add_order(event);
        case EventType::Cancel:
            return cancel_order(event);
        case EventType::Modify:
            return modify_order(event);
        case EventType::Trade:
            return trade_order(event);
    }
    return false;
}

std::optional<std::int64_t> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<std::int64_t> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<std::int64_t> OrderBook::spread() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

std::int64_t OrderBook::depth_at_price(Side side, std::int64_t price) const {
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second;
    }

    const auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second;
}

std::vector<PriceLevel> OrderBook::top_n(Side side, std::size_t n) const {
    std::vector<PriceLevel> levels;
    levels.reserve(n);

    if (side == Side::Buy) {
        for (const auto& [price, quantity] : bids_) {
            if (levels.size() == n) {
                break;
            }
            levels.emplace_back(price, quantity);
        }
        return levels;
    }

    for (const auto& [price, quantity] : asks_) {
        if (levels.size() == n) {
            break;
        }
        levels.emplace_back(price, quantity);
    }
    return levels;
}

std::int64_t OrderBook::total_depth(Side side) const {
    std::int64_t total = 0;

    if (side == Side::Buy) {
        for (const auto& [_, quantity] : bids_) {
            total += quantity;
        }
    } else {
        for (const auto& [_, quantity] : asks_) {
            total += quantity;
        }
    }

    return total;
}

std::size_t OrderBook::order_count() const noexcept {
    return orders_.size();
}

void OrderBook::clear() {
    orders_.clear();
    bids_.clear();
    asks_.clear();
}

bool OrderBook::add_order(const MarketEvent& event) {
    if (event.price < 0 || event.quantity <= 0) {
        return false;
    }

    const auto [_, inserted] = orders_.emplace(
        event.order_id,
        Order{
            .order_id = event.order_id,
            .side = event.side,
            .price = event.price,
            .quantity = event.quantity,
        });

    if (!inserted) {
        return false;
    }

    add_depth(event.side, event.price, event.quantity);
    return true;
}

bool OrderBook::cancel_order(const MarketEvent& event) {
    const auto it = orders_.find(event.order_id);
    if (it == orders_.end()) {
        // Orphan fallback: when enabled, reduce displayed depth at the event's
        // (side, price) by event.quantity. This handles LOBSTER-style replays
        // where the cancel references a pre-session order id that never had
        // an ADD in the captured stream. Quantity must be positive to know
        // how much depth to remove; a zero-quantity orphan cannot be resolved.
        if (config_.accept_unknown_order_ops && event.quantity > 0) {
            return reduce_depth(event.side, event.price, event.quantity);
        }
        return false;
    }

    const auto order = it->second;
    if (!reduce_depth(order.side, order.price, order.quantity)) {
        return false;
    }

    orders_.erase(it);
    return true;
}

bool OrderBook::modify_order(const MarketEvent& event) {
    if (event.price < 0 || event.quantity < 0) {
        return false;
    }

    const auto it = orders_.find(event.order_id);
    if (it == orders_.end()) {
        return false;
    }

    const auto old_order = it->second;
    if (!reduce_depth(old_order.side, old_order.price, old_order.quantity)) {
        return false;
    }

    if (event.quantity == 0) {
        orders_.erase(it);
        return true;
    }

    it->second.side = event.side;
    it->second.price = event.price;
    it->second.quantity = event.quantity;
    add_depth(event.side, event.price, event.quantity);
    return true;
}

bool OrderBook::trade_order(const MarketEvent& event) {
    if (event.quantity <= 0) {
        return false;
    }

    const auto it = orders_.find(event.order_id);
    if (it == orders_.end()) {
        // Orphan fallback: a trade against an unknown order id reduces
        // displayed depth at the event's (side, price) by event.quantity.
        // Used for LOBSTER-style replays where executions hit pre-session
        // resting orders that never appeared in the captured ADD stream.
        if (config_.accept_unknown_order_ops) {
            return reduce_depth(event.side, event.price, event.quantity);
        }
        return false;
    }

    const auto& order = it->second;
    if (event.side != order.side || event.price != order.price) {
        return false;
    }

    const auto traded_quantity = std::min(event.quantity, order.quantity);
    if (!reduce_depth(order.side, order.price, traded_quantity)) {
        return false;
    }

    if (traded_quantity == order.quantity) {
        orders_.erase(it);
    } else {
        it->second.quantity -= traded_quantity;
    }

    return true;
}

void OrderBook::add_depth(Side side, std::int64_t price, std::int64_t quantity) {
    if (quantity <= 0) {
        return;
    }

    if (side == Side::Buy) {
        bids_[price] += quantity;
    } else {
        asks_[price] += quantity;
    }
}

bool OrderBook::reduce_depth(Side side, std::int64_t price, std::int64_t quantity) {
    if (quantity <= 0) {
        return false;
    }

    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        if (it == bids_.end() || it->second < quantity) {
            return false;
        }

        it->second -= quantity;
        if (it->second == 0) {
            bids_.erase(it);
        }
        return true;
    }

    const auto it = asks_.find(price);
    if (it == asks_.end() || it->second < quantity) {
        return false;
    }

    it->second -= quantity;
    if (it->second == 0) {
        asks_.erase(it);
    }

    return true;
}

}  // namespace tickforge

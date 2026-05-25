#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

namespace tickforge {

enum class EventType {
    Add,
    Cancel,
    Modify,
    Trade,
};

enum class Side {
    Buy,
    Sell,
};

struct MarketEvent {
    std::uint64_t timestamp_ns{};
    EventType type{EventType::Add};
    std::uint64_t order_id{};
    Side side{Side::Buy};
    std::int64_t price{};
    std::int64_t quantity{};
};

inline std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::Add:
            return "ADD";
        case EventType::Cancel:
            return "CANCEL";
        case EventType::Modify:
            return "MODIFY";
        case EventType::Trade:
            return "TRADE";
    }
    return "UNKNOWN";
}

inline std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "BUY";
        case Side::Sell:
            return "SELL";
    }
    return "UNKNOWN";
}

inline std::optional<EventType> parse_event_type(std::string_view value) noexcept {
    if (value == "ADD") {
        return EventType::Add;
    }
    if (value == "CANCEL") {
        return EventType::Cancel;
    }
    if (value == "MODIFY") {
        return EventType::Modify;
    }
    if (value == "TRADE") {
        return EventType::Trade;
    }
    return std::nullopt;
}

inline std::optional<Side> parse_side(std::string_view value) noexcept {
    if (value == "BUY") {
        return Side::Buy;
    }
    if (value == "SELL") {
        return Side::Sell;
    }
    return std::nullopt;
}

inline std::ostream& operator<<(std::ostream& os, EventType type) {
    return os << to_string(type);
}

inline std::ostream& operator<<(std::ostream& os, Side side) {
    return os << to_string(side);
}

}  // namespace tickforge

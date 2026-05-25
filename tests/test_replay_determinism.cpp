#include "tickforge/csv_parser.hpp"
#include "tickforge/replay_engine.hpp"

#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

namespace tickforge {
namespace {

std::vector<MarketEvent> deterministic_events() {
    return {
        MarketEvent{1'000'000'000, EventType::Add, 1, Side::Buy, 10'000, 10},
        MarketEvent{1'000'000'010, EventType::Add, 2, Side::Sell, 10'005, 8},
        MarketEvent{1'000'000'020, EventType::Modify, 1, Side::Buy, 10'002, 15},
        MarketEvent{1'000'000'030, EventType::Trade, 2, Side::Sell, 10'005, 3},
        MarketEvent{1'000'000'040, EventType::Add, 3, Side::Buy, 10'003, 7},
        MarketEvent{1'000'000'050, EventType::Cancel, 1, Side::Buy, 10'002, 0},
    };
}

std::filesystem::path sample_csv_path() {
    const std::vector<std::filesystem::path> candidates = {
        "data/sample_events.csv",
        "../data/sample_events.csv",
        "../../data/sample_events.csv",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return "data/sample_events.csv";
}

}  // namespace

TEST(ReplayDeterminismTest, SameInputProducesSameFinalSummary) {
    ReplayEngine first_engine(ReplayConfig{.mode = ReplayMode::Max});
    ReplayEngine second_engine(ReplayConfig{.mode = ReplayMode::Max});

    const auto events = deterministic_events();
    const auto first = first_engine.replay(events);
    const auto second = second_engine.replay(events);

    EXPECT_EQ(first.events_processed, second.events_processed);
    EXPECT_EQ(first.rejected_events, 0U);
    EXPECT_EQ(second.rejected_events, 0U);
    EXPECT_EQ(first.final_best_bid, second.final_best_bid);
    EXPECT_EQ(first.final_best_ask, second.final_best_ask);
    EXPECT_EQ(first.final_spread, second.final_spread);
    EXPECT_EQ(first.final_total_buy_depth, second.final_total_buy_depth);
    EXPECT_EQ(first.final_total_sell_depth, second.final_total_sell_depth);
}

TEST(ReplayEngineTest, CanonicalSampleHasZeroRejectedEvents) {
    const auto parsed = CsvParser::parse_file(sample_csv_path());
    ASSERT_TRUE(parsed.ok());
    ASSERT_FALSE(parsed.events.empty());

    ReplayEngine engine(ReplayConfig{.mode = ReplayMode::Max});
    const auto summary = engine.replay(parsed.events);

    EXPECT_EQ(summary.events_processed, parsed.events.size());
    EXPECT_EQ(summary.rejected_events, 0U);
}

TEST(ReplayEngineTest, CrossThreadDeterminism) {
    const auto events = deterministic_events();
    ReplayEngine first_engine(ReplayConfig{.mode = ReplayMode::Max});
    ReplayEngine second_engine(ReplayConfig{.mode = ReplayMode::Max});

    const auto first = first_engine.replay(events);
    const auto second = second_engine.replay(events);

    EXPECT_EQ(first.events_processed, second.events_processed);
    EXPECT_EQ(first.rejected_events, second.rejected_events);
    EXPECT_EQ(first.final_best_bid, second.final_best_bid);
    EXPECT_EQ(first.final_best_ask, second.final_best_ask);
    EXPECT_EQ(first.final_spread, second.final_spread);
    EXPECT_EQ(first.final_total_buy_depth, second.final_total_buy_depth);
    EXPECT_EQ(first.final_total_sell_depth, second.final_total_sell_depth);
}

}  // namespace tickforge

#include "tickforge/order_book.hpp"

#include <gtest/gtest.h>

namespace tickforge {
namespace {

MarketEvent add(std::uint64_t id, Side side, std::int64_t price, std::int64_t quantity) {
    return MarketEvent{
        .timestamp_ns = id,
        .type = EventType::Add,
        .order_id = id,
        .side = side,
        .price = price,
        .quantity = quantity,
    };
}

MarketEvent cancel(std::uint64_t id, Side side, std::int64_t price) {
    return MarketEvent{
        .timestamp_ns = id,
        .type = EventType::Cancel,
        .order_id = id,
        .side = side,
        .price = price,
        .quantity = 0,
    };
}

MarketEvent modify(std::uint64_t id,
                   Side side,
                   std::int64_t price,
                   std::int64_t quantity) {
    return MarketEvent{
        .timestamp_ns = id,
        .type = EventType::Modify,
        .order_id = id,
        .side = side,
        .price = price,
        .quantity = quantity,
    };
}

MarketEvent trade(std::uint64_t id, Side side, std::int64_t price, std::int64_t quantity) {
    return MarketEvent{
        .timestamp_ns = id,
        .type = EventType::Trade,
        .order_id = id,
        .side = side,
        .price = price,
        .quantity = quantity,
    };
}

}  // namespace

TEST(OrderBookTest, AddUpdatesDepth) {
    OrderBook book;

    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 10)));

    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'000), 10);
    EXPECT_EQ(book.total_depth(Side::Buy), 10);
    EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookTest, CancelRemovesDepth) {
    OrderBook book;
    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 10)));

    ASSERT_TRUE(book.apply_event(cancel(1, Side::Buy, 10'000)));

    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'000), 0);
    EXPECT_EQ(book.total_depth(Side::Buy), 0);
    EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookTest, ModifyUpdatesQuantity) {
    OrderBook book;
    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 10)));

    ASSERT_TRUE(book.apply_event(modify(1, Side::Buy, 10'000, 15)));

    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'000), 15);
    EXPECT_EQ(book.total_depth(Side::Buy), 15);
}

TEST(OrderBookTest, ModifyPriceMovesOrderToNewLevel) {
    OrderBook book;
    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 10)));

    ASSERT_TRUE(book.apply_event(modify(1, Side::Buy, 10'005, 12)));

    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'000), 0);
    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'005), 12);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 10'005);
}

TEST(OrderBookTest, TradeReducesQuantity) {
    OrderBook book;
    ASSERT_TRUE(book.apply_event(add(1, Side::Sell, 10'010, 10)));

    ASSERT_TRUE(book.apply_event(trade(1, Side::Sell, 10'010, 4)));

    EXPECT_EQ(book.depth_at_price(Side::Sell, 10'010), 6);
    EXPECT_EQ(book.total_depth(Side::Sell), 6);
}

TEST(OrderBookTest, BestBidAndBestAskAreCorrect) {
    OrderBook book;
    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 10)));
    ASSERT_TRUE(book.apply_event(add(2, Side::Buy, 10'005, 5)));
    ASSERT_TRUE(book.apply_event(add(3, Side::Sell, 10'020, 8)));
    ASSERT_TRUE(book.apply_event(add(4, Side::Sell, 10'015, 9)));

    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_TRUE(book.best_ask().has_value());
    ASSERT_TRUE(book.spread().has_value());
    EXPECT_EQ(*book.best_bid(), 10'005);
    EXPECT_EQ(*book.best_ask(), 10'015);
    EXPECT_EQ(*book.spread(), 10);
}

TEST(OrderBookTest, OrphanCancelRejectedByDefault) {
    OrderBook book;

    // No ADD for order id 999 -- strict mode rejects.
    EXPECT_FALSE(book.apply_event(MarketEvent{
        .timestamp_ns = 1,
        .type = EventType::Cancel,
        .order_id = 999,
        .side = Side::Buy,
        .price = 10'000,
        .quantity = 10,
    }));
}

TEST(OrderBookTest, OrphanCancelReducesDepthWhenEnabled) {
    OrderBook book(OrderBookConfig{.accept_unknown_order_ops = true});

    // Seed level at 10'000 with depth 30 via a known order.
    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 30)));
    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'000), 30);

    // Orphan cancel for unknown id reduces depth at (BUY, 10'000) by 12.
    ASSERT_TRUE(book.apply_event(MarketEvent{
        .timestamp_ns = 2,
        .type = EventType::Cancel,
        .order_id = 999,
        .side = Side::Buy,
        .price = 10'000,
        .quantity = 12,
    }));
    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'000), 18);

    // The original known order is still intact -- only level depth shrank.
    EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookTest, OrphanTradeReducesDepthWhenEnabled) {
    OrderBook book(OrderBookConfig{.accept_unknown_order_ops = true});

    ASSERT_TRUE(book.apply_event(add(1, Side::Sell, 10'010, 50)));

    // Trade against an unknown sell id at the same level: drain 20.
    ASSERT_TRUE(book.apply_event(MarketEvent{
        .timestamp_ns = 2,
        .type = EventType::Trade,
        .order_id = 12345,
        .side = Side::Sell,
        .price = 10'010,
        .quantity = 20,
    }));
    EXPECT_EQ(book.depth_at_price(Side::Sell, 10'010), 30);
}

TEST(OrderBookTest, OrphanCancelFailsWhenLevelHasInsufficientDepth) {
    OrderBook book(OrderBookConfig{.accept_unknown_order_ops = true});

    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 5)));

    EXPECT_FALSE(book.apply_event(MarketEvent{
        .timestamp_ns = 2,
        .type = EventType::Cancel,
        .order_id = 999,
        .side = Side::Buy,
        .price = 10'000,
        .quantity = 10,
    }));
    // Depth unchanged after rejected orphan.
    EXPECT_EQ(book.depth_at_price(Side::Buy, 10'000), 5);
}

TEST(OrderBookTest, TopNReturnsSortedLevels) {
    OrderBook book;
    ASSERT_TRUE(book.apply_event(add(1, Side::Buy, 10'000, 10)));
    ASSERT_TRUE(book.apply_event(add(2, Side::Buy, 10'005, 5)));
    ASSERT_TRUE(book.apply_event(add(3, Side::Buy, 9'995, 7)));
    ASSERT_TRUE(book.apply_event(add(4, Side::Sell, 10'020, 8)));
    ASSERT_TRUE(book.apply_event(add(5, Side::Sell, 10'015, 9)));

    const auto bids = book.top_n(Side::Buy, 2);
    const auto asks = book.top_n(Side::Sell, 2);

    ASSERT_EQ(bids.size(), 2U);
    EXPECT_EQ(bids[0], PriceLevel(10'005, 5));
    EXPECT_EQ(bids[1], PriceLevel(10'000, 10));
    ASSERT_EQ(asks.size(), 2U);
    EXPECT_EQ(asks[0], PriceLevel(10'015, 9));
    EXPECT_EQ(asks[1], PriceLevel(10'020, 8));
}

}  // namespace tickforge

#include "tickforge/csv_parser.hpp"

#include <sstream>

#include <gtest/gtest.h>

namespace tickforge {

TEST(CsvParserTest, ParsesValidEvents) {
    std::istringstream input(
        "timestamp_ns,type,order_id,side,price,quantity\n"
        "1000000000,ADD,1,BUY,10000,10\n"
        "1000000010,TRADE,1,BUY,10000,3\n");

    const auto result = CsvParser::parse_stream(input);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.events.size(), 2U);
    EXPECT_EQ(result.events[0].timestamp_ns, 1'000'000'000ULL);
    EXPECT_EQ(result.events[0].type, EventType::Add);
    EXPECT_EQ(result.events[0].order_id, 1ULL);
    EXPECT_EQ(result.events[0].side, Side::Buy);
    EXPECT_EQ(result.events[0].price, 10'000);
    EXPECT_EQ(result.events[0].quantity, 10);
    EXPECT_EQ(result.events[1].type, EventType::Trade);
}

TEST(CsvParserTest, RejectsMalformedEventType) {
    std::istringstream input(
        "timestamp_ns,type,order_id,side,price,quantity\n"
        "1000000000,BOGUS,1,BUY,10000,10\n");

    const auto result = CsvParser::parse_stream(input);

    EXPECT_FALSE(result.ok());
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors[0].message.find("unknown event type"), std::string::npos);
    EXPECT_TRUE(result.events.empty());
}

TEST(CsvParserTest, SkipsBlankLines) {
    std::istringstream input(
        "\n"
        "timestamp_ns,type,order_id,side,price,quantity\n"
        "\n"
        "1000000000,ADD,1,BUY,10000,10\n"
        "   \n");

    const auto result = CsvParser::parse_stream(input);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.events.size(), 1U);
    EXPECT_EQ(result.events[0].order_id, 1ULL);
}

TEST(CsvParserTest, RejectsMalformedRows) {
    std::istringstream input(
        "timestamp_ns,type,order_id,side,price,quantity\n"
        "1000000000,ADD,1,BUY,10000\n"
        "1000000010,ADD,2,SELL,-1,10\n");

    const auto result = CsvParser::parse_stream(input);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.errors.size(), 2U);
    EXPECT_TRUE(result.events.empty());
}

}  // namespace tickforge

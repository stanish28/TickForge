#pragma once

#include <cstddef>
#include <filesystem>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

#include "tickforge/market_event.hpp"

namespace tickforge {

struct ParseError {
    std::size_t line{};
    std::string message;
    std::string row;
};

struct ParseResult {
    std::vector<MarketEvent> events;
    std::vector<ParseError> errors;

    [[nodiscard]] bool ok() const noexcept {
        return errors.empty();
    }
};

class CsvParser {
public:
    static ParseResult parse_file(const std::filesystem::path& path);
    static ParseResult parse_stream(std::istream& input, std::string_view source_name = "<stream>");
};

}  // namespace tickforge

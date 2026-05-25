#include "tickforge/csv_parser.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace tickforge {
namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                             value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                             value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<std::string_view> split_csv_line(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;

    while (start <= line.size()) {
        const auto comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            fields.push_back(trim(line.substr(start)));
            break;
        }

        fields.push_back(trim(line.substr(start, comma - start)));
        start = comma + 1;
    }

    return fields;
}

bool parse_uint64(std::string_view value, std::uint64_t& out) {
    if (value.empty()) {
        return false;
    }

    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_int64(std::string_view value, std::int64_t& out) {
    if (value.empty()) {
        return false;
    }

    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool is_header(const std::vector<std::string_view>& fields) {
    return fields.size() == 6 && fields[0] == "timestamp_ns" && fields[1] == "type" &&
           fields[2] == "order_id" && fields[3] == "side" && fields[4] == "price" &&
           fields[5] == "quantity";
}

std::string with_source(std::string_view source_name, std::string_view message) {
    std::string result;
    result.reserve(source_name.size() + message.size() + 2);
    result.append(source_name);
    result.append(": ");
    result.append(message);
    return result;
}

}  // namespace

ParseResult CsvParser::parse_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    ParseResult result;

    if (!file) {
        result.errors.push_back(ParseError{
            .line = 0,
            .message = "failed to open CSV file: " + path.string(),
            .row = {},
        });
        return result;
    }

    return parse_stream(file, path.string());
}

ParseResult CsvParser::parse_stream(std::istream& input, std::string_view source_name) {
    ParseResult result;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;

        const auto trimmed_line = trim(line);
        if (trimmed_line.empty()) {
            continue;
        }

        const auto fields = split_csv_line(trimmed_line);
        if (is_header(fields)) {
            continue;
        }

        auto add_error = [&](std::string_view message) {
            result.errors.push_back(ParseError{
                .line = line_number,
                .message = with_source(source_name, message),
                .row = std::string(trimmed_line),
            });
        };

        if (fields.size() != 6) {
            std::ostringstream oss;
            oss << "expected 6 CSV fields, got " << fields.size();
            add_error(oss.str());
            continue;
        }

        bool row_ok = true;
        MarketEvent event;

        if (!parse_uint64(fields[0], event.timestamp_ns)) {
            add_error("invalid timestamp_ns");
            row_ok = false;
        }

        const auto event_type = parse_event_type(fields[1]);
        if (!event_type) {
            add_error("unknown event type '" + std::string(fields[1]) + "'");
            row_ok = false;
        } else {
            event.type = *event_type;
        }

        if (!parse_uint64(fields[2], event.order_id)) {
            add_error("invalid order_id");
            row_ok = false;
        }

        const auto side = parse_side(fields[3]);
        if (!side) {
            add_error("unknown side '" + std::string(fields[3]) + "'");
            row_ok = false;
        } else {
            event.side = *side;
        }

        if (!parse_int64(fields[4], event.price) || event.price < 0) {
            add_error("invalid non-negative integer price");
            row_ok = false;
        }

        if (!parse_int64(fields[5], event.quantity) || event.quantity < 0) {
            add_error("invalid non-negative integer quantity");
            row_ok = false;
        }

        if (row_ok && event.type == EventType::Add && event.quantity == 0) {
            add_error("ADD quantity must be greater than zero");
            row_ok = false;
        }

        if (row_ok && event.type == EventType::Trade && event.quantity == 0) {
            add_error("TRADE quantity must be greater than zero");
            row_ok = false;
        }

        if (row_ok) {
            result.events.push_back(event);
        }
    }

    return result;
}

}  // namespace tickforge

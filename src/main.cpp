#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "tickforge/csv_parser.hpp"
#include "tickforge/order_book.hpp"
#include "tickforge/replay_engine.hpp"

namespace {

void print_usage(std::ostream& os) {
    os << "Usage:\n"
       << "  ./tickforge_replay <csv_path> --mode max [--report <report_path>] "
          "[--allow-orphan-events]\n"
       << "  ./tickforge_replay <csv_path> --mode realtime [--report <report_path>]\n"
       << "  ./tickforge_replay <csv_path> --mode speed --speed <multiplier> "
          "[--report <report_path>]\n"
       << "\n"
       << "  --allow-orphan-events  Treat CANCEL/TRADE events that reference an\n"
       << "                         unknown order_id as a level-only depth reduction\n"
       << "                         at (side, price) by quantity. Use this when\n"
       << "                         replaying exchange feeds whose messages reference\n"
       << "                         pre-session resting orders (e.g. LOBSTER samples).\n";
}

std::string format_optional(std::optional<std::int64_t> value) {
    return value ? std::to_string(*value) : "NA";
}

tickforge::ReplayMode parse_mode(const std::string& value) {
    if (value == "max") {
        return tickforge::ReplayMode::Max;
    }
    if (value == "realtime") {
        return tickforge::ReplayMode::Realtime;
    }
    if (value == "speed") {
        return tickforge::ReplayMode::Speed;
    }
    throw std::invalid_argument("unknown replay mode: " + value);
}

void print_summary(const tickforge::ReplaySummary& summary) {
    const auto& latency = summary.latency;

    std::cout << "TickForge replay summary\n";
    std::cout << "events_processed: " << summary.events_processed << '\n';
    std::cout << "rejected_events: " << summary.rejected_events << '\n';
    std::cout << "final_best_bid: " << format_optional(summary.final_best_bid) << '\n';
    std::cout << "final_best_ask: " << format_optional(summary.final_best_ask) << '\n';
    std::cout << "final_spread: " << format_optional(summary.final_spread) << '\n';
    std::cout << "final_total_buy_depth: " << summary.final_total_buy_depth << '\n';
    std::cout << "final_total_sell_depth: " << summary.final_total_sell_depth << '\n';
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "latency_min_ns: " << latency.min_ns << '\n';
    std::cout << "latency_max_ns: " << latency.max_ns << '\n';
    std::cout << "latency_mean_ns: " << latency.mean_ns << '\n';
    std::cout << "latency_p50_ns: " << latency.p50_ns << '\n';
    std::cout << "latency_p95_ns: " << latency.p95_ns << '\n';
    std::cout << "latency_p99_ns: " << latency.p99_ns << '\n';
    std::cout << "throughput_events_per_sec: " << latency.throughput_events_per_sec << '\n';
}

void print_top_levels(const tickforge::OrderBook& book, std::size_t n) {
    const auto bids = book.top_n(tickforge::Side::Buy, n);
    const auto asks = book.top_n(tickforge::Side::Sell, n);

    std::cout << "top_" << n << "_bids:";
    for (const auto& [price, quantity] : bids) {
        std::cout << ' ' << price << '@' << quantity;
    }
    std::cout << '\n';

    std::cout << "top_" << n << "_asks:";
    for (const auto& [price, quantity] : asks) {
        std::cout << ' ' << price << '@' << quantity;
    }
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(std::cerr);
        return 1;
    }

    std::filesystem::path csv_path = argv[1];
    tickforge::ReplayConfig config;
    std::optional<std::filesystem::path> report_path;

    try {
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];

            if (arg == "--mode") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("--mode requires a value");
                }
                config.mode = parse_mode(argv[++i]);
            } else if (arg == "--speed") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("--speed requires a numeric value");
                }
                config.speed_multiplier = std::stod(argv[++i]);
                if (config.speed_multiplier <= 0.0) {
                    throw std::invalid_argument("--speed must be greater than zero");
                }
            } else if (arg == "--report") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("--report requires a path");
                }
                report_path = std::filesystem::path(argv[++i]);
            } else if (arg == "--allow-orphan-events") {
                config.accept_unknown_order_ops = true;
            } else if (arg == "--help" || arg == "-h") {
                print_usage(std::cout);
                return 0;
            } else {
                throw std::invalid_argument("unknown argument: " + arg);
            }
        }

        const auto parsed = tickforge::CsvParser::parse_file(csv_path);
        if (!parsed.ok()) {
            std::cerr << "CSV parse failed with " << parsed.errors.size() << " error(s):\n";
            for (const auto& error : parsed.errors) {
                std::cerr << "  line " << error.line << ": " << error.message;
                if (!error.row.empty()) {
                    std::cerr << " | row: " << error.row;
                }
                std::cerr << '\n';
            }
            return 1;
        }

        tickforge::ReplayEngine engine(config);
        const auto summary = engine.replay(parsed.events);
        print_summary(summary);
        print_top_levels(engine.order_book(), 10);

        if (report_path) {
            engine.latency_profiler().write_csv(*report_path);
            std::cout << "latency_report: " << report_path->string() << '\n';
        }
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        print_usage(std::cerr);
        return 1;
    }

    return 0;
}

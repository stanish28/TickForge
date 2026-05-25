#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace tickforge {

struct LatencyStats {
    std::size_t count{};
    std::uint64_t min_ns{};
    std::uint64_t max_ns{};
    double mean_ns{};
    double p50_ns{};
    double p95_ns{};
    double p99_ns{};
    double throughput_events_per_sec{};
};

class LatencyProfiler {
public:
    void clear();
    void reserve(std::size_t count);
    void record(std::uint64_t latency_ns);

    [[nodiscard]] LatencyStats stats(std::uint64_t elapsed_ns) const;
    [[nodiscard]] const std::vector<std::uint64_t>& samples() const noexcept;

    void write_csv(const std::filesystem::path& path) const;

private:
    std::vector<std::uint64_t> samples_;
};

}  // namespace tickforge

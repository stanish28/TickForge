#include "tickforge/latency_profiler.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace tickforge {

void LatencyProfiler::clear() {
    samples_.clear();
}

void LatencyProfiler::reserve(std::size_t count) {
    samples_.reserve(count);
}

void LatencyProfiler::record(std::uint64_t latency_ns) {
    samples_.push_back(latency_ns);
}

LatencyStats LatencyProfiler::stats(std::uint64_t elapsed_ns) const {
    LatencyStats result;
    result.count = samples_.size();

    if (samples_.empty()) {
        return result;
    }

    std::vector<std::uint64_t> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());

    const auto percentile = [&](double q) -> double {
        const auto rank = static_cast<std::size_t>(
            std::ceil(q * static_cast<double>(sorted.size())));
        const auto index = rank == 0 ? 0 : std::min(rank - 1, sorted.size() - 1);
        return static_cast<double>(sorted[index]);
    };

    const auto sum = std::accumulate(
        samples_.begin(), samples_.end(), 0.0L,
        [](long double acc, std::uint64_t value) { return acc + static_cast<long double>(value); });

    result.min_ns = sorted.front();
    result.max_ns = sorted.back();
    result.mean_ns = static_cast<double>(sum / static_cast<long double>(samples_.size()));
    result.p50_ns = percentile(0.50);
    result.p95_ns = percentile(0.95);
    result.p99_ns = percentile(0.99);

    if (elapsed_ns > 0) {
        result.throughput_events_per_sec =
            static_cast<double>(samples_.size()) * 1'000'000'000.0 /
            static_cast<double>(elapsed_ns);
    }

    return result;
}

const std::vector<std::uint64_t>& LatencyProfiler::samples() const noexcept {
    return samples_;
}

void LatencyProfiler::write_csv(const std::filesystem::path& path) const {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to open latency report for writing: " + path.string());
    }

    out << "event_index,latency_ns\n";
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        out << i << ',' << samples_[i] << '\n';
    }
}

}  // namespace tickforge

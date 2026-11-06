#pragma once

#include "basalt/common/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace basalt::bench {

struct TelemetrySpan { std::array<std::int64_t, 11> timestamps_ns{}; };

struct LatencyDistribution {
    std::string stage{};
    std::uint64_t samples{};
    std::uint64_t p50_ns{};
    std::uint64_t p90_ns{};
    std::uint64_t p95_ns{};
    std::uint64_t p99_ns{};
    std::uint64_t p999_ns{};
};

struct TelemetrySnapshot {
    std::string clock_source{};
    std::uint64_t clock_resolution_ns{};
    std::uint64_t recorded_spans{};
    std::uint64_t sampled_out_spans{};
    std::uint64_t dropped_spans{};
    std::size_t queue_high_water{};
    std::array<std::uint64_t, 4> quality_counts{};
    std::array<LatencyDistribution, 11> latency{};
};

class PipelineTelemetry {
public:
    explicit PipelineTelemetry(const std::size_t capacity, const std::size_t sample_every = 1,
                                std::string clock_source = "steady_clock",
                                const std::uint64_t clock_resolution_ns = 1)
        : capacity_(capacity), sample_every_(sample_every), clock_source_(std::move(clock_source)),
          clock_resolution_ns_(clock_resolution_ns), spans_(capacity) {
        if (capacity_ == 0 || sample_every_ == 0 || clock_resolution_ns_ == 0)
            throw std::invalid_argument("Telemetry capacity, sample interval and clock resolution must be positive");
    }

    [[nodiscard]] bool record(const TelemetrySpan& span) {
        std::lock_guard lock(mutex_);
        const auto ordinal = seen_++;
        if (std::any_of(span.timestamps_ns.begin(), span.timestamps_ns.end(), [](const auto value) { return value <= 0; }) ||
            !std::is_sorted(span.timestamps_ns.begin(), span.timestamps_ns.end())) {
            ++dropped_;
            return false;
        }
        if (ordinal % sample_every_ != 0) { ++sampled_out_; return true; }
        if (recorded_ == capacity_) { ++dropped_; return false; }
        spans_[recorded_++] = span;
        return true;
    }

    void record_queue_depth(const std::size_t depth) noexcept {
        std::lock_guard lock(mutex_);
        queue_high_water_ = std::max(queue_high_water_, depth);
    }

    void record_quality(const common::FeedQualityState quality) noexcept {
        const auto index = static_cast<std::size_t>(quality);
        if (index >= quality_counts_.size()) return;
        std::lock_guard lock(mutex_);
        ++quality_counts_[index];
    }

    [[nodiscard]] TelemetrySnapshot snapshot() const {
        std::vector<TelemetrySpan> spans;
        TelemetrySnapshot result{};
        {
            std::lock_guard lock(mutex_);
            spans.assign(spans_.begin(), spans_.begin() + static_cast<std::ptrdiff_t>(recorded_));
            result.clock_source = clock_source_;
            result.clock_resolution_ns = clock_resolution_ns_;
            result.recorded_spans = recorded_;
            result.sampled_out_spans = sampled_out_;
            result.dropped_spans = dropped_;
            result.queue_high_water = queue_high_water_;
            result.quality_counts = quality_counts_;
        }
        static constexpr std::array<const char*, 11> names{
            "T0-T1", "T1-T2", "T2-T3", "T3-T4", "T4-T5", "T5-T6",
            "T6-T7", "T7-T8", "T8-T9", "T9-T10", "T0-T10"};
        for (std::size_t stage = 0; stage < names.size(); ++stage) {
            std::vector<std::uint64_t> values;
            values.reserve(spans.size());
            for (const auto& span : spans) {
                const std::size_t start = stage == 10 ? 0 : stage;
                const std::size_t end = stage == 10 ? 10 : stage + 1;
                values.push_back(static_cast<std::uint64_t>(span.timestamps_ns[end] - span.timestamps_ns[start]));
            }
            std::sort(values.begin(), values.end());
            const auto at = [&values](const double probability) -> std::uint64_t {
                if (values.empty()) return 0;
                const auto rank = static_cast<std::size_t>(std::ceil(probability * values.size()));
                return values[std::max<std::size_t>(1, rank) - 1];
            };
            result.latency[stage] = {names[stage], static_cast<std::uint64_t>(values.size()),
                at(0.50), at(0.90), at(0.95), at(0.99), at(0.999)};
        }
        return result;
    }

private:
    const std::size_t capacity_;
    const std::size_t sample_every_;
    const std::string clock_source_;
    const std::uint64_t clock_resolution_ns_;
    mutable std::mutex mutex_{};
    std::vector<TelemetrySpan> spans_{};
    std::size_t recorded_{};
    std::uint64_t seen_{};
    std::uint64_t sampled_out_{};
    std::uint64_t dropped_{};
    std::size_t queue_high_water_{};
    std::array<std::uint64_t, 4> quality_counts_{};
};

} // namespace basalt::bench

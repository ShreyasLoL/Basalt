#pragma once

#include "basalt/market_data/types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace basalt::market_data {

struct EventBusStats {
    std::size_t durable_depth{};
    std::size_t durable_capacity{};
    std::size_t quote_depth{};
    std::size_t quote_capacity{};
    std::uint64_t quote_replacements{};
    std::uint64_t quote_overflows{};
    std::uint64_t durable_timeouts{};
};

class BoundedMarketEventBus {
public:
    BoundedMarketEventBus(std::size_t durable_capacity, std::size_t quote_capacity);
    BoundedMarketEventBus(const BoundedMarketEventBus&) = delete;
    BoundedMarketEventBus& operator=(const BoundedMarketEventBus&) = delete;

    [[nodiscard]] bool publish_durable(
        CanonicalMarketEvent event, std::chrono::milliseconds maximum_wait);
    [[nodiscard]] std::optional<CanonicalMarketEvent> pop_durable(
        std::chrono::milliseconds maximum_wait);
    [[nodiscard]] bool publish_quote(CanonicalMarketEvent event);
    [[nodiscard]] std::vector<CanonicalMarketEvent> drain_quotes();
    void close() noexcept;
    [[nodiscard]] EventBusStats stats() const;

private:
    const std::size_t durable_capacity_;
    const std::size_t quote_capacity_;
    mutable std::mutex mutex_{};
    std::condition_variable not_empty_{};
    std::condition_variable not_full_{};
    std::deque<CanonicalMarketEvent> durable_{};
    std::deque<common::InstrumentId> quote_order_{};
    std::unordered_map<common::InstrumentId, CanonicalMarketEvent> latest_quotes_{};
    bool closed_{};
    std::uint64_t quote_replacements_{};
    std::uint64_t quote_overflows_{};
    std::uint64_t durable_timeouts_{};
};

} // namespace basalt::market_data

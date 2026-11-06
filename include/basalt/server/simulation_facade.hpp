#pragma once

#include "basalt/core/matching_engine.hpp"

#include <cstddef>
#include <mutex>
#include <vector>

namespace basalt::server {

struct MatchingView {
    BookSnapshot book{};
    EngineStats stats{};
    std::vector<Trade> trades{};
    std::vector<ActivityRecord> activity{};
};

// Validated thread-safe boundary shared by the HTTP and MCP adapters. It keeps
// the matching engine private and returns value snapshots to callers.
class SimulationFacade {
public:
    explicit SimulationFacade(bool seed_demo = true);

    [[nodiscard]] MatchingView inspect(std::size_t depth = 20,
                                       std::size_t trade_limit = 50,
                                       std::size_t activity_limit = 50) const;
    [[nodiscard]] BookSnapshot book(std::size_t depth = 20) const;
    [[nodiscard]] EngineStats stats() const;
    [[nodiscard]] std::vector<Trade> recent_trades(std::size_t limit = 50) const;
    [[nodiscard]] OrderResult submit_order(OrderRequest request);
    [[nodiscard]] std::vector<OrderResult> submit_batch(std::vector<OrderRequest> requests);
    [[nodiscard]] CancelResult cancel_order(OrderId id);
    [[nodiscard]] bool verify_invariants() const;
    void reset(bool seed_demo = false);

private:
    mutable std::mutex mutex_{};
    MatchingEngine engine_{};
    OrderId next_order_id_{10'000};
    [[nodiscard]] OrderResult submit_locked(OrderRequest request);
};

} // namespace basalt::server

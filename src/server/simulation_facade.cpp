#include "basalt/server/simulation_facade.hpp"

#include <limits>
#include <utility>

namespace basalt::server {

SimulationFacade::SimulationFacade(const bool seed_demo) {
    if (seed_demo) engine_.seed_demo_book();
}

MatchingView SimulationFacade::inspect(const std::size_t depth, const std::size_t trade_limit,
                                       const std::size_t activity_limit) const {
    std::lock_guard lock(mutex_);
    return {engine_.get_snapshot(depth), engine_.stats(), engine_.recent_trades(trade_limit),
            engine_.recent_activity(activity_limit)};
}

BookSnapshot SimulationFacade::book(const std::size_t depth) const {
    std::lock_guard lock(mutex_);
    return engine_.get_snapshot(depth);
}

EngineStats SimulationFacade::stats() const {
    std::lock_guard lock(mutex_);
    return engine_.stats();
}

std::vector<Trade> SimulationFacade::recent_trades(const std::size_t limit) const {
    std::lock_guard lock(mutex_);
    return engine_.recent_trades(limit);
}

OrderResult SimulationFacade::submit_locked(OrderRequest request) {
    if (request.id == 0) {
        while (engine_.book().has_order(next_order_id_) && next_order_id_ < std::numeric_limits<OrderId>::max())
            ++next_order_id_;
        request.id = next_order_id_;
        if (next_order_id_ < std::numeric_limits<OrderId>::max()) ++next_order_id_;
    } else if (request.id >= next_order_id_ && request.id < std::numeric_limits<OrderId>::max()) {
        next_order_id_ = request.id + 1U;
    }
    return engine_.submit_order(request);
}

OrderResult SimulationFacade::submit_order(OrderRequest request) {
    std::lock_guard lock(mutex_);
    return submit_locked(std::move(request));
}

std::vector<OrderResult> SimulationFacade::submit_batch(std::vector<OrderRequest> requests) {
    std::lock_guard lock(mutex_);
    std::vector<OrderResult> results;
    results.reserve(requests.size());
    for (auto& request : requests) results.push_back(submit_locked(std::move(request)));
    return results;
}

CancelResult SimulationFacade::cancel_order(const OrderId id) {
    std::lock_guard lock(mutex_);
    return engine_.cancel_order(id);
}

bool SimulationFacade::verify_invariants() const {
    std::lock_guard lock(mutex_);
    return engine_.book().verify_invariants();
}

void SimulationFacade::reset(const bool seed_demo) {
    std::lock_guard lock(mutex_);
    engine_.reset();
    if (seed_demo) engine_.seed_demo_book();
}

} // namespace basalt::server

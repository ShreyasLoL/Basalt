#include "basalt/market_data/event_bus.hpp"

#include <stdexcept>
#include <utility>

namespace basalt::market_data {

BoundedMarketEventBus::BoundedMarketEventBus(const std::size_t durable_capacity,
                                             const std::size_t quote_capacity)
    : durable_capacity_(durable_capacity), quote_capacity_(quote_capacity), durable_(durable_capacity) {
    if (durable_capacity_ == 0 || quote_capacity_ == 0) {
        throw std::invalid_argument("Market event bus capacities must be positive");
    }
}

bool BoundedMarketEventBus::publish_durable(CanonicalMarketEvent event,
                                           const std::chrono::milliseconds maximum_wait) {
    const auto deadline = std::chrono::steady_clock::now() + maximum_wait;
    while (true) {
        std::unique_lock lock(mutex_);
        const auto thread = std::this_thread::get_id();
        if (durable_producer_thread_ == std::thread::id{}) durable_producer_thread_ = thread;
        else if (durable_producer_thread_ != thread)
            throw std::logic_error("Durable event lane requires exactly one producer thread");
        if (closed_) return false;
        ++publishers_in_flight_;
        lock.unlock();

        const bool published = durable_.try_push(std::move(event));

        lock.lock();
        --publishers_in_flight_;
        if (published) {
            not_empty_.notify_one();
            return true;
        }
        if (closed_) {
            not_empty_.notify_all();
            return false;
        }
        const bool ready = not_full_.wait_until(lock, deadline,
            [this] { return closed_ || !durable_.full(); });
        if (!ready) {
            ++durable_timeouts_;
            return false;
        }
    }
}

std::optional<CanonicalMarketEvent> BoundedMarketEventBus::pop_durable(
    const std::chrono::milliseconds maximum_wait) {
    const auto deadline = std::chrono::steady_clock::now() + maximum_wait;
    {
        std::lock_guard lock(mutex_);
        const auto thread = std::this_thread::get_id();
        if (durable_consumer_thread_ == std::thread::id{}) durable_consumer_thread_ = thread;
        else if (durable_consumer_thread_ != thread)
            throw std::logic_error("Durable event lane requires exactly one consumer thread");
    }
    while (true) {
        auto event = durable_.try_pop();
        if (event) {
            std::lock_guard lock(mutex_);
            not_full_.notify_one();
            return event;
        }
        std::unique_lock lock(mutex_);
        const bool ready = not_empty_.wait_until(lock, deadline,
            [this] { return !durable_.empty() || (closed_ && publishers_in_flight_ == 0); });
        if (!ready || (closed_ && publishers_in_flight_ == 0 && durable_.empty()))
            return std::nullopt;
    }
}

bool BoundedMarketEventBus::publish_quote(CanonicalMarketEvent event) {
    if (event.kind != MarketEventKind::Quote || event.instrument_id == 0) return false;
    std::lock_guard lock(mutex_);
    if (closed_) return false;
    const auto found = latest_quotes_.find(event.instrument_id);
    if (found != latest_quotes_.end()) {
        found->second = std::move(event);
        ++quote_replacements_;
        return true;
    }
    if (latest_quotes_.size() >= quote_capacity_) {
        ++quote_overflows_;
        return false;
    }
    quote_order_.push_back(event.instrument_id);
    latest_quotes_.emplace(event.instrument_id, std::move(event));
    return true;
}

std::vector<CanonicalMarketEvent> BoundedMarketEventBus::drain_quotes() {
    std::lock_guard lock(mutex_);
    std::vector<CanonicalMarketEvent> result;
    result.reserve(latest_quotes_.size());
    while (!quote_order_.empty()) {
        const common::InstrumentId id = quote_order_.front();
        quote_order_.pop_front();
        const auto found = latest_quotes_.find(id);
        if (found == latest_quotes_.end()) continue;
        result.push_back(std::move(found->second));
        latest_quotes_.erase(found);
    }
    return result;
}

void BoundedMarketEventBus::close() noexcept {
    std::lock_guard lock(mutex_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
}

EventBusStats BoundedMarketEventBus::stats() const {
    std::lock_guard lock(mutex_);
    return EventBusStats{
        durable_.size(), durable_capacity_, latest_quotes_.size(), quote_capacity_,
        quote_replacements_, quote_overflows_, durable_timeouts_};
}

} // namespace basalt::market_data

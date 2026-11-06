#pragma once

#include "basalt/common/types.hpp"
#include "basalt/market_data/types.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace basalt::market_data {

struct ProviderCapabilities {
    bool trades{};
    bool quotes{};
    bool depth{};
    bool bars{};
    std::size_t max_subscriptions{};
};

struct ProviderConfig {
    std::string provider_name{};
    std::string endpoint{};
    std::string token_environment_variable{};
    bool enabled{};
    bool dry_run{true};
};

class IMarketDataProvider {
public:
    using EventSink = std::function<void(RawMarketEvent)>;

    virtual ~IMarketDataProvider() = default;
    [[nodiscard]] virtual common::Result<void, ProviderError> connect(const ProviderConfig& config) = 0;
    [[nodiscard]] virtual common::Result<void, ProviderError> subscribe(
        std::span<const common::InstrumentId> instrument_ids) = 0;
    virtual void set_event_sink(EventSink sink) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual ProviderCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

[[nodiscard]] inline ProviderError provider_error(std::string code, std::string message) {
    return ProviderError{std::move(code), std::move(message)};
}

} // namespace basalt::market_data

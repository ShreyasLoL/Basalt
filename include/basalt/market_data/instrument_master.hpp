#pragma once

#include "basalt/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::market_data {

enum class Exchange : std::uint8_t { Nse, Bse, Sim };

struct TradingSession {
    std::string open_local{};
    std::string close_local{};
};

struct ProviderSymbolMapping {
    std::string provider{};
    std::string provider_key{};
    std::int64_t valid_from_ns{};
    std::optional<std::int64_t> valid_to_ns{};
};

struct InstrumentDefinition {
    common::InstrumentId id{};
    Exchange exchange{Exchange::Sim};
    std::string segment{};
    std::string symbol{};
    std::string display_symbol{};
    std::string currency{"INR"};
    std::string timezone{"Asia/Kolkata"};
    common::MicroUnits tick_size_micro{};
    std::uint64_t lot_size{1};
    std::uint64_t quantity_step{1};
    bool active{true};
    std::optional<std::int64_t> active_from_ns{};
    std::optional<std::int64_t> active_to_ns{};
    std::optional<common::MicroUnits> lower_price_band_micro{};
    std::optional<common::MicroUnits> upper_price_band_micro{};
    std::vector<TradingSession> sessions{};
    std::vector<ProviderSymbolMapping> provider_mappings{};
};

class InstrumentMaster {
public:
    explicit InstrumentMaster(std::uint32_t catalog_version = 1) noexcept;

    [[nodiscard]] common::Result<void> add(InstrumentDefinition instrument);
    [[nodiscard]] const InstrumentDefinition* find(common::InstrumentId id) const noexcept;
    [[nodiscard]] common::Result<common::InstrumentId> resolve_symbol(
        Exchange exchange, std::string_view segment, std::string_view symbol) const;
    [[nodiscard]] common::Result<common::InstrumentId> resolve_provider(
        std::string_view provider, std::string_view provider_key,
        std::int64_t event_time_ns) const;
    [[nodiscard]] std::vector<InstrumentDefinition> list(
        std::optional<Exchange> exchange = std::nullopt,
        bool active_only = false) const;
    [[nodiscard]] std::uint32_t version() const noexcept { return catalog_version_; }
    [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }

    [[nodiscard]] static InstrumentMaster seeded_sim_catalog();

private:
    std::uint32_t catalog_version_;
    std::vector<InstrumentDefinition> instruments_{};
};

[[nodiscard]] std::string_view to_string(Exchange exchange) noexcept;
[[nodiscard]] common::Result<Exchange> parse_exchange(std::string_view text);

} // namespace basalt::market_data

#pragma once

#include "basalt/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::market_data {

enum class MarketEventKind : std::uint8_t { Trade, Quote, Depth, Bar, Status };

struct RawDepthLevel {
    std::string price{};
    std::string quantity{};
};

struct RawMarketEvent {
    std::uint32_t schema_version{1};
    std::string provider{};
    std::string event_id{};
    std::string provider_instrument_key{};
    MarketEventKind kind{MarketEventKind::Quote};
    std::int64_t event_time_ns{};
    std::optional<std::uint64_t> provider_sequence{};
    std::optional<std::string> price{};
    std::optional<std::string> quantity{};
    std::optional<std::string> bid{};
    std::optional<std::string> bid_quantity{};
    std::optional<std::string> ask{};
    std::optional<std::string> ask_quantity{};
    std::optional<std::string> open{};
    std::optional<std::string> high{};
    std::optional<std::string> low{};
    std::optional<std::string> close{};
    std::optional<std::string> volume{};
    std::vector<RawDepthLevel> bids{};
    std::vector<RawDepthLevel> asks{};
    std::string provenance_label{};
};

struct MarketDepthLevel {
    common::MicroUnits price_micro{};
    common::PriceTicks price_ticks{};
    common::Quantity quantity{};
};

struct OhlcvBar {
    common::MicroUnits open_micro{};
    common::MicroUnits high_micro{};
    common::MicroUnits low_micro{};
    common::MicroUnits close_micro{};
    common::Quantity volume{};
};

struct CanonicalMarketEvent {
    std::uint32_t schema_version{1};
    std::string event_id{};
    common::InstrumentId instrument_id{};
    std::string provider{};
    MarketEventKind kind{MarketEventKind::Quote};
    std::int64_t event_time_ns{};
    std::int64_t receive_time_utc_ns{};
    std::int64_t receive_time_mono_ns{};
    std::optional<std::uint64_t> provider_sequence{};
    common::DataSourceType source{common::DataSourceType::Derived};
    common::FeedQualityState quality{common::FeedQualityState::Ok};
    bool sequence_gap{};
    bool clock_skew{};
    std::uint64_t reconnect_generation{};
    std::optional<common::MicroUnits> price_micro{};
    std::optional<common::PriceTicks> price_ticks{};
    std::optional<common::Quantity> quantity{};
    std::optional<common::MicroUnits> bid_micro{};
    std::optional<common::PriceTicks> bid_ticks{};
    std::optional<common::Quantity> bid_quantity{};
    std::optional<common::MicroUnits> ask_micro{};
    std::optional<common::PriceTicks> ask_ticks{};
    std::optional<common::Quantity> ask_quantity{};
    std::vector<MarketDepthLevel> bids{};
    std::vector<MarketDepthLevel> asks{};
    std::optional<OhlcvBar> bar{};
    std::string provenance_label{};
};

struct ProviderError {
    std::string code{};
    std::string message{};
};

[[nodiscard]] std::string_view to_string(MarketEventKind kind) noexcept;
[[nodiscard]] common::Result<MarketEventKind> parse_event_kind(std::string_view text);

} // namespace basalt::market_data

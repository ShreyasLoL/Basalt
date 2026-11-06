#pragma once

#include "basalt/common/types.hpp"
#include "basalt/core/trade.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace basalt::bench {

enum class WorkloadProfile : std::uint8_t {
    Balanced,
    BuyHeavy,
    SellHeavy,
    AggressiveMatching,
    CancelHeavy,
    ConcentratedLevels,
    WideDistribution,
    BurstyArrivals,
    HighActiveOrderCount,
    MultiInstrument50,
    MarketDataBurst,
    SlowConsumerBackpressure
};

struct WorkloadCommand {
    ActionType action{ActionType::Submit};
    OrderRequest order{};
    OrderId cancel_order_id{0};
    common::InstrumentId instrument_id{1};
};

struct WorkloadProfileInfo {
    WorkloadProfile profile;
    std::string_view id;
    std::string_view name;
};

inline constexpr std::array<WorkloadProfileInfo, 12> WORKLOAD_PROFILES{{
    {WorkloadProfile::Balanced, "BALANCED", "1. Balanced Market Flow"},
    {WorkloadProfile::BuyHeavy, "BUY_HEAVY", "2. Buy Heavy"},
    {WorkloadProfile::SellHeavy, "SELL_HEAVY", "3. Sell Heavy"},
    {WorkloadProfile::AggressiveMatching, "AGGRESSIVE_MATCHING", "4. Aggressive Matching"},
    {WorkloadProfile::CancelHeavy, "CANCEL_HEAVY", "5. Cancel Heavy"},
    {WorkloadProfile::ConcentratedLevels, "CONCENTRATED_LEVELS", "6. Concentrated Levels"},
    {WorkloadProfile::WideDistribution, "WIDE_DISTRIBUTION", "7. Wide Distribution"},
    {WorkloadProfile::BurstyArrivals, "BURSTY_ARRIVALS", "8. Bursty Arrivals"},
    {WorkloadProfile::HighActiveOrderCount, "HIGH_ACTIVE_ORDERS", "9. High Active Order Count"},
    {WorkloadProfile::MultiInstrument50, "MULTI_INSTRUMENT_50", "10. Fifty Instrument Shards"},
    {WorkloadProfile::MarketDataBurst, "MARKET_DATA_BURST", "11. Market Data Burst"},
    {WorkloadProfile::SlowConsumerBackpressure, "SLOW_CONSUMER", "12. Slow Consumer Backpressure"}
}};

[[nodiscard]] std::string_view profile_id(WorkloadProfile profile) noexcept;
[[nodiscard]] std::string_view profile_name(WorkloadProfile profile) noexcept;
[[nodiscard]] bool parse_profile(std::string_view text, WorkloadProfile& profile) noexcept;

class WorkloadGenerator {
public:
    [[nodiscard]] static std::vector<WorkloadCommand> generate(
        WorkloadProfile profile, std::size_t count, std::uint64_t seed = 42);
};

} // namespace basalt::bench

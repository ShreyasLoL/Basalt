#pragma once

#include "atlas/core/trade.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace atlas::bench {

enum class WorkloadProfile : std::uint8_t {
    Balanced,
    BuyHeavy,
    SellHeavy,
    AggressiveMatching,
    CancelHeavy,
    ConcentratedLevels,
    WideDistribution
};

struct WorkloadCommand {
    ActionType action{ActionType::Submit};
    OrderRequest order{};
    OrderId cancel_order_id{0};
};

struct WorkloadProfileInfo {
    WorkloadProfile profile;
    std::string_view id;
    std::string_view name;
};

inline constexpr std::array<WorkloadProfileInfo, 7> WORKLOAD_PROFILES{{
    {WorkloadProfile::Balanced, "BALANCED", "1. Balanced Market Flow"},
    {WorkloadProfile::BuyHeavy, "BUY_HEAVY", "2. Buy Heavy"},
    {WorkloadProfile::SellHeavy, "SELL_HEAVY", "3. Sell Heavy"},
    {WorkloadProfile::AggressiveMatching, "AGGRESSIVE_MATCHING", "4. Aggressive Matching"},
    {WorkloadProfile::CancelHeavy, "CANCEL_HEAVY", "5. Cancel Heavy"},
    {WorkloadProfile::ConcentratedLevels, "CONCENTRATED_LEVELS", "6. Concentrated Levels"},
    {WorkloadProfile::WideDistribution, "WIDE_DISTRIBUTION", "7. Wide Distribution"}
}};

[[nodiscard]] std::string_view profile_id(WorkloadProfile profile) noexcept;
[[nodiscard]] std::string_view profile_name(WorkloadProfile profile) noexcept;
[[nodiscard]] bool parse_profile(std::string_view text, WorkloadProfile& profile) noexcept;

class WorkloadGenerator {
public:
    [[nodiscard]] static std::vector<WorkloadCommand> generate(
        WorkloadProfile profile, std::size_t count, std::uint64_t seed = 42);
};

} // namespace atlas::bench

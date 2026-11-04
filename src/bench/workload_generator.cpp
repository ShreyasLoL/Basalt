#include "atlas/bench/workload_generator.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace atlas::bench {
namespace {

constexpr Price MID_PRICE = 10000;

const WorkloadProfileInfo* find_info(const WorkloadProfile profile) noexcept {
    const auto found = std::find_if(WORKLOAD_PROFILES.begin(), WORKLOAD_PROFILES.end(),
        [profile](const WorkloadProfileInfo& info) { return info.profile == profile; });
    return found == WORKLOAD_PROFILES.end() ? nullptr : &*found;
}

Quantity random_quantity(std::mt19937_64& rng) {
    return std::uniform_int_distribution<Quantity>(1, 100)(rng);
}

Side random_side(std::mt19937_64& rng) {
    return (rng() & 1U) == 0 ? Side::Buy : Side::Sell;
}

Price normal_price(std::mt19937_64& rng) {
    std::normal_distribution<double> distribution(static_cast<double>(MID_PRICE), 25.0);
    const double sample = std::clamp(distribution(rng), 1.0,
                                     static_cast<double>(MAX_ORDER_PRICE));
    return static_cast<Price>(std::llround(sample));
}

} // namespace

std::string_view profile_id(const WorkloadProfile profile) noexcept {
    const auto* info = find_info(profile);
    return info == nullptr ? "UNKNOWN" : info->id;
}

std::string_view profile_name(const WorkloadProfile profile) noexcept {
    const auto* info = find_info(profile);
    return info == nullptr ? "Unknown Workload" : info->name;
}

bool parse_profile(const std::string_view text, WorkloadProfile& profile) noexcept {
    for (const auto& info : WORKLOAD_PROFILES) {
        if (text == info.id) {
            profile = info.profile;
            return true;
        }
    }
    return false;
}

std::vector<WorkloadCommand> WorkloadGenerator::generate(const WorkloadProfile profile,
                                                        const std::size_t count,
                                                        const std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<WorkloadCommand> commands;
    commands.reserve(count);
    std::vector<OrderId> cancel_candidates;
    cancel_candidates.reserve(count / 2);
    OrderId next_id = 1;

    auto add_limit = [&](const Side side, const Price price, const Quantity quantity) {
        const OrderId id = next_id++;
        commands.push_back(WorkloadCommand{
            ActionType::Submit,
            OrderRequest{id, side, OrderType::Limit, price, quantity}, 0});
        cancel_candidates.push_back(id);
    };
    auto add_market = [&](const Side side, const Quantity quantity) {
        const OrderId id = next_id++;
        commands.push_back(WorkloadCommand{
            ActionType::Submit,
            OrderRequest{id, side, OrderType::Market, 0, quantity}, 0});
    };
    auto add_cancel = [&]() {
        if (cancel_candidates.empty()) return false;
        const std::size_t index = static_cast<std::size_t>(rng() % cancel_candidates.size());
        const OrderId id = cancel_candidates[index];
        cancel_candidates[index] = cancel_candidates.back();
        cancel_candidates.pop_back();
        commands.push_back(WorkloadCommand{ActionType::Cancel, {}, id});
        return true;
    };

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t draw = rng() % 200;
        switch (profile) {
        case WorkloadProfile::Balanced:
            if (draw < 85) add_limit(Side::Buy, normal_price(rng), random_quantity(rng));
            else if (draw < 170) add_limit(Side::Sell, normal_price(rng), random_quantity(rng));
            else if (draw < 180) add_market(random_side(rng), random_quantity(rng));
            else if (!add_cancel()) add_limit(random_side(rng), normal_price(rng), random_quantity(rng));
            break;
        case WorkloadProfile::BuyHeavy:
            if (draw < 150) add_limit(Side::Buy, normal_price(rng), random_quantity(rng));
            else if (draw < 180) add_limit(Side::Sell, normal_price(rng), random_quantity(rng));
            else if (draw < 190) add_market(random_side(rng), random_quantity(rng));
            else if (!add_cancel()) add_limit(Side::Buy, normal_price(rng), random_quantity(rng));
            break;
        case WorkloadProfile::SellHeavy:
            if (draw < 30) add_limit(Side::Buy, normal_price(rng), random_quantity(rng));
            else if (draw < 180) add_limit(Side::Sell, normal_price(rng), random_quantity(rng));
            else if (draw < 190) add_market(random_side(rng), random_quantity(rng));
            else if (!add_cancel()) add_limit(Side::Sell, normal_price(rng), random_quantity(rng));
            break;
        case WorkloadProfile::AggressiveMatching: {
            if (draw < 90) {
                const Side side = random_side(rng);
                const Price price = side == Side::Buy
                    ? static_cast<Price>(10010 + rng() % 41)
                    : static_cast<Price>(9950 + rng() % 41);
                add_limit(side, price, random_quantity(rng));
            } else if (draw < 140) {
                add_market(random_side(rng), random_quantity(rng));
            } else {
                const Side side = random_side(rng);
                const Price price = side == Side::Buy
                    ? static_cast<Price>(9900 + rng() % 100)
                    : static_cast<Price>(10001 + rng() % 100);
                add_limit(side, price, random_quantity(rng));
            }
            break;
        }
        case WorkloadProfile::CancelHeavy:
            if (draw < 100) {
                const Side side = random_side(rng);
                const Price offset = static_cast<Price>(100 + rng() % 100);
                add_limit(side, side == Side::Buy ? MID_PRICE - offset : MID_PRICE + offset,
                          random_quantity(rng));
            } else if (draw < 190) {
                if (!add_cancel()) {
                    add_limit(random_side(rng), MID_PRICE + 150, random_quantity(rng));
                }
            } else {
                const Side side = random_side(rng);
                add_limit(side, side == Side::Buy ? MID_PRICE + 250 : MID_PRICE - 250,
                          random_quantity(rng));
            }
            break;
        case WorkloadProfile::ConcentratedLevels:
            if (draw < 180) {
                const Side side = random_side(rng);
                const Price price = static_cast<Price>(9995 + rng() % 10);
                add_limit(side, price, random_quantity(rng));
            } else if (!add_cancel()) {
                add_limit(random_side(rng), MID_PRICE, random_quantity(rng));
            }
            break;
        case WorkloadProfile::WideDistribution:
            if (draw < 180) {
                add_limit(random_side(rng), static_cast<Price>(7500 + rng() % 5000),
                          random_quantity(rng));
            } else if (!add_cancel()) {
                add_limit(random_side(rng), static_cast<Price>(7500 + rng() % 5000),
                          random_quantity(rng));
            }
            break;
        }
    }
    return commands;
}

} // namespace atlas::bench

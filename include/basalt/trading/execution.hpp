#pragma once

#include "basalt/trading/types.hpp"

#include <string>

namespace basalt {
class MatchingEngine;
}

namespace basalt::trading {

struct FeeLine {
    FeeComponentKind kind{FeeComponentKind::Other};
    std::int64_t amount_micro{};
    friend bool operator==(const FeeLine&, const FeeLine&) = default;
};

struct FeeCalculation {
    std::int64_t total_micro{};
    std::vector<FeeLine> lines{};
};

class FeeCalculator {
public:
    [[nodiscard]] static common::Result<FeeCalculation> calculate(
        Side side, std::int64_t notional_micro, const FeeSchedule& schedule);
};

class PaperExecutionSimulator {
public:
    explicit PaperExecutionSimulator(common::InstrumentId internal_book_instrument = 0)
        : internal_book_instrument_(internal_book_instrument) {}

    [[nodiscard]] common::Result<ExecutionReport> execute(
        const OrderIntent& intent, const ExecutionMarket& market,
        const ExecutionConfig& config, basalt::MatchingEngine* internal_book = nullptr) const;

private:
    common::InstrumentId internal_book_instrument_{};
};

[[nodiscard]] std::string_view to_string(PaperExecutionModel model) noexcept;

} // namespace basalt::trading

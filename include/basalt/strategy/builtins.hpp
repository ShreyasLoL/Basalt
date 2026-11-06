#pragma once

#include "basalt/strategy/runtime.hpp"

#include <memory>
#include <string_view>

namespace basalt::strategy {

enum class BuiltinKind : std::uint8_t {
    SmaCrossover, RsiMeanReversion, Bollinger, VwapReversion, MomentumBreakout, OrderBookImbalance
};

[[nodiscard]] std::unique_ptr<IStrategy> make_builtin(BuiltinKind kind);
[[nodiscard]] std::string_view to_string(BuiltinKind kind) noexcept;

} // namespace basalt::strategy

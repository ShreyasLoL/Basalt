#include "atlas/core/types.hpp"

namespace atlas {

std::string_view to_string(const Side value) noexcept {
    switch (value) {
    case Side::Buy: return "BUY";
    case Side::Sell: return "SELL";
    }
    return "UNKNOWN";
}

std::string_view to_string(const OrderType value) noexcept {
    switch (value) {
    case OrderType::Limit: return "LIMIT";
    case OrderType::Market: return "MARKET";
    }
    return "UNKNOWN";
}

std::string_view to_string(const OrderStatus value) noexcept {
    switch (value) {
    case OrderStatus::New: return "NEW";
    case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
    case OrderStatus::Filled: return "FILLED";
    case OrderStatus::Cancelled: return "CANCELLED";
    case OrderStatus::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}

std::string_view to_string(const ActionType value) noexcept {
    switch (value) {
    case ActionType::Submit: return "SUBMIT";
    case ActionType::Cancel: return "CANCEL";
    case ActionType::Trade: return "TRADE";
    case ActionType::Reject: return "REJECT";
    }
    return "UNKNOWN";
}

} // namespace atlas

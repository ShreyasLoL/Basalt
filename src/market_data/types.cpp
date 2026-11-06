#include "basalt/market_data/types.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace basalt::market_data {
namespace {

std::string uppercase_ascii(const std::string_view text) {
    std::string value(text);
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

} // namespace

std::string_view to_string(const MarketEventKind kind) noexcept {
    switch (kind) {
    case MarketEventKind::Trade: return "TRADE";
    case MarketEventKind::Quote: return "QUOTE";
    case MarketEventKind::Depth: return "DEPTH";
    case MarketEventKind::Bar: return "BAR";
    case MarketEventKind::Status: return "STATUS";
    }
    return "UNKNOWN";
}

common::Result<MarketEventKind> parse_event_kind(const std::string_view text) {
    const std::string value = uppercase_ascii(text);
    if (value == "TRADE") return common::Result<MarketEventKind>::success(MarketEventKind::Trade);
    if (value == "QUOTE") return common::Result<MarketEventKind>::success(MarketEventKind::Quote);
    if (value == "DEPTH") return common::Result<MarketEventKind>::success(MarketEventKind::Depth);
    if (value == "BAR") return common::Result<MarketEventKind>::success(MarketEventKind::Bar);
    if (value == "STATUS") return common::Result<MarketEventKind>::success(MarketEventKind::Status);
    return common::Result<MarketEventKind>::failure(
        common::Error{"INVALID_EVENT_KIND", "Market event kind is not supported"});
}

} // namespace basalt::market_data

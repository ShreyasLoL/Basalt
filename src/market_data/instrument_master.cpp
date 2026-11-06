#include "basalt/market_data/instrument_master.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace basalt::market_data {
namespace {

std::string uppercase_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

bool valid_clock(std::string_view value) {
    if (value.size() != 5 || value[2] != ':') return false;
    const auto digit = [](const char value) { return value >= '0' && value <= '9'; };
    if (!digit(value[0]) || !digit(value[1]) || !digit(value[3]) || !digit(value[4])) return false;
    const int hour = (value[0] - '0') * 10 + value[1] - '0';
    const int minute = (value[3] - '0') * 10 + value[4] - '0';
    return hour < 24 && minute < 60;
}

bool interval_overlaps(const std::int64_t left_from, const std::optional<std::int64_t> left_to,
                       const std::int64_t right_from, const std::optional<std::int64_t> right_to) {
    const std::int64_t left_end = left_to.value_or(std::numeric_limits<std::int64_t>::max());
    const std::int64_t right_end = right_to.value_or(std::numeric_limits<std::int64_t>::max());
    return left_from < right_end && right_from < left_end;
}

common::Result<void> invalid(std::string code, std::string message) {
    return common::Result<void>::failure(common::Error{std::move(code), std::move(message)});
}

} // namespace

InstrumentMaster::InstrumentMaster(const std::uint32_t catalog_version) noexcept
    : catalog_version_(catalog_version) {}

common::Result<void> InstrumentMaster::add(InstrumentDefinition instrument) {
    if (catalog_version_ == 0) return invalid("INVALID_CATALOG_VERSION", "Catalog version must be positive");
    if (instrument.id == 0) return invalid("INVALID_INSTRUMENT_ID", "Instrument ID must be positive");
    if (to_string(instrument.exchange) == "UNKNOWN") {
        return invalid("INVALID_EXCHANGE", "Exchange must be NSE, BSE or SIM");
    }
    if (instrument.segment.empty() || instrument.segment.size() > 32 ||
        instrument.symbol.empty() || instrument.symbol.size() > 64 ||
        instrument.display_symbol.empty() || instrument.display_symbol.size() > 96) {
        return invalid("INVALID_INSTRUMENT_SYMBOL", "Segment and instrument symbols must be non-empty and bounded");
    }
    if (instrument.currency != "INR" || instrument.timezone.empty() || instrument.timezone.size() > 64) {
        return invalid("INVALID_INSTRUMENT_CURRENCY", "Instrument currency must be INR and timezone must be specified");
    }
    if (instrument.tick_size_micro <= 0 || instrument.lot_size == 0 || instrument.quantity_step == 0) {
        return invalid("INVALID_INSTRUMENT_SCALE", "Tick size, lot size and quantity step must be positive");
    }
    if (instrument.active_from_ns && instrument.active_to_ns &&
        *instrument.active_to_ns <= *instrument.active_from_ns) {
        return invalid("INVALID_ACTIVE_INTERVAL", "Instrument active interval must have positive duration");
    }
    if ((instrument.lower_price_band_micro && *instrument.lower_price_band_micro <= 0) ||
        (instrument.upper_price_band_micro && *instrument.upper_price_band_micro <= 0) ||
        (instrument.lower_price_band_micro && instrument.upper_price_band_micro &&
         *instrument.lower_price_band_micro > *instrument.upper_price_band_micro)) {
        return invalid("INVALID_PRICE_BAND", "Price bands must be positive and ordered");
    }
    for (const TradingSession& session : instrument.sessions) {
        if (!valid_clock(session.open_local) || !valid_clock(session.close_local) ||
            session.open_local == session.close_local) {
            return invalid("INVALID_TRADING_SESSION", "Session times must use valid HH:MM values");
        }
    }
    for (const ProviderSymbolMapping& mapping : instrument.provider_mappings) {
        if (mapping.provider.empty() || mapping.provider.size() > 64 ||
            mapping.provider_key.empty() || mapping.provider_key.size() > 128 ||
            mapping.valid_from_ns < 0 ||
            (mapping.valid_to_ns && *mapping.valid_to_ns <= mapping.valid_from_ns)) {
            return invalid("INVALID_PROVIDER_MAPPING", "Provider mappings require a bounded key and valid effective interval");
        }
    }

    const std::string segment = uppercase_ascii(instrument.segment);
    const std::string symbol = uppercase_ascii(instrument.symbol);
    const std::string provider_symbol = uppercase_ascii(instrument.display_symbol);
    if (std::any_of(instruments_.begin(), instruments_.end(), [&](const InstrumentDefinition& current) {
            return current.id == instrument.id ||
                   (current.exchange == instrument.exchange && uppercase_ascii(current.segment) == segment &&
                    uppercase_ascii(current.symbol) == symbol) ||
                   uppercase_ascii(current.display_symbol) == provider_symbol;
        })) {
        return invalid("DUPLICATE_INSTRUMENT", "Instrument ID, exchange symbol or display symbol already exists");
    }

    for (const ProviderSymbolMapping& mapping : instrument.provider_mappings) {
        const std::string provider = uppercase_ascii(mapping.provider);
        const auto overlaps_existing = [&](const InstrumentDefinition& current) {
            return std::any_of(current.provider_mappings.begin(), current.provider_mappings.end(),
                [&](const ProviderSymbolMapping& other) {
                    return uppercase_ascii(other.provider) == provider &&
                           other.provider_key == mapping.provider_key &&
                           interval_overlaps(other.valid_from_ns, other.valid_to_ns,
                                             mapping.valid_from_ns, mapping.valid_to_ns);
                });
        };
        if (std::any_of(instruments_.begin(), instruments_.end(), overlaps_existing)) {
            return invalid("AMBIGUOUS_PROVIDER_MAPPING", "Provider symbol mappings cannot overlap in effective time");
        }
    }

    instrument.segment = segment;
    instrument.symbol = symbol;
    for (ProviderSymbolMapping& mapping : instrument.provider_mappings) {
        mapping.provider = uppercase_ascii(mapping.provider);
    }
    instruments_.push_back(std::move(instrument));
    return common::Result<void>::success();
}

const InstrumentDefinition* InstrumentMaster::find(const common::InstrumentId id) const noexcept {
    const auto found = std::find_if(instruments_.begin(), instruments_.end(),
        [id](const InstrumentDefinition& instrument) { return instrument.id == id; });
    return found == instruments_.end() ? nullptr : &*found;
}

common::Result<common::InstrumentId> InstrumentMaster::resolve_symbol(
    const Exchange exchange, const std::string_view segment, const std::string_view symbol) const {
    const std::string canonical_segment = uppercase_ascii(segment);
    const std::string canonical_symbol = uppercase_ascii(symbol);
    for (const InstrumentDefinition& instrument : instruments_) {
        if (instrument.exchange == exchange && instrument.segment == canonical_segment &&
            instrument.symbol == canonical_symbol && instrument.active) {
            return common::Result<common::InstrumentId>::success(instrument.id);
        }
    }
    return common::Result<common::InstrumentId>::failure(
        common::Error{"UNKNOWN_INSTRUMENT", "No active instrument matches the requested exchange symbol"});
}

common::Result<common::InstrumentId> InstrumentMaster::resolve_provider(
    const std::string_view provider_name, const std::string_view provider_key,
    const std::int64_t event_time_ns) const {
    const std::string provider = uppercase_ascii(provider_name);
    const InstrumentDefinition* match = nullptr;
    for (const InstrumentDefinition& instrument : instruments_) {
        if (!instrument.active || (instrument.active_from_ns && event_time_ns < *instrument.active_from_ns) ||
            (instrument.active_to_ns && event_time_ns >= *instrument.active_to_ns)) continue;
        for (const ProviderSymbolMapping& mapping : instrument.provider_mappings) {
            if (mapping.provider == provider && mapping.provider_key == provider_key &&
                event_time_ns >= mapping.valid_from_ns &&
                (!mapping.valid_to_ns || event_time_ns < *mapping.valid_to_ns)) {
                if (match != nullptr) {
                    return common::Result<common::InstrumentId>::failure(
                        common::Error{"AMBIGUOUS_PROVIDER_MAPPING", "Provider symbol resolves to multiple instruments"});
                }
                match = &instrument;
            }
        }
    }
    if (match == nullptr) {
        return common::Result<common::InstrumentId>::failure(
            common::Error{"UNKNOWN_PROVIDER_SYMBOL", "No effective provider mapping matches the event"});
    }
    return common::Result<common::InstrumentId>::success(match->id);
}

std::vector<InstrumentDefinition> InstrumentMaster::list(
    const std::optional<Exchange> exchange, const bool active_only) const {
    std::vector<InstrumentDefinition> result;
    for (const InstrumentDefinition& instrument : instruments_) {
        if ((!exchange || instrument.exchange == *exchange) && (!active_only || instrument.active)) {
            result.push_back(instrument);
        }
    }
    return result;
}

InstrumentMaster InstrumentMaster::seeded_sim_catalog() {
    InstrumentMaster master(1);
    constexpr std::array<std::string_view, 10> symbols{
        "RELIANCE", "TCS", "HDFCBANK", "INFY", "ICICIBANK", "SBIN",
        "BHARTIARTL", "ITC", "LT", "NIFTY50"};
    common::InstrumentId id = 9'001;
    for (const std::string_view symbol : symbols) {
        InstrumentDefinition instrument;
        instrument.id = id++;
        instrument.exchange = Exchange::Sim;
        instrument.segment = symbol == "NIFTY50" ? "INDEX" : "EQ";
        instrument.symbol = std::string(symbol);
        instrument.display_symbol = "SIM:" + std::string(symbol);
        instrument.tick_size_micro = 500;
        instrument.lot_size = 1;
        instrument.quantity_step = 1;
        instrument.active = true;
        instrument.sessions = {TradingSession{"09:15", "15:30"}};
        instrument.provider_mappings = {
            ProviderSymbolMapping{"SYNTHETIC", "SIM:" + std::string(symbol), 0, std::nullopt},
            ProviderSymbolMapping{"CSV_JSON_REPLAY", "SIM:" + std::string(symbol), 0, std::nullopt}};
        static_cast<void>(master.add(std::move(instrument)));
    }
    return master;
}

std::string_view to_string(const Exchange exchange) noexcept {
    switch (exchange) {
    case Exchange::Nse: return "NSE";
    case Exchange::Bse: return "BSE";
    case Exchange::Sim: return "SIM";
    }
    return "UNKNOWN";
}

common::Result<Exchange> parse_exchange(const std::string_view text) {
    const std::string value = uppercase_ascii(text);
    if (value == "NSE") return common::Result<Exchange>::success(Exchange::Nse);
    if (value == "BSE") return common::Result<Exchange>::success(Exchange::Bse);
    if (value == "SIM") return common::Result<Exchange>::success(Exchange::Sim);
    return common::Result<Exchange>::failure(
        common::Error{"INVALID_EXCHANGE", "Exchange must be NSE, BSE or SIM"});
}

} // namespace basalt::market_data

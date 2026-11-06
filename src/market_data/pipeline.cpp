#include "basalt/market_data/pipeline.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace basalt::market_data {
namespace {

constexpr std::size_t maximum_frame_bytes = 1'048'576;
constexpr std::size_t maximum_depth_levels = 200;
constexpr std::size_t maximum_dedup_keys = 65'536;

std::string uppercase_ascii(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

ProviderError make_error(std::string code, std::string message) {
    return ProviderError{std::move(code), std::move(message)};
}

std::uint64_t timestamp_distance(const std::int64_t left, const std::int64_t right) noexcept {
    const auto left_unsigned = static_cast<std::uint64_t>(left);
    const auto right_unsigned = static_cast<std::uint64_t>(right);
    return left >= right ? left_unsigned - right_unsigned : right_unsigned - left_unsigned;
}

common::Result<common::MicroUnits, ProviderError> parse_decimal_micro(const std::string_view text) {
    if (text.empty() || text.size() > 32) {
        return common::Result<common::MicroUnits, ProviderError>::failure(
            make_error("INVALID_PRICE", "Price must be a bounded positive decimal string"));
    }
    const std::size_t point = text.find('.');
    const std::string_view whole_text = point == std::string_view::npos ? text : text.substr(0, point);
    const std::string_view fractional_text = point == std::string_view::npos
        ? std::string_view{} : text.substr(point + 1);
    if (whole_text.empty() || fractional_text.size() > 4 ||
        (point != std::string_view::npos && fractional_text.empty())) {
        return common::Result<common::MicroUnits, ProviderError>::failure(
            make_error("INVALID_PRICE_PRECISION", "Price must have at most four decimal places"));
    }
    std::uint64_t whole = 0;
    const auto [whole_end, whole_error] = std::from_chars(
        whole_text.data(), whole_text.data() + whole_text.size(), whole);
    if (whole_error != std::errc{} || whole_end != whole_text.data() + whole_text.size() ||
        whole > static_cast<std::uint64_t>(std::numeric_limits<common::MicroUnits>::max()) /
                    static_cast<std::uint64_t>(common::PRICE_SCALE)) {
        return common::Result<common::MicroUnits, ProviderError>::failure(
            make_error("INVALID_PRICE", "Price is not a valid fixed-point decimal"));
    }
    std::uint64_t fractional = 0;
    if (!fractional_text.empty()) {
        const auto [fraction_end, fraction_error] = std::from_chars(
            fractional_text.data(), fractional_text.data() + fractional_text.size(), fractional);
        if (fraction_error != std::errc{} || fraction_end != fractional_text.data() + fractional_text.size()) {
            return common::Result<common::MicroUnits, ProviderError>::failure(
                make_error("INVALID_PRICE", "Price is not a valid fixed-point decimal"));
        }
        for (std::size_t i = fractional_text.size(); i < 4; ++i) fractional *= 10;
    }
    const std::uint64_t micro = whole * static_cast<std::uint64_t>(common::PRICE_SCALE) + fractional;
    if (micro > static_cast<std::uint64_t>(std::numeric_limits<common::MicroUnits>::max())) {
        return common::Result<common::MicroUnits, ProviderError>::failure(
            make_error("PRICE_OVERFLOW", "Price exceeds the fixed-point range"));
    }
    return common::Result<common::MicroUnits, ProviderError>::success(
        static_cast<common::MicroUnits>(micro));
}

common::Result<common::Quantity, ProviderError> parse_quantity(const std::string_view text) {
    if (text.empty() || text.size() > 24) {
        return common::Result<common::Quantity, ProviderError>::failure(
            make_error("INVALID_QUANTITY", "Quantity must be a bounded unsigned integer"));
    }
    common::Quantity quantity = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), quantity);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return common::Result<common::Quantity, ProviderError>::failure(
            make_error("INVALID_QUANTITY", "Quantity must be a bounded unsigned integer"));
    }
    return common::Result<common::Quantity, ProviderError>::success(quantity);
}

std::size_t raw_size(const RawMarketEvent& raw) {
    std::size_t size = raw.provider.size() + raw.event_id.size() + raw.provider_instrument_key.size() +
                       raw.provenance_label.size();
    const auto add_optional = [&size](const std::optional<std::string>& value) {
        if (value) size += value->size();
    };
    add_optional(raw.price); add_optional(raw.quantity); add_optional(raw.bid); add_optional(raw.bid_quantity);
    add_optional(raw.ask); add_optional(raw.ask_quantity); add_optional(raw.open); add_optional(raw.high);
    add_optional(raw.low); add_optional(raw.close); add_optional(raw.volume);
    for (const RawDepthLevel& level : raw.bids) size += level.price.size() + level.quantity.size();
    for (const RawDepthLevel& level : raw.asks) size += level.price.size() + level.quantity.size();
    return size;
}

std::string dedup_key(const RawMarketEvent& raw, const common::InstrumentId instrument_id) {
    const std::string provider = uppercase_ascii(raw.provider);
    const std::string instrument = std::to_string(instrument_id);
    if (raw.provider_sequence) {
        return "SEQ|" + provider + "|" + instrument + "|" + std::to_string(*raw.provider_sequence);
    }
    if (!raw.event_id.empty()) return "ID|" + provider + "|" + instrument + "|" + raw.event_id;
    std::string key = "HASH|" + provider + "|" + instrument + "|" +
        std::to_string(raw.event_time_ns) + "|" + std::string(to_string(raw.kind));
    const auto append = [&key](const std::optional<std::string>& value) {
        key.push_back('|');
        if (value) key.append(*value);
    };
    append(raw.price); append(raw.quantity); append(raw.bid); append(raw.ask);
    append(raw.open); append(raw.high); append(raw.low); append(raw.close); append(raw.volume);
    return key;
}

} // namespace

MarketDataPipeline::MarketDataPipeline(InstrumentMaster& instruments) : instruments_(instruments) {}

void MarketDataPipeline::register_provider(std::string provider, ProviderRegistration registration) {
    provider = uppercase_ascii(provider);
    const bool valid_name = !provider.empty() && std::all_of(provider.begin(), provider.end(), [](const unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
               character == '_' || character == '-' || character == '.';
    });
    if (!valid_name || registration.stale_timeout_ns == 0 || registration.max_clock_skew_ns == 0) {
        throw std::invalid_argument("Provider registration requires a name and positive time bounds");
    }
    std::lock_guard lock(mutex_);
    providers_.insert_or_assign(std::move(provider), registration);
}

void MarketDataPipeline::set_event_sinks(DurableSink durable_sink, QuoteSink quote_sink) {
    std::lock_guard lock(mutex_);
    durable_sink_ = std::move(durable_sink);
    quote_sink_ = std::move(quote_sink);
}

void MarketDataPipeline::remember_event_id(std::string key) {
    if (seen_event_ids_.insert(key).second) seen_event_order_.push_back(std::move(key));
    while (seen_event_order_.size() > maximum_dedup_keys) {
        seen_event_ids_.erase(seen_event_order_.front());
        seen_event_order_.pop_front();
    }
}

common::Result<CanonicalMarketEvent, ProviderError> MarketDataPipeline::process(
    const RawMarketEvent& raw, const std::int64_t receive_time_mono_ns,
    const std::int64_t receive_time_utc_ns) {
    std::lock_guard lock(mutex_);
    ++stats_.received;
    const std::string provider = uppercase_ascii(raw.provider);
    const auto provider_it = providers_.find(provider);
    if (provider_it == providers_.end()) {
        ++stats_.invalid;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error("UNKNOWN_PROVIDER", "Provider must be registered before events are accepted"));
    }
    const ProviderRegistration& registration = provider_it->second;
    if (raw_size(raw) > maximum_frame_bytes) {
        ++stats_.invalid;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error("FRAME_TOO_LARGE", "Decoded market event exceeds the 1 MiB frame limit"));
    }
    if (raw.schema_version != 1 || raw.provider_instrument_key.empty() ||
        raw.provider_instrument_key.size() > 128 || raw.event_time_ns <= 0 ||
        receive_time_mono_ns < 0 || receive_time_utc_ns <= 0) {
        ++stats_.invalid;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error("INVALID_EVENT_ENVELOPE", "Event schema, instrument key or timestamps are invalid"));
    }

    const auto resolved = instruments_.resolve_provider(provider, raw.provider_instrument_key, raw.event_time_ns);
    if (!resolved) {
        ++stats_.invalid;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error(resolved.error().code, resolved.error().message));
    }
    const common::InstrumentId instrument_id = resolved.value();
    const InstrumentDefinition* instrument = instruments_.find(instrument_id);
    if (instrument == nullptr) {
        ++stats_.invalid;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error("UNKNOWN_INSTRUMENT", "Resolved instrument is absent from the catalog"));
    }
    const std::string state_key = provider + "|" + std::to_string(instrument_id);
    ProviderInstrumentState& state = states_[state_key];
    state.stale_timeout_ns = registration.stale_timeout_ns;
    const auto invalid_event = [&](std::string code, std::string message) {
        ++stats_.invalid;
        ++state.invalid_events;
        state.quality = common::FeedQualityState::Degraded;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error(std::move(code), std::move(message)));
    };

    bool sequence_gap = false;
    if (raw.provider_sequence && state.last_provider_sequence) {
        if (*raw.provider_sequence == *state.last_provider_sequence) {
            ++stats_.duplicates;
            ++state.duplicates;
            return common::Result<CanonicalMarketEvent, ProviderError>::failure(
                make_error("DUPLICATE_EVENT", "Provider sequence was already processed"));
        }
        if (*raw.provider_sequence < *state.last_provider_sequence) {
            ++stats_.out_of_order;
            ++state.out_of_order;
            state.quality = common::FeedQualityState::Degraded;
            return common::Result<CanonicalMarketEvent, ProviderError>::failure(
                make_error("OUT_OF_ORDER_EVENT", "Provider sequence regressed"));
        }
        sequence_gap = *raw.provider_sequence - *state.last_provider_sequence > 1;
    }
    if (state.last_event_time_ns && raw.event_time_ns < *state.last_event_time_ns) {
        ++stats_.out_of_order;
        ++state.out_of_order;
        state.quality = common::FeedQualityState::Degraded;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error("OUT_OF_ORDER_EVENT", "Provider event timestamp regressed"));
    }

    const std::string key = dedup_key(raw, instrument_id);
    if (!raw.provider_sequence && seen_event_ids_.contains(key)) {
        ++stats_.duplicates;
        ++state.duplicates;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error("DUPLICATE_EVENT", "Event ID or normalized payload was already processed"));
    }

    CanonicalMarketEvent event;
    event.event_id = raw.event_id.empty() ? key : raw.event_id;
    event.instrument_id = instrument_id;
    event.provider = provider;
    event.kind = raw.kind;
    event.event_time_ns = raw.event_time_ns;
    event.receive_time_utc_ns = receive_time_utc_ns;
    event.receive_time_mono_ns = receive_time_mono_ns;
    event.provider_sequence = raw.provider_sequence;
    event.source = registration.source;
    event.sequence_gap = sequence_gap;
    event.reconnect_generation = state.reconnect_generation;
    event.provenance_label = raw.provenance_label;

    std::optional<ProviderError> conversion_error;
    const auto convert_price = [&](const std::optional<std::string>& input, const std::string_view field,
                                   std::optional<common::MicroUnits>& output,
                                   std::optional<common::PriceTicks>& ticks) {
        if (!input) return;
        const auto parsed = parse_decimal_micro(*input);
        if (!parsed) { conversion_error = parsed.error(); return; }
        if (parsed.value() <= 0) {
            conversion_error = make_error("INVALID_PRICE", std::string(field) + " must be positive");
            return;
        }
        if (parsed.value() % instrument->tick_size_micro != 0) {
            conversion_error = make_error("OFF_TICK_PRICE", std::string(field) + " does not align to the instrument tick size");
            return;
        }
        if ((instrument->lower_price_band_micro && parsed.value() < *instrument->lower_price_band_micro) ||
            (instrument->upper_price_band_micro && parsed.value() > *instrument->upper_price_band_micro)) {
            conversion_error = make_error("OUTSIDE_PRICE_BAND", std::string(field) + " is outside the instrument price band");
            return;
        }
        output = parsed.value();
        ticks = parsed.value() / instrument->tick_size_micro;
    };
    const auto convert_quantity = [&](const std::optional<std::string>& input, const std::string_view field,
                                      std::optional<common::Quantity>& output, const bool positive) {
        if (!input) return;
        const auto parsed = parse_quantity(*input);
        if (!parsed) { conversion_error = parsed.error(); return; }
        if ((positive && (parsed.value() == 0 || parsed.value() % instrument->lot_size != 0)) ||
            parsed.value() % instrument->quantity_step != 0) {
            conversion_error = make_error("INVALID_QUANTITY", std::string(field) + " must be positive and align to quantity step");
            return;
        }
        output = parsed.value();
    };

    convert_price(raw.price, "price", event.price_micro, event.price_ticks);
    convert_price(raw.bid, "bid", event.bid_micro, event.bid_ticks);
    convert_price(raw.ask, "ask", event.ask_micro, event.ask_ticks);
    convert_quantity(raw.quantity, "quantity", event.quantity, raw.kind == MarketEventKind::Trade);
    convert_quantity(raw.bid_quantity, "bid quantity", event.bid_quantity, false);
    convert_quantity(raw.ask_quantity, "ask quantity", event.ask_quantity, false);
    if (conversion_error) return invalid_event(conversion_error->code, conversion_error->message);

    if (raw.bids.size() > maximum_depth_levels || raw.asks.size() > maximum_depth_levels) {
        return invalid_event("DEPTH_LIMIT_EXCEEDED", "Depth event exceeds the 200 level limit per side");
    }
    const auto convert_levels = [&](const std::vector<RawDepthLevel>& source,
                                    std::vector<MarketDepthLevel>& destination,
                                    const bool bids) -> bool {
        destination.reserve(source.size());
        for (const RawDepthLevel& raw_level : source) {
            const auto price = parse_decimal_micro(raw_level.price);
            const auto quantity = parse_quantity(raw_level.quantity);
            if (!price || !quantity || price.value() <= 0 || quantity.value() == 0 ||
                price.value() % instrument->tick_size_micro != 0 ||
                quantity.value() % instrument->quantity_step != 0 ||
                quantity.value() % instrument->lot_size != 0) {
                conversion_error = make_error("INVALID_DEPTH_LEVEL", "Depth price and quantity must be positive and aligned to instrument rules");
                return false;
            }
            const common::PriceTicks ticks = price.value() / instrument->tick_size_micro;
            if (!destination.empty() &&
                ((bids && ticks >= destination.back().price_ticks) ||
                 (!bids && ticks <= destination.back().price_ticks))) {
                conversion_error = make_error("UNSORTED_DEPTH", "Bid levels must descend and ask levels must ascend");
                return false;
            }
            destination.push_back(MarketDepthLevel{price.value(), ticks, quantity.value()});
        }
        return true;
    };
    if (!convert_levels(raw.bids, event.bids, true) || !convert_levels(raw.asks, event.asks, false)) {
        return invalid_event(conversion_error->code, conversion_error->message);
    }

    if (raw.kind == MarketEventKind::Trade) {
        if (!event.price_micro || !event.quantity) {
            return invalid_event("MISSING_TRADE_FIELDS", "Trade events require a positive price and quantity");
        }
    } else if (raw.kind == MarketEventKind::Quote) {
        if (!event.price_micro && !event.bid_micro && !event.ask_micro) {
            return invalid_event("MISSING_QUOTE_FIELDS", "Quote events require a last price or bid/ask value");
        }
    } else if (raw.kind == MarketEventKind::Depth) {
        if (event.bids.empty() && event.asks.empty()) {
            return invalid_event("MISSING_DEPTH_FIELDS", "Depth events require at least one bid or ask level");
        }
    } else if (raw.kind == MarketEventKind::Bar) {
        OhlcvBar bar;
        const auto convert_bar_price = [&](const std::optional<std::string>& text,
                                           common::MicroUnits& value) -> bool {
            const auto parsed = text ? parse_decimal_micro(*text)
                                     : common::Result<common::MicroUnits, ProviderError>::failure(
                                           make_error("MISSING_BAR_FIELDS", "Bar events require OHLC and volume fields"));
            if (!parsed || parsed.value() <= 0 || parsed.value() % instrument->tick_size_micro != 0) {
                conversion_error = parsed ? make_error("INVALID_BAR_PRICE", "Bar prices must be positive and tick aligned") : parsed.error();
                return false;
            }
            value = parsed.value();
            return true;
        };
        if (!convert_bar_price(raw.open, bar.open_micro) || !convert_bar_price(raw.high, bar.high_micro) ||
            !convert_bar_price(raw.low, bar.low_micro) || !convert_bar_price(raw.close, bar.close_micro)) {
            return invalid_event(conversion_error->code, conversion_error->message);
        }
        if (!raw.volume) return invalid_event("MISSING_BAR_FIELDS", "Bar events require an integer volume");
        const auto volume = parse_quantity(*raw.volume);
        if (!volume) return invalid_event(volume.error().code, volume.error().message);
        bar.volume = volume.value();
        if (bar.high_micro < std::max(bar.open_micro, bar.close_micro) ||
            bar.low_micro > std::min(bar.open_micro, bar.close_micro) || bar.low_micro > bar.high_micro) {
            return invalid_event("INVALID_OHLC", "Bar high/low values do not contain the open and close");
        }
        event.bar = bar;
    } else if (raw.kind != MarketEventKind::Status) {
        return invalid_event("INVALID_EVENT_KIND", "Market event kind is not recognized");
    }

    const bool clock_skew = timestamp_distance(receive_time_utc_ns, raw.event_time_ns) >
                             registration.max_clock_skew_ns;
    const std::uint64_t age_ns = receive_time_utc_ns >= raw.event_time_ns
        ? timestamp_distance(receive_time_utc_ns, raw.event_time_ns) : 0;
    const bool stale = age_ns > registration.stale_timeout_ns;
    const bool crossed_quote = event.bid_micro && event.ask_micro && *event.bid_micro > *event.ask_micro;
    event.clock_skew = clock_skew;
    event.quality = stale ? common::FeedQualityState::Stale
        : (sequence_gap || clock_skew || crossed_quote ? common::FeedQualityState::Degraded
                                                       : common::FeedQualityState::Ok);
    if (sequence_gap) { ++stats_.sequence_gaps; ++state.sequence_gaps; }
    if (clock_skew) ++stats_.clock_skew_events;
    if (stale) ++stats_.stale_events;

    if (durable_sink_ && !durable_sink_(event)) {
        ++stats_.durable_backpressure;
        state.quality = common::FeedQualityState::Degraded;
        return common::Result<CanonicalMarketEvent, ProviderError>::failure(
            make_error("DURABLE_QUEUE_FULL", "Durable event channel is full; provider must backpressure or stop"));
    }

    if (!raw.provider_sequence) remember_event_id(key);
    if (raw.provider_sequence) state.last_provider_sequence = raw.provider_sequence;
    state.last_event_time_ns = raw.event_time_ns;
    state.last_receive_time_mono_ns = receive_time_mono_ns;
    state.quality = event.quality;
    ++stats_.accepted;
    if (event.kind == MarketEventKind::Quote && quote_sink_ && !quote_sink_(event)) {
        ++stats_.ui_queue_overflows;
    }
    return common::Result<CanonicalMarketEvent, ProviderError>::success(std::move(event));
}

void MarketDataPipeline::note_reconnect(const std::string_view provider_name,
                                        const std::optional<common::InstrumentId> instrument_id) {
    const std::string provider = uppercase_ascii(provider_name);
    std::lock_guard lock(mutex_);
    for (auto& [key, state] : states_) {
        if (key.starts_with(provider + "|") &&
            (!instrument_id || key == provider + "|" + std::to_string(*instrument_id))) {
            ++state.reconnect_generation;
            state.quality = common::FeedQualityState::Degraded;
        }
    }
}

std::vector<FeedHealthSnapshot> MarketDataPipeline::health(const std::int64_t now_mono_ns) const {
    std::lock_guard lock(mutex_);
    std::vector<FeedHealthSnapshot> result;
    result.reserve(states_.size());
    for (const auto& [key, state] : states_) {
        const std::size_t separator = key.find('|');
        const std::string provider = key.substr(0, separator);
        const common::InstrumentId instrument_id = static_cast<common::InstrumentId>(
            std::stoull(key.substr(separator + 1)));
        common::FeedQualityState quality = state.quality;
        if (state.last_receive_time_mono_ns && now_mono_ns >= *state.last_receive_time_mono_ns &&
            static_cast<std::uint64_t>(now_mono_ns - *state.last_receive_time_mono_ns) > state.stale_timeout_ns) {
            quality = common::FeedQualityState::Stale;
        }
        result.push_back(FeedHealthSnapshot{
            provider, instrument_id, quality, state.last_event_time_ns, state.last_receive_time_mono_ns,
            state.last_provider_sequence.value_or(0), state.sequence_gaps, state.duplicates,
            state.out_of_order, state.invalid_events, state.reconnect_generation});
    }
    return result;
}

PipelineStats MarketDataPipeline::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

} // namespace basalt::market_data

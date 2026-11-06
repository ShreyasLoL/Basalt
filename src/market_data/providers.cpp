#include "basalt/market_data/providers.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace basalt::market_data {
namespace {

using json = nlohmann::json;
constexpr std::size_t maximum_input_line_bytes = 1'048'576;
constexpr std::size_t maximum_replay_events = 1'000'000;
constexpr std::size_t maximum_provider_subscriptions = 1'000;

ProviderError make_error(std::string code, std::string message) {
    return ProviderError{std::move(code), std::move(message)};
}

std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

bool parse_signed(const std::string_view text, std::int64_t& value) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool parse_unsigned(const std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

common::Result<std::vector<std::string>, ProviderError> split_csv(const std::string_view line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char character = line[i];
        if (character == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(trim(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    if (quoted) {
        return common::Result<std::vector<std::string>, ProviderError>::failure(
            make_error("INVALID_CSV", "CSV contains an unterminated quoted field"));
    }
    fields.push_back(trim(field));
    return common::Result<std::vector<std::string>, ProviderError>::success(std::move(fields));
}

void set_optional(std::optional<std::string>& destination, const std::string& value) {
    if (!value.empty()) destination = value;
}

common::Result<RawMarketEvent, ProviderError> parse_csv_row(
    const std::vector<std::string>& headers, const std::vector<std::string>& fields,
    const std::size_t line_number) {
    if (fields.size() != headers.size()) {
        return common::Result<RawMarketEvent, ProviderError>::failure(make_error(
            "INVALID_CSV_ROW", "CSV line " + std::to_string(line_number) + " has the wrong field count"));
    }
    std::map<std::string, std::string> values;
    for (std::size_t i = 0; i < headers.size(); ++i) values.emplace(headers[i], fields[i]);
    const auto required = [&](const std::string& name) -> const std::string* {
        const auto found = values.find(name);
        return found == values.end() || found->second.empty() ? nullptr : &found->second;
    };
    const std::string* instrument_key = required("provider_instrument_key");
    const std::string* kind_text = required("kind");
    const std::string* event_time_text = required("event_time_ns");
    if (instrument_key == nullptr || kind_text == nullptr || event_time_text == nullptr) {
        return common::Result<RawMarketEvent, ProviderError>::failure(make_error(
            "MISSING_CSV_FIELDS", "CSV row requires provider_instrument_key, kind and event_time_ns"));
    }
    RawMarketEvent raw;
    raw.provider = "CSV_JSON_REPLAY";
    raw.provider_instrument_key = *instrument_key;
    const auto kind = parse_event_kind(*kind_text);
    if (!kind) return common::Result<RawMarketEvent, ProviderError>::failure(
        make_error(kind.error().code, "CSV line " + std::to_string(line_number) + ": " + kind.error().message));
    raw.kind = kind.value();
    if (!parse_signed(*event_time_text, raw.event_time_ns) || raw.event_time_ns <= 0) {
        return common::Result<RawMarketEvent, ProviderError>::failure(make_error(
            "INVALID_EVENT_TIME", "CSV line " + std::to_string(line_number) + " has an invalid UTC nanosecond timestamp"));
    }
    if (const std::string* event_id = required("event_id")) raw.event_id = *event_id;
    if (const std::string* sequence = required("provider_sequence")) {
        std::uint64_t parsed = 0;
        if (!parse_unsigned(*sequence, parsed)) return common::Result<RawMarketEvent, ProviderError>::failure(
            make_error("INVALID_PROVIDER_SEQUENCE", "CSV line " + std::to_string(line_number) + " has an invalid provider sequence"));
        raw.provider_sequence = parsed;
    }
    for (const auto& [column, destination] : std::array<std::pair<std::string_view, std::optional<std::string>*>, 11>{
            std::pair{"price", &raw.price}, {"quantity", &raw.quantity}, {"bid", &raw.bid},
            {"bid_quantity", &raw.bid_quantity}, {"ask", &raw.ask}, {"ask_quantity", &raw.ask_quantity},
            {"open", &raw.open}, {"high", &raw.high}, {"low", &raw.low},
            {"close", &raw.close}, {"volume", &raw.volume}}) {
        const auto found = values.find(std::string(column));
        if (found != values.end()) set_optional(*destination, found->second);
    }
    if (const std::string* label = required("provenance_label")) raw.provenance_label = *label;
    return common::Result<RawMarketEvent, ProviderError>::success(std::move(raw));
}

common::Result<std::string, ProviderError> json_text(const json& object, const char* key,
                                                     const bool required) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) {
        if (required) return common::Result<std::string, ProviderError>::failure(
            make_error("MISSING_JSON_FIELD", std::string("JSON replay row requires ") + key));
        return common::Result<std::string, ProviderError>::success({});
    }
    if (found->is_string()) return common::Result<std::string, ProviderError>::success(found->get<std::string>());
    if (found->is_number_integer() || found->is_number_unsigned() || found->is_number_float()) {
        return common::Result<std::string, ProviderError>::success(found->dump());
    }
    return common::Result<std::string, ProviderError>::failure(
        make_error("INVALID_JSON_FIELD", std::string("JSON replay field has invalid type: ") + key));
}

common::Result<RawMarketEvent, ProviderError> parse_json_event(const json& object,
                                                               const std::size_t line_number) {
    if (!object.is_object()) return common::Result<RawMarketEvent, ProviderError>::failure(
        make_error("INVALID_JSON_ROW", "JSON replay line " + std::to_string(line_number) + " must be an object"));
    RawMarketEvent raw;
    raw.provider = "CSV_JSON_REPLAY";
    const auto instrument = json_text(object, "provider_instrument_key", true);
    const auto kind_text = json_text(object, "kind", true);
    const auto event_time = json_text(object, "event_time_ns", true);
    if (!instrument || !kind_text || !event_time) {
        const ProviderError& error = !instrument ? instrument.error() : (!kind_text ? kind_text.error() : event_time.error());
        return common::Result<RawMarketEvent, ProviderError>::failure(
            make_error(error.code, "JSON replay line " + std::to_string(line_number) + ": " + error.message));
    }
    raw.provider_instrument_key = instrument.value();
    const auto kind = parse_event_kind(kind_text.value());
    if (!kind) return common::Result<RawMarketEvent, ProviderError>::failure(
        make_error(kind.error().code, "JSON replay line " + std::to_string(line_number) + ": " + kind.error().message));
    raw.kind = kind.value();
    if (!parse_signed(event_time.value(), raw.event_time_ns) || raw.event_time_ns <= 0) {
        return common::Result<RawMarketEvent, ProviderError>::failure(make_error(
            "INVALID_EVENT_TIME", "JSON replay line " + std::to_string(line_number) + " has an invalid event_time_ns"));
    }
    const auto optional_field = [&](const char* key, std::optional<std::string>& destination) -> bool {
        const auto value = json_text(object, key, false);
        if (!value) return false;
        if (!value.value().empty()) destination = value.value();
        return true;
    };
    const auto event_id = json_text(object, "event_id", false);
    const auto sequence = json_text(object, "provider_sequence", false);
    if (!event_id || !sequence) return common::Result<RawMarketEvent, ProviderError>::failure(
        make_error("INVALID_JSON_FIELD", "JSON replay event_id or provider_sequence has an invalid type"));
    raw.event_id = event_id.value();
    if (!sequence.value().empty()) {
        std::uint64_t parsed = 0;
        if (!parse_unsigned(sequence.value(), parsed)) return common::Result<RawMarketEvent, ProviderError>::failure(
            make_error("INVALID_PROVIDER_SEQUENCE", "JSON replay line has an invalid provider_sequence"));
        raw.provider_sequence = parsed;
    }
    for (const auto& [key, destination] : std::array<std::pair<const char*, std::optional<std::string>*>, 11>{
            std::pair{"price", &raw.price}, {"quantity", &raw.quantity}, {"bid", &raw.bid},
            {"bid_quantity", &raw.bid_quantity}, {"ask", &raw.ask}, {"ask_quantity", &raw.ask_quantity},
            {"open", &raw.open}, {"high", &raw.high}, {"low", &raw.low},
            {"close", &raw.close}, {"volume", &raw.volume}}) {
        if (!optional_field(key, *destination)) return common::Result<RawMarketEvent, ProviderError>::failure(
            make_error("INVALID_JSON_FIELD", std::string("JSON replay field has invalid type: ") + key));
    }
    const auto provenance = json_text(object, "provenance_label", false);
    if (!provenance) return common::Result<RawMarketEvent, ProviderError>::failure(provenance.error());
    raw.provenance_label = provenance.value();
    const auto parse_levels = [&](const char* key, std::vector<RawDepthLevel>& levels) -> bool {
        const auto found = object.find(key);
        if (found == object.end() || found->is_null()) return true;
        if (!found->is_array() || found->size() > 200) return false;
        for (const json& level : *found) {
            if (!level.is_object()) return false;
            const auto price = json_text(level, "price", true);
            const auto quantity = json_text(level, "quantity", true);
            if (!price || !quantity) return false;
            levels.push_back(RawDepthLevel{price.value(), quantity.value()});
        }
        return true;
    };
    if (!parse_levels("bids", raw.bids) || !parse_levels("asks", raw.asks)) {
        return common::Result<RawMarketEvent, ProviderError>::failure(
            make_error("INVALID_JSON_DEPTH", "JSON replay depth must contain bounded price/quantity objects"));
    }
    return common::Result<RawMarketEvent, ProviderError>::success(std::move(raw));
}

std::string format_micro(const std::int64_t value) {
    const std::int64_t whole = value / common::PRICE_SCALE;
    std::int64_t fractional = value % common::PRICE_SCALE;
    std::string fraction = std::to_string(fractional);
    fraction.insert(0, 4 - fraction.size(), '0');
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    return fraction.empty() ? std::to_string(whole) : std::to_string(whole) + "." + fraction;
}

bool config_matches(const ProviderConfig& config, const std::string_view provider) {
    return config.enabled && config.dry_run &&
           (config.provider_name.empty() || uppercase_ascii(config.provider_name) == provider);
}

} // namespace

DeterministicSyntheticProvider::DeterministicSyntheticProvider(
    const InstrumentMaster& instruments, const std::int64_t base_event_time_ns)
    : instruments_(instruments), base_event_time_ns_(base_event_time_ns) {}

common::Result<void, ProviderError> DeterministicSyntheticProvider::connect(const ProviderConfig& config) {
    if (!config_matches(config, name())) return common::Result<void, ProviderError>::failure(
        make_error("PROVIDER_DISABLED", "Synthetic provider requires enabled dry-run configuration"));
    connected_ = true;
    return common::Result<void, ProviderError>::success();
}

common::Result<void, ProviderError> DeterministicSyntheticProvider::subscribe(
    const std::span<const common::InstrumentId> instrument_ids) {
    if (!connected_) return common::Result<void, ProviderError>::failure(
        make_error("PROVIDER_DISCONNECTED", "Synthetic provider is not connected"));
    if (instrument_ids.size() > maximum_provider_subscriptions) return common::Result<void, ProviderError>::failure(
        make_error("SUBSCRIPTION_LIMIT", "Synthetic provider supports at most 1000 instruments"));
    subscriptions_.clear();
    for (const common::InstrumentId id : instrument_ids) {
        const InstrumentDefinition* instrument = instruments_.find(id);
        if (instrument == nullptr || instrument->exchange != Exchange::Sim || !instrument->active) {
            return common::Result<void, ProviderError>::failure(
                make_error("INVALID_SYNTHETIC_INSTRUMENT", "Synthetic feed accepts active SIM instruments only"));
        }
        if (std::find(subscriptions_.begin(), subscriptions_.end(), id) == subscriptions_.end()) {
            subscriptions_.push_back(id);
        }
    }
    return common::Result<void, ProviderError>::success();
}

void DeterministicSyntheticProvider::set_event_sink(EventSink sink) { event_sink_ = std::move(sink); }
void DeterministicSyntheticProvider::stop() noexcept { connected_ = false; subscriptions_.clear(); }
ProviderCapabilities DeterministicSyntheticProvider::capabilities() const noexcept {
    return ProviderCapabilities{true, true, true, true, maximum_provider_subscriptions};
}
std::string_view DeterministicSyntheticProvider::name() const noexcept { return "SYNTHETIC"; }

common::Result<std::size_t, ProviderError> DeterministicSyntheticProvider::step(const std::size_t cycles) {
    if (!connected_) return common::Result<std::size_t, ProviderError>::failure(
        make_error("PROVIDER_DISCONNECTED", "Synthetic provider is not connected"));
    if (!event_sink_) return common::Result<std::size_t, ProviderError>::failure(
        make_error("EVENT_SINK_MISSING", "Synthetic provider requires an event sink"));
    if (cycles == 0 || cycles > 100'000) return common::Result<std::size_t, ProviderError>::failure(
        make_error("INVALID_STEP_COUNT", "Synthetic step count must be between 1 and 100000"));
    if (subscriptions_.size() != 0 && cycles > std::numeric_limits<std::size_t>::max() / subscriptions_.size()) {
        return common::Result<std::size_t, ProviderError>::failure(
            make_error("STEP_COUNT_OVERFLOW", "Synthetic step would exceed the bounded event count"));
    }
    const std::size_t maximum_events = cycles * subscriptions_.size();
    if (maximum_events > 1'000'000 || cycle_ > std::numeric_limits<std::uint64_t>::max() - cycles) {
        return common::Result<std::size_t, ProviderError>::failure(
            make_error("STEP_COUNT_OVERFLOW", "Synthetic event sequence or timestamp reached its bound"));
    }
    std::size_t emitted = 0;
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        ++cycle_;
        std::int64_t event_time_ns = 0;
        if (base_event_time_ns_ > 0) {
            const auto offset = common::checked_multiply(static_cast<std::int64_t>(cycle_), 1'000'000'000LL);
            const auto timestamp = offset ? common::checked_add(base_event_time_ns_, offset.value())
                                          : common::Result<std::int64_t>::failure(offset.error());
            if (!timestamp) return common::Result<std::size_t, ProviderError>::failure(
                make_error("STEP_COUNT_OVERFLOW", "Synthetic event timestamp exceeded the signed nanosecond range"));
            event_time_ns = timestamp.value();
        } else {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            event_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            if (event_time_ns <= last_event_time_ns_) {
                if (last_event_time_ns_ == std::numeric_limits<std::int64_t>::max()) {
                    return common::Result<std::size_t, ProviderError>::failure(
                        make_error("STEP_COUNT_OVERFLOW", "Synthetic wall clock exceeded the signed nanosecond range"));
                }
                event_time_ns = last_event_time_ns_ + 1;
            }
        }
        last_event_time_ns_ = event_time_ns;
        for (const common::InstrumentId id : subscriptions_) {
            const InstrumentDefinition* instrument = instruments_.find(id);
            if (instrument == nullptr) continue;
            std::uint64_t& instrument_sequence = sequence_by_instrument_[id];
            if (instrument_sequence == std::numeric_limits<std::uint64_t>::max()) {
                return common::Result<std::size_t, ProviderError>::failure(
                    make_error("SEQUENCE_OVERFLOW", "Synthetic provider sequence reached its bound"));
            }
            const std::uint64_t sequence = ++instrument_sequence;
            const std::int64_t tick = instrument->tick_size_micro;
            const std::int64_t offset = static_cast<std::int64_t>(sequence % 11) - 5;
            const std::int64_t price = 1'000'000 + static_cast<std::int64_t>(id % 101) * tick + offset * tick;
            RawMarketEvent event;
            event.provider = std::string(name());
            event.event_id = "SIM-" + std::to_string(id) + "-" + std::to_string(sequence);
            event.provider_instrument_key = "SIM:" + instrument->symbol;
            event.kind = MarketEventKind::Quote;
            event.event_time_ns = event_time_ns;
            event.provider_sequence = sequence;
            event.price = format_micro(price);
            event.bid = format_micro(price - tick);
            event.ask = format_micro(price + tick);
            event.bid_quantity = std::to_string(10 + sequence % 90);
            event.ask_quantity = std::to_string(10 + sequence % 90);
            event.provenance_label = "Deterministic local synthetic fixture; not exchange data";
            event_sink_(std::move(event));
            ++emitted;
        }
    }
    return common::Result<std::size_t, ProviderError>::success(emitted);
}

CsvJsonReplayProvider::CsvJsonReplayProvider(const InstrumentMaster& instruments) : instruments_(instruments) {}

common::Result<void, ProviderError> CsvJsonReplayProvider::connect(const ProviderConfig& config) {
    if (!config_matches(config, name())) return common::Result<void, ProviderError>::failure(
        make_error("PROVIDER_DISABLED", "Local replay provider requires enabled dry-run configuration"));
    connected_ = true;
    return common::Result<void, ProviderError>::success();
}

common::Result<void, ProviderError> CsvJsonReplayProvider::subscribe(
    const std::span<const common::InstrumentId> instrument_ids) {
    if (!connected_) return common::Result<void, ProviderError>::failure(
        make_error("PROVIDER_DISCONNECTED", "Replay provider is not connected"));
    if (instrument_ids.size() > maximum_provider_subscriptions) return common::Result<void, ProviderError>::failure(
        make_error("SUBSCRIPTION_LIMIT", "Replay provider supports at most 1000 instruments"));
    subscriptions_.clear();
    for (const common::InstrumentId id : instrument_ids) {
        const InstrumentDefinition* instrument = instruments_.find(id);
        if (instrument == nullptr || !instrument->active) return common::Result<void, ProviderError>::failure(
            make_error("UNKNOWN_INSTRUMENT", "Replay subscription contains an unknown or inactive instrument"));
        subscriptions_.insert(id);
    }
    return common::Result<void, ProviderError>::success();
}

void CsvJsonReplayProvider::set_event_sink(EventSink sink) { event_sink_ = std::move(sink); }
void CsvJsonReplayProvider::stop() noexcept { connected_ = false; subscriptions_.clear(); }
ProviderCapabilities CsvJsonReplayProvider::capabilities() const noexcept {
    return ProviderCapabilities{true, true, true, true, maximum_provider_subscriptions};
}
std::string_view CsvJsonReplayProvider::name() const noexcept { return "CSV_JSON_REPLAY"; }

common::Result<std::size_t, ProviderError> CsvJsonReplayProvider::load_csv(std::istream& input) {
    std::vector<RawMarketEvent> parsed_events;
    std::string line;
    std::size_t line_number = 0;
    std::vector<std::string> headers;
    std::map<std::string, std::size_t> header_indices;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > maximum_input_line_bytes) return common::Result<std::size_t, ProviderError>::failure(
            make_error("FRAME_TOO_LARGE", "CSV replay line exceeds the 1 MiB frame limit"));
        if (line_number == 1 && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        if (trim(line).empty()) continue;
        const auto fields = split_csv(line);
        if (!fields) return common::Result<std::size_t, ProviderError>::failure(fields.error());
        if (headers.empty()) {
            headers = fields.value();
            for (std::size_t i = 0; i < headers.size(); ++i) {
                headers[i] = lowercase_ascii(trim(headers[i]));
                if (!header_indices.emplace(headers[i], i).second) return common::Result<std::size_t, ProviderError>::failure(
                    make_error("DUPLICATE_CSV_HEADER", "CSV header contains a duplicate column"));
            }
            if (!header_indices.contains("provider_instrument_key") || !header_indices.contains("kind") ||
                !header_indices.contains("event_time_ns")) return common::Result<std::size_t, ProviderError>::failure(
                    make_error("MISSING_CSV_HEADER", "CSV requires provider_instrument_key, kind and event_time_ns columns"));
            continue;
        }
        const auto event = parse_csv_row(headers, fields.value(), line_number);
        if (!event) return common::Result<std::size_t, ProviderError>::failure(event.error());
        parsed_events.push_back(event.value());
        if (parsed_events.size() > maximum_replay_events) return common::Result<std::size_t, ProviderError>::failure(
            make_error("REPLAY_EVENT_LIMIT", "Replay file exceeds one million events"));
    }
    if (input.bad()) return common::Result<std::size_t, ProviderError>::failure(
        make_error("CSV_READ_FAILED", "Failed while reading the CSV replay stream"));
    if (headers.empty()) return common::Result<std::size_t, ProviderError>::failure(
        make_error("EMPTY_CSV", "CSV replay stream has no header"));
    events_ = std::move(parsed_events);
    cursor_ = 0;
    return common::Result<std::size_t, ProviderError>::success(events_.size());
}

common::Result<std::size_t, ProviderError> CsvJsonReplayProvider::load_json_lines(std::istream& input) {
    std::vector<RawMarketEvent> parsed_events;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > maximum_input_line_bytes) return common::Result<std::size_t, ProviderError>::failure(
            make_error("FRAME_TOO_LARGE", "JSON replay line exceeds the 1 MiB frame limit"));
        if (trim(line).empty()) continue;
        try {
            const auto event = parse_json_event(json::parse(line), line_number);
            if (!event) return common::Result<std::size_t, ProviderError>::failure(event.error());
            parsed_events.push_back(event.value());
        } catch (const json::exception& error) {
            return common::Result<std::size_t, ProviderError>::failure(make_error(
                "INVALID_JSON", "JSON replay line " + std::to_string(line_number) + " is invalid: " + error.what()));
        }
        if (parsed_events.size() > maximum_replay_events) return common::Result<std::size_t, ProviderError>::failure(
            make_error("REPLAY_EVENT_LIMIT", "Replay file exceeds one million events"));
    }
    if (input.bad()) return common::Result<std::size_t, ProviderError>::failure(
        make_error("JSON_READ_FAILED", "Failed while reading the JSON replay stream"));
    events_ = std::move(parsed_events);
    cursor_ = 0;
    return common::Result<std::size_t, ProviderError>::success(events_.size());
}

common::Result<std::size_t, ProviderError> CsvJsonReplayProvider::step(const std::size_t maximum_events) {
    if (!connected_) return common::Result<std::size_t, ProviderError>::failure(
        make_error("PROVIDER_DISCONNECTED", "Replay provider is not connected"));
    if (!event_sink_) return common::Result<std::size_t, ProviderError>::failure(
        make_error("EVENT_SINK_MISSING", "Replay provider requires an event sink"));
    if (maximum_events == 0 || maximum_events > 100'000) return common::Result<std::size_t, ProviderError>::failure(
        make_error("INVALID_STEP_COUNT", "Replay step count must be between 1 and 100000"));
    std::size_t emitted = 0;
    while (cursor_ < events_.size() && emitted < maximum_events) {
        RawMarketEvent event = events_[cursor_++];
        if (!subscriptions_.empty()) {
            const auto resolved = instruments_.resolve_provider(name(), event.provider_instrument_key, event.event_time_ns);
            if (!resolved) return common::Result<std::size_t, ProviderError>::failure(
                make_error(resolved.error().code, resolved.error().message));
            if (!subscriptions_.contains(resolved.value())) continue;
        }
        event_sink_(std::move(event));
        ++emitted;
    }
    return common::Result<std::size_t, ProviderError>::success(emitted);
}

void CsvJsonReplayProvider::rewind() noexcept { cursor_ = 0; }

common::Result<void, ProviderError> DiagnosticOnlyProvider::connect(const ProviderConfig& config) {
    if (!config.enabled) return common::Result<void, ProviderError>::failure(
        make_error("PROVIDER_DISABLED", "Provider diagnostic adapter is disabled"));
    if (!config.dry_run) return common::Result<void, ProviderError>::failure(
        make_error("LIVE_PROVIDER_DISABLED", "Live provider authentication and network access are not implemented"));
    if (!config.provider_name.empty() && uppercase_ascii(config.provider_name) != name()) {
        return common::Result<void, ProviderError>::failure(
            make_error("PROVIDER_CONFIG_MISMATCH", "Provider configuration name does not match the diagnostic adapter"));
    }
    return common::Result<void, ProviderError>::failure(make_error(
        "DIAGNOSTIC_ONLY", std::string(name()) + " is diagnostic-only and remains DISCONNECTED; no credentials are read and no network connection is made"));
}

common::Result<void, ProviderError> DiagnosticOnlyProvider::subscribe(
    const std::span<const common::InstrumentId> instrument_ids) {
    static_cast<void>(instrument_ids);
    return common::Result<void, ProviderError>::failure(
        make_error("PROVIDER_DISCONNECTED", "Diagnostic-only provider does not accept subscriptions"));
}
void DiagnosticOnlyProvider::set_event_sink(EventSink sink) { static_cast<void>(sink); }
void DiagnosticOnlyProvider::stop() noexcept {}
ProviderCapabilities DiagnosticOnlyProvider::capabilities() const noexcept { return {}; }
std::string_view UpstoxProviderStub::name() const noexcept { return "UPSTOX_V3_DIAGNOSTIC"; }
std::string_view FyersProviderAdapter::name() const noexcept { return "FYERS_DIAGNOSTIC"; }

} // namespace basalt::market_data

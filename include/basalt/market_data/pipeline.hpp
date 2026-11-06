#pragma once

#include "basalt/market_data/instrument_master.hpp"
#include "basalt/market_data/types.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace basalt::market_data {

struct ProviderRegistration {
    common::DataSourceType source{common::DataSourceType::Derived};
    std::uint64_t stale_timeout_ns{5'000'000'000ULL};
    std::uint64_t max_clock_skew_ns{5'000'000'000ULL};
};

struct FeedHealthSnapshot {
    std::string provider{};
    common::InstrumentId instrument_id{};
    common::FeedQualityState quality{common::FeedQualityState::Disconnected};
    std::optional<std::int64_t> last_event_time_ns{};
    std::optional<std::int64_t> last_receive_time_mono_ns{};
    std::uint64_t last_provider_sequence{};
    std::uint64_t sequence_gaps{};
    std::uint64_t duplicates{};
    std::uint64_t out_of_order{};
    std::uint64_t invalid_events{};
    std::uint64_t reconnect_generation{};
};

struct PipelineStats {
    std::uint64_t received{};
    std::uint64_t accepted{};
    std::uint64_t invalid{};
    std::uint64_t duplicates{};
    std::uint64_t out_of_order{};
    std::uint64_t sequence_gaps{};
    std::uint64_t clock_skew_events{};
    std::uint64_t stale_events{};
    std::uint64_t durable_backpressure{};
    std::uint64_t ui_queue_overflows{};
};

class MarketDataPipeline {
public:
    using DurableSink = std::function<bool(CanonicalMarketEvent)>;
    using QuoteSink = std::function<bool(CanonicalMarketEvent)>;

    explicit MarketDataPipeline(InstrumentMaster& instruments);
    void register_provider(std::string provider, ProviderRegistration registration);
    void set_event_sinks(DurableSink durable_sink, QuoteSink quote_sink);

    [[nodiscard]] common::Result<CanonicalMarketEvent, ProviderError> process(
        const RawMarketEvent& raw, std::int64_t receive_time_mono_ns,
        std::int64_t receive_time_utc_ns);
    void note_reconnect(std::string_view provider,
                        std::optional<common::InstrumentId> instrument_id = std::nullopt);
    [[nodiscard]] std::vector<FeedHealthSnapshot> health(
        std::int64_t now_mono_ns) const;
    [[nodiscard]] PipelineStats stats() const;

private:
    struct ProviderInstrumentState {
        common::FeedQualityState quality{common::FeedQualityState::Disconnected};
        std::optional<std::uint64_t> last_provider_sequence{};
        std::optional<std::int64_t> last_event_time_ns{};
        std::optional<std::int64_t> last_receive_time_mono_ns{};
        std::uint64_t sequence_gaps{};
        std::uint64_t duplicates{};
        std::uint64_t out_of_order{};
        std::uint64_t invalid_events{};
        std::uint64_t reconnect_generation{};
        std::uint64_t stale_timeout_ns{};
    };

    InstrumentMaster& instruments_;
    std::unordered_map<std::string, ProviderRegistration> providers_{};
    std::unordered_map<std::string, ProviderInstrumentState> states_{};
    std::unordered_set<std::string> seen_event_ids_{};
    std::deque<std::string> seen_event_order_{};
    mutable std::mutex mutex_{};
    DurableSink durable_sink_{};
    QuoteSink quote_sink_{};
    PipelineStats stats_{};

    void remember_event_id(std::string key);
};

} // namespace basalt::market_data

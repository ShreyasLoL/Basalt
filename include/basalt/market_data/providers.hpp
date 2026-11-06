#pragma once

#include "basalt/market_data/instrument_master.hpp"
#include "basalt/market_data/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace basalt::market_data {

class DeterministicSyntheticProvider final : public IMarketDataProvider {
public:
    explicit DeterministicSyntheticProvider(const InstrumentMaster& instruments,
                                             std::int64_t base_event_time_ns = 0);

    [[nodiscard]] common::Result<void, ProviderError> connect(const ProviderConfig& config) override;
    [[nodiscard]] common::Result<void, ProviderError> subscribe(
        std::span<const common::InstrumentId> instrument_ids) override;
    void set_event_sink(EventSink sink) override;
    void stop() noexcept override;
    [[nodiscard]] ProviderCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] common::Result<std::size_t, ProviderError> step(std::size_t cycles);

private:
    const InstrumentMaster& instruments_;
    std::int64_t base_event_time_ns_;
    EventSink event_sink_{};
    std::vector<common::InstrumentId> subscriptions_{};
    std::unordered_map<common::InstrumentId, std::uint64_t> sequence_by_instrument_{};
    std::uint64_t cycle_{};
    std::int64_t last_event_time_ns_{};
    bool connected_{};
};

class CsvJsonReplayProvider final : public IMarketDataProvider {
public:
    explicit CsvJsonReplayProvider(const InstrumentMaster& instruments);

    [[nodiscard]] common::Result<void, ProviderError> connect(const ProviderConfig& config) override;
    [[nodiscard]] common::Result<void, ProviderError> subscribe(
        std::span<const common::InstrumentId> instrument_ids) override;
    void set_event_sink(EventSink sink) override;
    void stop() noexcept override;
    [[nodiscard]] ProviderCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;

    [[nodiscard]] common::Result<std::size_t, ProviderError> load_csv(std::istream& input);
    [[nodiscard]] common::Result<std::size_t, ProviderError> load_json_lines(std::istream& input);
    [[nodiscard]] common::Result<std::size_t, ProviderError> step(std::size_t maximum_events);
    void rewind() noexcept;
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t event_count() const noexcept { return events_.size(); }

private:
    const InstrumentMaster& instruments_;
    EventSink event_sink_{};
    std::vector<RawMarketEvent> events_{};
    std::unordered_set<common::InstrumentId> subscriptions_{};
    std::size_t cursor_{};
    bool connected_{};
};

class DiagnosticOnlyProvider : public IMarketDataProvider {
public:
    [[nodiscard]] common::Result<void, ProviderError> connect(const ProviderConfig& config) override;
    [[nodiscard]] common::Result<void, ProviderError> subscribe(
        std::span<const common::InstrumentId> instrument_ids) override;
    void set_event_sink(EventSink sink) override;
    void stop() noexcept override;
    [[nodiscard]] ProviderCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override = 0;
};

class UpstoxProviderStub final : public DiagnosticOnlyProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
};

class FyersProviderAdapter final : public DiagnosticOnlyProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
};

} // namespace basalt::market_data

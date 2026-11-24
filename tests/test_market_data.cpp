#include "basalt/market_data/event_bus.hpp"
#include "basalt/market_data/instrument_master.hpp"
#include "basalt/market_data/pipeline.hpp"
#include "basalt/market_data/providers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace basalt::market_data {
namespace {

constexpr std::int64_t event_time_ns = 1'800'000'000'000'000'000LL;

common::InstrumentId sim_reliance_id(const InstrumentMaster& master) {
    const auto resolved = master.resolve_provider("SYNTHETIC", "SIM:RELIANCE", event_time_ns);
    return resolved ? resolved.value() : 0;
}

RawMarketEvent quote(std::uint64_t sequence = 1) {
    RawMarketEvent raw;
    raw.provider = "SYNTHETIC";
    raw.event_id = "Q-" + std::to_string(sequence);
    raw.provider_instrument_key = "SIM:RELIANCE";
    raw.kind = MarketEventKind::Quote;
    raw.event_time_ns = event_time_ns;
    raw.provider_sequence = sequence;
    raw.price = "100.00";
    raw.bid = "99.95";
    raw.ask = "100.05";
    raw.bid_quantity = "3";
    raw.ask_quantity = "4";
    return raw;
}

TEST(InstrumentMaster, SeededCatalogIsVersionedAndClearlySimulationOnly) {
    const InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    EXPECT_EQ(master.version(), 1U);
    EXPECT_EQ(master.size(), 10U);
    const auto resolved = master.resolve_provider("synthetic", "SIM:RELIANCE", event_time_ns);
    ASSERT_TRUE(resolved);
    const InstrumentDefinition* instrument = master.find(resolved.value());
    ASSERT_NE(instrument, nullptr);
    EXPECT_EQ(instrument->exchange, Exchange::Sim);
    EXPECT_EQ(instrument->display_symbol, "SIM:RELIANCE");
    EXPECT_EQ(master.resolve_symbol(Exchange::Sim, "eq", "reliance").value(), resolved.value());
    EXPECT_FALSE(master.resolve_provider("UPSTOX_V3_DIAGNOSTIC", "SIM:RELIANCE", event_time_ns));
}

TEST(InstrumentMaster, RejectsInvalidAndAmbiguousEffectiveMappings) {
    InstrumentMaster master(3);
    InstrumentDefinition instrument;
    instrument.id = 1;
    instrument.exchange = Exchange::Nse;
    instrument.segment = "EQ";
    instrument.symbol = "TEST";
    instrument.display_symbol = "NSE:TEST";
    instrument.tick_size_micro = 500;
    instrument.provider_mappings = {ProviderSymbolMapping{"FEED", "ABC", 10, 20}};
    ASSERT_TRUE(master.add(instrument));

    InstrumentDefinition overlap = instrument;
    overlap.id = 2;
    overlap.symbol = "OTHER";
    overlap.display_symbol = "NSE:OTHER";
    overlap.provider_mappings = {ProviderSymbolMapping{"FEED", "ABC", 19, 30}};
    const auto duplicate_mapping = master.add(overlap);
    EXPECT_FALSE(duplicate_mapping);
    EXPECT_EQ(duplicate_mapping.error().code, "AMBIGUOUS_PROVIDER_MAPPING");

    InstrumentDefinition invalid_tick = instrument;
    invalid_tick.id = 3;
    invalid_tick.symbol = "BAD";
    invalid_tick.display_symbol = "NSE:BAD";
    invalid_tick.tick_size_micro = 0;
    EXPECT_EQ(master.add(invalid_tick).error().code, "INVALID_INSTRUMENT_SCALE");
    EXPECT_EQ(master.resolve_provider("FEED", "ABC", 19).value(), 1U);
    EXPECT_FALSE(master.resolve_provider("FEED", "ABC", 30));
}

TEST(MarketDataPipeline, NormalizesQuoteToFixedPointAndTicks) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    MarketDataPipeline pipeline(master);
    pipeline.register_provider("SYNTHETIC", ProviderRegistration{common::DataSourceType::Synthetic, 5'000'000'000, 5'000'000'000});
    const auto result = pipeline.process(quote(), 100, event_time_ns + 100);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().instrument_id, sim_reliance_id(master));
    EXPECT_EQ(result.value().price_micro, 1'000'000);
    EXPECT_EQ(result.value().price_ticks, 2'000);
    EXPECT_EQ(result.value().bid_micro, 999'500);
    EXPECT_EQ(result.value().ask_micro, 1'000'500);
    EXPECT_EQ(result.value().source, common::DataSourceType::Synthetic);
    EXPECT_EQ(result.value().quality, common::FeedQualityState::Ok);
    EXPECT_EQ(pipeline.stats().accepted, 1U);
}

TEST(MarketDataPipeline, RejectsOffTickMalformedAndUnmappedEvents) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    MarketDataPipeline pipeline(master);
    pipeline.register_provider("SYNTHETIC", {});
    RawMarketEvent off_tick = quote();
    off_tick.price = "100.03";
    EXPECT_EQ(pipeline.process(off_tick, 100, event_time_ns + 100).error().code, "OFF_TICK_PRICE");
    RawMarketEvent malformed = quote(2);
    malformed.price = "100.00001";
    EXPECT_EQ(pipeline.process(malformed, 101, event_time_ns + 101).error().code, "INVALID_PRICE_PRECISION");
    RawMarketEvent unknown = quote(3);
    unknown.provider_instrument_key = "SIM:UNKNOWN";
    EXPECT_EQ(pipeline.process(unknown, 102, event_time_ns + 102).error().code, "UNKNOWN_PROVIDER_SYMBOL");
    EXPECT_EQ(pipeline.stats().invalid, 3U);
}

TEST(MarketDataPipeline, DetectsDuplicatesOutOfOrderAndSequenceGaps) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    MarketDataPipeline pipeline(master);
    pipeline.register_provider("SYNTHETIC", {});
    ASSERT_TRUE(pipeline.process(quote(1), 100, event_time_ns + 100));
    auto gap = pipeline.process(quote(3), 200, event_time_ns + 200);
    ASSERT_TRUE(gap);
    EXPECT_TRUE(gap.value().sequence_gap);
    EXPECT_EQ(gap.value().quality, common::FeedQualityState::Degraded);
    EXPECT_EQ(pipeline.process(quote(3), 300, event_time_ns + 300).error().code, "DUPLICATE_EVENT");
    EXPECT_EQ(pipeline.process(quote(2), 400, event_time_ns + 400).error().code, "OUT_OF_ORDER_EVENT");
    const PipelineStats stats = pipeline.stats();
    EXPECT_EQ(stats.sequence_gaps, 1U);
    EXPECT_EQ(stats.duplicates, 1U);
    EXPECT_EQ(stats.out_of_order, 1U);
}

TEST(MarketDataPipeline, MarksStaleClockSkewAndReconnectHealth) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    MarketDataPipeline pipeline(master);
    pipeline.register_provider("SYNTHETIC", ProviderRegistration{common::DataSourceType::Synthetic, 1'000, 5'000});
    auto stale = pipeline.process(quote(), 10, event_time_ns + 2'000);
    ASSERT_TRUE(stale);
    EXPECT_EQ(stale.value().quality, common::FeedQualityState::Stale);
    pipeline.note_reconnect("synthetic", sim_reliance_id(master));
    const auto health = pipeline.health(2'000);
    ASSERT_EQ(health.size(), 1U);
    EXPECT_EQ(health.front().quality, common::FeedQualityState::Stale);
    EXPECT_EQ(health.front().reconnect_generation, 1U);
    EXPECT_EQ(pipeline.stats().stale_events, 1U);

    MarketDataPipeline skew_pipeline(master);
    skew_pipeline.register_provider("SYNTHETIC", ProviderRegistration{common::DataSourceType::Synthetic, 10'000'000'000, 1'000});
    const auto skew = skew_pipeline.process(quote(), 10, event_time_ns + 10'000);
    ASSERT_TRUE(skew);
    EXPECT_TRUE(skew.value().clock_skew);
    EXPECT_EQ(skew.value().quality, common::FeedQualityState::Degraded);
}

TEST(MarketDataPipeline, ValidatesDepthOrderingAndBarSemantics) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    MarketDataPipeline pipeline(master);
    pipeline.register_provider("SYNTHETIC", {});
    RawMarketEvent depth = quote();
    depth.kind = MarketEventKind::Depth;
    depth.price.reset();
    depth.bid.reset();
    depth.ask.reset();
    depth.provider_sequence = 1;
    depth.bids = {{"100.00", "2"}, {"100.05", "3"}};
    EXPECT_EQ(pipeline.process(depth, 100, event_time_ns + 100).error().code, "UNSORTED_DEPTH");

    RawMarketEvent bar;
    bar.provider = "SYNTHETIC";
    bar.provider_instrument_key = "SIM:RELIANCE";
    bar.kind = MarketEventKind::Bar;
    bar.event_time_ns = event_time_ns;
    bar.open = "100.00";
    bar.high = "101.00";
    bar.low = "99.00";
    bar.close = "100.50";
    bar.volume = "10";
    const auto valid_bar = pipeline.process(bar, 200, event_time_ns + 200);
    ASSERT_TRUE(valid_bar) << valid_bar.error().message;
    ASSERT_TRUE(valid_bar.value().bar.has_value());
    EXPECT_EQ(valid_bar.value().bar->volume, 10U);

    bar.event_time_ns += 1;
    bar.high = "100.00";
    EXPECT_EQ(pipeline.process(bar, 300, event_time_ns + 300).error().code, "INVALID_OHLC");
}

TEST(MarketDataPipeline, ReportsDurableBackpressureAndCoalescedQuoteOverflow) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    MarketDataPipeline pipeline(master);
    pipeline.register_provider("SYNTHETIC", {});
    pipeline.set_event_sinks([](CanonicalMarketEvent) { return false; }, {});
    EXPECT_EQ(pipeline.process(quote(), 100, event_time_ns + 100).error().code, "DURABLE_QUEUE_FULL");
    EXPECT_EQ(pipeline.stats().durable_backpressure, 1U);

    BoundedMarketEventBus bus(1, 1);
    const auto instrument_id = sim_reliance_id(master);
    CanonicalMarketEvent durable;
    durable.instrument_id = instrument_id;
    durable.kind = MarketEventKind::Trade;
    EXPECT_TRUE(bus.publish_durable(durable, std::chrono::milliseconds(0)));
    EXPECT_FALSE(bus.publish_durable(durable, std::chrono::milliseconds(0)));
    EXPECT_EQ(bus.stats().durable_timeouts, 1U);
    ASSERT_TRUE(bus.pop_durable(std::chrono::milliseconds(0)).has_value());

    CanonicalMarketEvent quote_event;
    quote_event.instrument_id = instrument_id;
    quote_event.kind = MarketEventKind::Quote;
    quote_event.price_micro = 1'000'000;
    EXPECT_TRUE(bus.publish_quote(quote_event));
    quote_event.price_micro = 1'000'500;
    EXPECT_TRUE(bus.publish_quote(quote_event));
    quote_event.instrument_id += 1;
    EXPECT_FALSE(bus.publish_quote(quote_event));
    const auto quotes = bus.drain_quotes();
    ASSERT_EQ(quotes.size(), 1U);
    EXPECT_EQ(quotes.front().price_micro, 1'000'500);
    EXPECT_EQ(bus.stats().quote_replacements, 1U);
    EXPECT_EQ(bus.stats().quote_overflows, 1U);
}

TEST(SpscRingBufferTest, CoversEmptyFullAndRepeatedWraparound) {
    common::SpscRingBuffer<std::uint64_t> ring(2);
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.try_pop());
    EXPECT_TRUE(ring.try_push(std::uint64_t{1}));
    EXPECT_TRUE(ring.try_push(std::uint64_t{2}));
    EXPECT_TRUE(ring.full());
    EXPECT_FALSE(ring.try_push(std::uint64_t{3}));
    ASSERT_EQ(ring.try_pop(), 1U);
    EXPECT_TRUE(ring.try_push(std::uint64_t{3}));
    ASSERT_EQ(ring.try_pop(), 2U);
    ASSERT_EQ(ring.try_pop(), 3U);
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRingBufferTest, TransfersEverySequenceAcrossConcurrentWraparound) {
    struct Message { std::uint64_t sequence{}; std::uint64_t inverse{}; };
    constexpr std::uint64_t count = 200'000;
    common::SpscRingBuffer<Message> ring(64);
    std::atomic<bool> valid{true};
    std::thread consumer([&] {
        for (std::uint64_t expected = 0; expected < count;) {
            auto message = ring.try_pop();
            if (!message) {
                std::this_thread::yield();
                continue;
            }
            if (message->sequence != expected || message->inverse != ~expected)
                valid.store(false, std::memory_order_relaxed);
            ++expected;
        }
    });
    for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
        Message message{sequence, ~sequence};
        while (!ring.try_push(std::move(message))) std::this_thread::yield();
    }
    consumer.join();
    EXPECT_TRUE(valid.load(std::memory_order_relaxed));
    EXPECT_TRUE(ring.empty());
}

TEST(BoundedMarketEventBusTest, TransfersDurableEventsBetweenOnePublisherAndConsumer) {
    constexpr std::uint64_t count = 20'000;
    BoundedMarketEventBus bus(32, 1);
    std::atomic<bool> publish_failed{false};
    std::thread producer([&] {
        for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
            CanonicalMarketEvent event{};
            event.event_id = "durable-" + std::to_string(sequence);
            event.instrument_id = 7;
            event.kind = MarketEventKind::Trade;
            event.price_micro = static_cast<std::int64_t>(sequence + 1U);
            if (!bus.publish_durable(std::move(event), std::chrono::milliseconds(100))) {
                publish_failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });
    for (std::uint64_t expected = 0; expected < count;) {
        const auto event = bus.pop_durable(std::chrono::milliseconds(100));
        if (!event) continue;
        EXPECT_EQ(event->event_id, "durable-" + std::to_string(expected));
        EXPECT_EQ(event->price_micro, static_cast<std::int64_t>(expected + 1U));
        ++expected;
    }
    producer.join();
    bus.close();
    EXPECT_FALSE(publish_failed.load(std::memory_order_relaxed));
    EXPECT_FALSE(bus.pop_durable(std::chrono::milliseconds(0)).has_value());
}

TEST(MarketDataProviders, SyntheticProviderProducesRepeatableSimOnlyEvents) {
    const InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    const common::InstrumentId id = sim_reliance_id(master);
    DeterministicSyntheticProvider provider(master, event_time_ns);
    const std::array<common::InstrumentId, 1> ids{id};
    EXPECT_FALSE(provider.subscribe(ids));
    ASSERT_TRUE(provider.connect(ProviderConfig{"SYNTHETIC", "", "", true, true}));
    ASSERT_TRUE(provider.subscribe(ids));
    std::vector<RawMarketEvent> events;
    provider.set_event_sink([&events](RawMarketEvent event) { events.push_back(std::move(event)); });
    const auto stepped = provider.step(3);
    ASSERT_TRUE(stepped);
    EXPECT_EQ(stepped.value(), 3U);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].provider_instrument_key, "SIM:RELIANCE");
    EXPECT_EQ(events[0].provider_sequence, 1U);
    EXPECT_EQ(events[1].provider_sequence, 2U);
    EXPECT_EQ(events[0].price, "100.4");
    EXPECT_TRUE(provider.capabilities().quotes);
    provider.stop();
    EXPECT_EQ(provider.step(1).error().code, "PROVIDER_DISCONNECTED");
}

TEST(MarketDataProviders, ReplayReadsCsvAndJsonLinesWithoutExternalServices) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    CsvJsonReplayProvider provider(master);
    std::istringstream csv(
        "event_id,provider_instrument_key,kind,event_time_ns,provider_sequence,price,quantity\n"
        "R1,SIM:RELIANCE,trade,1800000000000000000,1,100.00,2\n"
        "R2,SIM:RELIANCE,trade,1800000001000000000,2,100.05,1\n");
    const auto loaded = provider.load_csv(csv);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded.value(), 2U);
    ASSERT_TRUE(provider.connect(ProviderConfig{"CSV_JSON_REPLAY", "", "", true, true}));
    const common::InstrumentId id = sim_reliance_id(master);
    const std::array<common::InstrumentId, 1> ids{id};
    ASSERT_TRUE(provider.subscribe(ids));
    std::vector<RawMarketEvent> emitted;
    provider.set_event_sink([&emitted](RawMarketEvent event) { emitted.push_back(std::move(event)); });
    const auto count = provider.step(10);
    ASSERT_TRUE(count);
    EXPECT_EQ(count.value(), 2U);
    EXPECT_EQ(emitted.size(), 2U);
    EXPECT_EQ(emitted.front().provider, "CSV_JSON_REPLAY");

    std::istringstream json_lines(
        R"({"event_id":"J1","provider_instrument_key":"SIM:RELIANCE","kind":"quote","event_time_ns":1800000000000000000,"price":"100.00","bid":"99.95","ask":"100.05"})"
        "\n"
        R"({"event_id":"J2","provider_instrument_key":"SIM:RELIANCE","kind":"depth","event_time_ns":1800000001000000000,"bids":[{"price":"100.00","quantity":"2"}]})");
    const auto json_loaded = provider.load_json_lines(json_lines);
    ASSERT_TRUE(json_loaded) << json_loaded.error().message;
    EXPECT_EQ(json_loaded.value(), 2U);
    EXPECT_EQ(provider.cursor(), 0U);
}

TEST(MarketDataProviders, ReplayReadersRejectOversizedLinesBeforeParsing) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    CsvJsonReplayProvider provider(master);
    std::istringstream csv(std::string(1'048'577, 'x'));
    const auto csv_result = provider.load_csv(csv);
    ASSERT_FALSE(csv_result);
    EXPECT_EQ(csv_result.error().code, "FRAME_TOO_LARGE");

    std::istringstream json_lines(std::string(1'048'577, ' '));
    const auto json_result = provider.load_json_lines(json_lines);
    ASSERT_FALSE(json_result);
    EXPECT_EQ(json_result.error().code, "FRAME_TOO_LARGE");

    std::string wide_header;
    for (std::size_t column = 0; column < 65; ++column) {
        if (column != 0) wide_header += ',';
        wide_header += 'x';
    }
    std::istringstream wide_csv(wide_header);
    const auto wide_result = provider.load_csv(wide_csv);
    ASSERT_FALSE(wide_result);
    EXPECT_EQ(wide_result.error().code, "CSV_COLUMN_LIMIT");
}

TEST(MarketDataProviders, JsonReplayRejectsUnknownFieldsAndExcessiveNesting) {
    InstrumentMaster master = InstrumentMaster::seeded_sim_catalog();
    CsvJsonReplayProvider provider(master);
    std::istringstream versioned(
        R"({"schema_version":1,"provider_instrument_key":"SIM:RELIANCE","kind":"quote","event_time_ns":1800000000000000000})");
    const auto versioned_result = provider.load_json_lines(versioned);
    ASSERT_TRUE(versioned_result) << versioned_result.error().message;
    EXPECT_EQ(versioned_result.value(), 1U);

    std::istringstream unsupported_version(
        R"({"schema_version":2,"provider_instrument_key":"SIM:RELIANCE","kind":"quote","event_time_ns":1800000000000000000})");
    const auto unsupported_result = provider.load_json_lines(unsupported_version);
    ASSERT_FALSE(unsupported_result);
    EXPECT_EQ(unsupported_result.error().code, "INVALID_SCHEMA_VERSION");

    std::istringstream unknown_field(
        R"({"provider_instrument_key":"SIM:RELIANCE","kind":"quote","event_time_ns":1800000000000000000,"unexpected":true})");
    const auto unknown_result = provider.load_json_lines(unknown_field);
    ASSERT_FALSE(unknown_result);
    EXPECT_EQ(unknown_result.error().code, "UNKNOWN_JSON_FIELD");

    std::string too_deep =
        R"({"provider_instrument_key":"SIM:RELIANCE","kind":"quote","event_time_ns":1800000000000000000,"extra":)";
    too_deep.append(33, '[');
    too_deep += '0';
    too_deep.append(33, ']');
    too_deep += '}';
    std::istringstream deep_input(too_deep);
    const auto deep_result = provider.load_json_lines(deep_input);
    ASSERT_FALSE(deep_result);
    EXPECT_EQ(deep_result.error().code, "JSON_DEPTH_LIMIT");
}

TEST(MarketDataProviders, DiagnosticAdaptersStayDisconnectedAndReadNoSecrets) {
    UpstoxProviderStub upstox;
    FyersProviderAdapter fyers;
    const ProviderConfig diagnostic{"UPSTOX_V3_DIAGNOSTIC", "", "BROKER_TOKEN", true, true};
    const auto upstox_result = upstox.connect(diagnostic);
    EXPECT_FALSE(upstox_result);
    EXPECT_EQ(upstox_result.error().code, "DIAGNOSTIC_ONLY");
    EXPECT_FALSE(upstox.capabilities().quotes);
    EXPECT_EQ(fyers.connect(ProviderConfig{"FYERS_DIAGNOSTIC", "", "FYERS_TOKEN", true, true}).error().code,
              "DIAGNOSTIC_ONLY");
    EXPECT_EQ(upstox.connect(ProviderConfig{"UPSTOX_V3_DIAGNOSTIC", "", "", true, false}).error().code,
              "LIVE_PROVIDER_DISABLED");
}

TEST(MarketDataTypes, ParsesOnlyKnownEventKindsAndExchanges) {
    EXPECT_EQ(parse_event_kind("quote").value(), MarketEventKind::Quote);
    EXPECT_EQ(parse_exchange("nse").value(), Exchange::Nse);
    EXPECT_FALSE(parse_event_kind("order"));
    EXPECT_FALSE(parse_exchange("MCX"));
}

} // namespace
} // namespace basalt::market_data

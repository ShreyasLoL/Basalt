#include "basalt/market_data/instrument_master.hpp"
#include "basalt/market_data/providers.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (size > 256U * 1024U) return 0;
    static constexpr char empty[] = "";
    const auto* bytes = size == 0 ? empty : reinterpret_cast<const char*>(data);
    const std::string input(bytes, size);
    std::istringstream stream(input);
    static const auto instruments = basalt::market_data::InstrumentMaster::seeded_sim_catalog();
    basalt::market_data::CsvJsonReplayProvider provider(instruments);
    const auto loaded = provider.load_json_lines(stream);
    if (!loaded) return 0;

    const auto connected = provider.connect(
        {"CSV_JSON_REPLAY", "", "", true, true});
    if (!connected) return 0;
    provider.set_event_sink([](basalt::market_data::RawMarketEvent) {});
    (void)provider.step(100'000);
    provider.stop();
    return 0;
}

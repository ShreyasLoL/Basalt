#include "basalt/core/matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {

std::uint64_t read_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index)
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    return value;
}

void check_engine(const basalt::MatchingEngine& engine) {
    const auto snapshot = engine.get_snapshot(64);
    const auto stats = engine.stats();
    if (!engine.book().verify_invariants() ||
        stats.active_order_count != snapshot.resting_orders.size() ||
        (snapshot.has_best_bid && snapshot.has_best_ask &&
         snapshot.best_bid >= snapshot.best_ask) ||
        (snapshot.has_spread && snapshot.spread < 0)) {
        std::abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    constexpr std::size_t command_size = 27;
    constexpr std::size_t maximum_commands = 256;
    if (size > 64U * 1024U) return 0;

    basalt::MatchingEngine engine;
    const auto price_modulus = static_cast<std::uint64_t>(basalt::MAX_ORDER_PRICE) + 2U;
    constexpr std::uint64_t quantity_modulus = basalt::MAX_ORDER_QUANTITY + 2U;
    std::size_t commands = 0;
    for (std::size_t offset = 0; size - offset >= command_size && commands < maximum_commands;
         offset += command_size, ++commands) {
        const auto* command = data + offset;
        const auto operation = command[0] % 4U;
        if (operation <= 1U) {
            const auto raw_price = read_u64(command + 3);
            const auto raw_quantity = read_u64(command + 11);
            const auto id = read_u64(command + 19);
            (void)engine.submit_order({
                id,
                static_cast<basalt::Side>(command[1]),
                static_cast<basalt::OrderType>(command[2]),
                static_cast<basalt::Price>(raw_price % price_modulus),
                raw_quantity % quantity_modulus});
        } else if (operation == 2U) {
            (void)engine.cancel_order(read_u64(command + 19));
        } else {
            engine.reset();
        }
        check_engine(engine);
    }
    check_engine(engine);
    return 0;
}

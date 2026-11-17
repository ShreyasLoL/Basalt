#include "basalt/platform/platform_runner.hpp"

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {

bool parse_ticks(const std::string_view text, std::size_t& ticks) {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed == 0 || parsed > 100'000) {
        return false;
    }
    ticks = static_cast<std::size_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t ticks = 12;
    if (argc == 3 && std::string_view(argv[1]) == "--ticks") {
        if (!parse_ticks(argv[2], ticks)) {
            std::cerr << "--ticks must be an integer between 1 and 100000\n";
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "Usage: basalt_demo [--ticks 1-100000]\n";
        return 2;
    }

    basalt::platform::PlatformRunner runner;
    const auto result = runner.run(ticks);
    if (!result) {
        std::cerr << "Synthetic platform session failed: " << result.error().code
                  << ": " << result.error().message << '\n';
        return 1;
    }
    const auto& snapshot = result.value();
    std::cout << "BASALT deterministic synthetic session\n"
              << "seed: 42\n"
              << "ticks processed: " << snapshot.ticks_processed << '\n'
              << "signals: " << snapshot.signals << '\n'
              << "orders: " << snapshot.orders << '\n'
              << "fills: " << snapshot.fills << '\n'
              << "risk rejections: " << snapshot.risk_rejections;
    for (const auto& code : snapshot.risk_rejection_codes) std::cout << ' ' << code;
    std::cout << '\n'
              << "ledger cash (micro-INR): " << snapshot.portfolio.cash_balance_micro << '\n'
              << "ledger realized PnL (micro-INR): " << snapshot.portfolio.realized_pnl_micro << '\n'
              << "ledger unrealized PnL (micro-INR): " << snapshot.portfolio.unrealized_pnl_micro << '\n'
              << "ledger total PnL (micro-INR): " << snapshot.total_pnl_micro << '\n'
              << "ledger hash: " << std::hex << std::setw(16) << std::setfill('0')
              << snapshot.ledger_hash << std::dec << '\n'
              << "portfolio API: GET /api/v1/portfolio/1\n";
    return runner.verify_reconciliation() ? 0 : 1;
}

#include "basalt/bench/benchmark_runner.hpp"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool parse_size(const std::string_view text, std::size_t& value) {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed == 0 ||
        parsed > 5'000'000) return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

void print_usage() {
    std::cout << "Usage: basalt_bench [--orders N] [--profile ID] [--no-baseline]\n"
                 "Profiles: BALANCED, BUY_HEAVY, SELL_HEAVY, AGGRESSIVE_MATCHING,\n"
                 "          CANCEL_HEAVY, CONCENTRATED_LEVELS, WIDE_DISTRIBUTION\n";
}

} // namespace

int main(int argc, char** argv) {
    std::size_t order_count = 50000;
    bool compare_baseline = true;
    bool all_profiles = true;
    basalt::bench::WorkloadProfile selected = basalt::bench::WorkloadProfile::Balanced;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--orders") {
            if (i + 1 >= argc || !parse_size(argv[++i], order_count)) {
                std::cerr << "--orders must be an integer from 1 to 5000000\n";
                return 2;
            }
        } else if (argument == "--profile") {
            if (i + 1 >= argc ||
                !basalt::bench::parse_profile(argv[++i], selected)) {
                std::cerr << "--profile must name one of the seven workload IDs\n";
                return 2;
            }
            all_profiles = false;
        } else if (argument == "--no-baseline") {
            compare_baseline = false;
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            print_usage();
            return 2;
        }
    }

    try {
        const basalt::bench::BenchmarkRunner runner;
        const auto results = all_profiles
            ? runner.run_all(order_count, compare_baseline)
            : runner.run(selected, order_count, compare_baseline);
        std::cout << "BASALT benchmark | operations per profile: " << order_count << '\n';
        std::cout << basalt::bench::format_ascii_table(results);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

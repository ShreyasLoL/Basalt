#include "basalt/bench/benchmark_runner.hpp"
#include "basalt/common/fixed_math.hpp"
#include "basalt/persistence/event_log.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
    std::cout << "Usage: basalt_bench [--orders N] [--profile ID] [--no-baseline] [--arena-pressure]\n"
                 "       basalt_bench --microbench fixed-math|event-log [--orders N]\n"
                 "Profiles: BALANCED, BUY_HEAVY, SELL_HEAVY, AGGRESSIVE_MATCHING,\n"
                 "          CANCEL_HEAVY, CONCENTRATED_LEVELS, WIDE_DISTRIBUTION,\n"
                 "          BURSTY_ARRIVALS, HIGH_ACTIVE_ORDERS, MULTI_INSTRUMENT_50,\n"
                 "          MARKET_DATA_BURST, SLOW_CONSUMER\n";
}

std::uint64_t legacy_ratio_bps(const std::uint64_t numerator, const std::uint64_t denominator) {
    const auto whole = numerator / denominator;
    if (whole > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 10'000))
        return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto remainder = numerator % denominator;
    const auto complement = denominator - remainder;
    std::uint64_t carry = 0;
    std::uint64_t fraction = 0;
    for (std::uint32_t i = 0; i < 10'000; ++i) {
        if (carry >= complement) {
            carry -= complement;
            ++fraction;
        } else {
            carry += remainder;
        }
    }
    return whole * 10'000 + fraction;
}

void run_fixed_math_microbench(const std::size_t count) {
    struct Input { std::uint64_t numerator; std::uint64_t denominator; };
    std::vector<Input> inputs;
    inputs.reserve(count);
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (std::size_t i = 0; i < count; ++i) {
        state ^= state << 7U;
        state ^= state >> 9U;
        const auto denominator = (state | 1ULL) % 1'000'000'007ULL + 1ULL;
        state ^= state << 8U;
        inputs.push_back({state % (denominator * 3ULL), denominator});
    }

    std::uint64_t fast_checksum = 0;
    const auto fast_start = std::chrono::steady_clock::now();
    for (const auto& input : inputs)
        fast_checksum += static_cast<std::uint64_t>(basalt::common::ratio_bps(input.numerator, input.denominator).value());
    const auto fast_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - fast_start).count();

    std::uint64_t legacy_checksum = 0;
    const auto legacy_start = std::chrono::steady_clock::now();
    for (const auto& input : inputs)
        legacy_checksum += legacy_ratio_bps(input.numerator, input.denominator);
    const auto legacy_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - legacy_start).count();

    std::cout << "Fixed math ratio_bps | operations=" << count << '\n'
              << "O(1) multiply/divide: " << fast_ns << " ns total, "
              << (count == 0 ? 0 : fast_ns / static_cast<std::int64_t>(count))
              << " ns/op, checksum=" << fast_checksum << '\n'
              << "Legacy 10,000-step loop: " << legacy_ns << " ns total, "
              << (count == 0 ? 0 : legacy_ns / static_cast<std::int64_t>(count))
              << " ns/op, checksum=" << legacy_checksum << '\n';
    if (fast_checksum != legacy_checksum) throw std::runtime_error("Fixed math benchmark implementations diverged");
}

void run_event_log_microbench(const std::size_t count) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("basalt-event-log-bench-" + std::to_string(stamp) + ".blog");
    basalt::persistence::EventLog log(path);
    const auto recovered = log.recover();
    if (!recovered) throw std::runtime_error(recovered.error().code + ": " + recovered.error().message);
    std::vector<std::string> payloads;
    payloads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) payloads.push_back("benchmark-event-" + std::to_string(i));
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto appended = log.append_next(payloads[i]);
        if (!appended) throw std::runtime_error(appended.error().code + ": " + appended.error().message);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    const auto closed = log.close();
    if (!closed) throw std::runtime_error(closed.error().code + ": " + closed.error().message);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::cout << "EventLog persistent append | operations=" << count
              << " | durable flush each append | total=" << elapsed << " ns | "
              << (count == 0 ? 0 : elapsed / static_cast<std::int64_t>(count)) << " ns/op\n";
}

void print_environment() {
    std::cout << "Environment: ";
#if defined(_WIN32)
    std::cout << "Windows";
#elif defined(__linux__)
    std::cout << "Linux";
#elif defined(__APPLE__)
    std::cout << "macOS";
#else
    std::cout << "unknown-OS";
#endif
#if defined(__clang__)
    std::cout << ", Clang " << __clang_version__;
#elif defined(_MSC_VER)
    std::cout << ", MSVC " << _MSC_VER;
#elif defined(__GNUC__)
    std::cout << ", GCC " << __VERSION__;
#endif
    std::cout << ", hardware threads=" << std::thread::hardware_concurrency()
              << ", steady_clock, seed=42\n";
}

} // namespace

int main(int argc, char** argv) {
    std::size_t order_count = 50000;
    std::size_t micro_count = 1000;
    bool explicit_order_count = false;
    bool compare_baseline = true;
    bool all_profiles = true;
    bool arena_pressure = false;
    std::string_view microbench{};
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
            explicit_order_count = true;
            micro_count = order_count;
        } else if (argument == "--profile") {
            if (i + 1 >= argc ||
                !basalt::bench::parse_profile(argv[++i], selected)) {
                std::cerr << "--profile must name one of the twelve workload IDs\n";
                return 2;
            }
            all_profiles = false;
        } else if (argument == "--no-baseline") {
            compare_baseline = false;
        } else if (argument == "--arena-pressure") {
            arena_pressure = true;
        } else if (argument == "--microbench") {
            if (i + 1 >= argc) {
                std::cerr << "--microbench requires fixed-math or event-log\n";
                return 2;
            }
            microbench = argv[++i];
            if (microbench != "fixed-math" && microbench != "event-log") {
                std::cerr << "--microbench requires fixed-math or event-log\n";
                return 2;
            }
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            print_usage();
            return 2;
        }
    }

    try {
        print_environment();
        if (!microbench.empty()) {
            if (!explicit_order_count) micro_count = 1000;
            if (microbench == "event-log" && micro_count > 10'000)
                throw std::invalid_argument("EventLog microbenchmark is capped at 10,000 durable appends");
            if (microbench == "fixed-math") run_fixed_math_microbench(micro_count);
            else run_event_log_microbench(micro_count);
            return 0;
        }
        const basalt::bench::BenchmarkRunner runner;
        if (arena_pressure) {
            if (order_count < 2) throw std::invalid_argument("Arena pressure requires at least two orders");
            const auto capacity = std::max<std::size_t>(1, order_count / 4U);
            const auto result = runner.run_arena_pressure(order_count, capacity);
            std::cout << "BASALT arena pressure | operations=" << order_count
                      << " | intentionally undersized pool capacity=" << capacity << '\n';
            std::cout << basalt::bench::format_ascii_table({result});
            return 0;
        }
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

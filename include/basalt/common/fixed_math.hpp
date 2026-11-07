#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace basalt::common {

inline constexpr std::uint64_t BPS_SCALE = 10'000;

namespace detail {

struct Uint128 {
    std::uint64_t high{};
    std::uint64_t low{};
};

[[nodiscard]] constexpr Uint128 mul_u64_to_u128(const std::uint64_t left,
                                                const std::uint64_t right) noexcept {
    const std::uint64_t left_lo = left & 0xFFFF'FFFFULL;
    const std::uint64_t left_hi = left >> 32U;
    const std::uint64_t right_lo = right & 0xFFFF'FFFFULL;
    const std::uint64_t right_hi = right >> 32U;

    const std::uint64_t p0 = left_lo * right_lo;
    const std::uint64_t p1 = left_lo * right_hi;
    const std::uint64_t p2 = left_hi * right_lo;
    const std::uint64_t p3 = left_hi * right_hi;

    const std::uint64_t middle =
        (p0 >> 32U) + (p1 & 0xFFFF'FFFFULL) + (p2 & 0xFFFF'FFFFULL);
    const std::uint64_t low = (p0 & 0xFFFF'FFFFULL) | (middle << 32U);
    const std::uint64_t high = p3 + (p1 >> 32U) + (p2 >> 32U) + (middle >> 32U);
    return Uint128{high, low};
}

// Precondition: dividend.high < divisor (guarantees quotient fits in uint64_t).
[[nodiscard]] constexpr std::uint64_t div_u128_by_u64(const Uint128 dividend,
                                                      const std::uint64_t divisor) noexcept {
    if (dividend.high == 0) {
        return dividend.low / divisor;
    }
    std::uint64_t quotient = 0;
    std::uint64_t remainder = dividend.high;
    for (int bit = 63; bit >= 0; --bit) {
        const std::uint64_t carry = remainder >> 63U;
        remainder = (remainder << 1U) | ((dividend.low >> static_cast<unsigned>(bit)) & 1ULL);
        if (carry != 0 || remainder >= divisor) {
            remainder -= divisor;
            quotient |= (1ULL << static_cast<unsigned>(bit));
        }
    }
    return quotient;
}

// Computes floor((remainder * BPS_SCALE) / denominator) for remainder < denominator.
[[nodiscard]] constexpr std::uint64_t scale_remainder_bps_portable(
    const std::uint64_t remainder, const std::uint64_t denominator) noexcept {
    if (remainder == 0 || denominator == 0) {
        return 0;
    }
    const auto product = mul_u64_to_u128(remainder, BPS_SCALE);
    return div_u128_by_u64(product, denominator);
}

[[nodiscard]] constexpr std::uint64_t scale_remainder_bps(
    const std::uint64_t remainder, const std::uint64_t denominator) noexcept {
    if (remainder == 0 || denominator == 0) {
        return 0;
    }
#ifdef __SIZEOF_INT128__
    __extension__ using uint128_t = unsigned __int128;
    const uint128_t product =
        static_cast<uint128_t>(remainder) * static_cast<uint128_t>(BPS_SCALE);
#if !defined(_MSC_VER)
    return static_cast<std::uint64_t>(product / static_cast<uint128_t>(denominator));
#else
    return div_u128_by_u64(
        Uint128{static_cast<std::uint64_t>(product >> 64U), static_cast<std::uint64_t>(product)},
        denominator);
#endif
#else
    return scale_remainder_bps_portable(remainder, denominator);
#endif
}

} // namespace detail

// Computes floor((numerator * 10,000) / denominator), returning nullopt when denominator == 0
// and saturating at int64_t::max() on whole-part overflow.
[[nodiscard]] constexpr std::optional<std::int64_t> ratio_bps(
    const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
    if (denominator == 0) {
        return std::nullopt;
    }
    const auto whole = numerator / denominator;
    if (whole > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() /
                                           static_cast<std::int64_t>(BPS_SCALE))) {
        return std::numeric_limits<std::int64_t>::max();
    }
    const auto remainder = numerator % denominator;
    const auto fraction = detail::scale_remainder_bps(remainder, denominator);
    return static_cast<std::int64_t>(whole * BPS_SCALE + fraction);
}

// Computes floor((numerator * 10,000) / denominator) clamped to [0, 10,000],
// returning 0 when numerator == 0 or denominator == 0.
[[nodiscard]] constexpr std::uint64_t clamped_ratio_bps(
    const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
    if (denominator == 0 || numerator == 0) {
        return 0;
    }
    if (numerator >= denominator) {
        return BPS_SCALE;
    }
    return detail::scale_remainder_bps(numerator, denominator);
}

} // namespace basalt::common

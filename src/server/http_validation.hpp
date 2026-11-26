#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

namespace basalt::server::detail {

enum class MutationBodySchema { Order, Step, Reset, Benchmark };

struct MutationBodyValidation {
    nlohmann::json body{};
    const char* code{};
    const char* message{};

    [[nodiscard]] explicit operator bool() const noexcept { return code == nullptr; }
};

// Shared by the HTTP handlers and the libFuzzer target so parser and schema
// checks run against the same production validation boundary.
[[nodiscard]] MutationBodyValidation validate_mutation_body(
    std::string_view text, MutationBodySchema schema);

} // namespace basalt::server::detail

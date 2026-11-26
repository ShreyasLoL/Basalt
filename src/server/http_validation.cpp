#include "http_validation.hpp"

#include "basalt/bench/workload_generator.hpp"
#include "basalt/core/types.hpp"
#include "basalt/server/request_guard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>

namespace basalt::server::detail {
namespace {

using Json = nlohmann::json;
constexpr std::size_t maximum_json_depth = 16;

MutationBodyValidation invalid(const char* code, const char* message) {
    return {Json{}, code, message};
}

bool json_depth_within_limit(const std::string_view text) {
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char ch : text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') in_string = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > maximum_json_depth) return false;
        } else if ((ch == '}' || ch == ']') && depth > 0) {
            --depth;
        }
    }
    return true;
}

bool fields_valid(const Json& body, const std::span<const std::string_view> required,
                  const std::span<const std::string_view> allowed,
                  MutationBodyValidation& error) {
    for (const auto field : required) {
        if (!body.contains(std::string(field))) {
            error = invalid("INVALID_SCHEMA", "A required request field is missing");
            return false;
        }
    }
    for (auto field = body.begin(); field != body.end(); ++field) {
        if (std::find(allowed.begin(), allowed.end(), field.key()) == allowed.end()) {
            error = invalid("INVALID_SCHEMA", "Request contains an unexpected field");
            return false;
        }
    }
    return true;
}

bool unsigned_value(const Json& value, std::uint64_t& output) {
    if (value.type() == Json::value_t::number_unsigned) {
        output = value.get<std::uint64_t>();
        return true;
    }
    if (value.type() != Json::value_t::number_integer) return false;
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) return false;
    output = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool unsigned_range(const Json& value, const std::uint64_t minimum,
                    const std::uint64_t maximum, std::uint64_t& output) {
    return unsigned_value(value, output) && output >= minimum && output <= maximum;
}

bool signed_range(const Json& value, const std::int64_t minimum,
                  const std::int64_t maximum, std::int64_t& output) {
    if (value.type() == Json::value_t::number_integer) {
        output = value.get<std::int64_t>();
    } else if (value.type() == Json::value_t::number_unsigned) {
        const auto unsigned_number = value.get<std::uint64_t>();
        if (unsigned_number > static_cast<std::uint64_t>(maximum)) return false;
        output = static_cast<std::int64_t>(unsigned_number);
    } else {
        return false;
    }
    return output >= minimum && output <= maximum;
}

bool equals_ascii_case_insensitive(const std::string_view value, const std::string_view expected) {
    if (value.size() != expected.size()) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[index]);
        const auto right = static_cast<unsigned char>(expected[index]);
        if (std::toupper(left) != right) return false;
    }
    return true;
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

std::optional<MutationBodyValidation> validate_values(const Json& body,
                                                       const MutationBodySchema schema) {
    switch (schema) {
    case MutationBodySchema::Order: {
        if (body.contains("id")) {
            std::uint64_t id = 0;
            if (!unsigned_value(body.at("id"), id))
                return invalid("INVALID_FIELD", "id must be an unsigned integer");
        }
        const auto& side = body.at("side");
        if (!side.is_string() || side.get_ref<const std::string&>().size() > 8 ||
            (!equals_ascii_case_insensitive(side.get_ref<const std::string&>(), "BUY") &&
             !equals_ascii_case_insensitive(side.get_ref<const std::string&>(), "SELL")))
            return invalid("INVALID_FIELD", "side must be BUY or SELL");
        const auto& type = body.at("type");
        if (!type.is_string() || type.get_ref<const std::string&>().size() > 8 ||
            (!equals_ascii_case_insensitive(type.get_ref<const std::string&>(), "LIMIT") &&
             !equals_ascii_case_insensitive(type.get_ref<const std::string&>(), "MARKET")))
            return invalid("INVALID_FIELD", "type must be LIMIT or MARKET");
        std::uint64_t quantity = 0;
        if (!unsigned_range(body.at("quantity"), 1, basalt::MAX_ORDER_QUANTITY, quantity))
            return invalid("INVALID_FIELD", "quantity must be an integer between 1 and 1000000000");
        if (equals_ascii_case_insensitive(type.get_ref<const std::string&>(), "LIMIT")) {
            if (!body.contains("price"))
                return invalid("INVALID_FIELD", "LIMIT orders require an integer price");
            std::int64_t price = 0;
            if (!signed_range(body.at("price"), 1, basalt::MAX_ORDER_PRICE, price))
                return invalid("INVALID_FIELD", "price must be an integer between 1 and 1000000000000");
        } else if (body.contains("price")) {
            std::int64_t price = 0;
            if (!signed_range(body.at("price"), 0, 0, price))
                return invalid("INVALID_FIELD", "MARKET order price, when present, must be zero");
        }
        break;
    }
    case MutationBodySchema::Step:
        if (body.contains("count")) {
            std::uint64_t count = 0;
            if (!unsigned_range(body.at("count"), 1, 500, count))
                return invalid("INVALID_FIELD", "count must be an integer between 1 and 500");
        }
        break;
    case MutationBodySchema::Reset:
        if (body.contains("seed_demo") && !body.at("seed_demo").is_boolean())
            return invalid("INVALID_FIELD", "seed_demo must be a boolean");
        break;
    case MutationBodySchema::Benchmark:
        if (body.contains("profile")) {
            const auto& profile = body.at("profile");
            if (!profile.is_string() || profile.get_ref<const std::string&>().size() > 32)
                return invalid("INVALID_FIELD", "profile must name a supported workload");
            const auto normalized = uppercase(profile.get<std::string>());
            bench::WorkloadProfile parsed{};
            if (normalized != "ALL" && !bench::parse_profile(normalized, parsed))
                return invalid("INVALID_FIELD", "profile must be ALL or a supported workload");
        }
        if (body.contains("order_count")) {
            std::uint64_t count = 0;
            if (!unsigned_range(body.at("order_count"), 1,
                                MAX_HTTP_BENCHMARK_ORDER_COUNT, count))
                return invalid("INVALID_FIELD", "order_count must be an integer between 1 and 10000");
        }
        if (body.contains("compare_baseline") && !body.at("compare_baseline").is_boolean())
            return invalid("INVALID_FIELD", "compare_baseline must be a boolean");
        break;
    }
    return std::nullopt;
}

} // namespace

MutationBodyValidation validate_mutation_body(const std::string_view text,
                                               const MutationBodySchema schema) {
    const bool allow_empty = schema != MutationBodySchema::Order;
    Json body;
    if (text.empty() && allow_empty) {
        body = Json::object();
    } else {
        if (!json_depth_within_limit(text))
            return invalid("JSON_TOO_DEEP", "JSON request exceeds the maximum nesting depth");
        try {
            body = Json::parse(text);
        } catch (const Json::exception&) {
            return invalid("INVALID_JSON", "Request body must be valid JSON");
        }
        if (!body.is_object()) return invalid("INVALID_SCHEMA", "Request body must be a JSON object");
    }

    static constexpr std::array<std::string_view, 0> no_fields{};
    static constexpr std::array<std::string_view, 3> order_required{"side", "type", "quantity"};
    static constexpr std::array<std::string_view, 5> order_allowed{"id", "side", "type", "price", "quantity"};
    static constexpr std::array<std::string_view, 1> step_allowed{"count"};
    static constexpr std::array<std::string_view, 1> reset_allowed{"seed_demo"};
    static constexpr std::array<std::string_view, 3> benchmark_allowed{"profile", "order_count", "compare_baseline"};
    std::span<const std::string_view> required = no_fields;
    std::span<const std::string_view> allowed = no_fields;
    switch (schema) {
    case MutationBodySchema::Order:
        required = order_required;
        allowed = order_allowed;
        break;
    case MutationBodySchema::Step:
        allowed = step_allowed;
        break;
    case MutationBodySchema::Reset:
        allowed = reset_allowed;
        break;
    case MutationBodySchema::Benchmark:
        allowed = benchmark_allowed;
        break;
    }

    MutationBodyValidation field_error{};
    if (!fields_valid(body, required, allowed, field_error)) return field_error;
    if (const auto value_error = validate_values(body, schema)) return *value_error;
    return {std::move(body), nullptr, nullptr};
}

} // namespace basalt::server::detail

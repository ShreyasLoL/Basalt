#pragma once

#include "basalt/common/types.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace basalt::common {

enum class LogOutput : std::uint8_t { Json, Console };

struct AppConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint32_t port{8080};
    bool allow_remote_bind{false};
    LogOutput log_output{LogOutput::Json};
    std::uint32_t event_queue_capacity{65'536};
    std::uint32_t ui_queue_capacity{1'024};
    std::uint64_t stale_timeout_ms{5'000};
};

class ConfigLoader {
public:
    using Environment = std::unordered_map<std::string, std::string>;

    [[nodiscard]] static Result<AppConfig> from_environment() {
        Environment values;
        constexpr std::string_view names[]{"BASALT_BIND_ADDRESS", "BASALT_PORT",
            "BASALT_ALLOW_REMOTE_BIND", "BASALT_LOG_FORMAT",
            "BASALT_EVENT_QUEUE_CAPACITY", "BASALT_UI_QUEUE_CAPACITY",
            "BASALT_STALE_TIMEOUT_MS"};
        for (const std::string_view name : names) {
            const std::string terminated_name(name);
#if defined(_WIN32)
            char* value = nullptr;
            std::size_t value_size = 0;
            if (_dupenv_s(&value, &value_size, terminated_name.c_str()) == 0 && value != nullptr) {
                values.emplace(terminated_name, value);
                std::free(value);
            }
#else
            if (const char* value = std::getenv(terminated_name.c_str()); value != nullptr) {
                values.emplace(terminated_name, value);
            }
#endif
        }
        return parse(values);
    }

    [[nodiscard]] static Result<AppConfig> parse(const Environment& values) {
        AppConfig config;
        if (const auto found = values.find("BASALT_BIND_ADDRESS"); found != values.end()) {
            config.bind_address = found->second;
        }
        if (const auto found = values.find("BASALT_PORT"); found != values.end()) {
            if (!parse_integer(found->second, config.port)) {
                return failure("INVALID_PORT", "BASALT_PORT must be an unsigned integer");
            }
        }
        if (const auto found = values.find("BASALT_ALLOW_REMOTE_BIND"); found != values.end()) {
            if (!parse_boolean(found->second, config.allow_remote_bind)) {
                return failure("INVALID_BOOLEAN", "BASALT_ALLOW_REMOTE_BIND must be true or false");
            }
        }
        if (const auto found = values.find("BASALT_LOG_FORMAT"); found != values.end()) {
            if (equals_ascii_insensitive(found->second, "json")) config.log_output = LogOutput::Json;
            else if (equals_ascii_insensitive(found->second, "console")) {
                config.log_output = LogOutput::Console;
            } else {
                return failure("INVALID_LOG_FORMAT", "BASALT_LOG_FORMAT must be json or console");
            }
        }
        if (const auto found = values.find("BASALT_EVENT_QUEUE_CAPACITY"); found != values.end() &&
            !parse_integer(found->second, config.event_queue_capacity)) {
            return failure("INVALID_QUEUE_CAPACITY", "BASALT_EVENT_QUEUE_CAPACITY must be an unsigned integer");
        }
        if (const auto found = values.find("BASALT_UI_QUEUE_CAPACITY"); found != values.end() &&
            !parse_integer(found->second, config.ui_queue_capacity)) {
            return failure("INVALID_QUEUE_CAPACITY", "BASALT_UI_QUEUE_CAPACITY must be an unsigned integer");
        }
        if (const auto found = values.find("BASALT_STALE_TIMEOUT_MS"); found != values.end() &&
            !parse_integer(found->second, config.stale_timeout_ms)) {
            return failure("INVALID_STALE_TIMEOUT", "BASALT_STALE_TIMEOUT_MS must be an unsigned integer");
        }
        return validate(std::move(config));
    }

    [[nodiscard]] static Result<AppConfig> validate(AppConfig config) {
        if (config.port == 0 || config.port > 65'535) {
            return failure("INVALID_PORT", "Port must be between 1 and 65535");
        }
        if (config.bind_address.empty() || config.bind_address.size() > 255) {
            return failure("INVALID_BIND_ADDRESS", "Bind address must contain 1 to 255 characters");
        }
        for (const char character : config.bind_address) {
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                return failure("INVALID_BIND_ADDRESS", "Bind address cannot contain whitespace");
            }
        }
        if (!config.allow_remote_bind && config.bind_address != "127.0.0.1" &&
            config.bind_address != "::1") {
            return failure("REMOTE_BIND_DISABLED", "Non-loopback bind requires explicit opt-in");
        }
        if (config.event_queue_capacity == 0 || config.event_queue_capacity > 1'000'000 ||
            config.ui_queue_capacity == 0 || config.ui_queue_capacity > 1'000'000) {
            return failure("INVALID_QUEUE_CAPACITY", "Queue capacity must be between 1 and 1000000");
        }
        if (config.stale_timeout_ms == 0 || config.stale_timeout_ms > 86'400'000) {
            return failure("INVALID_STALE_TIMEOUT", "Stale timeout must be between 1 ms and 24 hours");
        }
        return Result<AppConfig>::success(std::move(config));
    }

private:
    template <typename Integer>
    [[nodiscard]] static bool parse_integer(const std::string_view text, Integer& value) noexcept {
        if (text.empty()) return false;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && end == text.data() + text.size();
    }

    [[nodiscard]] static bool equals_ascii_insensitive(const std::string_view left,
                                                        const std::string_view right) noexcept {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i) {
            char character = left[i];
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
            if (character != right[i]) return false;
        }
        return true;
    }

    [[nodiscard]] static bool parse_boolean(const std::string_view text, bool& value) noexcept {
        if (equals_ascii_insensitive(text, "true") || text == "1") {
            value = true;
            return true;
        }
        if (equals_ascii_insensitive(text, "false") || text == "0") {
            value = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] static Result<AppConfig> failure(std::string code, std::string message) {
        return Result<AppConfig>::failure(Error{std::move(code), std::move(message)});
    }
};

} // namespace basalt::common

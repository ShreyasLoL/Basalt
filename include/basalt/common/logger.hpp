#pragma once

#include <initializer_list>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace basalt::common {

enum class LogLevel : unsigned char { Debug, Info, Warning, Error };
enum class LogFormat : unsigned char { Json, Console };

struct LogField {
    std::string_view name;
    std::string_view value;
};

class Logger {
public:
    explicit Logger(std::ostream& output = std::clog,
                    const LogFormat format = LogFormat::Json,
                    const LogLevel minimum_level = LogLevel::Info) noexcept
        : output_(output), format_(format), minimum_level_(minimum_level) {}

    void log(const LogLevel level, const std::string_view event,
             const std::initializer_list<LogField> fields = {}) {
        if (static_cast<unsigned char>(level) < static_cast<unsigned char>(minimum_level_)) return;
        std::lock_guard<std::mutex> guard(mutex_);
        if (format_ == LogFormat::Json) {
            output_ << "{\"level\":\"" << level_name(level) << "\",\"event\":\"";
            write_json_string(output_, redact_embedded(event));
            output_ << '"';
            for (const LogField& field : fields) {
                output_ << ",\"";
                write_json_string(output_, redact_embedded(field.name));
                output_ << "\":\"";
                const std::string value = is_secret_key(field.name)
                    ? "[REDACTED]" : redact_embedded(field.value);
                write_json_string(output_, value);
                output_ << '"';
            }
            output_ << "}\n";
        } else {
            output_ << '[' << level_name(level) << "] " << redact_embedded(event);
            for (const LogField& field : fields) {
                output_ << ' ' << redact_embedded(field.name) << '=';
                if (is_secret_key(field.name)) output_ << "[REDACTED]";
                else output_ << redact_embedded(field.value);
            }
            output_ << '\n';
        }
        output_.flush();
    }

private:
    static constexpr std::string_view level_name(const LogLevel level) noexcept {
        switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        }
        return "UNKNOWN";
    }

    static char lower_ascii(const char value) noexcept {
        if (value >= 'A' && value <= 'Z') return static_cast<char>(value - 'A' + 'a');
        return value;
    }

    static bool equals_ascii_insensitive(const std::string_view left,
                                         const std::string_view right) noexcept {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (lower_ascii(left[i]) != right[i]) return false;
        }
        return true;
    }

    static bool is_secret_key(const std::string_view key) {
        const std::string normalized = [&key] {
            std::string result;
            result.reserve(key.size());
            for (const char value : key) result.push_back(lower_ascii(value));
            return result;
        }();
        return normalized.find("token") != std::string::npos ||
               normalized.find("password") != std::string::npos ||
               normalized.find("secret") != std::string::npos ||
               normalized.find("api_key") != std::string::npos ||
               normalized.find("apikey") != std::string::npos ||
               normalized.find("authorization") != std::string::npos ||
               normalized.find("private_key") != std::string::npos;
    }

    static bool is_secret_name_at(const std::string_view input, const std::size_t offset,
                                  std::size_t& name_end) noexcept {
        constexpr std::string_view names[]{"access_token", "refresh_token", "client_secret",
                                           "api_key", "apikey", "password", "secret",
                                           "authorization", "private_key", "token"};
        for (const std::string_view name : names) {
            if (offset + name.size() > input.size()) continue;
            if (!equals_ascii_insensitive(input.substr(offset, name.size()), name)) continue;
            name_end = offset + name.size();
            return true;
        }
        return false;
    }

    static std::string redact_embedded(const std::string_view input) {
        std::string result;
        result.reserve(input.size());
        std::size_t cursor = 0;
        while (cursor < input.size()) {
            std::size_t name_end = cursor;
            if (is_secret_name_at(input, cursor, name_end)) {
                std::size_t separator = name_end;
                while (separator < input.size() &&
                       (input[separator] == ' ' || input[separator] == '\t')) ++separator;
                if (separator < input.size() &&
                    (input[separator] == '=' || input[separator] == ':')) {
                    result.append(input.substr(cursor, separator - cursor + 1));
                    result += "[REDACTED]";
                    cursor = separator + 1;
                    while (cursor < input.size() && input[cursor] != ',' && input[cursor] != ';') ++cursor;
                    continue;
                }
            }
            result.push_back(input[cursor]);
            ++cursor;
        }
        return result;
    }

    static void write_json_string(std::ostream& output, const std::string_view value) {
        for (const unsigned char character : value) {
            switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) output << '?';
                else output << static_cast<char>(character);
            }
        }
    }

    std::ostream& output_;
    LogFormat format_;
    LogLevel minimum_level_;
    std::mutex mutex_{};
};

} // namespace basalt::common

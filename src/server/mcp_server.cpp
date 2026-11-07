#include "basalt/server/mcp_server.hpp"

#include "basalt/backtest/backtest.hpp"
#include "basalt/bench/benchmark_runner.hpp"
#include "basalt/common/types.hpp"
#include "basalt/core/matching_engine.hpp"
#include "basalt/persistence/event_log.hpp"
#include "basalt/persistence/history.hpp"
#include "basalt/server/simulation_facade.hpp"
#include "basalt/strategy/builtins.hpp"
#include "basalt/trading/risk.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef BASALT_GIT_REVISION
#define BASALT_GIT_REVISION "unknown"
#endif
#ifndef BASALT_GIT_WORKTREE_DIRTY
#define BASALT_GIT_WORKTREE_DIRTY true
#endif

namespace basalt::server {
namespace {

using Json = nlohmann::json;
using SteadyClock = std::chrono::steady_clock;
using ToolProperties = std::map<std::string, Json, std::less<>>;

constexpr std::size_t MAX_FRAME_BYTES = 256U * 1024U;
constexpr std::size_t MAX_AUDIT_BYTES = 1024U * 1024U;
constexpr std::size_t MAX_AUDIT_LINE_BYTES = 16U * 1024U;
constexpr std::size_t MAX_INLINE_BARS = 1000;
constexpr std::uint64_t MAX_BENCHMARK_OPERATIONS = 10'000;
constexpr std::int64_t SIM_BAR_INTERVAL_NS = 60'000'000'000LL;
constexpr std::int64_t SIM_FIRST_OPEN_NS = 1'000'000'000'000LL;
constexpr std::array<std::string_view, 4> SUPPORTED_PROTOCOLS{
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

Json object_schema(ToolProperties properties, std::vector<std::string> required = {}) {
    Json schema{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    for (auto& [key, value] : properties) schema["properties"][key] = std::move(value);
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

Json integer_schema(const std::int64_t minimum = 0, const std::int64_t maximum = 0) {
    Json schema{{"type", "integer"}, {"minimum", minimum}};
    if (maximum > 0) schema["maximum"] = maximum;
    return schema;
}

Json string_enum(std::initializer_list<std::string_view> values) {
    Json schema{{"type", "string"}, {"enum", Json::array()}};
    for (const auto value : values) schema["enum"].push_back(value);
    return schema;
}

Json bar_schema() {
    return object_schema({
        {"open_micro", integer_schema(1, 1'000'000'000)},
        {"high_micro", integer_schema(1, 1'000'000'000)},
        {"low_micro", integer_schema(1, 1'000'000'000)},
        {"close_micro", integer_schema(1, 1'000'000'000)},
        {"volume", integer_schema(1, 1'000'000'000)}},
        {"open_micro", "high_micro", "low_micro", "close_micro", "volume"});
}

Json bars_schema() {
    return Json{{"type", "array"}, {"minItems", 3}, {"maxItems", MAX_INLINE_BARS},
                {"items", bar_schema()}};
}

struct ToolDefinition {
    std::string_view name;
    std::string_view description;
    Json input_schema;
    bool read_only;
};

std::vector<ToolDefinition> tool_definitions() {
    const Json optional_limit = integer_schema(1, 50);
    const Json order_limit = integer_schema(1, 100);
    return {
        {"get_system_health", "Inspect BASALT process and local subsystem availability.",
            object_schema({}), true},
        {"get_market_watch", "Read the bounded market watch when a provider-backed quote service is connected.",
            object_schema({{"limit", optional_limit}, {"segment", string_enum({"SIM", "NSE", "BSE"})}}), true},
        {"get_instrument_snapshot", "Read a provider-backed instrument snapshot by numeric instrument ID.",
            object_schema({{"instrument_id", integer_schema(1)}}, {"instrument_id"}), true},
        {"get_order_book", "Read either the internal simulated book or an external observed book.",
            object_schema({{"source", string_enum({"internal_simulated", "external_observed"})},
                           {"depth", integer_schema(1, 100)}, {"instrument_id", integer_schema(1)}}, {"source"}), true},
        {"get_recent_trades", "Read recent executions from the internal simulated matching engine.",
            object_schema({{"limit", order_limit}}), true},
        {"get_paper_positions", "Read paper positions for a numeric account ID.",
            object_schema({{"account_id", integer_schema(1)}}, {"account_id"}), true},
        {"get_portfolio_summary", "Read paper cash, equity, P&L and fee totals for an account.",
            object_schema({{"account_id", integer_schema(1)}}, {"account_id"}), true},
        {"get_strategy_status", "Read lifecycle and signal status for a numeric strategy ID.",
            object_schema({{"strategy_id", integer_schema(1)}}, {"strategy_id"}), true},
        {"inspect_feed_health", "Inspect provider feed state and counters for an optional instrument ID.",
            object_schema({{"instrument_id", integer_schema(1)}}), true},
        {"run_backtest", "Run a bounded paper backtest over caller-supplied inline OHLCV bars.",
            object_schema({{"strategy", string_enum({"SMA_CROSSOVER", "RSI_MEAN_REVERSION", "BOLLINGER",
                                                       "VWAP_REVERSION", "MOMENTUM_BREAKOUT", "ORDER_BOOK_IMBALANCE"})},
                           {"bars", bars_schema()}, {"seed", integer_schema(0, 9'223'372'036'854'775'000LL)}},
                          {"strategy", "bars"}), false},
        {"run_replay", "Start or control one bounded in-memory replay of inline paper bars.",
            object_schema({{"action", string_enum({"start", "step", "play", "pause"})},
                           {"bars", bars_schema()}, {"steps", integer_schema(1, 500)},
                           {"speed", string_enum({"1x", "5x", "10x", "MAX"})}}, {"action"}), false},
        {"run_test_suite", "Run a named in-process BASALT verification suite; no command strings are accepted.",
            object_schema({{"suite", string_enum({"matching", "analytics", "all"})}}, {"suite"}), false},
        {"run_benchmark", "Run one allowlisted workload with bounded operation count and implementation selection.",
            object_schema({{"profile", string_enum({"BALANCED", "BUY_HEAVY", "SELL_HEAVY", "AGGRESSIVE_MATCHING",
                "CANCEL_HEAVY", "CONCENTRATED_LEVELS", "WIDE_DISTRIBUTION", "BURSTY_ARRIVALS",
                "HIGH_ACTIVE_ORDERS", "MULTI_INSTRUMENT_50", "MARKET_DATA_BURST", "SLOW_CONSUMER"})},
                {"orders", integer_schema(1, static_cast<std::int64_t>(MAX_BENCHMARK_OPERATIONS))},
                {"implementation", string_enum({"all", "map_list", "vector", "arena"})}}), false},
        {"trigger_kill_switch", "Latch a global, account or strategy paper-mode kill switch. It cannot be cleared through MCP.",
            object_schema({{"scope", string_enum({"global", "account", "strategy"})},
                           {"target_id", integer_schema(1)}, {"reason", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 512}}}},
                          {"scope", "reason"}), false}
    };
}

const ToolDefinition* find_tool(const std::string_view name) {
    static const auto definitions = tool_definitions();
    const auto found = std::find_if(definitions.begin(), definitions.end(), [name](const auto& item) {
        return item.name == name;
    });
    return found == definitions.end() ? nullptr : &*found;
}

Json tools_list_json() {
    Json tools = Json::array();
    for (const auto& definition : tool_definitions()) {
        tools.push_back({{"name", definition.name}, {"description", definition.description},
            {"inputSchema", definition.input_schema},
            {"annotations", {{"readOnlyHint", definition.read_only},
                             {"destructiveHint", definition.name == "trigger_kill_switch"},
                             {"idempotentHint", definition.read_only}, {"openWorldHint", false}}}});
    }
    return Json{{"tools", std::move(tools)}};
}

bool is_unsigned_integer(const Json& value) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) return true;
    return value.get<std::int64_t>() >= 0;
}

bool uint_value(const Json& value, std::uint64_t& output) {
    if (!is_unsigned_integer(value)) return false;
    if (value.is_number_unsigned()) output = value.get<std::uint64_t>();
    else output = static_cast<std::uint64_t>(value.get<std::int64_t>());
    return true;
}

struct ValidationError {
    std::string code;
    std::string message;
};

ValidationError validate_arguments(const ToolDefinition& definition, const Json& args) {
    if (!args.is_object()) return {"INVALID_ARGUMENTS", "Arguments must be a JSON object."};
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (!definition.input_schema["properties"].contains(it.key()))
            return {"UNKNOWN_ARGUMENT", "Arguments contain an unsupported field."};
    }
    if (definition.input_schema.contains("required")) {
        for (const auto& required : definition.input_schema["required"])
            if (!args.contains(required.get<std::string>()))
                return {"REQUIRED_ARGUMENT", "A required argument is missing."};
    }
    for (auto it = args.begin(); it != args.end(); ++it) {
        const Json& schema = definition.input_schema["properties"][it.key()];
        const Json& value = it.value();
        const std::string type = schema.value("type", std::string{});
        if ((type == "integer" && !value.is_number_integer()) ||
            (type == "string" && !value.is_string()) ||
            (type == "array" && !value.is_array()))
            return {"ARGUMENT_TYPE", "An argument has the wrong JSON type."};
        if (type == "integer") {
            std::uint64_t number{};
            if (!uint_value(value, number)) return {"ARGUMENT_RANGE", "An integer argument is outside its permitted range."};
            if (schema.contains("minimum") && number < schema["minimum"].get<std::uint64_t>())
                return {"ARGUMENT_RANGE", "An integer argument is outside its permitted range."};
            if (schema.contains("maximum") && number > schema["maximum"].get<std::uint64_t>())
                return {"ARGUMENT_RANGE", "An integer argument is outside its permitted range."};
        }
        if (type == "string") {
            const auto text = value.get<std::string>();
            if (schema.contains("enum")) {
                bool found = false;
                for (const auto& candidate : schema["enum"])
                    found = found || (candidate.is_string() && candidate.get<std::string>() == text);
                if (!found) return {"ARGUMENT_ENUM", "A string argument is not in its permitted set."};
            }
            if (schema.contains("minLength") && text.size() < schema["minLength"].get<std::size_t>())
                return {"ARGUMENT_LENGTH", "A string argument is outside its permitted length."};
            if (schema.contains("maxLength") && text.size() > schema["maxLength"].get<std::size_t>())
                return {"ARGUMENT_LENGTH", "A string argument is outside its permitted length."};
        }
        if (type == "array") {
            if (value.size() < schema["minItems"].get<std::size_t>() ||
                value.size() > schema["maxItems"].get<std::size_t>())
                return {"ARGUMENT_SIZE", "An array argument is outside its permitted size."};
            const Json item_schema = schema["items"];
            ToolDefinition item_definition{"inline_bar", "", object_schema({
                {"open_micro", item_schema["properties"]["open_micro"]},
                {"high_micro", item_schema["properties"]["high_micro"]},
                {"low_micro", item_schema["properties"]["low_micro"]},
                {"close_micro", item_schema["properties"]["close_micro"]},
                {"volume", item_schema["properties"]["volume"]}},
                {"open_micro", "high_micro", "low_micro", "close_micro", "volume"}), false};
            for (const auto& bar : value) {
                auto nested = validate_arguments(item_definition, bar);
                if (!nested.code.empty()) return {"BAR_" + nested.code, "An OHLCV bar is invalid."};
            }
        }
    }
    // Required integer parameters are range-checked too; optional bounds above are
    // checked for every supplied value, including signed/oversized JSON numbers.
    return {};
}

Json error_content(std::string code, std::string message) {
    return Json{{"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
}

Json mcp_tool_response(const Json& id, const Json& structured, const bool is_error) {
    return Json{{"jsonrpc", "2.0"}, {"id", id},
        {"result", {{"content", Json::array({Json{{"type", "text"}, {"text", structured.dump()}}})},
                    {"structuredContent", structured}, {"isError", is_error}}}};
}

Json json_rpc_error(const Json& id, const int code, const std::string_view message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json trade_json(const Trade& trade) {
    return Json{{"trade_id", trade.trade_id}, {"buy_order_id", trade.buy_order_id},
        {"sell_order_id", trade.sell_order_id}, {"aggressor_side", to_string(trade.aggressor_side)},
        {"price", trade.price}, {"quantity", trade.quantity}, {"sequence", trade.sequence},
        {"timestamp_ns", trade.timestamp_ns}};
}

Json level_json(const PriceLevelSummary& level) {
    return Json{{"price", level.price}, {"quantity", level.total_volume}, {"order_count", level.order_count}};
}

Json snapshot_json(const BookSnapshot& snapshot, const std::size_t depth) {
    Json bids = Json::array();
    Json asks = Json::array();
    for (std::size_t i = 0; i < std::min(depth, snapshot.bids.size()); ++i) bids.push_back(level_json(snapshot.bids[i]));
    for (std::size_t i = 0; i < std::min(depth, snapshot.asks.size()); ++i) asks.push_back(level_json(snapshot.asks[i]));
    return Json{{"bids", std::move(bids)}, {"asks", std::move(asks)},
        {"best_bid", snapshot.has_best_bid ? Json(snapshot.best_bid) : Json(nullptr)},
        {"best_ask", snapshot.has_best_ask ? Json(snapshot.best_ask) : Json(nullptr)},
        {"spread", snapshot.has_spread ? Json(snapshot.spread) : Json(nullptr)},
        {"depth", depth}};
}

Json metric_json(const backtest::AnalyticsMetricValue& metric) {
    return Json{{"name", metric.name}, {"value", metric.value ? Json(*metric.value) : Json(nullptr)},
        {"status", metric.status}, {"units", metric.units},
        {"amount_micro", metric.amount_micro ? Json(*metric.amount_micro) : Json(nullptr)}};
}

bool valid_request_id(const Json& id) {
    return id.is_string() || id.is_number_integer();
}

std::int64_t system_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<std::filesystem::path> read_env_data_dir() {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "BASALT_DATA_DIR") == 0 && value != nullptr) {
        std::string text(value);
        std::free(value);
        if (!text.empty()) return std::filesystem::path(std::move(text));
    }
#else
    if (const char* value = std::getenv("BASALT_DATA_DIR"); value != nullptr && value[0] != '\0') {
        return std::filesystem::path(value);
    }
#endif
    return std::nullopt;
}

std::filesystem::path executable_directory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) break;
        if (copied < buffer.size() - 1U) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        if (buffer.size() >= 32768U) break;
        buffer.resize(buffer.size() * 2U);
    }
#elif defined(__linux__)
    std::error_code error;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !exe.empty()) return exe.parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 1024;
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) return std::filesystem::path(buffer.c_str()).parent_path();
    } else {
        return std::filesystem::path(buffer.c_str()).parent_path();
    }
#endif
    return {};
}

bool prepare_owner_only_dir(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error || !std::filesystem::is_directory(dir, error) || error) return false;
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    return true;
}

} // namespace

std::filesystem::path McpServer::resolve_data_dir(
    std::optional<std::filesystem::path> env_dir_override,
    std::optional<std::filesystem::path> exe_dir_override,
    std::ostream* warning_stream) {
    const auto env_candidate = env_dir_override.has_value() ? env_dir_override : read_env_data_dir();
    if (env_candidate && !env_candidate->empty() && prepare_owner_only_dir(*env_candidate)) {
        return *env_candidate;
    }
    const auto exe_candidate = exe_dir_override.has_value() ? *exe_dir_override : executable_directory();
    if (!exe_candidate.empty() && prepare_owner_only_dir(exe_candidate)) {
        return exe_candidate;
    }
    std::ostream& warn = warning_stream != nullptr ? *warning_stream : std::cerr;
    warn << "Warning: BASALT MCP falling back to OS temporary directory for local data storage\n";
    std::error_code error;
    auto temp_dir = std::filesystem::temp_directory_path(error);
    if (!temp_dir.empty()) {
        temp_dir /= "basalt-mcp";
        (void)prepare_owner_only_dir(temp_dir);
    }
    return temp_dir;
}

struct McpServer::Impl {
    struct ToolOutcome {
        Json content;
        bool error{};
        std::string error_code;
    };
    struct CachedRequest {
        std::string fingerprint;
        Json response;
    };

    std::filesystem::path audit_path;
    Clock clock;
    persistence::EventLog kill_log;
    trading::KillSwitch kill_switch;
    SimulationFacade simulation;
    bench::BenchmarkRunner benchmark_runner;
    std::unique_ptr<backtest::ReplayController> replay;
    bool initialized{};
    bool stopping{};
    std::string negotiated_protocol;
    std::string init_error;
    std::uint64_t total_tool_calls{};
    std::map<std::string, std::deque<SteadyClock::time_point>, std::less<>> rate_windows;
    std::map<std::string, CachedRequest, std::less<>> idempotency_cache;
    std::deque<std::string> cache_order;

    Impl(std::filesystem::path audit, std::filesystem::path kill_path, Clock supplied_clock)
        : audit_path(audit.empty() ? (McpServer::resolve_data_dir() / "mcp_audit.jsonl") : std::move(audit)),
          clock(supplied_clock ? std::move(supplied_clock) : Clock(system_now_ns)),
          kill_log(kill_path.empty() ? (McpServer::resolve_data_dir() / "mcp_killswitch.blog") : std::move(kill_path), 4096),
          kill_switch([this](const trading::KillSwitchEvent& event) { return persist_kill(event); }) {
        const auto recovered = kill_log.recover();
        if (!recovered) return;
        std::error_code error;
        if (std::filesystem::file_size(kill_log.path(), error) > MAX_AUDIT_BYTES || error) return;
        const auto frames = kill_log.read_all();
        if (!frames || !kill_switch.restore(frames.value())) {
            init_error = "KILL_LOG_INVALID";
            return;
        }
        std::filesystem::permissions(kill_log.path(), std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
    }

    common::Result<void> persist_kill(const trading::KillSwitchEvent& event) {
        std::error_code error;
        const auto size = std::filesystem::file_size(kill_log.path(), error);
        if (error || size > MAX_AUDIT_BYTES) return common::Result<void>::failure(
            {"KILL_LOG_LIMIT", "Kill-switch persistence limit reached"});
        Json frame{{"kind", "kill_switch"}, {"sequence", event.sequence},
            {"scope", event.scope == trading::KillScope::Global ? "GLOBAL" :
                (event.scope == trading::KillScope::Account ? "ACCOUNT" : "STRATEGY")},
            {"target_id", event.target_id}, {"active", event.active}, {"actor", "local-stdio"},
            {"reason", "[REDACTED]"}, {"time_ns", event.time_ns},
            {"working_orders_cancelled", event.working_orders_cancelled}};
        if (frame.dump().size() > 4096U) return common::Result<void>::failure(
            {"KILL_LOG_LIMIT", "Kill-switch record exceeds its fixed size"});
        const auto appended = kill_log.append(event.sequence, frame.dump());
        if (!appended) return appended;
        std::filesystem::permissions(kill_log.path(), std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
        return common::Result<void>::success();
    }

    common::Result<void> append_audit(const std::string_view tool, const Json& arguments,
                                      const Json& id, const std::string_view outcome,
                                      const std::string_view error_code = {}) {
        Json keys = Json::array();
        std::size_t unknown_key_count = 0;
        if (arguments.is_object()) {
            const auto* definition = find_tool(tool);
            for (auto it = arguments.begin(); it != arguments.end(); ++it) {
                if (definition != nullptr && definition->input_schema["properties"].contains(it.key()))
                    keys.push_back(it.key());
                else
                    ++unknown_key_count;
            }
        }
        const std::string id_text = id.dump();
        Json record{{"time_ns", clock()}, {"actor", "local-stdio"}, {"method", "tools/call"},
            {"tool", find_tool(tool) == nullptr ? "UNRECOGNIZED" : std::string(tool)},
            {"request_id_sha256", persistence::HistoryStore::sha256(id_text)},
            {"parameter_keys", std::move(keys)}, {"unknown_key_count", unknown_key_count},
            {"outcome", outcome}};
        if (!error_code.empty()) record["error_code"] = error_code;
        std::string line = record.dump();
        line.push_back('\n');
        if (line.size() > MAX_AUDIT_LINE_BYTES) return common::Result<void>::failure(
            {"AUDIT_RECORD_LIMIT", "Audit record exceeds its fixed size"});
        std::error_code error;
        if (!audit_path.parent_path().empty()) {
            std::filesystem::create_directories(audit_path.parent_path(), error);
            if (error) return common::Result<void>::failure({"AUDIT_DIRECTORY", "Could not initialize local audit storage"});
        }
        std::uintmax_t size = 0;
        if (std::filesystem::exists(audit_path, error)) {
            if (error) return common::Result<void>::failure({"AUDIT_STAT", "Could not inspect local audit storage"});
            size = std::filesystem::file_size(audit_path, error);
            if (error || size > MAX_AUDIT_BYTES) return common::Result<void>::failure(
                {"AUDIT_LIMIT", "Local audit storage exceeds its fixed size"});
        }
        if (size + line.size() > MAX_AUDIT_BYTES) {
            auto rotated = audit_path;
            rotated += ".1";
            std::filesystem::remove(rotated, error);
            if (error) return common::Result<void>::failure({"AUDIT_ROTATE_REMOVE", "Could not rotate local audit storage"});
            if (size != 0) {
                std::ifstream source(audit_path, std::ios::binary);
                std::ofstream backup(rotated, std::ios::binary | std::ios::trunc);
                if (!source || !backup)
                    return common::Result<void>::failure({"AUDIT_ROTATE_OPEN", "Could not rotate local audit storage"});
                std::array<char, 8192> buffer{};
                while (source) {
                    source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = source.gcount();
                    if (count > 0) backup.write(buffer.data(), count);
                }
                backup.flush();
                if (source.bad() || !backup)
                    return common::Result<void>::failure({"AUDIT_ROTATE_COPY", "Could not rotate local audit storage"});
                backup.close();
                source.close();
                std::ofstream truncate(audit_path, std::ios::binary | std::ios::trunc);
                truncate.flush();
                if (!truncate)
                    return common::Result<void>::failure({"AUDIT_ROTATE_TRUNCATE", "Could not rotate local audit storage"});
            }
        }
        {
            std::ofstream output(audit_path, std::ios::binary | std::ios::app);
            if (!output) return common::Result<void>::failure({"AUDIT_OPEN", "Could not write local audit storage"});
            output.write(line.data(), static_cast<std::streamsize>(line.size()));
            output.flush();
            if (!output) return common::Result<void>::failure({"AUDIT_WRITE", "Could not write local audit storage"});
        }
        std::filesystem::permissions(audit_path, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
        return common::Result<void>::success();
    }

    static std::string fingerprint(const Json& request) {
        return persistence::HistoryStore::sha256(request.dump());
    }

    bool rate_allowed(const std::string& name, const bool controlled) {
        std::size_t limit = controlled ? 10U : 120U;
        if (name == "run_benchmark") limit = 5;
        else if (name == "trigger_kill_switch") limit = 5;
        auto& window = rate_windows[name];
        const auto now = SteadyClock::now();
        while (!window.empty() && now - window.front() >= std::chrono::minutes(1)) window.pop_front();
        if (window.size() >= limit) return false;
        window.push_back(now);
        return true;
    }

    Json initialize(const Json& id, const Json& params) {
        if (!params.is_object() || !params.contains("protocolVersion") ||
            !params["protocolVersion"].is_string())
            return json_rpc_error(id, -32602, "initialize requires a supported protocolVersion.");
        const auto requested = params["protocolVersion"].get<std::string>();
        const auto supported = std::find(SUPPORTED_PROTOCOLS.begin(), SUPPORTED_PROTOCOLS.end(), requested);
        if (supported == SUPPORTED_PROTOCOLS.end())
            return json_rpc_error(id, -32602, "Unsupported MCP protocol version.");
        negotiated_protocol = requested;
        initialized = true;
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {
            {"protocolVersion", negotiated_protocol}, {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", "basalt-local"}, {"version", "1.0.0"}}},
            {"instructions", "Local BASALT simulation and inspection tools. No live trading, shell, arbitrary file or credential access."}}}};
    }

    Json invalid_tool_result(const Json& id, const std::string& code, const std::string& message) {
        return mcp_tool_response(id, error_content(code, message), true);
    }

    Json call_tool_response(const Json& id, const Json& request, const std::string& tool,
                            const Json& arguments) {
        const ToolDefinition* definition = find_tool(tool);
        if (definition == nullptr) {
            const auto audited = append_audit(tool, arguments, id, "REJECTED", "UNKNOWN_TOOL");
            return invalid_tool_result(id, audited ? "UNKNOWN_TOOL" : "AUDIT_UNAVAILABLE",
                audited ? "Tool is not available." : "Local audit storage is unavailable.");
        }
        if (const auto validation = validate_arguments(*definition, arguments); !validation.code.empty()) {
            const auto audited = append_audit(tool, arguments, id, "REJECTED", validation.code);
            return invalid_tool_result(id, audited ? validation.code : "AUDIT_UNAVAILABLE",
                audited ? validation.message : "Local audit storage is unavailable.");
        }
        const bool controlled = !definition->read_only;
        const std::string id_key = id.dump();
        const std::string request_fingerprint = fingerprint(request);
        if (controlled) {
            const auto cached = idempotency_cache.find(id_key);
            if (cached != idempotency_cache.end()) {
                if (cached->second.fingerprint == request_fingerprint) {
                    const auto audited = append_audit(tool, arguments, id, "IDEMPOTENT_REPLAY");
                    return audited ? cached->second.response : invalid_tool_result(
                        id, "AUDIT_UNAVAILABLE", "Local audit storage is unavailable.");
                }
                const auto audited = append_audit(tool, arguments, id, "REJECTED", "IDEMPOTENCY_CONFLICT");
                return invalid_tool_result(id, audited ? "IDEMPOTENCY_CONFLICT" : "AUDIT_UNAVAILABLE",
                    audited ? "Request ID was already used for a different controlled call." :
                        "Local audit storage is unavailable.");
            }
        }
        if (!rate_allowed(tool, controlled)) {
            const auto audited = append_audit(tool, arguments, id, "RATE_LIMITED", "RATE_LIMITED");
            return invalid_tool_result(id, audited ? "RATE_LIMITED" : "AUDIT_UNAVAILABLE",
                audited ? "Tool rate limit exceeded; retry after the one-minute window." :
                    "Local audit storage is unavailable.");
        }
        if (controlled) {
            if (!append_audit(tool, arguments, id, "STARTED"))
                return invalid_tool_result(id, "AUDIT_UNAVAILABLE", "Local audit storage is unavailable.");
        }
        ToolOutcome outcome;
        try {
            outcome = execute_tool(tool, arguments);
        } catch (const std::exception&) {
            outcome = {error_content("TOOL_EXECUTION_FAILED", "The bounded tool operation failed."), true,
                       "TOOL_EXECUTION_FAILED"};
        } catch (...) {
            outcome = {error_content("TOOL_EXECUTION_FAILED", "The bounded tool operation failed."), true,
                       "TOOL_EXECUTION_FAILED"};
        }
        const std::string_view audit_outcome = outcome.error ? "FAILED" : "SUCCEEDED";
        const auto audited = append_audit(tool, arguments, id, audit_outcome, outcome.error_code);
        Json response = audited ? mcp_tool_response(id, outcome.content, outcome.error) :
            invalid_tool_result(id, audited.error().code, "Local audit storage is unavailable.");
        if (controlled) {
            idempotency_cache.emplace(id_key, CachedRequest{request_fingerprint, response});
            cache_order.push_back(id_key);
            while (cache_order.size() > 256U) {
                idempotency_cache.erase(cache_order.front());
                cache_order.pop_front();
            }
        }
        return response;
    }

    Json process(const std::string& line) {
        if (line.size() > MAX_FRAME_BYTES) return json_rpc_error(nullptr, -32600, "Request frame exceeds the fixed size limit.");
        Json request;
        try { request = Json::parse(line); }
        catch (const std::exception&) { return json_rpc_error(nullptr, -32700, "Parse error."); }
        if (!request.is_object() || !request.contains("jsonrpc") || !request["jsonrpc"].is_string() ||
            request["jsonrpc"].get<std::string>() != "2.0" ||
            !request.contains("method") || !request["method"].is_string())
            return json_rpc_error(request.is_object() && request.contains("id") ? request["id"] : Json(nullptr),
                                  -32600, "Invalid JSON-RPC request.");
        const bool has_id = request.contains("id");
        const Json id = has_id ? request["id"] : Json(nullptr);
        if (has_id && !valid_request_id(id)) return json_rpc_error(nullptr, -32600, "Invalid JSON-RPC request ID.");
        const std::string method = request["method"].get<std::string>();
        const Json params = request.value("params", Json::object());
        if (method == "notifications/initialized" || method == "$/cancelRequest") return Json();
        if (method == "exit") { stopping = true; return Json(); }
        if (!has_id) return Json();
        if (!init_error.empty()) return json_rpc_error(id, -32603, "Local MCP storage initialization failed.");
        if (method == "initialize") return initialize(id, params);
        if (method == "ping") return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}};
        if (method == "shutdown") {
            stopping = true;
            return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}};
        }
        if (method == "tools/list") {
            if (!initialized) return json_rpc_error(id, -32002, "Initialize the MCP session first.");
            if (!params.is_object()) return json_rpc_error(id, -32602, "tools/list params must be an object.");
            return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", tools_list_json()}};
        }
        if (method == "tools/call") {
            if (!initialized) return json_rpc_error(id, -32002, "Initialize the MCP session first.");
            if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
                return json_rpc_error(id, -32602, "tools/call requires a tool name.");
            const std::string tool = params["name"].get<std::string>();
            const Json arguments = params.value("arguments", Json::object());
            ++total_tool_calls;
            return call_tool_response(id, request, tool, arguments);
        }
        return json_rpc_error(id, -32601, "Method not found.");
    }

    ToolOutcome execute_tool(const std::string& name, const Json& args);
    common::Result<std::vector<backtest::BacktestEvent>> make_bars(const Json& values) const;
    ToolOutcome run_backtest(const Json& args);
    ToolOutcome run_replay(const Json& args);
};

common::Result<std::vector<backtest::BacktestEvent>> McpServer::Impl::make_bars(const Json& values) const {
    if (!values.is_array() || values.size() < 3U || values.size() > MAX_INLINE_BARS)
        return common::Result<std::vector<backtest::BacktestEvent>>::failure(
            {"BARS_INVALID", "Inline bar count must be between 3 and 1000."});
    std::vector<backtest::BacktestEvent> events;
    events.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const Json& item = values[i];
        std::uint64_t open{}, high{}, low{}, close{}, volume{};
        if (!item.is_object() || !uint_value(item.value("open_micro", Json()), open) ||
            !uint_value(item.value("high_micro", Json()), high) ||
            !uint_value(item.value("low_micro", Json()), low) ||
            !uint_value(item.value("close_micro", Json()), close) ||
            !uint_value(item.value("volume", Json()), volume) || open == 0 || high == 0 || low == 0 ||
            close == 0 || volume == 0 || open > 1'000'000'000ULL || high > 1'000'000'000ULL ||
            low > 1'000'000'000ULL || close > 1'000'000'000ULL || volume > 1'000'000'000ULL ||
            high < std::max(open, close) || low > std::min(open, close))
            return common::Result<std::vector<backtest::BacktestEvent>>::failure(
                {"BAR_INVALID", "Inline OHLCV values are inconsistent or out of range."});
        const auto open_time = SIM_FIRST_OPEN_NS + static_cast<std::int64_t>(i) * SIM_BAR_INTERVAL_NS;
        const auto close_time = open_time + SIM_BAR_INTERVAL_NS - 1;
        backtest::BacktestEvent event{};
        event.sequence = static_cast<common::EventSequence>(i + 1U);
        event.open_time_ns = open_time;
        event.close_time_ns = close_time;
        event.session_key = "MCP-SIM-24x7-1";
        event.event.event_id = "mcp-inline-" + std::to_string(i + 1U);
        event.event.instrument_id = 1;
        event.event.provider = "mcp-inline";
        event.event.kind = market_data::MarketEventKind::Bar;
        event.event.event_time_ns = close_time;
        event.event.receive_time_utc_ns = close_time;
        event.event.source = common::DataSourceType::Historical;
        event.event.quality = common::FeedQualityState::Ok;
        event.event.provider_sequence = static_cast<std::uint64_t>(i + 1U);
        event.event.bar = market_data::OhlcvBar{static_cast<std::int64_t>(open),
            static_cast<std::int64_t>(high), static_cast<std::int64_t>(low),
            static_cast<std::int64_t>(close), volume};
        events.push_back(std::move(event));
    }
    return common::Result<std::vector<backtest::BacktestEvent>>::success(std::move(events));
}

McpServer::Impl::ToolOutcome McpServer::Impl::run_backtest(const Json& args) {
    auto parsed_events = make_bars(args["bars"]);
    if (!parsed_events) return {error_content(parsed_events.error().code, parsed_events.error().message),
                                true, parsed_events.error().code};
    std::vector<backtest::BacktestEvent> events = std::move(parsed_events).value();
    const std::string strategy_name = args["strategy"].get<std::string>();
    strategy::BuiltinKind kind = strategy::BuiltinKind::SmaCrossover;
    if (strategy_name == "RSI_MEAN_REVERSION") kind = strategy::BuiltinKind::RsiMeanReversion;
    else if (strategy_name == "BOLLINGER") kind = strategy::BuiltinKind::Bollinger;
    else if (strategy_name == "VWAP_REVERSION") kind = strategy::BuiltinKind::VwapReversion;
    else if (strategy_name == "MOMENTUM_BREAKOUT") kind = strategy::BuiltinKind::MomentumBreakout;
    else if (strategy_name == "ORDER_BOOK_IMBALANCE") kind = strategy::BuiltinKind::OrderBookImbalance;

    backtest::BacktestConfig config{};
    config.dataset_id = "mcp-inline-" + persistence::HistoryStore::sha256(args["bars"].dump()).substr(0, 16);
    config.dataset_sha256 = persistence::HistoryStore::sha256(args["bars"].dump());
    config.data_source = "caller-supplied inline OHLCV";
    config.license_tag = "MCP_INLINE_USER_SUPPLIED";
    config.corporate_action_policy = "unspecified";
    config.instrument_master_version = 1;
    config.instrument.id = 1;
    config.instrument.segment = "SIM";
    config.instrument.symbol = "MCP-SIM";
    config.instrument.display_symbol = "MCP-SIM";
    config.instrument.currency = "INR";
    config.instrument.tick_size_micro = 1;
    config.instrument.lot_size = 1;
    config.instrument.quantity_step = 1;
    config.account_id = 1;
    config.strategy_id = 1;
    config.initial_cash_micro = 1'000'000'000'000LL;
    config.timezone = "UTC";
    config.exchange_calendar = "MCP-SIM-24x7";
    config.seed = args.value("seed", std::uint64_t{42});
    config.execution.model = trading::PaperExecutionModel::BarApproximation;
    config.execution.model_version = 1;
    config.execution.tick_size_micro = 1;
    config.execution.slippage_ticks = 1;
    config.execution.participation_bps = 10'000;
    config.half_spread_micro = 1;
    config.risk_limits.max_order_notional_micro = 1'000'000'000'000LL;
    config.risk_limits.max_order_quantity = 100'000;
    config.risk_limits.max_position_quantity = 100'000;
    config.risk_limits.max_gross_exposure_micro = 1'000'000'000'000LL;
    config.risk_limits.max_net_exposure_micro = 1'000'000'000'000LL;
    config.risk_limits.max_instrument_position_quantity = 100'000;
    config.risk_limits.max_instrument_notional_micro = 1'000'000'000'000LL;
    config.risk_limits.max_strategy_exposure_micro = 1'000'000'000'000LL;
    config.splits.train = {events.front().close_time_ns, events[1].close_time_ns};
    config.splits.validation = {events[1].close_time_ns, events[2].close_time_ns};
    config.splits.test = {events[2].close_time_ns, events.back().close_time_ns + 1};
    config.selected_split = backtest::DataSplit::Test;
    config.bar_interval_ns = static_cast<std::uint64_t>(SIM_BAR_INTERVAL_NS);
    config.seed = args.value("seed", std::uint64_t{42});

    strategy::StrategyConfig strategy_config{};
    strategy_config.random_seed = config.seed;
    strategy_config.order_quantity = 1;
    strategy_config.sizing_budget_micro = 1'000'000'000LL;
    if (kind == strategy::BuiltinKind::SmaCrossover)
        strategy_config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};
    backtest::BacktestEngine backtest_engine;
    auto result = backtest_engine.run(strategy::make_builtin(kind), strategy_config, events, config);
    if (!result) return {error_content(result.error().code, "Backtest input or simulation configuration was rejected."),
                         true, result.error().code};
    const auto& value = result.value();
    Json metrics = Json::array();
    for (const auto& metric : value.analytics.metrics) metrics.push_back(metric_json(metric));
    return {Json{{"valid", value.valid}, {"strategy", strategy_name}, {"data_split", "TEST"},
        {"source", "caller_supplied_inline_bars"}, {"bars_supplied", events.size()},
        {"executions", value.execution_count}, {"completed_orders", value.completed_orders},
        {"cancelled_orders", value.cancelled_orders}, {"rejected_orders", value.rejected_orders},
        {"input_sha256", value.input_event_sha256}, {"manifest_sha256", value.manifest_sha256},
        {"output_sha256", value.output_sha256}, {"analytics", std::move(metrics)},
        {"warnings", value.warnings}}, false, {}};
}

McpServer::Impl::ToolOutcome McpServer::Impl::run_replay(const Json& args) {
    const std::string action = args["action"].get<std::string>();
    if (action == "start") {
        if (!args.contains("bars")) return {error_content("BARS_REQUIRED", "Starting a replay requires inline bars."),
                                             true, "BARS_REQUIRED"};
        auto events = make_bars(args["bars"]);
        if (!events) return {error_content(events.error().code, events.error().message), true, events.error().code};
        replay = std::make_unique<backtest::ReplayController>(std::move(events).value());
        const auto valid = replay->validate();
        if (!valid) { replay.reset(); return {error_content(valid.error().code, "Replay bars were rejected."), true, valid.error().code}; }
    } else if (!replay) {
        return {error_content("REPLAY_NOT_STARTED", "Start a replay with inline bars first."), true, "REPLAY_NOT_STARTED"};
    }
    if (args.contains("speed")) {
        const auto speed = args["speed"].get<std::string>();
        if (speed == "1x") replay->set_speed(backtest::ReplaySpeed::OneX);
        else if (speed == "5x") replay->set_speed(backtest::ReplaySpeed::FiveX);
        else if (speed == "10x") replay->set_speed(backtest::ReplaySpeed::TenX);
        else replay->set_speed(backtest::ReplaySpeed::Max);
    }
    std::size_t advanced = 0;
    if (action == "pause") replay->pause();
    else if (action == "step") {
        if (replay->playing()) return {error_content("REPLAY_MUST_BE_PAUSED", "Pause replay before stepping."),
                                       true, "REPLAY_MUST_BE_PAUSED"};
        const auto count = args.value("steps", std::uint64_t{1});
        for (; advanced < count; ++advanced) {
            const auto stepped = replay->step_one();
            if (!stepped) return {error_content(stepped.error().code, "Replay has no next event."),
                                  true, stepped.error().code};
        }
    } else if (action == "play") {
        const auto count = args.value("steps", std::uint64_t{100});
        replay->play();
        while (advanced < count && replay->next()) ++advanced;
        replay->pause(); // Each call advances at most 500 events and returns to a controllable pause.
    }
    return {Json{{"action", action}, {"playing", replay->playing()},
        {"speed", backtest::to_string(replay->speed())}, {"advanced", advanced},
        {"current_sequence", replay->current_sequence()}, {"virtual_time_ns", replay->virtual_time_ns()},
        {"cursor", replay->cursor()}}, false, {}};
}

McpServer::Impl::ToolOutcome McpServer::Impl::execute_tool(const std::string& name, const Json& args) {
    if (name == "get_system_health") {
        const auto stats = simulation.stats();
        return {Json{{"status", "online"}, {"mode", "simulation_only"},
            {"build", {{"revision", BASALT_GIT_REVISION}, {"dirty", BASALT_GIT_WORKTREE_DIRTY}}},
            {"services", {{"matching_engine", "available"}, {"mcp_stdio", "available"},
                {"market_data_provider", "unavailable"}, {"portfolio_ledger", "unavailable"},
                {"strategy_runtime", "unavailable"}, {"external_feed", "disconnected"}}},
            {"matching_engine", {{"active_orders", stats.active_order_count},
                {"trades", stats.total_trades_executed}, {"orders_submitted", stats.total_orders_submitted}}},
            {"mcp", {{"tool_calls", total_tool_calls}, {"protocol_version", negotiated_protocol},
                     {"audit_file", "logs/mcp_audit.jsonl"}}},
            {"kill_switch", {{"global_active", kill_switch.active(0, 0)},
                {"persisted_event_count", kill_switch.events().size()}}},
            {"live_execution_enabled", false}, {"credentials_read", false}}, false, {}};
    }
    if (name == "get_market_watch")
        return {Json{{"available", false}, {"items", Json::array()},
            {"reason", "No provider-backed market-watch read model is connected in this process."}}, false, {}};
    if (name == "get_instrument_snapshot")
        return {Json{{"available", false}, {"instrument_id", args["instrument_id"]},
            {"snapshot", nullptr}, {"reason", "No provider-backed instrument snapshot is connected."}}, false, {}};
    if (name == "get_order_book") {
        if (args["source"] == "external_observed")
            return {Json{{"available", false}, {"source", "external_observed"}, {"bids", Json::array()},
                {"asks", Json::array()}, {"reason", "External observed depth is not connected."}}, false, {}};
        if (args.contains("instrument_id") && args["instrument_id"] != 1U)
            return {Json{{"available", false}, {"source", "internal_simulated"},
                {"instrument_id", args["instrument_id"]}, {"reason", "The legacy demo book has no instrument mapping."}}, false, {}};
        const std::size_t depth = args.value("depth", std::uint64_t{20});
        return {Json{{"available", true}, {"source", "internal_simulated"},
            {"scope", "LEGACY_SINGLE_BOOK"}, {"instrument_id", nullptr},
            {"book", snapshot_json(simulation.book(depth), depth)}}, false, {}};
    }
    if (name == "get_recent_trades") {
        const auto trades = simulation.recent_trades(args.value("limit", std::uint64_t{50}));
        Json items = Json::array();
        for (const auto& trade : trades) items.push_back(trade_json(trade));
        return {Json{{"available", true}, {"source", "internal_simulated"},
            {"execution_model", "legacy matching engine"}, {"items", std::move(items)}}, false, {}};
    }
    if (name == "get_paper_positions")
        return {Json{{"available", false}, {"account_id", args["account_id"]},
            {"positions", Json::array()}, {"reason", "No paper ledger read model is connected in this process."}}, false, {}};
    if (name == "get_portfolio_summary")
        return {Json{{"available", false}, {"account_id", args["account_id"]},
            {"reason", "No paper ledger summary is connected in this process."}}, false, {}};
    if (name == "get_strategy_status")
        return {Json{{"available", false}, {"strategy_id", args["strategy_id"]},
            {"reason", "No strategy runtime is connected in this process."}}, false, {}};
    if (name == "inspect_feed_health")
        return {Json{{"available", false}, {"connected_providers", Json::array()},
            {"reason", "No provider feed is connected in this process."}}, false, {}};
    if (name == "run_backtest") return run_backtest(args);
    if (name == "run_replay") return run_replay(args);
    if (name == "run_test_suite") {
        const std::string suite = args["suite"].get<std::string>();
        Json checks = Json::array();
        bool passed = true;
        if (suite == "matching" || suite == "all") {
            SimulationFacade probe;
            const bool invariants = probe.verify_invariants();
            passed = passed && invariants;
            checks.push_back({{"name", "legacy_matching_invariants"}, {"passed", invariants}});
        }
        if (suite == "analytics" || suite == "all") {
            const auto report = backtest::calculate_metrics({});
            const bool metrics = report.metrics.size() == 18U;
            passed = passed && metrics;
            checks.push_back({{"name", "analytics_metric_catalog"}, {"passed", metrics},
                              {"metric_count", report.metrics.size()}});
        }
        return {Json{{"suite", suite}, {"passed", passed}, {"checks", std::move(checks)},
            {"execution", "in_process_allowlist"}}, !passed, passed ? "" : "SUITE_FAILED"};
    }
    if (name == "run_benchmark") {
        bench::WorkloadProfile profile{};
        if (!bench::parse_profile(args.value("profile", std::string("BALANCED")), profile))
            return {error_content("PROFILE_INVALID", "Workload profile is not available."), true, "PROFILE_INVALID"};
        const std::size_t order_count = args.value("orders", std::uint64_t{1000});
        const std::string implementation = args.value("implementation", std::string("all"));
        const bool event_bus_profile = profile == bench::WorkloadProfile::MarketDataBurst ||
            profile == bench::WorkloadProfile::SlowConsumerBackpressure;
        if (event_bus_profile && implementation != "all")
            return {error_content("IMPLEMENTATION_UNAVAILABLE", "This workload measures the bounded event bus only."),
                    true, "IMPLEMENTATION_UNAVAILABLE"};
        const auto results = benchmark_runner.run(profile, order_count, true);
        Json rows = Json::array();
        for (const auto& item : results) {
            const bool selected = implementation == "all" ||
                (implementation == "map_list" && item.implementation == "StandardOrderBook (map/list)") ||
                (implementation == "vector" && item.implementation == "BaselineOrderBook (vector)") ||
                (implementation == "arena" && item.implementation == "ArenaOrderBook (pooled nodes)");
            if (!selected) continue;
            rows.push_back({{"profile", item.profile_name}, {"implementation", item.implementation},
                {"orders_processed", item.orders_processed}, {"trades_executed", item.trades_executed},
                {"cancellations_executed", item.cancellations_executed},
                {"events_accepted", item.events_accepted}, {"events_dropped", item.events_dropped},
                {"queue_high_water", item.queue_high_water}, {"pool_exhaustions", item.pool_exhaustions},
                {"pool_allocations", item.pool_allocations}, {"pool_capacity", item.pool_capacity},
                {"total_time_ms", item.total_time_ms}, {"throughput_ops_per_sec", item.throughput_ops_per_sec},
                {"avg_latency_ns", item.avg_latency_ns}, {"p50_latency_ns", item.p50_latency_ns},
                {"p90_latency_ns", item.p90_latency_ns}, {"p95_latency_ns", item.p95_latency_ns},
                {"p99_latency_ns", item.p99_latency_ns}, {"p999_latency_ns", item.p999_latency_ns}});
        }
        return {Json{{"profile", bench::profile_id(profile)}, {"requested_implementation", implementation},
            {"orders", order_count}, {"results", std::move(rows)}}, false, {}};
    }
    if (name == "trigger_kill_switch") {
        const std::string scope_text = args["scope"].get<std::string>();
        const std::uint64_t target = args.value("target_id", std::uint64_t{0});
        const std::string reason = args["reason"].get<std::string>();
        if ((scope_text == "global" && target != 0) || (scope_text != "global" && target == 0))
            return {error_content("KILL_TARGET_INVALID", "Global scope takes no target; account and strategy scopes require a target ID."),
                    true, "KILL_TARGET_INVALID"};
        if (reason.find_first_not_of(" \t\r\n") == std::string::npos)
            return {error_content("KILL_REASON_REQUIRED", "Kill-switch reason must contain a non-space character."),
                    true, "KILL_REASON_REQUIRED"};
        const auto time_ns = clock();
        if (time_ns <= 0) return {error_content("CLOCK_UNAVAILABLE", "System time is unavailable."), true, "CLOCK_UNAVAILABLE"};
        const auto scope = scope_text == "global" ? trading::KillScope::Global :
            (scope_text == "account" ? trading::KillScope::Account : trading::KillScope::Strategy);
        const auto event = kill_switch.activate(scope, target, "local-stdio", reason, time_ns);
        if (!event.persisted) {
            constexpr std::string_view message =
                "Kill switch remains latched active in-process, but durable persistence failed.";
            return {Json{{"error", {{"code", "KILL_SWITCH_NOT_PERSISTED"}, {"message", message}}},
                         {"code", "KILL_SWITCH_NOT_PERSISTED"},
                         {"message", message},
                         {"active", event.active},
                         {"scope", scope_text},
                         {"target_id", event.target_id},
                         {"persisted", false},
                         {"working_orders_cancelled", event.working_orders_cancelled},
                         {"reason", "[REDACTED]"}},
                    true, "KILL_SWITCH_NOT_PERSISTED"};
        }
        return {Json{{"active", event.active}, {"scope", scope_text}, {"target_id", event.target_id},
            {"persisted", event.persisted}, {"working_orders_cancelled", event.working_orders_cancelled},
            {"reason", "[REDACTED]"}}, false, {}};
    }
    return {error_content("UNKNOWN_TOOL", "Tool is not available."), true, "UNKNOWN_TOOL"};
}

McpServer::McpServer(std::filesystem::path audit_path, std::filesystem::path kill_log_path, Clock clock)
    : impl_(std::make_unique<Impl>(std::move(audit_path), std::move(kill_log_path), std::move(clock))) {}

McpServer::~McpServer() = default;

std::string McpServer::process_line(const std::string& line) {
    try {
        const Json response = impl_->process(line);
        return response.is_null() ? std::string{} : response.dump();
    } catch (const std::exception&) {
        return json_rpc_error(nullptr, -32603, "Internal protocol error.").dump();
    } catch (...) {
        return json_rpc_error(nullptr, -32603, "Internal protocol error.").dump();
    }
}

bool McpServer::run_stdio(std::istream& input, std::ostream& output) {
    std::string line;
    line.reserve(4096);
    bool oversized = false;
    char character{};
    while (input.get(character)) {
        if (character != '\n') {
            if (!oversized) {
                if (line.size() == MAX_FRAME_BYTES) oversized = true;
                else line.push_back(character);
            }
            continue;
        }
        if (oversized) {
            output << json_rpc_error(nullptr, -32600, "Request frame exceeds the fixed size limit.").dump() << '\n';
            output.flush();
        } else {
            const auto response = process_line(line);
            if (!response.empty()) { output << response << '\n'; output.flush(); }
        }
        if (!output) return false;
        line.clear();
        oversized = false;
        if (impl_->stopping) return true;
    }
    return input.eof() && static_cast<bool>(output);
}

} // namespace basalt::server

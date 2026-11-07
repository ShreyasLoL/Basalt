#include "basalt/server/mcp_server.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

namespace {

using Json = nlohmann::json;
using basalt::server::McpServer;

Json bars() {
    return Json::array({
        Json{{"open_micro", 10}, {"high_micro", 10}, {"low_micro", 10}, {"close_micro", 10}, {"volume", 100}},
        Json{{"open_micro", 10}, {"high_micro", 10}, {"low_micro", 10}, {"close_micro", 10}, {"volume", 100}},
        Json{{"open_micro", 20}, {"high_micro", 20}, {"low_micro", 20}, {"close_micro", 20}, {"volume", 100}},
        Json{{"open_micro", 21}, {"high_micro", 23}, {"low_micro", 20}, {"close_micro", 22}, {"volume", 100}}
    });
}

class McpTest : public testing::Test {
protected:
    std::filesystem::path directory;
    std::filesystem::path audit_path;
    std::filesystem::path kill_path;
    std::unique_ptr<McpServer> server;
    std::int64_t now_ns{1'800'000'000'000'000'000LL};

    void SetUp() override {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() / ("basalt-mcp-tests-" + std::to_string(unique));
        std::filesystem::create_directories(directory);
        audit_path = directory / "mcp_audit.jsonl";
        kill_path = directory / "mcp_killswitch.blog";
        server = std::make_unique<McpServer>(audit_path, kill_path, [this] { return now_ns++; });
    }

    void TearDown() override {
        server.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    Json send(const Json& request) {
        const auto response = server->process_line(request.dump());
        if (response.empty()) return Json();
        return Json::parse(response);
    }

    void initialize() {
        const auto response = send({{"jsonrpc", "2.0"}, {"id", "init"}, {"method", "initialize"},
            {"params", {{"protocolVersion", "2025-11-25"}, {"capabilities", Json::object()},
                         {"clientInfo", {{"name", "mcp-tests"}, {"version", "1"}}}}}});
        ASSERT_TRUE(response.contains("result"));
        ASSERT_EQ(response["result"]["protocolVersion"], "2025-11-25");
        EXPECT_EQ(send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}), Json());
    }

    Json call(const std::string& id, const std::string& tool, Json args = Json::object()) {
        return send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
            {"params", {{"name", tool}, {"arguments", std::move(args)}}}});
    }

    static bool is_tool_error(const Json& response) {
        return response.contains("result") && response["result"].value("isError", false);
    }
};

TEST_F(McpTest, NegotiatesLifecycleAndDiscoversFourteenClosedTools) {
    auto before_initialize = send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
    ASSERT_TRUE(before_initialize.contains("error"));
    EXPECT_EQ(before_initialize["error"]["code"], -32002);

    initialize();
    const auto response = send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    ASSERT_TRUE(response.contains("result"));
    const auto& tools = response["result"]["tools"];
    ASSERT_EQ(tools.size(), 14U);
    EXPECT_EQ(tools.front()["name"], "get_system_health");
    EXPECT_EQ(tools.back()["name"], "trigger_kill_switch");
    for (const auto& tool : tools) {
        EXPECT_EQ(tool["inputSchema"]["additionalProperties"], false);
        EXPECT_TRUE(tool["annotations"].contains("readOnlyHint"));
        EXPECT_FALSE(tool["annotations"]["openWorldHint"].get<bool>());
    }
    auto unsupported = send({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "initialize"},
        {"params", {{"protocolVersion", "2099-01-01"}}}});
    EXPECT_TRUE(unsupported.contains("error"));
    EXPECT_EQ(unsupported["error"]["code"], -32602);
}

TEST_F(McpTest, ReadToolsReturnRealSimulationStateAndTruthfulUnavailableServices) {
    initialize();
    auto health = call("health", "get_system_health");
    ASSERT_FALSE(is_tool_error(health));
    EXPECT_EQ(health["result"]["structuredContent"]["mode"], "simulation_only");
    EXPECT_FALSE(health["result"]["structuredContent"]["live_execution_enabled"].get<bool>());

    auto book = call("book", "get_order_book", {{"source", "internal_simulated"}, {"depth", 10}});
    ASSERT_FALSE(is_tool_error(book));
    EXPECT_EQ(book["result"]["structuredContent"]["available"], true);
    EXPECT_FALSE(book["result"]["structuredContent"]["book"]["bids"].empty());

    auto watch = call("watch", "get_market_watch", {{"limit", 5}});
    ASSERT_FALSE(is_tool_error(watch));
    EXPECT_EQ(watch["result"]["structuredContent"]["available"], false);
    EXPECT_TRUE(watch["result"]["structuredContent"]["items"].empty());
}

TEST_F(McpTest, ClosedSchemasAndUnknownToolsFailWithoutEchoingArguments) {
    initialize();
    const std::string secret = "token-value-must-not-leak";
    auto invalid = call("invalid", "get_system_health", {{"unexpected", secret}});
    ASSERT_TRUE(is_tool_error(invalid));
    EXPECT_EQ(invalid["result"]["structuredContent"]["error"]["code"], "UNKNOWN_ARGUMENT");
    EXPECT_EQ(invalid.dump().find(secret), std::string::npos);

    for (const auto& name : {"execute_shell", "read_file", "read_environment", "place_live_order"}) {
        auto unknown = call(std::string("unknown-") + name, name, {{"value", secret}});
        ASSERT_TRUE(is_tool_error(unknown));
        EXPECT_EQ(unknown.dump().find(secret), std::string::npos);
    }
    std::ifstream audit(audit_path, std::ios::binary);
    const std::string records((std::istreambuf_iterator<char>(audit)), std::istreambuf_iterator<char>());
    EXPECT_EQ(records.find(secret), std::string::npos);
    EXPECT_NE(records.find("UNKNOWN_ARGUMENT"), std::string::npos);
    EXPECT_NE(records.find("UNRECOGNIZED"), std::string::npos);
}

TEST_F(McpTest, InlineBacktestUsesBoundedEventsAndReturnsEighteenMetrics) {
    initialize();
    auto response = call("backtest-1", "run_backtest", {{"strategy", "SMA_CROSSOVER"}, {"bars", bars()}});
    ASSERT_FALSE(is_tool_error(response)) << response.dump();
    const auto& result = response["result"]["structuredContent"];
    EXPECT_TRUE(result["valid"].get<bool>());
    EXPECT_EQ(result["bars_supplied"], 4U);
    EXPECT_EQ(result["data_split"], "TEST");
    EXPECT_EQ(result["analytics"].size(), 18U);
    EXPECT_EQ(result["input_sha256"].get<std::string>().size(), 64U);

    auto invalid_bars = bars();
    invalid_bars[0]["high_micro"] = 1;
    auto rejected = call("backtest-2", "run_backtest", {{"strategy", "SMA_CROSSOVER"}, {"bars", invalid_bars}});
    EXPECT_TRUE(is_tool_error(rejected));
}

TEST_F(McpTest, ReplayIsBoundedAndControlledCallsAreIdempotentByRequestId) {
    initialize();
    const Json request{{"jsonrpc", "2.0"}, {"id", "same-id"}, {"method", "tools/call"},
        {"params", {{"name", "run_replay"}, {"arguments", {{"action", "start"}, {"bars", bars()}}}}}};
    const auto first = send(request);
    ASSERT_FALSE(is_tool_error(first)) << first.dump();
    EXPECT_EQ(first["result"]["structuredContent"]["cursor"], 0U);
    EXPECT_EQ(send(request), first);

    auto conflict = request;
    conflict["params"]["arguments"] = {{"action", "step"}, {"steps", 1}};
    auto rejected = send(conflict);
    ASSERT_TRUE(is_tool_error(rejected));
    EXPECT_EQ(rejected["result"]["structuredContent"]["error"]["code"], "IDEMPOTENCY_CONFLICT");

    auto step = call("step-1", "run_replay", {{"action", "step"}, {"steps", 2}});
    ASSERT_FALSE(is_tool_error(step)) << step.dump();
    EXPECT_EQ(step["result"]["structuredContent"]["advanced"], 2U);
    EXPECT_EQ(step["result"]["structuredContent"]["current_sequence"], 2U);
}

TEST_F(McpTest, NamedSuitesAndAllowlistedBenchmarkRunInProcess) {
    initialize();
    auto suite = call("suite", "run_test_suite", {{"suite", "all"}});
    ASSERT_FALSE(is_tool_error(suite)) << suite.dump();
    EXPECT_TRUE(suite["result"]["structuredContent"]["passed"].get<bool>());
    EXPECT_EQ(suite["result"]["structuredContent"]["checks"].size(), 2U);

    auto benchmark = call("bench", "run_benchmark", {{"profile", "BALANCED"}, {"orders", 1},
                                                        {"implementation", "map_list"}});
    ASSERT_FALSE(is_tool_error(benchmark)) << benchmark.dump();
    const auto& results = benchmark["result"]["structuredContent"]["results"];
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0]["orders_processed"], 1U);
    EXPECT_EQ(results[0]["implementation"], "StandardOrderBook (map/list)");

    auto excessive = call("bench-too-large", "run_benchmark", {{"profile", "BALANCED"}, {"orders", 10001}});
    EXPECT_TRUE(is_tool_error(excessive));
}

TEST_F(McpTest, KillSwitchPersistsOnlyRedactedReasonAndRateLimitsAfterFiveCalls) {
    initialize();
    const std::string secret = "private-incident-credential-123";
    for (std::uint64_t i = 1; i <= 5; ++i) {
        auto response = call("kill-" + std::to_string(i), "trigger_kill_switch",
            {{"scope", "account"}, {"target_id", i}, {"reason", secret}});
        ASSERT_FALSE(is_tool_error(response)) << response.dump();
        EXPECT_EQ(response["result"]["structuredContent"]["reason"], "[REDACTED]");
        EXPECT_EQ(response.dump().find(secret), std::string::npos);
    }
    auto limited = call("kill-6", "trigger_kill_switch",
        {{"scope", "account"}, {"target_id", 6}, {"reason", secret}});
    ASSERT_TRUE(is_tool_error(limited));
    EXPECT_EQ(limited["result"]["structuredContent"]["error"]["code"], "RATE_LIMITED");

    std::ifstream audit(audit_path, std::ios::binary);
    const std::string audit_text((std::istreambuf_iterator<char>(audit)), std::istreambuf_iterator<char>());
    EXPECT_EQ(audit_text.find(secret), std::string::npos);
    std::ifstream log(kill_path, std::ios::binary);
    const std::string kill_text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    EXPECT_EQ(kill_text.find(secret), std::string::npos);
    EXPECT_NE(kill_text.find("[REDACTED]"), std::string::npos);
}

TEST_F(McpTest, GlobalKillSwitchLatchSurvivesProcessRestart) {
    initialize();
    auto activated = call("global-kill", "trigger_kill_switch",
        {{"scope", "global"}, {"reason", "operator emergency stop"}});
    ASSERT_FALSE(is_tool_error(activated)) << activated.dump();
    EXPECT_TRUE(activated["result"]["structuredContent"]["active"].get<bool>());

    server.reset();
    server = std::make_unique<McpServer>(audit_path, kill_path, [this] { return now_ns++; });
    initialize();
    auto health = call("after-restart", "get_system_health");
    ASSERT_FALSE(is_tool_error(health));
    EXPECT_TRUE(health["result"]["structuredContent"]["kill_switch"]["global_active"].get<bool>());
    EXPECT_EQ(health["result"]["structuredContent"]["kill_switch"]["persisted_event_count"], 1U);
}

TEST_F(McpTest, AuditFileRotatesWithinTheConfiguredStorageBound) {
    initialize();
    constexpr std::size_t max_audit = 1024U * 1024U;
    {
        std::ofstream seed(audit_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(seed.good());
        const std::string record(max_audit - 64U, 'x');
        seed.write(record.data(), static_cast<std::streamsize>(record.size()));
        ASSERT_TRUE(seed.good());
    }
    auto response = call("rotation", "get_system_health");
    ASSERT_FALSE(is_tool_error(response)) << response.dump();
    auto backup = audit_path;
    backup += ".1";
    ASSERT_TRUE(std::filesystem::exists(backup));
    EXPECT_EQ(std::filesystem::file_size(backup), max_audit - 64U);
    EXPECT_LT(std::filesystem::file_size(audit_path), max_audit);
    EXPECT_LE(std::filesystem::file_size(backup) + std::filesystem::file_size(audit_path), 2U * max_audit);
}

TEST_F(McpTest, StdioWritesOnlyJsonRpcFramesAndHonorsShutdown) {
    std::stringstream input;
    input << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})" << '\n';
    input << R"({"jsonrpc":"2.0","method":"notifications/initialized"})" << '\n';
    input << R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" << '\n';
    input << R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})" << '\n';
    std::stringstream output;
    EXPECT_TRUE(server->run_stdio(input, output));
    std::string line;
    std::size_t frames = 0;
    while (std::getline(output, line)) {
        const Json frame = Json::parse(line);
        EXPECT_EQ(frame["jsonrpc"], "2.0");
        EXPECT_TRUE(frame.contains("id"));
        ++frames;
    }
    EXPECT_EQ(frames, 3U);
    EXPECT_EQ(output.str().find("token"), std::string::npos);
}

TEST_F(McpTest, OversizeFramesAreRejectedWithBoundedProtocolError) {
    const auto response = Json::parse(server->process_line(std::string(256U * 1024U + 1U, 'x')));
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], -32600);
    EXPECT_LT(response.dump().size(), 256U);
}

TEST_F(McpTest, TriggerKillSwitchReportsErrorWhenNotPersistedWhileKeepingInProcessLatch) {
    // Make kill_path read-only so durable persistence fails on activation.
    std::error_code ec;
    std::filesystem::permissions(kill_path, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, ec);
    initialize();
    auto readonly_resp = call("kill-readonly", "trigger_kill_switch",
                              {{"scope", "global"}, {"reason", "disk read-only"}});
    std::filesystem::permissions(kill_path,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    ASSERT_TRUE(is_tool_error(readonly_resp)) << readonly_resp.dump();
    const auto& content = readonly_resp["result"]["structuredContent"];
    EXPECT_EQ(content["error"]["code"], "KILL_SWITCH_NOT_PERSISTED");
    EXPECT_EQ(content["code"], "KILL_SWITCH_NOT_PERSISTED");
    EXPECT_FALSE(content["persisted"].get<bool>());
    EXPECT_TRUE(content["active"].get<bool>());
    EXPECT_NE(content["error"]["message"].get<std::string>().find("in-process"), std::string::npos);

    auto health = call("health-after-unpersisted-kill", "get_system_health");
    ASSERT_FALSE(is_tool_error(health));
    EXPECT_TRUE(health["result"]["structuredContent"]["kill_switch"]["global_active"].get<bool>());

    // Also verify with an invalid kill-log path (a directory instead of a file).
    const auto invalid_kill_dir = directory / "invalid_kill_log_dir";
    std::filesystem::create_directories(invalid_kill_dir);
    server = std::make_unique<McpServer>(audit_path, invalid_kill_dir, [this] { return now_ns++; });
    initialize();
    auto invalid_resp = call("kill-invalid-path", "trigger_kill_switch",
                             {{"scope", "global"}, {"reason", "invalid path"}});
    ASSERT_TRUE(is_tool_error(invalid_resp)) << invalid_resp.dump();
    EXPECT_EQ(invalid_resp["result"]["structuredContent"]["error"]["code"], "KILL_SWITCH_NOT_PERSISTED");
    EXPECT_TRUE(invalid_resp["result"]["structuredContent"]["active"].get<bool>());
}

TEST_F(McpTest, ResolveDataDirUsesEnvThenExecutableDirThenTempWithWarning) {
    const auto env_dir = directory / "env_data_dir";
    const auto exe_dir = directory / "exe_data_dir";
    std::ostringstream warnings;
    EXPECT_EQ(McpServer::resolve_data_dir(env_dir, exe_dir, &warnings), env_dir);
    EXPECT_TRUE(std::filesystem::is_directory(env_dir));
    EXPECT_TRUE(warnings.str().empty());

    EXPECT_EQ(McpServer::resolve_data_dir(std::filesystem::path{}, exe_dir, &warnings), exe_dir);
    EXPECT_TRUE(std::filesystem::is_directory(exe_dir));
    EXPECT_TRUE(warnings.str().empty());

    const auto blocker = directory / "file_blocker";
    {
        std::ofstream out(blocker, std::ios::binary);
        out << "not a directory";
    }
    const auto fallback = McpServer::resolve_data_dir(blocker / "sub", blocker / "sub2", &warnings);
    EXPECT_FALSE(fallback.empty());
    EXPECT_TRUE(std::filesystem::is_directory(fallback));
    EXPECT_NE(warnings.str().find("Warning"), std::string::npos);
}

} // namespace

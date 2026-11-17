#include "basalt/platform/platform_runner.hpp"
#include "basalt/server/http_server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

TEST(PlatformRunnerIntegrationTest, DeterministicSyntheticPathReachesPortfolioApiAndHonorsRiskAndKillSwitch) {
    basalt::platform::PlatformRunner first;
    basalt::platform::PlatformRunner second;

    const auto first_run = first.run(8);
    const auto second_run = second.run(8);
    ASSERT_TRUE(first_run) << first_run.error().message;
    ASSERT_TRUE(second_run) << second_run.error().message;

    EXPECT_EQ(first_run.value().ticks_processed, 8U);
    EXPECT_EQ(first_run.value().signals, 6U);
    EXPECT_EQ(first_run.value().orders, 4U);
    EXPECT_EQ(first_run.value().fills, 4U);
    ASSERT_EQ(first_run.value().risk_rejections, 2U);
    EXPECT_EQ(first_run.value().risk_rejection_codes,
              (std::vector<std::string>{"ORDER_QUANTITY_LIMIT", "ORDER_QUANTITY_LIMIT"}));
    EXPECT_EQ(first_run.value().ledger_hash, second_run.value().ledger_hash);
    EXPECT_EQ(first_run.value().total_pnl_micro, second_run.value().total_pnl_micro);
    ASSERT_TRUE(first.verify_reconciliation());
    ASSERT_TRUE(second.verify_reconciliation());

    basalt::platform::PlatformRunnerConfig alternate_config;
    alternate_config.random_seed = 43;
    basalt::platform::PlatformRunner alternate(alternate_config);
    const auto alternate_run = alternate.run(8);
    ASSERT_TRUE(alternate_run);
    EXPECT_NE(first_run.value().ledger_hash, alternate_run.value().ledger_hash);

    const auto kill = first.activate_kill_switch("integration-test", "stop after first session");
    EXPECT_TRUE(kill.active);
    const auto halted_run = first.run(4);
    ASSERT_TRUE(halted_run);
    EXPECT_TRUE(halted_run.value().execution_halted);
    EXPECT_EQ(halted_run.value().ticks_processed, first_run.value().ticks_processed);
    EXPECT_EQ(halted_run.value().orders, first_run.value().orders);
    EXPECT_EQ(halted_run.value().fills, first_run.value().fills);
    EXPECT_EQ(halted_run.value().ledger_hash, first_run.value().ledger_hash);
    ASSERT_TRUE(first.verify_reconciliation());

    basalt::server::HttpServer server(basalt::common::AppConfig{}, &first);
    ASSERT_TRUE(server.bind_to_any_port());
    std::thread worker([&server] { static_cast<void>(server.run_bound()); });
    struct ServerCleanup {
        basalt::server::HttpServer& server;
        std::thread& worker;
        ~ServerCleanup() {
            server.stop();
            if (worker.joinable()) worker.join();
        }
    } cleanup{server, worker};
    server.wait_until_ready();

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Get("/api/v1/portfolio/1");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200);
    const Json body = Json::parse(response->body);
    EXPECT_EQ(body.at("schema_version"), 1);
    EXPECT_EQ(body.at("account_id"), 1);
    EXPECT_EQ(body.at("cash_balance_micro"), halted_run.value().portfolio.cash_balance_micro);
    EXPECT_EQ(body.at("fees_micro"), halted_run.value().portfolio.total_fees_micro);
    EXPECT_EQ(body.at("realized_pnl_micro"), halted_run.value().portfolio.realized_pnl_micro);
    EXPECT_EQ(body.at("unrealized_pnl_micro"), halted_run.value().portfolio.unrealized_pnl_micro);
    EXPECT_TRUE(body.at("positions").is_array());
    EXPECT_TRUE(body.at("data_age_ns").is_null());

    const auto missing = client.Get("/api/v1/portfolio/999");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_EQ(Json::parse(missing->body).at("code"), "ACCOUNT_NOT_FOUND");
}

} // namespace

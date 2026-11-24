#include "basalt/server/http_server.hpp"
#include "basalt/server/request_guard.hpp"
#include "basalt/server/simulation_facade.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using basalt::server::validate_request_origin;
using Json = nlohmann::json;

constexpr int kPort = 8080;
constexpr std::optional<std::string_view> kNone = std::nullopt;

// ---------------------------------------------------------------------------
// Pure policy tests
// ---------------------------------------------------------------------------

TEST(RequestGuardTest, AcceptsOnlyLoopbackHostAuthoritiesForTheServerPort) {
    using basalt::server::is_allowed_host;
    EXPECT_TRUE(is_allowed_host("127.0.0.1:8080", kPort));
    EXPECT_TRUE(is_allowed_host("localhost:8080", kPort));
    EXPECT_TRUE(is_allowed_host("LOCALHOST:8080", kPort));
    EXPECT_TRUE(is_allowed_host("[::1]:8080", kPort));
    EXPECT_FALSE(is_allowed_host("127.0.0.1:8081", kPort));
    EXPECT_FALSE(is_allowed_host("localhost", kPort));
    EXPECT_FALSE(is_allowed_host("attacker.example:8080", kPort)); // DNS rebinding
    EXPECT_FALSE(is_allowed_host("127.0.0.1.attacker.example:8080", kPort));
    EXPECT_FALSE(is_allowed_host("", kPort));
    EXPECT_TRUE(is_allowed_host("localhost", 80));
    EXPECT_TRUE(is_allowed_host("localhost:80", 80));
}

TEST(SimulationFacadeTest, SeedAndBatchUseMatchingEnginesSingleAutomaticIdSequence) {
    basalt::server::SimulationFacade facade(true);
    const auto first = facade.submit_order({0, basalt::Side::Buy, basalt::OrderType::Limit, 9000, 1});
    ASSERT_EQ(first.status, basalt::OrderStatus::New);
    EXPECT_EQ(first.order_id, 11U);
    const auto batch = facade.submit_batch({
        {0, basalt::Side::Buy, basalt::OrderType::Limit, 8999, 1},
        {0, basalt::Side::Buy, basalt::OrderType::Limit, 8998, 1}});
    ASSERT_EQ(batch.size(), 2U);
    EXPECT_EQ(batch[0].order_id, 12U);
    EXPECT_EQ(batch[1].order_id, 13U);
    EXPECT_TRUE(facade.verify_invariants());
}

TEST(RequestGuardTest, ExplicitRemoteBindAddressIsAddedButWildcardIsNot) {
    using basalt::server::is_allowed_host;
    EXPECT_TRUE(is_allowed_host("192.168.1.5:8080", kPort, "192.168.1.5"));
    EXPECT_FALSE(is_allowed_host("192.168.1.6:8080", kPort, "192.168.1.5"));
    EXPECT_FALSE(is_allowed_host("0.0.0.0:8080", kPort, "0.0.0.0"));
    EXPECT_TRUE(is_allowed_host("[fd00::1]:8080", kPort, "fd00::1"));
}

TEST(RequestGuardTest, OriginMustBeAnAllowlistedHttpLoopbackOrigin) {
    using basalt::server::is_allowed_origin;
    EXPECT_TRUE(is_allowed_origin("http://127.0.0.1:8080", kPort));
    EXPECT_TRUE(is_allowed_origin("http://localhost:8080", kPort));
    EXPECT_FALSE(is_allowed_origin("http://localhost:5173", kPort));
    EXPECT_FALSE(is_allowed_origin("https://localhost:8080", kPort));
    EXPECT_FALSE(is_allowed_origin("http://evil.example", kPort));
    EXPECT_FALSE(is_allowed_origin("http://localhost:8080/path", kPort));
    EXPECT_FALSE(is_allowed_origin("null", kPort));
    EXPECT_FALSE(is_allowed_origin("", kPort));
    EXPECT_FALSE(is_allowed_origin("*", kPort));
}

TEST(RequestGuardTest, ExplicitOriginAllowlistIsExactAndDoesNotChangeHostPolicy) {
    using basalt::server::is_allowed_origin;
    const std::vector<std::string> allowlist{"http://localhost:5173", "https://research.example.com"};
    EXPECT_TRUE(is_allowed_origin("http://localhost:5173", kPort, "127.0.0.1", allowlist));
    EXPECT_TRUE(is_allowed_origin("https://research.example.com", kPort, "127.0.0.1", allowlist));
    EXPECT_FALSE(is_allowed_origin("https://research.example.com/", kPort, "127.0.0.1", allowlist));
    EXPECT_FALSE(is_allowed_origin("http://localhost:5174", kPort, "127.0.0.1", allowlist));
}

TEST(RequestGuardTest, JsonMediaTypeIgnoresParametersAndCase) {
    using basalt::server::is_json_content_type;
    EXPECT_TRUE(is_json_content_type("application/json"));
    EXPECT_TRUE(is_json_content_type("Application/JSON; charset=utf-8"));
    EXPECT_TRUE(is_json_content_type(" application/json ;charset=utf-8"));
    EXPECT_FALSE(is_json_content_type("text/plain"));
    EXPECT_FALSE(is_json_content_type("application/x-www-form-urlencoded"));
    EXPECT_FALSE(is_json_content_type("multipart/form-data; boundary=x"));
    EXPECT_FALSE(is_json_content_type("application/jsonp"));
    EXPECT_FALSE(is_json_content_type(""));
}

TEST(RequestGuardTest, ValidateRequestOriginAppliesHostOriginThenMediaType) {
    const auto ok = validate_request_origin("127.0.0.1:8080", kNone, kNone, "GET", kPort);
    EXPECT_TRUE(ok.allowed());

    const auto missing_host = validate_request_origin(kNone, kNone, kNone, "GET", kPort);
    EXPECT_EQ(missing_host.status, 403);
    EXPECT_EQ(missing_host.code, "FORBIDDEN_HOST");

    // Host is checked first even if the media type is also wrong.
    const auto rebinding = validate_request_origin("evil.example:8080", kNone, "text/plain", "POST", kPort);
    EXPECT_EQ(rebinding.status, 403);
    EXPECT_EQ(rebinding.code, "FORBIDDEN_HOST");

    const auto cross_origin = validate_request_origin("127.0.0.1:8080", "http://evil.example",
                                                      "application/json", "POST", kPort);
    EXPECT_EQ(cross_origin.status, 403);
    EXPECT_EQ(cross_origin.code, "FORBIDDEN_ORIGIN");

    const auto empty_origin = validate_request_origin("127.0.0.1:8080", "", kNone, "GET", kPort);
    EXPECT_EQ(empty_origin.code, "FORBIDDEN_ORIGIN");

    for (const char* method : {"POST", "DELETE", "PUT", "PATCH"}) {
        const auto no_type = validate_request_origin("localhost:8080", kNone, kNone, method, kPort);
        EXPECT_EQ(no_type.status, 415) << method;
        EXPECT_EQ(no_type.code, "UNSUPPORTED_MEDIA_TYPE") << method;
        const auto form = validate_request_origin("localhost:8080", "http://localhost:8080",
                                                  "text/plain", method, kPort);
        EXPECT_EQ(form.status, 415) << method;
        const auto json = validate_request_origin("localhost:8080", "http://localhost:8080",
                                                  "application/json; charset=utf-8", method, kPort);
        EXPECT_TRUE(json.allowed()) << method;
    }

    // GET and OPTIONS do not need a media type.
    EXPECT_TRUE(validate_request_origin("localhost:8080", "http://localhost:8080", kNone, "OPTIONS", kPort).allowed());
    EXPECT_EQ(validate_request_origin("localhost:8080", "http://evil.example", kNone, "OPTIONS", kPort).status, 403);
}

TEST(RequestGuardTest, BenchmarkOrderCountIsCappedAtTenThousand) {
    using basalt::server::valid_benchmark_order_count;
    EXPECT_FALSE(valid_benchmark_order_count(0));
    EXPECT_TRUE(valid_benchmark_order_count(1));
    EXPECT_TRUE(valid_benchmark_order_count(10'000));
    EXPECT_FALSE(valid_benchmark_order_count(10'001));
    EXPECT_FALSE(valid_benchmark_order_count(1'000'000));
    EXPECT_EQ(basalt::server::MAX_HTTP_BENCHMARK_ORDER_COUNT, 10'000U);
}

// ---------------------------------------------------------------------------
// HTTP-level tests against a real loopback listener on an ephemeral port
// ---------------------------------------------------------------------------

class HttpServerTest : public testing::Test {
protected:
    std::unique_ptr<basalt::server::HttpServer> server;
    std::unique_ptr<basalt::platform::PlatformRunner> platform;
    std::thread worker;
    int port{};

    [[nodiscard]] virtual std::vector<std::string> configured_origins() const { return {}; }

    void SetUp() override {
        platform = std::make_unique<basalt::platform::PlatformRunner>();
        ASSERT_TRUE(platform->run(12));
        basalt::common::AppConfig config;
        config.cors_allowed_origins = configured_origins();
        server = std::make_unique<basalt::server::HttpServer>(config, platform.get());
        ASSERT_TRUE(server->bind_to_any_port());
        port = server->port();
        worker = std::thread([this] { static_cast<void>(server->run_bound()); });
        server->wait_until_ready();
    }

    void TearDown() override {
        server->stop();
        if (worker.joinable()) worker.join();
    }

    [[nodiscard]] httplib::Client client() const {
        httplib::Client result("127.0.0.1", port);
        result.set_connection_timeout(5);
        result.set_read_timeout(30);
        return result;
    }

    [[nodiscard]] std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }
};

class ConfiguredCorsHttpServerTest : public HttpServerTest {
protected:
    [[nodiscard]] std::vector<std::string> configured_origins() const override {
        return {"http://localhost:5173"};
    }
};

TEST_F(HttpServerTest, SameOriginGetSucceedsWithoutWildcardCors) {
    auto cli = client();
    const auto response = cli.Get("/api/state");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_FALSE(response->has_header("Access-Control-Allow-Origin"));

    const auto with_origin = cli.Get("/api/state", httplib::Headers{{"Origin", origin()}});
    ASSERT_TRUE(with_origin);
    EXPECT_EQ(with_origin->status, 200);
    EXPECT_EQ(with_origin->get_header_value("Access-Control-Allow-Origin"), origin());
    EXPECT_NE(with_origin->get_header_value("Access-Control-Allow-Origin"), "*");
}

TEST_F(HttpServerTest, CompilesPatchedHttpTransportAndRejectsPercentEncodedOriginAuthority) {
    EXPECT_STREQ(CPPHTTPLIB_VERSION, "0.58.0");

    auto cli = client();
    const auto response = cli.Get("/api/v1/health", httplib::Headers{{
        "Origin", "http://localhost%3a" + std::to_string(port)}});
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 403);
    EXPECT_EQ(Json::parse(response->body).at("code"), "FORBIDDEN_ORIGIN");
    EXPECT_FALSE(response->has_header("Access-Control-Allow-Origin"));
}

TEST_F(HttpServerTest, RejectsForeignHostHeaderAsDnsRebinding) {
    auto cli = client();
    const auto response = cli.Get("/api/state", httplib::Headers{{"Host", "attacker.example:" + std::to_string(port)}});
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 403);
    EXPECT_EQ(Json::parse(response->body).at("code"), "FORBIDDEN_HOST");

    const auto root = cli.Get("/", httplib::Headers{{"Host", "attacker.example"}});
    ASSERT_TRUE(root);
    EXPECT_EQ(root->status, 403);
}

TEST_F(HttpServerTest, RejectsMismatchedOriginOnReadsAndMutations) {
    auto cli = client();
    const httplib::Headers evil{{"Origin", "http://evil.example"}};
    const auto read = cli.Get("/api/state", evil);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->status, 403);
    EXPECT_FALSE(read->has_header("Access-Control-Allow-Origin"));

    const auto before = Json::parse(cli.Get("/api/state")->body).at("market").at("total_orders_submitted");
    const auto write = cli.Post("/api/orders", evil,
                                R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1})", "application/json");
    ASSERT_TRUE(write);
    EXPECT_EQ(write->status, 403);
    const auto after = Json::parse(cli.Get("/api/state")->body).at("market").at("total_orders_submitted");
    EXPECT_EQ(before, after);
}

TEST_F(HttpServerTest, StateChangingRequestsRequireJsonContentType) {
    auto cli = client();
    const auto text = cli.Post("/api/orders", R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1})",
                               "text/plain");
    ASSERT_TRUE(text);
    EXPECT_EQ(text->status, 415);

    const auto reset_form = cli.Post("/api/reset", "seed_demo=true", "application/x-www-form-urlencoded");
    ASSERT_TRUE(reset_form);
    EXPECT_EQ(reset_form->status, 415);

    const auto cancel_untyped = cli.Delete("/api/orders/1");
    ASSERT_TRUE(cancel_untyped);
    EXPECT_EQ(cancel_untyped->status, 415);

    const auto json = cli.Post("/api/orders", R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1})",
                               "application/json");
    ASSERT_TRUE(json);
    EXPECT_EQ(json->status, 200);
    const auto order_id = Json::parse(json->body).at("order_id").get<std::uint64_t>();

    const auto cancel = cli.Delete("/api/orders/" + std::to_string(order_id), httplib::Headers{}, "", "application/json");
    ASSERT_TRUE(cancel);
    EXPECT_EQ(cancel->status, 200);
}

TEST_F(HttpServerTest, PreflightAllowsOnlyAllowlistedOrigins) {
    auto cli = client();
    const auto allowed = cli.Options("/api/orders", httplib::Headers{
        {"Origin", origin()}, {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "content-type"}});
    ASSERT_TRUE(allowed);
    EXPECT_EQ(allowed->status, 204);
    EXPECT_EQ(allowed->get_header_value("Access-Control-Allow-Origin"), origin());
    EXPECT_NE(allowed->get_header_value("Access-Control-Allow-Methods").find("POST"), std::string::npos);
    EXPECT_EQ(allowed->get_header_value("Access-Control-Allow-Headers"),
              "Content-Type, X-Request-ID, Idempotency-Key");

    const auto localhost = cli.Options("/api/orders", httplib::Headers{
        {"Origin", "http://localhost:" + std::to_string(port)}, {"Access-Control-Request-Method", "POST"}});
    ASSERT_TRUE(localhost);
    EXPECT_EQ(localhost->status, 204);

    const auto denied = cli.Options("/api/orders", httplib::Headers{
        {"Origin", "http://evil.example"}, {"Access-Control-Request-Method", "POST"}});
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 403);
    EXPECT_FALSE(denied->has_header("Access-Control-Allow-Origin"));
}

TEST_F(ConfiguredCorsHttpServerTest, AllowsOnlyConfiguredOriginForReadsAndPreflight) {
    auto cli = client();
    const auto allowed = cli.Get("/api/v1/health", httplib::Headers{{"Origin", "http://localhost:5173"}});
    ASSERT_TRUE(allowed);
    EXPECT_EQ(allowed->status, 200);
    EXPECT_EQ(allowed->get_header_value("Access-Control-Allow-Origin"), "http://localhost:5173");

    const auto preflight = cli.Options("/api/v1/orders", httplib::Headers{
        {"Origin", "http://localhost:5173"}, {"Access-Control-Request-Method", "PATCH"},
        {"Access-Control-Request-Headers", "content-type,x-request-id"}});
    ASSERT_TRUE(preflight);
    EXPECT_EQ(preflight->status, 204);
    EXPECT_NE(preflight->get_header_value("Access-Control-Allow-Methods").find("PATCH"), std::string::npos);
    EXPECT_NE(preflight->get_header_value("Access-Control-Allow-Headers").find("X-Request-ID"), std::string::npos);

    const auto denied = cli.Get("/api/v1/health", httplib::Headers{{"Origin", "http://localhost:5174"}});
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 403);
    EXPECT_FALSE(denied->has_header("Access-Control-Allow-Origin"));
}

TEST_F(HttpServerTest, V1ReadRoutesReturnSchemaVersionedFrontendShapes) {
    auto cli = client();
    const auto health = cli.Get("/api/v1/health");
    ASSERT_TRUE(health);
    ASSERT_EQ(health->status, 200);
    const auto health_json = Json::parse(health->body);
    EXPECT_EQ(health_json.at("schema_version"), 1);
    EXPECT_EQ(health_json.at("state"), "OK");
    EXPECT_FALSE(health_json.at("websocket_available").get<bool>());

    const auto watch = cli.Get("/api/v1/market/watch");
    ASSERT_TRUE(watch);
    ASSERT_EQ(watch->status, 200);
    const auto instruments = Json::parse(watch->body).at("instruments");
    ASSERT_FALSE(instruments.empty());
    EXPECT_TRUE(instruments.at(0).contains("instrumentId"));
    EXPECT_TRUE(instruments.at(0).contains("lastPrice"));
    EXPECT_TRUE(instruments.at(0).contains("priceUnit"));
    EXPECT_FALSE(instruments.at(0).contains("instrument_id"));

    const auto snapshot = cli.Get("/api/v1/instruments/9001/snapshot?depth=20");
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->status, 200);
    const auto snapshot_json = Json::parse(snapshot->body);
    EXPECT_EQ(snapshot_json.at("schema_version"), 1);
    EXPECT_TRUE(snapshot_json.at("book").contains("restingOrders"));
    EXPECT_TRUE(snapshot_json.at("book").contains("bestBid"));
    EXPECT_TRUE(snapshot_json.at("trades").is_array());
    EXPECT_TRUE(snapshot_json.at("activity").is_array());

    const auto metadata = cli.Get("/api/v1/instruments/9001");
    ASSERT_TRUE(metadata);
    ASSERT_EQ(metadata->status, 200);
    EXPECT_TRUE(Json::parse(metadata->body).contains("tickSizeMicro"));

    const auto portfolio = cli.Get("/api/v1/portfolio/1");
    ASSERT_TRUE(portfolio);
    ASSERT_EQ(portfolio->status, 200);
    const auto portfolio_json = Json::parse(portfolio->body);
    EXPECT_TRUE(portfolio_json.contains("cashMicro"));
    EXPECT_TRUE(portfolio_json.contains("realizedPnlMicro"));
    EXPECT_FALSE(portfolio_json.contains("cash_balance_micro"));

    for (const auto& path : {"/api/v1/risk/1", "/api/v1/system/metrics", "/api/v1/trades?limit=10"}) {
        const auto response = cli.Get(path);
        ASSERT_TRUE(response) << path;
        ASSERT_EQ(response->status, 200) << path;
        EXPECT_EQ(Json::parse(response->body).at("schema_version"), 1) << path;
    }
}

TEST_F(HttpServerTest, V1ValidationUnsupportedRoutesAndPayloadLimitAreStructured) {
    auto cli = client();
    for (const auto& path : {"/api/v1/instruments/9001/snapshot?depth=0",
                             "/api/v1/instruments/9001/snapshot?depth=201",
                             "/api/v1/instruments/not-an-id"}) {
        const auto response = cli.Get(path);
        ASSERT_TRUE(response) << path;
        EXPECT_EQ(response->status, 400) << path;
        EXPECT_EQ(Json::parse(response->body).at("schema_version"), 1) << path;
    }

    const auto unsupported = cli.Get("/api/v1/backtests/abc");
    ASSERT_TRUE(unsupported);
    EXPECT_EQ(unsupported->status, 501);
    EXPECT_EQ(Json::parse(unsupported->body).at("code"), "NOT_IMPLEMENTED");

    const auto stream = cli.Get("/api/v1/stream");
    ASSERT_TRUE(stream);
    EXPECT_EQ(stream->status, 501);
    EXPECT_EQ(Json::parse(stream->body).at("code"), "STREAM_UNAVAILABLE");

    const auto malformed = cli.Post("/api/orders", "{", "application/json");
    ASSERT_TRUE(malformed);
    EXPECT_EQ(malformed->status, 400);

    const std::string oversized(70U * 1024U, 'x');
    const auto too_large = cli.Post("/api/orders", oversized, "application/json");
    ASSERT_TRUE(too_large);
    EXPECT_EQ(too_large->status, 413);
}

TEST_F(HttpServerTest, BenchmarkRejectsOrderCountsAboveTheMcpCap) {
    auto cli = client();
    for (const char* body : {R"({"order_count":10001})", R"({"order_count":0})",
                             R"({"order_count":-5})", R"({"order_count":1000000})",
                             R"({"order_count":"100"})"}) {
        const auto response = cli.Post("/api/benchmark", body, "application/json");
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
    }
    const auto valid = cli.Post("/api/benchmark",
                                R"({"profile":"BALANCED","order_count":100,"compare_baseline":false})",
                                "application/json");
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->status, 200);
    const auto results = Json::parse(valid->body).at("results");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results.at(0).at("orders_processed"), 100);
}

} // namespace

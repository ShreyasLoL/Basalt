#include "basalt/server/http_server.hpp"
#include "basalt/server/request_guard.hpp"
#include "basalt/server/simulation_facade.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

using basalt::server::validate_request_origin;
using Json = nlohmann::json;

constexpr int kPort = 8080;
constexpr std::optional<std::string_view> kNone = std::nullopt;

int send_raw_http_request(const int port, const std::string& request) {
#ifdef _WIN32
    const SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) return -1;
    const auto close_socket = [&] { ::closesocket(socket_handle); };
    const DWORD receive_timeout_ms = 2000;
    static_cast<void>(::setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&receive_timeout_ms), sizeof(receive_timeout_ms)));
#else
    const int socket_handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0) return -1;
    const auto close_socket = [&] { ::close(socket_handle); };
    const timeval receive_timeout{2, 0};
    static_cast<void>(::setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
        &receive_timeout, sizeof(receive_timeout)));
#endif
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr) != 1 ||
        ::connect(socket_handle, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        close_socket();
        return -1;
    }

    std::size_t offset = 0;
    while (offset < request.size()) {
#ifdef _WIN32
        const int sent = ::send(socket_handle, request.data() + offset,
            static_cast<int>(request.size() - offset), 0);
#else
        const auto sent = ::send(socket_handle, request.data() + offset, request.size() - offset, 0);
#endif
        if (sent <= 0) {
            close_socket();
            return -1;
        }
        offset += static_cast<std::size_t>(sent);
    }

    std::array<char, 256> response{};
    const auto received = ::recv(socket_handle, response.data(), static_cast<int>(response.size()), 0);
    close_socket();
    if (received <= 0) return -1;
    const std::string_view first_line(response.data(), static_cast<std::size_t>(received));
    if (!first_line.starts_with("HTTP/1.1 ") || first_line.size() < 12) return -1;
    int status = 0;
    const auto [end, error] = std::from_chars(first_line.data() + 9, first_line.data() + 12, status);
    return error == std::errc{} && end == first_line.data() + 12 ? status : -1;
}

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

TEST(RequestGuardTest, RequestPolicyNeverAcceptsRemoteAuthorities) {
    using basalt::server::is_allowed_host;
    EXPECT_FALSE(is_allowed_host("192.168.1.5:8080", kPort));
    EXPECT_FALSE(is_allowed_host("0.0.0.0:8080", kPort));
    EXPECT_FALSE(is_allowed_host("[fd00::1]:8080", kPort));
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
    EXPECT_TRUE(is_allowed_origin("http://localhost:5173", kPort, allowlist));
    EXPECT_TRUE(is_allowed_origin("https://research.example.com", kPort, allowlist));
    EXPECT_FALSE(is_allowed_origin("https://research.example.com/", kPort, allowlist));
    EXPECT_FALSE(is_allowed_origin("http://localhost:5174", kPort, allowlist));
}

TEST(RequestGuardTest, JsonMediaTypeAcceptsOnlyValidUtf8CharsetParameters) {
    using basalt::server::is_json_content_type;
    EXPECT_TRUE(is_json_content_type("application/json"));
    EXPECT_TRUE(is_json_content_type("Application/JSON; charset=utf-8"));
    EXPECT_TRUE(is_json_content_type(" application/json ;charset=utf-8"));
    EXPECT_TRUE(is_json_content_type("application/json; Charset = \"UTF-8\""));
    EXPECT_FALSE(is_json_content_type("text/plain"));
    EXPECT_FALSE(is_json_content_type("application/x-www-form-urlencoded"));
    EXPECT_FALSE(is_json_content_type("multipart/form-data; boundary=x"));
    EXPECT_FALSE(is_json_content_type("application/jsonp"));
    EXPECT_FALSE(is_json_content_type("application/json;"));
    EXPECT_FALSE(is_json_content_type("application/json; charset"));
    EXPECT_FALSE(is_json_content_type("application/json; charset="));
    EXPECT_FALSE(is_json_content_type("application/json; charset=latin1"));
    EXPECT_FALSE(is_json_content_type("application/json; charset=utf-8; charset=utf-8"));
    EXPECT_FALSE(is_json_content_type("application/json; charset=utf-8; boundary=x"));
    EXPECT_FALSE(is_json_content_type("application/json; charset=\"utf-8"));
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

TEST(HttpServerConfigTest, DirectConfigCannotRequestRemoteBinding) {
    basalt::common::AppConfig config;
    config.bind_address = "192.0.2.10";
    EXPECT_THROW((basalt::server::HttpServer(config)), std::invalid_argument);
}

TEST(RequestGuardTest, BenchmarkOrderCountIsCappedAtTenThousand) {
    using basalt::server::valid_benchmark_order_count;
    EXPECT_FALSE(valid_benchmark_order_count(0));
    EXPECT_TRUE(valid_benchmark_order_count(1));
    EXPECT_TRUE(valid_benchmark_order_count(10'000));
    EXPECT_FALSE(valid_benchmark_order_count(10'001));
    EXPECT_FALSE(valid_benchmark_order_count(1'000'000));
    EXPECT_EQ(basalt::server::MAX_HTTP_BENCHMARK_ORDER_COUNT, 10'000U);
    EXPECT_EQ(basalt::server::MAX_HTTP_BENCHMARK_REQUESTS_PER_MINUTE, 5U);
}

TEST(HttpServerQueueTest, ThreadPoolRejectsRequestsBeyondItsQueueBound) {
    httplib::ThreadPool pool(1, 1, 1);
    std::mutex mutex;
    std::condition_variable condition;
    bool worker_started = false;
    bool release_worker = false;
    ASSERT_TRUE(pool.enqueue([&] {
        std::unique_lock lock(mutex);
        worker_started = true;
        condition.notify_one();
        condition.wait(lock, [&] { return release_worker; });
    }));
    bool started = false;
    {
        std::unique_lock lock(mutex);
        started = condition.wait_for(lock, std::chrono::seconds(2), [&] { return worker_started; });
    }
    if (!started) {
        {
            std::lock_guard lock(mutex);
            release_worker = true;
        }
        condition.notify_one();
        pool.shutdown();
        FAIL() << "HTTP worker did not start in time";
        return;
    }
    EXPECT_TRUE(pool.enqueue([] {}));
    EXPECT_FALSE(pool.enqueue([] {}));
    {
        std::lock_guard lock(mutex);
        release_worker = true;
    }
    condition.notify_one();
    pool.shutdown();
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

TEST_F(HttpServerTest, BrowserSecurityHeadersCoverUiAndApiResponses) {
    auto cli = client();
    const auto verify_headers = [](const auto& response) {
        ASSERT_TRUE(response);
        EXPECT_EQ(response->get_header_value("X-Content-Type-Options"), "nosniff");
        EXPECT_EQ(response->get_header_value("X-Frame-Options"), "DENY");
        EXPECT_EQ(response->get_header_value("Referrer-Policy"), "no-referrer");
        EXPECT_EQ(response->get_header_value("Permissions-Policy"),
                  "camera=(), microphone=(), geolocation=()");
    };
    const auto ui = cli.Get("/");
    const auto api = cli.Get("/api/state");
    verify_headers(ui);
    verify_headers(api);
    ASSERT_TRUE(ui);
    ASSERT_TRUE(api);
    const auto csp = ui->get_header_value("Content-Security-Policy");
    if (ui->body.find("Research Workstation") != std::string::npos) {
        EXPECT_NE(csp.find("default-src 'self'"), std::string::npos);
        EXPECT_NE(csp.find("script-src 'self'"), std::string::npos);
        EXPECT_NE(csp.find("object-src 'none'"), std::string::npos);
        EXPECT_NE(csp.find("frame-ancestors 'none'"), std::string::npos);
        EXPECT_EQ(csp.find("upgrade-insecure-requests"), std::string::npos);
    } else {
        // The inline-script legacy fallback remains available when no Vite bundle
        // exists, so the strict production CSP is applied only to the bundle.
        EXPECT_TRUE(csp.empty());
    }
    EXPECT_TRUE(api->get_header_value("Content-Security-Policy").empty());
}

TEST_F(HttpServerTest, RejectsOversizedUriHeadersAndAmbiguousHostOrFraming) {
    const std::string host = "127.0.0.1:" + std::to_string(port);
    const std::string valid = "GET /api/v1/health HTTP/1.1\r\nHost: " + host +
        "\r\nConnection: close\r\n\r\n";
    EXPECT_EQ(send_raw_http_request(port, valid), 200);

    const std::string long_uri = "GET /api/v1/health?x=" + std::string(5000, 'a') +
        " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    EXPECT_NE(send_raw_http_request(port, long_uri), 200);

    const std::string long_header = "GET /api/v1/health HTTP/1.1\r\nHost: " + host +
        "\r\nX-Bulk: " + std::string(5000, 'a') + "\r\nConnection: close\r\n\r\n";
    EXPECT_NE(send_raw_http_request(port, long_header), 200);

    std::string many_headers = "GET /api/v1/health HTTP/1.1\r\nHost: " + host + "\r\n";
    for (int i = 0; i < CPPHTTPLIB_HEADER_MAX_COUNT; ++i) {
        many_headers += "X-" + std::to_string(i) + ": x\r\n";
    }
    many_headers += "Connection: close\r\n\r\n";
    EXPECT_NE(send_raw_http_request(port, many_headers), 200);

    const std::string duplicate_host = "GET /api/v1/health HTTP/1.1\r\nHost: " + host +
        "\r\nHost: attacker.example\r\nConnection: close\r\n\r\n";
    EXPECT_NE(send_raw_http_request(port, duplicate_host), 200);

    const std::string duplicate_origin = "GET /api/v1/health HTTP/1.1\r\nHost: " + host +
        "\r\nOrigin: http://localhost:" + std::to_string(port) +
        "\r\nOrigin: http://attacker.example\r\nConnection: close\r\n\r\n";
    EXPECT_NE(send_raw_http_request(port, duplicate_origin), 200);

    const std::string conflicting_framing = "POST /api/orders HTTP/1.1\r\nHost: " + host +
        "\r\nContent-Type: application/json\r\nContent-Length: 0\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    EXPECT_NE(send_raw_http_request(port, conflicting_framing), 200);
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

    const auto malformed_type = cli.Post("/api/orders",
        R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1})",
        "application/json; charset=");
    ASSERT_TRUE(malformed_type);
    EXPECT_EQ(malformed_type->status, 415);

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

TEST_F(HttpServerTest, OrderJsonSchemaRejectsInvalidInputBeforeStateChanges) {
    auto cli = client();
    const auto initial_state = cli.Get("/api/state");
    ASSERT_TRUE(initial_state);
    const auto initial_market = Json::parse(initial_state->body).at("market");
    std::vector<std::string> invalid_bodies{
        "{",
        "[]",
        R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1,"extra":true})",
        R"({"side":"BUY","type":"LIMIT","price":9000})",
        R"({"side":1,"type":"LIMIT","price":9000,"quantity":1})",
        R"({"side":"SHORT","type":"LIMIT","price":9000,"quantity":1})",
        R"({"side":"BUY","type":null,"price":9000,"quantity":1})",
        R"({"side":"BUY","type":"STOP","price":9000,"quantity":1})",
        R"({"side":"BUY","type":"LIMIT","price":9000.5,"quantity":1})",
        R"({"side":"BUY","type":"LIMIT","price":0,"quantity":1})",
        R"({"side":"BUY","type":"LIMIT","price":1000000000001,"quantity":1})",
        R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1.5})",
        R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":-1})",
        R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1000000001})",
        R"({"side":"BUY","type":"LIMIT","quantity":1})",
        R"({"id":-1,"side":"BUY","type":"LIMIT","price":9000,"quantity":1})",
        R"({"side":"BUY","type":"MARKET","price":3,"quantity":1})",
    };
    std::string too_deep = R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1,"extra":)";
    too_deep.append(17, '[');
    too_deep += '0';
    too_deep.append(17, ']');
    too_deep += '}';
    invalid_bodies.push_back(std::move(too_deep));

    for (const auto& body : invalid_bodies) {
        const auto response = cli.Post("/api/orders", body, "application/json");
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
        const auto error = Json::parse(response->body);
        EXPECT_EQ(error.at("error"), "request_error");
        EXPECT_TRUE(error.contains("code"));
        EXPECT_FALSE(error.at("message").get<std::string>().empty());
        EXPECT_EQ(response->body.find("type_error"), std::string::npos);
    }

    const auto state = cli.Get("/api/state");
    ASSERT_TRUE(state);
    const auto market = Json::parse(state->body).at("market");
    EXPECT_EQ(market.at("total_orders_submitted"), initial_market.at("total_orders_submitted"));
    EXPECT_EQ(market.at("active_order_count"), initial_market.at("active_order_count"));

    const auto market_order = cli.Post("/api/orders",
        R"({"side":"BUY","type":"MARKET","price":0,"quantity":1})", "application/json");
    ASSERT_TRUE(market_order);
    EXPECT_EQ(market_order->status, 200);
}

TEST_F(HttpServerTest, SimulationAndResetSchemasRejectInvalidValuesWithoutMutation) {
    auto cli = client();
    const auto resting = cli.Post("/api/orders",
        R"({"side":"BUY","type":"LIMIT","price":9000,"quantity":1})", "application/json");
    ASSERT_TRUE(resting);
    ASSERT_EQ(resting->status, 200);
    const auto before_invalid_requests = cli.Get("/api/state");
    ASSERT_TRUE(before_invalid_requests);
    const auto original_order_count = Json::parse(before_invalid_requests->body)
                                          .at("book").at("resting_orders").size();

    for (const char* body : {"[]", "null", R"({"extra":true})", R"({"count":0})",
                             R"({"count":501})", R"({"count":-1})", R"({"count":1.5})",
                             R"({"count":"5"})", R"({"count":null})"}) {
        const auto response = cli.Post("/api/sim/step", body, "application/json");
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
    }
    for (const char* body : {"[]", "null", R"({"seed_demo":1})", R"({"seed_demo":null})",
                             R"({"seed_demo":false,"unexpected":true})"}) {
        const auto response = cli.Post("/api/reset", body, "application/json");
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
    }
    const auto ignored_delete_body = cli.Delete("/api/orders/1", httplib::Headers{},
                                                 "{}", "application/json");
    ASSERT_TRUE(ignored_delete_body);
    EXPECT_EQ(ignored_delete_body->status, 400);

    const auto state = cli.Get("/api/state");
    ASSERT_TRUE(state);
    const auto book = Json::parse(state->body).at("book");
    EXPECT_EQ(book.at("resting_orders").size(), original_order_count);

    const auto empty_step = cli.Post("/api/sim/step", "", "application/json");
    ASSERT_TRUE(empty_step);
    EXPECT_EQ(empty_step->status, 200);
    EXPECT_EQ(Json::parse(empty_step->body).at("executed"), 1);
    const auto empty_reset = cli.Post("/api/reset", "", "application/json");
    ASSERT_TRUE(empty_reset);
    EXPECT_EQ(empty_reset->status, 200);
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

TEST_F(HttpServerTest, BenchmarkSchemaRejectsUnknownFieldsAndBadValuesBeforeAdmission) {
    auto cli = client();
    std::vector<std::string> invalid_bodies{
        "[]",
        R"({"unknown":true})",
        R"({"profile":4})",
        R"({"profile":"NOT_A_PROFILE"})",
        R"({"order_count":-1})",
        R"({"order_count":1.25})",
        R"({"order_count":"10"})",
        R"({"order_count":null})",
        R"({"compare_baseline":"false"})",
    };
    std::string too_deep = R"({"unexpected":)";
    too_deep.append(17, '{');
    too_deep += '"';
    too_deep += "x";
    too_deep += '\"';
    too_deep += ':';
    too_deep += '1';
    too_deep.append(17, '}');
    too_deep += '}';
    invalid_bodies.push_back(std::move(too_deep));

    for (const auto& body : invalid_bodies) {
        const auto response = cli.Post("/api/benchmark", body, "application/json");
        ASSERT_TRUE(response) << body;
        EXPECT_EQ(response->status, 400) << body;
        const auto error = Json::parse(response->body);
        EXPECT_TRUE(error.contains("code"));
        EXPECT_EQ(response->body.find("exception"), std::string::npos);
    }

    const auto valid = cli.Post("/api/benchmark",
        R"({"profile":"BALANCED","order_count":1,"compare_baseline":false})",
        "application/json");
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->status, 200) << valid->body;
}

TEST_F(HttpServerTest, BenchmarkRequestsAreRateLimitedAfterInputValidation) {
    EXPECT_EQ(CPPHTTPLIB_REQUEST_URI_MAX_LENGTH, 4096);
    EXPECT_EQ(CPPHTTPLIB_HEADER_MAX_LENGTH, 4096);
    EXPECT_EQ(CPPHTTPLIB_HEADER_MAX_COUNT, 64);

    auto cli = client();
    const std::string body = R"({"order_count":1,"compare_baseline":false})";
    for (std::size_t i = 0; i < basalt::server::MAX_HTTP_BENCHMARK_REQUESTS_PER_MINUTE; ++i) {
        const auto response = cli.Post("/api/benchmark", body, "application/json");
        ASSERT_TRUE(response) << "benchmark request " << i;
        EXPECT_EQ(response->status, 200) << response->body;
    }

    const auto limited = cli.Post("/api/benchmark", body, "application/json");
    ASSERT_TRUE(limited);
    EXPECT_EQ(limited->status, 429);
    EXPECT_EQ(limited->get_header_value("Retry-After"), "60");
    EXPECT_EQ(Json::parse(limited->body).at("code"), "RATE_LIMITED");
}

} // namespace

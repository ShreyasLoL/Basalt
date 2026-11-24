#include "basalt/server/http_server.hpp"
#include "basalt/server/request_guard.hpp"

#include "basalt_embedded_ui.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace basalt::server {
namespace {

using json = nlohmann::json;

// CORS is not wildcarded. The pre-routing guard echoes only same-origin or
// explicitly configured origins; preflight responses add the method/header list.
void set_preflight_headers(httplib::Response& response) {
    response.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    response.set_header("Access-Control-Allow-Headers", "Content-Type, X-Request-ID, Idempotency-Key");
    response.set_header("Access-Control-Max-Age", "600");
}

void send_json(httplib::Response& response, const json& value, const int status = 200) {
    response.status = status;
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

std::optional<std::string_view> optional_header(const httplib::Request& request, const char* name) {
    if (!request.has_header(name)) return std::nullopt;
    const auto range = request.headers.equal_range(name);
    return std::string_view(range.first->second);
}

json error_json(const std::string& message) {
    return json{{"error", "request_error"}, {"message", message}};
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

bool parse_side(const std::string& value, Side& side) {
    const std::string normalized = uppercase(value);
    if (normalized == "BUY") { side = Side::Buy; return true; }
    if (normalized == "SELL") { side = Side::Sell; return true; }
    return false;
}

bool parse_order_type(const std::string& value, OrderType& type) {
    const std::string normalized = uppercase(value);
    if (normalized == "LIMIT") { type = OrderType::Limit; return true; }
    if (normalized == "MARKET") { type = OrderType::Market; return true; }
    return false;
}

json to_json(const Trade& trade) {
    return json{{"trade_id", trade.trade_id}, {"buy_order_id", trade.buy_order_id},
                {"sell_order_id", trade.sell_order_id},
                {"aggressor_order_id", trade.aggressor_order_id},
                {"resting_order_id", trade.resting_order_id},
                {"aggressor_side", std::string(basalt::to_string(trade.aggressor_side))},
                {"price", trade.price}, {"quantity", trade.quantity},
                {"sequence", trade.sequence}, {"timestamp_ns", trade.timestamp_ns}};
}

json to_json(const Order& order) {
    return json{{"id", order.id}, {"side", std::string(basalt::to_string(order.side))},
                {"type", std::string(basalt::to_string(order.type))}, {"price", order.price},
                {"initial_quantity", order.initial_quantity},
                {"remaining_quantity", order.remaining_quantity},
                {"sequence", order.sequence}, {"timestamp_ns", order.timestamp_ns}};
}

json to_json(const PriceLevelSummary& level) {
    return json{{"price", level.price}, {"total_volume", level.total_volume},
                {"order_count", level.order_count}};
}

json to_json(const ActivityRecord& activity) {
    return json{{"sequence", activity.sequence},
                {"action", std::string(basalt::to_string(activity.action))},
                {"order_id", activity.order_id},
                {"side", std::string(basalt::to_string(activity.side))},
                {"order_type", std::string(basalt::to_string(activity.order_type))},
                {"price", activity.price}, {"quantity", activity.quantity},
                {"status", std::string(basalt::to_string(activity.status))},
                {"message", activity.message}};
}

template <typename Range, typename Convert>
json to_json_array(const Range& values, Convert convert) {
    json result = json::array();
    for (const auto& value : values) result.push_back(convert(value));
    return result;
}

bool parse_unsigned_path(const std::string& text, std::uint64_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

std::string request_id(const httplib::Request& request) {
    if (request.has_header("X-Request-ID")) return request.get_header_value("X-Request-ID");
    if (request.has_param("request_id")) return request.get_param_value("request_id");
    return {};
}

void send_v1_error(httplib::Response& response, const httplib::Request& request,
                   const int status, const std::string& code, const std::string& message,
                   json details = json::object()) {
    send_json(response, json{{"schema_version", 1}, {"code", code}, {"message", message},
                             {"request_id", request_id(request)}, {"details", std::move(details)}}, status);
}

json market_row(const platform::PlatformMarketSnapshot& item) {
    const auto& event = item.event;
    json last = nullptr;
    json bid = nullptr;
    json ask = nullptr;
    json spread = nullptr;
    json volume = nullptr;
    json source = nullptr;
    std::string quality = "DISCONNECTED";
    json event_time = nullptr;
    json receive_time = nullptr;
    if (event) {
        last = event->price_micro ? json(*event->price_micro) : json(nullptr);
        bid = event->bid_micro ? json(*event->bid_micro) : json(nullptr);
        ask = event->ask_micro ? json(*event->ask_micro) : json(nullptr);
        if (event->bid_micro && event->ask_micro) spread = *event->ask_micro - *event->bid_micro;
        volume = event->quantity ? json(*event->quantity) : json(nullptr);
        source = std::string(common::to_string(event->source));
        quality = std::string(common::to_string(event->quality));
        event_time = event->event_time_ns;
        receive_time = event->receive_time_utc_ns;
    }
    return json{{"instrumentId", std::to_string(item.instrument.id)},
                {"symbol", item.instrument.display_symbol.empty()
                    ? item.instrument.symbol : item.instrument.display_symbol},
                {"exchange", std::string(market_data::to_string(item.instrument.exchange))},
                {"priceUnit", "MICRO_INR"}, {"lastPrice", std::move(last)},
                {"bidPrice", std::move(bid)}, {"askPrice", std::move(ask)},
                {"spread", std::move(spread)}, {"volume", std::move(volume)},
                {"source", std::move(source)}, {"quality", quality},
                {"eventTimeNs", std::move(event_time)}, {"receiveTimeNs", std::move(receive_time)}};
}

bool parse_bounded_depth(const httplib::Request& request, std::size_t& depth) {
    depth = 20;
    if (!request.has_param("depth")) return true;
    std::uint64_t requested = 0;
    if (!parse_unsigned_path(request.get_param_value("depth"), requested) || requested < 1 || requested > 200)
        return false;
    depth = static_cast<std::size_t>(requested);
    return true;
}

json trade_json(const trading::ExecutionFill& fill) {
    const bool is_buy = fill.side == trading::Side::Buy;
    return json{{"tradeId", fill.execution_id},
                {"instrumentId", std::to_string(fill.instrument_id)},
                {"price", fill.price_micro}, {"priceUnit", "MICRO_INR"},
                {"quantity", fill.quantity}, {"buyOrderId", is_buy ? fill.order_id : 0},
                {"sellOrderId", is_buy ? 0 : fill.order_id},
                {"aggressorSide", is_buy ? "BUY" : "SELL"},
                {"eventTimeNs", fill.time_ns},
                {"model", "Conservative quote paper execution"}};
}

} // namespace

HttpServer::HttpServer(const int port) : port_(port) {
    if (port_ < 1 || port_ > 65535) throw std::invalid_argument("Port must be between 1 and 65535");
    register_routes();
}

HttpServer::HttpServer(const common::AppConfig& config, platform::PlatformRunner* platform_runner)
    : port_(static_cast<int>(config.port)), bind_address_(config.bind_address),
      cors_allowed_origins_(config.cors_allowed_origins), platform_runner_(platform_runner) {
    const auto validated = common::ConfigLoader::validate(config);
    if (!validated) throw std::invalid_argument(validated.error().message);
    register_routes();
}

std::string HttpServer::ui_html() {
#ifdef BASALT_FRONTEND_DIST_DIR
    {
        std::ifstream app_input(BASALT_FRONTEND_DIST_DIR "/index.html", std::ios::binary);
        if (app_input) {
            std::string contents((std::istreambuf_iterator<char>(app_input)), std::istreambuf_iterator<char>());
            if (!contents.empty()) return contents;
        }
    }
#endif
    constexpr std::array<const char*, 4> candidates{
        "web/index.html", "../web/index.html", "../../web/index.html", "../../../web/index.html"};
    for (const char* path : candidates) {
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!contents.empty()) return contents;
    }
    return embedded::INDEX_HTML;
}

bool HttpServer::guard_request(const httplib::Request& request, httplib::Response& response) const {
    constexpr std::array<const char*, 5> unique_security_headers{
        "Host", "Origin", "Content-Type", "Content-Length", "Transfer-Encoding"};
    for (const char* name : unique_security_headers) {
        if (request.headers.count(name) > 1) {
            send_json(response, json{{"error", "AMBIGUOUS_HEADERS"},
                                     {"code", "AMBIGUOUS_HEADERS"},
                                     {"message", "Security-sensitive headers must not be repeated"}}, 400);
            return false;
        }
    }
    const auto origin = optional_header(request, "Origin");
    const auto decision = validate_request_origin(optional_header(request, "Host"), origin,
                                                  optional_header(request, "Content-Type"),
                                                  request.method, port_,
                                                  cors_allowed_origins_);
    response.set_header("Vary", "Origin");
    if (!decision.allowed()) {
        send_json(response, json{{"error", decision.code}, {"code", decision.code},
                                 {"message", decision.message}}, decision.status);
        return false;
    }
    if (origin) response.set_header("Access-Control-Allow-Origin", std::string(*origin));
    return true;
}

void HttpServer::register_routes() {
    server_.set_payload_max_length(64U * 1024U);
    server_.set_read_timeout(5, 0);
    server_.set_write_timeout(5, 0);
    server_.set_keep_alive_timeout(2);
    server_.set_keep_alive_max_count(5);
    server_.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 32); };
#ifdef BASALT_FRONTEND_DIST_DIR
    static_cast<void>(server_.set_mount_point("/assets", BASALT_FRONTEND_DIST_DIR "/assets"));
#endif
    // Transport policy for every request (UI, assets and API): loopback Host only
    // (DNS-rebinding defence), allowlisted Origin when present, and JSON media type
    // for state-changing methods. For bodiless requests this runs before routing;
    // when a request carries a body, cpp-httplib drains the bounded body first so
    // Winsock never resets the socket with unread bytes in SO_RCVBUF.
    server_.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response& response) {
        const bool has_body = (request.has_header("Content-Length") &&
                               request.get_header_value("Content-Length") != "0") ||
                              request.has_header("Transfer-Encoding");
        if (has_body) return httplib::Server::HandlerResponse::Unhandled;
        return guard_request(request, response) ? httplib::Server::HandlerResponse::Unhandled
                                                : httplib::Server::HandlerResponse::Handled;
    });

    server_.set_error_handler([this](const httplib::Request& request, httplib::Response& response) {
        if (response.status == 403 || response.status == 415) return;
        (void)guard_request(request, response);
    });

    server_.Get("/", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(ui_html(), "text/html; charset=utf-8");
    });

    server_.Options(R"(/api/.*)", [](const httplib::Request& request, httplib::Response& response) {
        response.status = 204;
        if (request.has_header("Origin")) set_preflight_headers(response);
    });

    server_.Get("/api/v1/health", [this](const httplib::Request&, httplib::Response& response) {
        const auto elapsed = std::chrono::steady_clock::now() - start_time_;
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        const bool connected = platform_runner_ != nullptr;
        send_json(response, json{{"schema_version", 1},
                                 {"state", connected ? "OK" : "DEGRADED"},
                                 {"version", "1.0.0"},
                                 {"uptime_seconds", std::max<std::int64_t>(0, uptime)},
                                 {"storage", "in_memory_session"},
                                 {"websocket_available", false},
                                 {"message", connected
                                     ? "REST v1 connected; WebSocket streaming is unavailable in this server build"
                                     : "HTTP server is running without a platform read model"}});
    });

    server_.Get("/api/v1/market/watch", [this](const httplib::Request& request,
                                                  httplib::Response& response) {
        if (platform_runner_ == nullptr) {
            send_v1_error(response, request, 503, "PLATFORM_UNAVAILABLE",
                          "Market data read model is not connected");
            return;
        }
        json instruments = json::array();
        for (const auto& item : platform_runner_->market_watch()) instruments.push_back(market_row(item));
        send_json(response, json{{"schema_version", 1}, {"instruments", std::move(instruments)}});
    });

    server_.Get(R"(/api/v1/instruments/([^/]+)/snapshot)",
                [this](const httplib::Request& request, httplib::Response& response) {
        std::size_t depth = 20;
        if (!parse_bounded_depth(request, depth)) {
            send_v1_error(response, request, 400, "INVALID_ARGUMENT", "depth must be an integer from 1 to 200");
            return;
        }
        static_cast<void>(depth);
        if (platform_runner_ == nullptr) {
            send_v1_error(response, request, 503, "PLATFORM_UNAVAILABLE",
                          "Instrument snapshot read model is not connected");
            return;
        }
        common::InstrumentId instrument_id = 0;
        if (!parse_unsigned_path(request.matches[1], instrument_id) || instrument_id == 0) {
            send_v1_error(response, request, 400, "INVALID_ARGUMENT", "Instrument ID must be a positive integer");
            return;
        }
        const auto snapshot = platform_runner_->market_snapshot(instrument_id);
        if (!snapshot) {
            send_v1_error(response, request, 404, snapshot.error().code, snapshot.error().message);
            return;
        }
        const auto& event = snapshot.value().event;
        const auto row = market_row(snapshot.value());
        json quote{{"instrumentId", std::to_string(instrument_id)},
                   {"lastPrice", row.at("lastPrice")}, {"bidPrice", row.at("bidPrice")},
                   {"askPrice", row.at("askPrice")}, {"spread", row.at("spread")},
                   {"source", row.at("source")}, {"quality", row.at("quality")},
                   {"eventTimeNs", row.at("eventTimeNs")}, {"receiveTimeNs", row.at("receiveTimeNs")},
                   {"eventSeq", event && event->provider_sequence ? json(*event->provider_sequence) : json(0)}};
        const json book{{"instrumentId", std::to_string(instrument_id)}, {"priceUnit", "MICRO_INR"},
                        {"bids", json::array()}, {"asks", json::array()},
                        {"restingOrders", json::array()}, {"bestBid", row.at("bidPrice")},
                        {"bestAsk", row.at("askPrice")}, {"spread", row.at("spread")}};
        json trades = json::array();
        for (const auto& fill : platform_runner_->recent_fills(200)) {
            if (fill.instrument_id == instrument_id) trades.push_back(trade_json(fill));
        }
        const json event_sequence = quote.at("eventSeq");
        send_json(response, json{{"schema_version", 1}, {"quote", std::move(quote)},
                                 {"depth", json{{"bids", json::array()}, {"asks", json::array()}}},
                                 {"book", book}, {"trades", std::move(trades)},
                                 {"activity", json::array()}, {"sequence", event_sequence}});
    });

    server_.Get(R"(/api/v1/instruments/([^/]+))",
                [this](const httplib::Request& request, httplib::Response& response) {
        common::InstrumentId instrument_id = 0;
        if (!parse_unsigned_path(request.matches[1], instrument_id) || instrument_id == 0) {
            send_v1_error(response, request, 400, "INVALID_ARGUMENT", "Instrument ID must be a positive integer");
            return;
        }
        if (platform_runner_ == nullptr) {
            send_v1_error(response, request, 503, "PLATFORM_UNAVAILABLE",
                          "Instrument catalog is not connected");
            return;
        }
        const auto found = platform_runner_->market_snapshot(instrument_id);
        if (!found) {
            send_v1_error(response, request, 404, found.error().code, found.error().message);
            return;
        }
        const auto& instrument = found.value().instrument;
        json sessions = json::array();
        for (const auto& session : instrument.sessions)
            sessions.push_back({{"openLocal", session.open_local}, {"closeLocal", session.close_local}});
        json mappings = json::array();
        for (const auto& mapping : instrument.provider_mappings)
            mappings.push_back({{"provider", mapping.provider}, {"providerKey", mapping.provider_key},
                                {"validFromNs", mapping.valid_from_ns},
                                {"validToNs", mapping.valid_to_ns ? json(*mapping.valid_to_ns) : json(nullptr)}});
        send_json(response, json{{"schema_version", 1}, {"instrumentId", std::to_string(instrument.id)},
                                 {"exchange", std::string(market_data::to_string(instrument.exchange))},
                                 {"segment", instrument.segment}, {"symbol", instrument.symbol},
                                 {"displaySymbol", instrument.display_symbol}, {"currency", instrument.currency},
                                 {"timezone", instrument.timezone}, {"priceUnit", "MICRO_INR"},
                                 {"tickSizeMicro", instrument.tick_size_micro}, {"lotSize", instrument.lot_size},
                                 {"quantityStep", instrument.quantity_step}, {"active", instrument.active},
                                 {"sessions", std::move(sessions)}, {"providerMappings", std::move(mappings)},
                                 {"mappingStatus", instrument.provider_mappings.empty() ? "UNMAPPED" : "MAPPED"}});
    });

    server_.Get(R"(/api/v1/portfolio/([^/]+))",
                [this](const httplib::Request& request, httplib::Response& response) {
        common::AccountId account_id = 0;
        if (!parse_unsigned_path(request.matches[1], account_id) || account_id == 0) {
            send_v1_error(response, request, 400, "INVALID_ARGUMENT", "Account ID must be a positive integer");
            return;
        }
        if (platform_runner_ == nullptr) {
            send_v1_error(response, request, 503, "PLATFORM_UNAVAILABLE", "Portfolio read model is not connected");
            return;
        }
        const auto portfolio = platform_runner_->portfolio(account_id);
        if (!portfolio) {
            const int status = portfolio.error().code == "ACCOUNT_NOT_FOUND" ? 404 : 500;
            send_v1_error(response, request, status, portfolio.error().code, portfolio.error().message);
            return;
        }
        std::int64_t market_value = 0;
        bool all_positions_marked = true;
        json positions = json::array();
        for (const auto& [instrument_id, position] : portfolio.value().positions) {
            if (position.mark_micro) {
                const auto value = common::checked_multiply(*position.mark_micro, position.quantity);
                if (value) {
                    const auto total = common::checked_add(market_value, value.value());
                    if (total) market_value = total.value();
                    else all_positions_marked = false;
                } else all_positions_marked = false;
            } else all_positions_marked = false;
            positions.push_back({{"instrumentId", std::to_string(instrument_id)},
                                 {"quantity", position.quantity},
                                 {"averageCostMicro", position.average_cost_micro},
                                 {"markMicro", position.mark_micro ? json(*position.mark_micro) : json(nullptr)},
                                 {"markSource", position.mark_source}});
        }
        json equity = nullptr;
        if (all_positions_marked) {
            const auto calculated = common::checked_add(portfolio.value().cash_balance_micro, market_value);
            if (calculated) equity = calculated.value();
        }
        send_json(response, json{{"schema_version", 1}, {"accountId", std::to_string(account_id)},
                                 {"cashMicro", portfolio.value().cash_balance_micro},
                                 {"equityMicro", std::move(equity)},
                                 {"realizedPnlMicro", portfolio.value().realized_pnl_micro},
                                 {"unrealizedPnlMicro", portfolio.value().unrealized_pnl_micro},
                                 {"feesMicro", portfolio.value().total_fees_micro},
                                 {"positions", std::move(positions)}, {"dataAgeNs", nullptr}});
    });

    server_.Get(R"(/api/v1/risk/([^/]+))",
                [this](const httplib::Request& request, httplib::Response& response) {
        common::AccountId account_id = 0;
        if (!parse_unsigned_path(request.matches[1], account_id) || account_id == 0) {
            send_v1_error(response, request, 400, "INVALID_ARGUMENT", "Account ID must be a positive integer");
            return;
        }
        if (platform_runner_ == nullptr) {
            send_v1_error(response, request, 503, "PLATFORM_UNAVAILABLE", "Risk read model is not connected");
            return;
        }
        const auto state = platform_runner_->snapshot();
        if (!state) {
            send_v1_error(response, request, 500, state.error().code, state.error().message);
            return;
        }
        if (state.value().portfolio.account_id != account_id) {
            send_v1_error(response, request, 404, "ACCOUNT_NOT_FOUND", "Risk read model is available only for the active paper account");
            return;
        }
        const auto& limits = state.value().risk_limits;
        const json limit_rows = json::array({
            json{{"name", "Max order quantity"}, {"used", nullptr}, {"limit", limits.max_order_quantity}, {"unit", "shares"}},
            json{{"name", "Max position quantity"}, {"used", nullptr}, {"limit", limits.max_position_quantity}, {"unit", "shares"}},
            json{{"name", "Max order notional"}, {"used", nullptr}, {"limit", limits.max_order_notional_micro}, {"unit", "micro-INR"}},
            json{{"name", "Max gross exposure"}, {"used", nullptr}, {"limit", limits.max_gross_exposure_micro}, {"unit", "micro-INR"}},
            json{{"name", "Max open orders"}, {"used", nullptr}, {"limit", limits.max_open_orders}, {"unit", "orders"}}
        });
        json rejections = json::array();
        const auto& recent_rejections = state.value().risk_rejection_events;
        const auto first_rejection = recent_rejections.size() > 100 ? recent_rejections.size() - 100 : 0;
        for (auto index = first_rejection; index < recent_rejections.size(); ++index) {
            const auto& [code, event_sequence] = recent_rejections[index];
            rejections.push_back({{"code", code}, {"message", code}, {"eventSeq", event_sequence}});
        }
        send_json(response, json{{"schema_version", 1}, {"killSwitch", state.value().execution_halted},
                                 {"limits", limit_rows}, {"rejections", std::move(rejections)}});
    });

    server_.Get("/api/v1/trades", [this](const httplib::Request& request, httplib::Response& response) {
        if (platform_runner_ == nullptr) {
            send_v1_error(response, request, 503, "PLATFORM_UNAVAILABLE", "Trade read model is not connected");
            return;
        }
        std::uint64_t limit = 100;
        if (request.has_param("limit") &&
            (!parse_unsigned_path(request.get_param_value("limit"), limit) || limit == 0 || limit > 200)) {
            send_v1_error(response, request, 400, "INVALID_ARGUMENT", "limit must be an integer from 1 to 200");
            return;
        }
        std::optional<common::InstrumentId> instrument_filter;
        if (request.has_param("instrument_id")) {
            common::InstrumentId id = 0;
            if (!parse_unsigned_path(request.get_param_value("instrument_id"), id) || id == 0) {
                send_v1_error(response, request, 400, "INVALID_ARGUMENT", "instrument_id must be a positive integer");
                return;
            }
            instrument_filter = id;
        }
        json trades = json::array();
        for (const auto& fill : platform_runner_->recent_fills(200)) {
            if (!instrument_filter || fill.instrument_id == *instrument_filter) {
                trades.push_back(trade_json(fill));
                if (trades.size() >= limit) break;
            }
        }
        send_json(response, json{{"schema_version", 1}, {"trades", std::move(trades)}});
    });

    server_.Get("/api/v1/system/metrics", [this](const httplib::Request& request,
                                                    httplib::Response& response) {
        if (platform_runner_ == nullptr) {
            send_v1_error(response, request, 503, "PLATFORM_UNAVAILABLE", "Runtime metrics are not connected");
            return;
        }
        const auto metrics = platform_runner_->telemetry();
        const auto& pipeline = metrics.pipeline;
        const auto& bus = metrics.event_bus;
        send_json(response, json{{"schema_version", 1}, {"stages", json::array()},
                                 {"eventRate", nullptr},
                                 {"queueDepth", bus.durable_depth + bus.quote_depth},
                                 {"replacements", bus.quote_replacements},
                                 {"dropped", bus.quote_overflows + bus.durable_timeouts},
                                 {"duplicates", pipeline.duplicates}, {"outOfOrder", pipeline.out_of_order},
                                 {"received", pipeline.received}, {"accepted", pipeline.accepted},
                                 {"invalid", pipeline.invalid}});
    });

    server_.Get("/api/v1/stream", [](const httplib::Request& request, httplib::Response& response) {
        send_v1_error(response, request, 501, "STREAM_UNAVAILABLE",
                      "The current cpp-httplib gateway has no WebSocket upgrade support");
    });

    server_.Get("/api/state", [this](const httplib::Request& request, httplib::Response& response) {
        std::size_t depth = 20;
        if (request.has_param("depth")) {
            std::uint64_t requested = 0;
            if (!parse_unsigned_path(request.get_param_value("depth"), requested)) {
                send_json(response, error_json("depth must be a non-negative integer"), 400);
                return;
            }
            depth = static_cast<std::size_t>(std::min<std::uint64_t>(requested, 100));
        }

        json payload;
        const auto view = simulation_.inspect(depth, 50, 50);
        const auto& snapshot = view.book;
        const auto& stats = view.stats;
        const auto& trades = view.trades;
        const auto& activity = view.activity;
        payload["market"] = {
                {"best_bid", snapshot.best_bid}, {"has_best_bid", snapshot.has_best_bid},
                {"best_bid_volume", snapshot.best_bid_volume},
                {"best_ask", snapshot.best_ask}, {"has_best_ask", snapshot.has_best_ask},
                {"best_ask_volume", snapshot.best_ask_volume},
                {"spread", snapshot.spread}, {"has_spread", snapshot.has_spread},
                {"last_trade_price", stats.last_trade_price},
                {"has_last_trade_price", stats.has_last_trade_price},
                {"last_trade_quantity", stats.last_trade_quantity},
                {"total_volume_traded", stats.total_volume_traded},
                {"total_trades_executed", stats.total_trades_executed},
                {"total_orders_submitted", stats.total_orders_submitted},
                {"total_orders_rejected", stats.total_orders_rejected},
                {"total_orders_cancelled", stats.total_orders_cancelled},
                {"active_order_count", stats.active_order_count},
                {"bid_level_count", stats.bid_level_count}, {"ask_level_count", stats.ask_level_count}
            };
            payload["book"] = {
                {"bids", to_json_array(snapshot.bids, [](const auto& level) { return to_json(level); })},
                {"asks", to_json_array(snapshot.asks, [](const auto& level) { return to_json(level); })},
                {"resting_orders", to_json_array(snapshot.resting_orders,
                                                   [](const auto& order) { return to_json(order); })}
            };
            payload["trades"] = to_json_array(trades, [](const auto& trade) { return to_json(trade); });
        payload["activity"] = to_json_array(activity,
                                            [](const auto& record) { return to_json(record); });
        send_json(response, payload);
    });

    server_.Post("/api/orders", [this](const httplib::Request& request, httplib::Response& response) {
        if (!guard_request(request, response)) return;
        try {
            const json body = json::parse(request.body);
            OrderRequest order{};
            order.id = body.value("id", OrderId{0});
            const std::string side_text = body.value("side", std::string{});
            const std::string type_text = body.value("type", std::string{});
            if (!parse_side(side_text, order.side)) {
                send_json(response, error_json("side must be BUY or SELL"), 400);
                return;
            }
            if (!parse_order_type(type_text, order.type)) {
                send_json(response, error_json("type must be LIMIT or MARKET"), 400);
                return;
            }
            order.price = order.type == OrderType::Market ? 0 : body.value("price", Price{0});
            order.quantity = body.value("quantity", Quantity{0});

            json result_json;
            int status_code = 200;
            {
                const OrderResult result = simulation_.submit_order(order);
                result_json = {
                    {"order_id", result.order_id},
                    {"status", std::string(basalt::to_string(result.status))},
                    {"initial_quantity", result.initial_quantity},
                    {"filled_quantity", result.filled_quantity},
                    {"remaining_quantity", result.remaining_quantity},
                    {"sequence", result.sequence}, {"message", result.message},
                    {"trades", to_json_array(result.trades,
                                               [](const auto& trade) { return to_json(trade); })}
                };
                if (result.status == OrderStatus::Rejected) status_code = 400;
            }
            send_json(response, result_json, status_code);
        } catch (const json::exception& error) {
            send_json(response, error_json(error.what()), 400);
        }
    });

    server_.Delete(R"(/api/orders/(\d+))",
        [this](const httplib::Request& request, httplib::Response& response) {
            if (!guard_request(request, response)) return;
            if (request.matches.size() < 2) {
                send_json(response, error_json("Order ID is required"), 400);
                return;
            }
            std::uint64_t id = 0;
            if (!parse_unsigned_path(request.matches[1].str(), id)) {
                send_json(response, error_json("Order ID must be numeric"), 400);
                return;
            }
            CancelResult result;
            result = simulation_.cancel_order(id);
            send_json(response, {
                {"order_id", result.order_id}, {"success", result.success},
                {"status", std::string(basalt::to_string(result.status))},
                {"cancelled_price", result.cancelled_price},
                {"cancelled_quantity", result.cancelled_quantity},
                {"sequence", result.sequence}, {"message", result.message}
            }, result.success ? 200 : 404);
        });

    server_.Post("/api/sim/step", [this](const httplib::Request& request, httplib::Response& response) {
        if (!guard_request(request, response)) return;
        std::size_t count = 1;
        try {
            if (!request.body.empty()) {
                const json body = json::parse(request.body);
                const std::size_t requested = body.value("count", std::size_t{1});
                count = std::clamp<std::size_t>(requested, 1, 500);
            }
        } catch (const json::exception& error) {
            send_json(response, error_json(error.what()), 400);
            return;
        }

        std::size_t trades_generated = 0;
        std::vector<OrderRequest> requests;
        requests.reserve(count);
        {
            std::lock_guard<std::mutex> lock(simulation_rng_mutex_);
            for (std::size_t i = 0; i < count; ++i) {
                const Side side = (simulation_rng_() & 1U) == 0 ? Side::Buy : Side::Sell;
                const bool market = simulation_rng_() % 100 < 8;
                const Quantity quantity = 10 + simulation_rng_() % 91;
                Price price = 0;
                if (!market) {
                    const Price offset = static_cast<Price>(10 + simulation_rng_() % 41);
                    const bool aggressive = simulation_rng_() % 100 < 15;
                    if (side == Side::Buy) price = aggressive ? 10000 + offset : 10000 - offset;
                    else price = aggressive ? 10000 - offset : 10000 + offset;
                }
                requests.push_back(OrderRequest{
                    0, side, market ? OrderType::Market : OrderType::Limit, price, quantity});
            }
        }
        const auto results = simulation_.submit_batch(std::move(requests));
        for (const auto& result : results) trades_generated += result.trades.size();
        const std::size_t active_orders = simulation_.stats().active_order_count;
        send_json(response, {{"executed", count}, {"active_orders", active_orders},
                             {"trades_generated", trades_generated}});
    });

    server_.Post("/api/reset", [this](const httplib::Request& request, httplib::Response& response) {
        if (!guard_request(request, response)) return;
        bool seed_demo = false;
        try {
            if (!request.body.empty()) {
                const json body = json::parse(request.body);
                seed_demo = body.value("seed_demo", false);
            }
        } catch (const json::exception& error) {
            send_json(response, error_json(error.what()), 400);
            return;
        }
        simulation_.reset(seed_demo);
        send_json(response, {{"reset", true}, {"seed_demo", seed_demo}});
    });

    server_.Post("/api/benchmark", [this](const httplib::Request& request, httplib::Response& response) {
        if (!guard_request(request, response)) return;
        try {
            const json body = request.body.empty() ? json::object() : json::parse(request.body);
            const std::string profile_text = uppercase(body.value("profile", std::string("ALL")));
            const auto requested_count = body.find("order_count");
            std::uint64_t order_count = MAX_HTTP_BENCHMARK_ORDER_COUNT;
            if (requested_count != body.end()) {
                if (!requested_count->is_number_integer()) {
                    send_json(response, error_json("order_count must be an integer between 1 and 10000"), 400);
                    return;
                }
                if (requested_count->is_number_integer() && requested_count->get<std::int64_t>() < 0) {
                    send_json(response, error_json("order_count must be between 1 and 10000"), 400);
                    return;
                }
                order_count = requested_count->get<std::uint64_t>();
            }
            const bool compare_baseline = body.value("compare_baseline", true);
            if (!valid_benchmark_order_count(order_count)) {
                send_json(response, error_json("order_count must be between 1 and 10000"), 400);
                return;
            }
            if (!admit_benchmark_request()) {
                response.set_header("Retry-After", "60");
                send_json(response, json{{"error", "HTTP benchmark rate limit exceeded"},
                                         {"code", "RATE_LIMITED"},
                                         {"message", "Retry after the one-minute benchmark window."}}, 429);
                return;
            }
            std::vector<bench::BenchmarkResult> results;
            if (profile_text == "ALL") {
                results = benchmark_runner_.run_all(static_cast<std::size_t>(order_count), compare_baseline);
            } else {
                bench::WorkloadProfile profile{};
                if (!bench::parse_profile(profile_text, profile)) {
                    send_json(response, error_json("Unknown workload profile"), 400);
                    return;
                }
                results = benchmark_runner_.run(profile, static_cast<std::size_t>(order_count), compare_baseline);
            }
            json output = json::array();
            for (const auto& result : results) {
                output.push_back({
                    {"profile_name", result.profile_name},
                    {"implementation", result.implementation},
                    {"orders_processed", result.orders_processed},
                    {"trades_executed", result.trades_executed},
                    {"cancellations_executed", result.cancellations_executed},
                    {"total_time_ms", result.total_time_ms},
                    {"throughput_ops_per_sec", result.throughput_ops_per_sec},
                    {"avg_latency_ns", result.avg_latency_ns},
                    {"p50_latency_ns", result.p50_latency_ns},
                    {"p95_latency_ns", result.p95_latency_ns},
                    {"p99_latency_ns", result.p99_latency_ns},
                    {"p999_latency_ns", result.p999_latency_ns}
                });
            }
            send_json(response, {{"results", std::move(output)}});
        } catch (const json::exception& error) {
            send_json(response, error_json(error.what()), 400);
        } catch (const std::exception& error) {
            send_json(response, error_json(error.what()), 500);
        }
    });

    const auto unsupported_v1 = [](const httplib::Request& request, httplib::Response& response) {
        send_v1_error(response, request, 501, "NOT_IMPLEMENTED",
                      "This schema-versioned route is documented but is not implemented by this server build");
    };
    server_.Get(R"(/api/v1(?:/.*)?)", unsupported_v1);
    server_.Post(R"(/api/v1(?:/.*)?)", unsupported_v1);
    server_.Put(R"(/api/v1(?:/.*)?)", unsupported_v1);
    server_.Patch(R"(/api/v1(?:/.*)?)", unsupported_v1);
    server_.Delete(R"(/api/v1(?:/.*)?)", unsupported_v1);
}

bool HttpServer::admit_benchmark_request() {
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::now();
    const auto cutoff = now - 1min;
    std::lock_guard lock(benchmark_rate_mutex_);
    while (!benchmark_request_times_.empty() && benchmark_request_times_.front() <= cutoff) {
        benchmark_request_times_.pop_front();
    }
    if (benchmark_request_times_.size() >= MAX_HTTP_BENCHMARK_REQUESTS_PER_MINUTE) return false;
    benchmark_request_times_.push_back(now);
    return true;
}

bool HttpServer::run() {
    return server_.listen(bind_address_, port_);
}

bool HttpServer::bind_to_any_port() {
    const int bound = server_.bind_to_any_port(bind_address_);
    if (bound <= 0) return false;
    port_ = bound;
    return true;
}

bool HttpServer::run_bound() {
    return server_.listen_after_bind();
}

void HttpServer::wait_until_ready() const {
    server_.wait_until_ready();
}

void HttpServer::stop() noexcept {
    server_.stop();
}

} // namespace basalt::server

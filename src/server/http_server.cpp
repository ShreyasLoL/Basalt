#include "atlas/server/http_server.hpp"

#include "atlas_embedded_ui.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

namespace atlas::server {
namespace {

using json = nlohmann::json;

void set_cors(httplib::Response& response) {
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
}

void send_json(httplib::Response& response, const json& value, const int status = 200) {
    response.status = status;
    set_cors(response);
    response.set_content(value.dump(), "application/json; charset=utf-8");
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
                {"aggressor_side", std::string(atlas::to_string(trade.aggressor_side))},
                {"price", trade.price}, {"quantity", trade.quantity},
                {"sequence", trade.sequence}, {"timestamp_ns", trade.timestamp_ns}};
}

json to_json(const Order& order) {
    return json{{"id", order.id}, {"side", std::string(atlas::to_string(order.side))},
                {"type", std::string(atlas::to_string(order.type))}, {"price", order.price},
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
                {"action", std::string(atlas::to_string(activity.action))},
                {"order_id", activity.order_id},
                {"side", std::string(atlas::to_string(activity.side))},
                {"order_type", std::string(atlas::to_string(activity.order_type))},
                {"price", activity.price}, {"quantity", activity.quantity},
                {"status", std::string(atlas::to_string(activity.status))},
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

} // namespace

HttpServer::HttpServer(const int port) : port_(port) {
    if (port_ < 1 || port_ > 65535) throw std::invalid_argument("Port must be between 1 and 65535");
    engine_.seed_demo_book();
    register_routes();
}

std::string HttpServer::ui_html() {
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

void HttpServer::register_routes() {
    server_.Get("/", [](const httplib::Request&, httplib::Response& response) {
        set_cors(response);
        response.set_content(ui_html(), "text/html; charset=utf-8");
    });

    server_.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
        set_cors(response);
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
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
            const BookSnapshot snapshot = engine_.get_snapshot(depth);
            const EngineStats stats = engine_.stats();
            const auto trades = engine_.recent_trades(50);
            const auto activity = engine_.recent_activity(50);
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
        }
        send_json(response, payload);
    });

    server_.Post("/api/orders", [this](const httplib::Request& request, httplib::Response& response) {
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
                std::lock_guard<std::mutex> lock(engine_mutex_);
                if (order.id == 0) {
                    while (engine_.book().has_order(next_order_id_)) ++next_order_id_;
                    order.id = next_order_id_++;
                } else if (order.id >= next_order_id_ && order.id < std::numeric_limits<OrderId>::max()) {
                    next_order_id_ = order.id + 1;
                }

                const OrderResult result = engine_.submit_order(order);
                result_json = {
                    {"order_id", result.order_id},
                    {"status", std::string(atlas::to_string(result.status))},
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
            {
                std::lock_guard<std::mutex> lock(engine_mutex_);
                result = engine_.cancel_order(id);
            }
            send_json(response, {
                {"order_id", result.order_id}, {"success", result.success},
                {"status", std::string(atlas::to_string(result.status))},
                {"cancelled_price", result.cancelled_price},
                {"cancelled_quantity", result.cancelled_quantity},
                {"sequence", result.sequence}, {"message", result.message}
            }, result.success ? 200 : 404);
        });

    server_.Post("/api/sim/step", [this](const httplib::Request& request, httplib::Response& response) {
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
        std::size_t active_orders = 0;
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
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
                const OrderId id = next_order_id_++;
                const auto result = engine_.submit_order(OrderRequest{
                    id, side, market ? OrderType::Market : OrderType::Limit, price, quantity});
                trades_generated += result.trades.size();
            }
            active_orders = engine_.book().active_order_count();
        }
        send_json(response, {{"executed", count}, {"active_orders", active_orders},
                             {"trades_generated", trades_generated}});
    });

    server_.Post("/api/reset", [this](const httplib::Request& request, httplib::Response& response) {
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
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
            engine_.reset();
            if (seed_demo) engine_.seed_demo_book();
        }
        send_json(response, {{"reset", true}, {"seed_demo", seed_demo}});
    });

    server_.Post("/api/benchmark", [this](const httplib::Request& request, httplib::Response& response) {
        try {
            const json body = request.body.empty() ? json::object() : json::parse(request.body);
            const std::string profile_text = uppercase(body.value("profile", std::string("ALL")));
            const std::size_t order_count = body.value("order_count", std::size_t{100000});
            const bool compare_baseline = body.value("compare_baseline", true);
            if (order_count == 0 || order_count > 1'000'000) {
                send_json(response, error_json("order_count must be between 1 and 1000000"), 400);
                return;
            }
            std::vector<bench::BenchmarkResult> results;
            if (profile_text == "ALL") {
                results = benchmark_runner_.run_all(order_count, compare_baseline);
            } else {
                bench::WorkloadProfile profile{};
                if (!bench::parse_profile(profile_text, profile)) {
                    send_json(response, error_json("Unknown workload profile"), 400);
                    return;
                }
                results = benchmark_runner_.run(profile, order_count, compare_baseline);
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
}

bool HttpServer::run() {
    return server_.listen("127.0.0.1", port_);
}

void HttpServer::stop() noexcept {
    server_.stop();
}

} // namespace atlas::server

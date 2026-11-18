#include "basalt/trading/execution.hpp"

#include "basalt/core/matching_engine.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <optional>
#include <string>

namespace basalt::trading {
namespace {

using Json = nlohmann::json;
constexpr std::size_t MAX_FEE_SCHEDULE_BYTES = 64U * 1024U;

bool has_only_keys(const Json& value, const std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) return false;
    }
    return true;
}

bool has_required_keys(const Json& value, const std::initializer_list<std::string_view> required) {
    return value.is_object() && std::all_of(required.begin(), required.end(),
        [&value](const auto key) { return value.contains(key); });
}

std::optional<std::uint64_t> read_unsigned(const Json& value) {
    if (!value.is_number_unsigned()) return std::nullopt;
    try { return value.get<std::uint64_t>(); }
    catch (const std::exception&) { return std::nullopt; }
}

std::optional<std::int64_t> read_integer(const Json& value) {
    try {
        if (value.is_number_unsigned()) {
            const auto raw = value.get<std::uint64_t>();
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
            return static_cast<std::int64_t>(raw);
        }
        if (value.is_number_integer()) return value.get<std::int64_t>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<FeeComponentKind> parse_fee_component_kind(const std::string_view text) {
    if (text == "BROKERAGE") return FeeComponentKind::Brokerage;
    if (text == "SECURITIES_TRANSACTION_TAX") return FeeComponentKind::SecuritiesTransactionTax;
    if (text == "EXCHANGE_TRANSACTION") return FeeComponentKind::ExchangeTransaction;
    if (text == "SEBI_TURNOVER") return FeeComponentKind::SebiTurnover;
    if (text == "STAMP_DUTY") return FeeComponentKind::StampDuty;
    if (text == "GST") return FeeComponentKind::Gst;
    if (text == "OTHER") return FeeComponentKind::Other;
    return std::nullopt;
}

common::Result<std::int64_t> rate_fee(const std::int64_t amount,
                                      const std::uint32_t basis_points) {
    if (amount < 0 || basis_points > 10'000U)
        return common::Result<std::int64_t>::failure({"FEE_SCHEDULE_INVALID", "Fee amount or basis-point rate is invalid"});
    const auto whole = (amount / 10'000) * static_cast<std::int64_t>(basis_points);
    const auto remainder_product = (amount % 10'000) * static_cast<std::int64_t>(basis_points);
    const auto remainder_fee = (remainder_product + 9'999) / 10'000;
    return common::checked_add(whole, remainder_fee);
}

common::Result<std::int64_t> order_notional(const common::MicroUnits price,
                                            const common::Quantity quantity) {
    if (price <= 0 || quantity == 0 || quantity > static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))
        return common::Result<std::int64_t>::failure({"EXECUTION_VALUE", "Execution price or quantity is outside supported bounds"});
    return common::checked_multiply(price, static_cast<std::int64_t>(quantity));
}

common::Result<common::MicroUnits> slippage_amount(const common::MicroUnits tick,
                                                    const std::uint64_t ticks) {
    if (tick <= 0 || ticks > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return common::Result<common::MicroUnits>::failure({"EXECUTION_CONFIG", "Tick size or slippage ticks are invalid"});
    return common::checked_multiply(tick, static_cast<std::int64_t>(ticks));
}

common::Quantity participation(const common::Quantity quantity,
                               const std::uint32_t basis_points) {
    const auto whole = quantity / 10'000U;
    const auto remainder = quantity % 10'000U;
    return whole * basis_points + (remainder * basis_points) / 10'000U;
}

bool fresh(const ExecutionMarket& market, const ExecutionConfig& config) {
    if (market.event_sequence == 0 || market.quality != common::FeedQualityState::Ok || market.sequence_gap ||
        market.event_time_ns <= 0 || market.now_ns < market.event_time_ns) return false;
    return static_cast<std::uint64_t>(market.now_ns - market.event_time_ns) <= config.stale_after_ns;
}

common::Result<void> add_fill(ExecutionReport& report, const common::MicroUnits price,
                              const common::Quantity quantity, const OrderIntent& intent,
                              const ExecutionConfig& config) {
    if (quantity == 0) return common::Result<void>::success();
    const auto notional = order_notional(price, quantity);
    if (!notional) return common::Result<void>::failure(notional.error());
    const auto fees = FeeCalculator::calculate(intent.side, notional.value(), config.fee_schedule);
    if (!fees) return common::Result<void>::failure(fees.error());
    if (quantity > std::numeric_limits<common::Quantity>::max() - report.filled_quantity)
        return common::Result<void>::failure({"EXECUTION_VALUE", "Filled quantity overflow"});
    report.fills.push_back({price, quantity, fees.value().total_micro});
    report.filled_quantity += quantity;
    return common::Result<void>::success();
}

void finalize(ExecutionReport& report) {
    report.remaining_quantity = report.requested_quantity - report.filled_quantity;
    if (report.filled_quantity == 0) report.status = "UNFILLED";
    else if (report.remaining_quantity == 0) report.status = "FILLED";
    else report.status = "PARTIALLY_FILLED";
}

bool limit_allows(const OrderIntent& intent, const common::MicroUnits price) {
    if (intent.type == OrderType::Market) return true;
    if (!intent.limit_price_micro) return false;
    return intent.side == Side::Buy ? price <= *intent.limit_price_micro
                                    : price >= *intent.limit_price_micro;
}

common::Result<void> add_slippage(ExecutionReport& report, const common::MicroUnits unit_cost,
                                  const common::Quantity quantity) {
    if (unit_cost <= 0 || quantity == 0) return common::Result<void>::success();
    const auto cost = order_notional(unit_cost, quantity);
    if (!cost) return common::Result<void>::failure(cost.error());
    const auto total = common::checked_add(report.slippage_micro, cost.value());
    if (!total) return common::Result<void>::failure(total.error());
    report.slippage_micro = total.value();
    return common::Result<void>::success();
}

common::Result<ExecutionReport> unavailable(ExecutionReport report,
                                              const ExecutionConfig& config,
                                              std::string reason) {
    if (!config.leave_unfilled_working)
        return common::Result<ExecutionReport>::failure({std::move(reason), "Required paper execution data is unavailable, stale, or unsafe"});
    report.reason = std::move(reason);
    report.status = "WORKING";
    report.remaining_quantity = report.requested_quantity;
    return common::Result<ExecutionReport>::success(std::move(report));
}

} // namespace

common::Result<FeeSchedule> parse_fee_schedule_json(const std::string_view bytes) {
    if (bytes.empty() || bytes.size() > MAX_FEE_SCHEDULE_BYTES)
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_SIZE", "Fee schedule JSON must be between 1 byte and 64 KiB"});
    Json root;
    try {
        root = Json::parse(bytes);
    } catch (const std::exception&) {
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_JSON", "Fee schedule JSON is malformed"});
    }
    if (!has_only_keys(root, {"format_version", "schedule"}) ||
        !has_required_keys(root, {"format_version", "schedule"}) ||
        !root["format_version"].is_number_unsigned() || root["format_version"].get<std::uint64_t>() != 1U)
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_SCHEMA", "Fee schedule requires format_version 1 and a schedule object"});

    const auto& source = root["schedule"];
    const std::initializer_list<std::string_view> allowed_schedule_keys{"id", "version", "source", "venue", "segment", "product",
        "valid_from_ns", "valid_to_ns", "gst_bps", "components"};
    const std::initializer_list<std::string_view> required_schedule_keys{"id", "version", "source", "venue", "segment", "product",
        "valid_from_ns", "valid_to_ns", "gst_bps", "components"};
    if (!has_only_keys(source, allowed_schedule_keys) || !has_required_keys(source, required_schedule_keys))
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_SCHEMA", "Fee schedule has missing or unknown fields"});
    if (!source["id"].is_string() || !source["version"].is_number_unsigned() ||
        !source["source"].is_string() || !source["venue"].is_string() ||
        !source["segment"].is_string() || !source["product"].is_string() ||
        !source["valid_from_ns"].is_number_integer() || !source["valid_to_ns"].is_number_integer() ||
        !source["gst_bps"].is_number_unsigned() || !source["components"].is_array())
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_SCHEMA", "Fee schedule fields have invalid types"});

    const auto version = read_unsigned(source["version"]);
    const auto valid_from = read_integer(source["valid_from_ns"]);
    const auto valid_to = read_integer(source["valid_to_ns"]);
    const auto gst_basis_points = read_unsigned(source["gst_bps"]);
    if (!version || *version == 0 || *version > std::numeric_limits<std::uint32_t>::max() ||
        !valid_from || !valid_to || *valid_from < 0 || *valid_to < 0 ||
        !gst_basis_points || *gst_basis_points > 10'000U)
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_INVALID", "Fee schedule metadata is outside supported bounds"});

    FeeSchedule schedule;
    try {
        schedule.schedule_id = source["id"].get<std::string>();
        schedule.version = static_cast<std::uint32_t>(*version);
        schedule.source = source["source"].get<std::string>();
        schedule.venue = source["venue"].get<std::string>();
        schedule.segment = source["segment"].get<std::string>();
        schedule.product = source["product"].get<std::string>();
        schedule.valid_from_ns = *valid_from;
        schedule.valid_to_ns = *valid_to;
        schedule.gst_basis_points = static_cast<std::uint32_t>(*gst_basis_points);
    } catch (const std::exception&) {
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_SCHEMA", "Fee schedule metadata is outside supported bounds"});
    }
    if (schedule.schedule_id.empty() || schedule.schedule_id.size() > 128U || schedule.version == 0 ||
        schedule.source.empty() || schedule.source.size() > 256U || schedule.venue.empty() ||
        schedule.segment.empty() || schedule.product.empty() || schedule.valid_from_ns < 0 ||
        schedule.valid_to_ns < 0 || schedule.gst_basis_points > 10'000U ||
        (schedule.valid_from_ns > 0 && schedule.valid_to_ns > 0 &&
         schedule.valid_to_ns < schedule.valid_from_ns) || source["components"].size() > 32U)
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_INVALID", "Fee schedule metadata or bounds are invalid"});

    for (const auto& value : source["components"]) {
        const std::initializer_list<std::string_view> allowed_component_keys{"kind", "buy_bps", "sell_bps", "fixed_micro", "cap_micro", "gst_eligible"};
        const std::initializer_list<std::string_view> required_component_keys{"kind", "buy_bps", "sell_bps", "fixed_micro", "cap_micro", "gst_eligible"};
        if (!has_only_keys(value, allowed_component_keys) || !has_required_keys(value, required_component_keys) ||
            !value["kind"].is_string() || !value["buy_bps"].is_number_unsigned() ||
            !value["sell_bps"].is_number_unsigned() || !value["fixed_micro"].is_number_integer() ||
            !value["cap_micro"].is_number_integer() || !value["gst_eligible"].is_boolean())
            return common::Result<FeeSchedule>::failure({"FEE_COMPONENT_SCHEMA", "Fee component has missing, unknown or mistyped fields"});
        const auto kind = parse_fee_component_kind(value["kind"].get_ref<const std::string&>());
        if (!kind) return common::Result<FeeSchedule>::failure({"FEE_COMPONENT_KIND", "Fee component kind is unknown"});
        const auto buy = read_unsigned(value["buy_bps"]);
        const auto sell = read_unsigned(value["sell_bps"]);
        const auto fixed = read_integer(value["fixed_micro"]);
        const auto cap = read_integer(value["cap_micro"]);
        if (!buy || !sell || !fixed || !cap || *buy > 10'000U || *sell > 10'000U || *fixed < 0 || *cap < 0)
            return common::Result<FeeSchedule>::failure({"FEE_COMPONENT_INVALID", "Fee component value is outside supported bounds"});
        schedule.components.push_back({*kind, static_cast<std::uint32_t>(*buy), static_cast<std::uint32_t>(*sell),
                                        *fixed, *cap, value["gst_eligible"].get<bool>()});
    }
    const auto validated = FeeCalculator::calculate(Side::Buy, 0, schedule);
    if (!validated) return common::Result<FeeSchedule>::failure(validated.error());
    return common::Result<FeeSchedule>::success(std::move(schedule));
}

common::Result<FeeSchedule> load_fee_schedule_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_READ", "Could not inspect fee schedule file"});
    if (size == 0 || size > MAX_FEE_SCHEDULE_BYTES)
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_SIZE", "Fee schedule file must be between 1 byte and 64 KiB"});
    std::ifstream input(path, std::ios::binary);
    if (!input) return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_READ", "Could not open fee schedule file"});
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        return common::Result<FeeSchedule>::failure({"FEE_SCHEDULE_READ", "Could not read the bounded fee schedule file"});
    return parse_fee_schedule_json(bytes);
}

common::Result<FeeCalculation> FeeCalculator::calculate(const Side side,
                                                         const std::int64_t notional_micro,
                                                         const FeeSchedule& schedule) {
    if (notional_micro < 0 || schedule.schedule_id.empty() || schedule.source.empty() ||
        schedule.version == 0 || schedule.gst_basis_points > 10'000U ||
        (schedule.valid_from_ns > 0 && schedule.valid_to_ns > 0 && schedule.valid_to_ns < schedule.valid_from_ns))
        return common::Result<FeeCalculation>::failure({"FEE_SCHEDULE_INVALID", "Fee schedule identity, notional or GST rate is invalid"});
    FeeCalculation calculation;
    std::int64_t gst_eligible_total = 0;
    for (const auto& component : schedule.components) {
        const auto basis_points = side == Side::Buy ? component.buy_basis_points : component.sell_basis_points;
        if (component.fixed_micro < 0 || component.cap_micro < 0 || basis_points > 10'000U)
            return common::Result<FeeCalculation>::failure({"FEE_SCHEDULE_INVALID", "Fee component contains an invalid fixed amount, cap or rate"});
        const auto proportional = rate_fee(notional_micro, basis_points);
        if (!proportional) return common::Result<FeeCalculation>::failure(proportional.error());
        auto line = common::checked_add(proportional.value(), component.fixed_micro);
        if (!line) return common::Result<FeeCalculation>::failure(line.error());
        if (component.cap_micro > 0) line = common::Result<std::int64_t>::success(std::min(line.value(), component.cap_micro));
        calculation.lines.push_back({component.kind, line.value()});
        const auto total = common::checked_add(calculation.total_micro, line.value());
        if (!total) return common::Result<FeeCalculation>::failure(total.error());
        calculation.total_micro = total.value();
        if (component.gst_eligible) {
            const auto taxable = common::checked_add(gst_eligible_total, line.value());
            if (!taxable) return common::Result<FeeCalculation>::failure(taxable.error());
            gst_eligible_total = taxable.value();
        }
    }
    if (schedule.gst_basis_points > 0 && gst_eligible_total > 0) {
        const auto gst = rate_fee(gst_eligible_total, schedule.gst_basis_points);
        if (!gst) return common::Result<FeeCalculation>::failure(gst.error());
        const auto total = common::checked_add(calculation.total_micro, gst.value());
        if (!total) return common::Result<FeeCalculation>::failure(total.error());
        calculation.lines.push_back({FeeComponentKind::Gst, gst.value()});
        calculation.total_micro = total.value();
    }
    return common::Result<FeeCalculation>::success(std::move(calculation));
}

std::string_view to_string(const PaperExecutionModel model) noexcept {
    switch (model) {
    case PaperExecutionModel::ConservativeQuote: return "CONSERVATIVE_QUOTE";
    case PaperExecutionModel::BarApproximation: return "BAR_APPROXIMATION";
    case PaperExecutionModel::OrderBookReplay: return "ORDER_BOOK_REPLAY";
    case PaperExecutionModel::InternalClob: return "INTERNAL_CLOB";
    }
    return "UNKNOWN";
}

common::Result<ExecutionReport> PaperExecutionSimulator::execute(
    const OrderIntent& intent, const ExecutionMarket& market,
    const ExecutionConfig& config, basalt::MatchingEngine* internal_book) const {
    if (intent.client_order_id.empty() || intent.account_id == 0 || intent.instrument_id == 0 ||
        intent.quantity == 0 || intent.tick_size_micro <= 0 || intent.lot_size == 0 ||
        intent.quantity % intent.lot_size != 0 || config.model_version == 0 ||
        config.participation_bps > 10'000U || config.tick_size_micro != intent.tick_size_micro)
        return common::Result<ExecutionReport>::failure({"INVALID_ORDER", "Paper order or execution configuration is invalid"});
    if (intent.type == OrderType::Limit && (!intent.limit_price_micro || *intent.limit_price_micro <= 0 ||
                                             *intent.limit_price_micro % intent.tick_size_micro != 0))
        return common::Result<ExecutionReport>::failure({"INVALID_TICK", "Limit price is missing, non-positive or off tick"});

    ExecutionReport report;
    report.client_order_id = intent.client_order_id;
    report.account_id = intent.account_id;
    report.instrument_id = intent.instrument_id;
    report.strategy_id = intent.strategy_id;
    report.side = intent.side;
    report.model = config.model;
    report.model_version = config.model_version;
    report.requested_quantity = intent.quantity;
    report.remaining_quantity = intent.quantity;
    report.reference_event_sequence = market.event_sequence;
    report.source = market.source;
    report.quality = market.quality;
    report.latency_ns = config.simulated_latency_ns;
    report.fee_schedule_id = config.fee_schedule.schedule_id;
    report.fee_schedule_version = config.fee_schedule.version;
    report.fee_schedule_source = config.fee_schedule.source;

    if (config.model == PaperExecutionModel::InternalClob) {
        if (internal_book == nullptr || internal_book_instrument_ == 0 ||
            internal_book_instrument_ != intent.instrument_id || intent.order_id == 0)
            return common::Result<ExecutionReport>::failure({"INTERNAL_BOOK_REQUIRED", "Internal CLOB requires its instrument-bound simulated MatchingEngine and numeric order ID"});
        report.source = common::DataSourceType::Derived;
        report.quality = common::FeedQualityState::Ok;
        if (intent.type == OrderType::Limit && *intent.limit_price_micro > basalt::MAX_ORDER_PRICE)
            return common::Result<ExecutionReport>::failure({"INTERNAL_BOOK_LIMIT", "Limit price exceeds the legacy MatchingEngine range"});
        const auto max_notional = order_notional(basalt::MAX_ORDER_PRICE, intent.quantity);
        if (!max_notional) return common::Result<ExecutionReport>::failure(max_notional.error());
        const auto fee_preflight = FeeCalculator::calculate(intent.side, max_notional.value(), config.fee_schedule);
        if (!fee_preflight) return common::Result<ExecutionReport>::failure(fee_preflight.error());
        const auto side = intent.side == Side::Buy ? basalt::Side::Buy : basalt::Side::Sell;
        const auto type = intent.type == OrderType::Market ? basalt::OrderType::Market : basalt::OrderType::Limit;
        const auto price = intent.type == OrderType::Market ? 0 : *intent.limit_price_micro;
        const auto result = internal_book->submit_order({intent.order_id, side, type, price, intent.quantity});
        if (result.status == basalt::OrderStatus::Rejected)
            return common::Result<ExecutionReport>::failure({"INTERNAL_BOOK_REJECTED", result.message});
        for (const auto& trade : result.trades) {
            const auto fill = add_fill(report, trade.price, trade.quantity, intent, config);
            if (!fill) return common::Result<ExecutionReport>::failure(fill.error());
            report.reference_event_sequence = trade.sequence;
        }
        report.remaining_quantity = result.remaining_quantity;
        if (result.status == basalt::OrderStatus::Filled) report.status = "FILLED";
        else if (result.status == basalt::OrderStatus::PartiallyFilled) report.status = "PARTIALLY_FILLED";
        else if (result.status == basalt::OrderStatus::Cancelled) report.status = "CANCELLED";
        else report.status = "WORKING";
        report.reason = "SIMULATED_INTERNAL_CLOB";
        return common::Result<ExecutionReport>::success(std::move(report));
    }

    if (!fresh(market, config)) return unavailable(std::move(report), config, "STALE_DATA");
    const auto slippage = slippage_amount(config.tick_size_micro, config.slippage_ticks);
    if (!slippage) return common::Result<ExecutionReport>::failure(slippage.error());

    if (config.model == PaperExecutionModel::ConservativeQuote) {
        const auto touch = intent.side == Side::Buy ? market.ask_micro : market.bid_micro;
        const auto visible = intent.side == Side::Buy ? market.ask_quantity : market.bid_quantity;
        if (!touch || !visible) return unavailable(std::move(report), config, "DATA_UNAVAILABLE");
        if (*touch <= 0 || *touch % intent.tick_size_micro != 0)
            return common::Result<ExecutionReport>::failure({"INVALID_TICK", "Observed quote is non-positive or off tick"});
        if (!limit_allows(intent, *touch)) {
            report.reason = "LIMIT_NOT_MARKETABLE";
            report.status = config.leave_unfilled_working ? "WORKING" : "UNFILLED";
            return common::Result<ExecutionReport>::success(std::move(report));
        }
        auto fill_price = *touch;
        if (intent.type == OrderType::Market && intent.side == Side::Buy) {
            const auto price = common::checked_add(fill_price, slippage.value());
            if (!price) return common::Result<ExecutionReport>::failure(price.error());
            fill_price = price.value();
        } else if (intent.type == OrderType::Market && intent.side == Side::Sell) {
            const auto price = common::checked_add(fill_price, -slippage.value());
            if (!price || price.value() <= 0) return common::Result<ExecutionReport>::failure({"EXECUTION_VALUE", "Adverse sell slippage produced an invalid price"});
            fill_price = price.value();
        }
        const auto fill_quantity = std::min(intent.quantity, participation(*visible, config.participation_bps));
        const auto added = add_fill(report, fill_price, fill_quantity, intent, config);
        if (!added) return common::Result<ExecutionReport>::failure(added.error());
        if (intent.type == OrderType::Market && config.slippage_ticks > 0) {
            const auto cost = add_slippage(report, slippage.value(), fill_quantity);
            if (!cost) return common::Result<ExecutionReport>::failure(cost.error());
        }
        report.reason = fill_quantity == 0 ? "NO_VISIBLE_LIQUIDITY" : "SIMULATED_QUOTE_TOUCH";
        finalize(report);
        return common::Result<ExecutionReport>::success(std::move(report));
    }

    if (config.model == PaperExecutionModel::BarApproximation) {
        if (!market.next_bar || market.signal_bar_close_ns <= 0 ||
            market.next_bar_open_ns <= market.signal_bar_close_ns ||
            market.half_spread_micro < 0) return unavailable(std::move(report), config, "NEXT_BAR_UNAVAILABLE");
        const auto& bar = *market.next_bar;
        if (bar.open_micro <= 0 || bar.high_micro < bar.open_micro || bar.high_micro < bar.close_micro ||
            bar.low_micro <= 0 || bar.low_micro > bar.open_micro || bar.low_micro > bar.close_micro)
            return common::Result<ExecutionReport>::failure({"INVALID_BAR", "Next-bar OHLC values are inconsistent"});
        common::MicroUnits fill_price{};
        if (intent.type == OrderType::Market) {
            const auto half_spread = intent.side == Side::Buy ? market.half_spread_micro : -market.half_spread_micro;
            const auto side_price = common::checked_add(bar.open_micro, half_spread);
            if (!side_price) return common::Result<ExecutionReport>::failure(side_price.error());
            const auto cost = intent.side == Side::Buy ? common::checked_add(side_price.value(), slippage.value())
                                                       : common::checked_add(side_price.value(), -slippage.value());
            if (!cost || cost.value() <= 0) return common::Result<ExecutionReport>::failure({"EXECUTION_VALUE", "Next-bar adverse price is invalid"});
            fill_price = cost.value();
            report.latency_ns = static_cast<std::uint64_t>(market.next_bar_open_ns - market.signal_bar_close_ns);
        } else {
            const bool breached = intent.side == Side::Buy ? bar.low_micro < *intent.limit_price_micro
                                                           : bar.high_micro > *intent.limit_price_micro;
            if (!breached) {
                report.reason = "LIMIT_NOT_BREACHED";
                report.status = config.leave_unfilled_working ? "WORKING" : "UNFILLED";
                return common::Result<ExecutionReport>::success(std::move(report));
            }
            fill_price = *intent.limit_price_micro; // Pessimistic price when OHLC cannot establish path order.
        }
        const auto fill_quantity = std::min(intent.quantity, participation(bar.volume, config.participation_bps));
        if (intent.type == OrderType::Market) {
            const auto unit_cost = common::checked_add(market.half_spread_micro, slippage.value());
            if (!unit_cost) return common::Result<ExecutionReport>::failure(unit_cost.error());
            const auto slippage_cost = add_slippage(report, unit_cost.value(), fill_quantity);
            if (!slippage_cost) return common::Result<ExecutionReport>::failure(slippage_cost.error());
        }
        const auto added = add_fill(report, fill_price, fill_quantity, intent, config);
        if (!added) return common::Result<ExecutionReport>::failure(added.error());
        report.reason = "SIMULATED_NEXT_BAR_APPROXIMATION";
        finalize(report);
        return common::Result<ExecutionReport>::success(std::move(report));
    }

    if (config.model == PaperExecutionModel::OrderBookReplay) {
        if (market.sequence_gap || market.now_ns < market.event_time_ns || market.event_time_ns <= 0)
            return unavailable(std::move(report), config, "SEQUENCE_GAP");
        if (intent.created_at_ns <= 0 || config.simulated_latency_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            intent.created_at_ns > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(config.simulated_latency_ns) ||
            market.event_time_ns > intent.created_at_ns + static_cast<std::int64_t>(config.simulated_latency_ns))
            return unavailable(std::move(report), config, "DEPTH_NOT_AVAILABLE_AT_ARRIVAL");
        const auto& levels = intent.side == Side::Buy ? market.asks : market.bids;
        if (levels.empty()) return unavailable(std::move(report), config, "DEPTH_NOT_AVAILABLE");
        common::MicroUnits previous = intent.side == Side::Buy ? 0 : std::numeric_limits<common::MicroUnits>::max();
        common::MicroUnits best = levels.front().price_micro;
        common::Quantity remaining = intent.quantity;
        for (const auto& level : levels) {
            if (level.price_micro <= 0 || level.price_micro % intent.tick_size_micro != 0 || level.quantity == 0 ||
                (intent.side == Side::Buy ? (previous != 0 && level.price_micro < previous)
                                          : (level.price_micro > previous)))
                return common::Result<ExecutionReport>::failure({"INVALID_DEPTH", "Recorded depth is invalid or not price ordered"});
            previous = level.price_micro;
            if (!limit_allows(intent, level.price_micro)) break;
            const auto available = participation(level.quantity, config.participation_bps);
            const auto quantity = std::min(remaining, available);
            if (quantity == 0) continue;
            const auto added = add_fill(report, level.price_micro, quantity, intent, config);
            if (!added) return common::Result<ExecutionReport>::failure(added.error());
            const auto adverse = intent.side == Side::Buy ? std::max<common::MicroUnits>(0, level.price_micro - best)
                                                          : std::max<common::MicroUnits>(0, best - level.price_micro);
            const auto slip = add_slippage(report, adverse, quantity);
            if (!slip) return common::Result<ExecutionReport>::failure(slip.error());
            remaining -= quantity;
            if (remaining == 0) break;
        }
        report.reason = "SIMULATED_RECORDED_DEPTH_SWEEP";
        finalize(report);
        return common::Result<ExecutionReport>::success(std::move(report));
    }

    return common::Result<ExecutionReport>::failure({"EXECUTION_MODEL_UNSUPPORTED", "Unsupported or invalid paper execution model"});
}

} // namespace basalt::trading

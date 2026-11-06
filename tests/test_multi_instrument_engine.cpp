#include "basalt/core/multi_instrument_engine.hpp"

#include <gtest/gtest.h>

namespace basalt {
namespace {

AdvancedOrderRequest order(const OrderId id, const common::InstrumentId instrument,
                           const Side side, const AdvancedOrderType type,
                           const Price price, const Quantity quantity,
                           std::string account = {}, std::string participant = {}) {
    AdvancedOrderRequest request{};
    request.id = id;
    request.instrument_id = instrument;
    request.side = side;
    request.type = type;
    request.price = price;
    request.quantity = quantity;
    request.account_id = std::move(account);
    request.participant_id = std::move(participant);
    return request;
}

void expect_invariants(const MultiInstrumentEngine& engine) {
    std::string error;
    EXPECT_TRUE(engine.verify_invariants(&error)) << error;
}

} // namespace

TEST(MultiInstrumentEngine, BooksAreIsolatedAndCannotCrossInstruments) {
    MultiInstrumentEngine engine;
    ASSERT_EQ(engine.submit_order(order(1, 101, Side::Sell, AdvancedOrderType::Limit, 100, 5)).status,
              OrderStatus::New);
    const auto other = engine.submit_order(order(2, 202, Side::Buy, AdvancedOrderType::Market, 0, 5));
    EXPECT_EQ(other.status, OrderStatus::Cancelled);
    EXPECT_TRUE(other.trades.empty());
    EXPECT_EQ(engine.get_snapshot(101).asks.front().total_volume, 5U);
    EXPECT_TRUE(engine.get_snapshot(202).asks.empty());
    EXPECT_EQ(engine.instruments(), (std::vector<common::InstrumentId>{101, 202}));
    expect_invariants(engine);
}

TEST(MultiInstrumentEngine, IocFillsAvailableLiquidityAndCancelsRemainder) {
    MultiInstrumentEngine engine;
    ASSERT_EQ(engine.submit_order(order(1, 7, Side::Sell, AdvancedOrderType::Limit, 100, 3)).status,
              OrderStatus::New);
    auto ioc = order(2, 7, Side::Buy, AdvancedOrderType::Ioc, 101, 5);
    const auto result = engine.submit_order(ioc);
    EXPECT_EQ(result.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(result.filled_quantity, 3U);
    EXPECT_EQ(result.remaining_quantity, 2U);
    EXPECT_EQ(engine.active_order_count(), 0U);
    expect_invariants(engine);
}

TEST(MultiInstrumentEngine, FokFailureIsNonMutatingAndSuccessSweepsLevels) {
    MultiInstrumentEngine engine;
    ASSERT_EQ(engine.submit_order(order(1, 7, Side::Sell, AdvancedOrderType::Limit, 100, 2)).status,
              OrderStatus::New);
    ASSERT_EQ(engine.submit_order(order(2, 7, Side::Sell, AdvancedOrderType::Limit, 101, 2)).status,
              OrderStatus::New);
    const auto before = engine.get_snapshot(7);
    auto insufficient = order(3, 7, Side::Buy, AdvancedOrderType::Fok, 101, 5);
    const auto failed = engine.submit_order(insufficient);
    EXPECT_EQ(failed.status, OrderStatus::Cancelled);
    EXPECT_EQ(failed.sequence, 0U);
    EXPECT_EQ(engine.get_snapshot(7), before);
    auto enough = order(4, 7, Side::Buy, AdvancedOrderType::Fok, 101, 4);
    const auto filled = engine.submit_order(enough);
    ASSERT_EQ(filled.status, OrderStatus::Filled);
    ASSERT_EQ(filled.trades.size(), 2U);
    EXPECT_EQ(filled.trades[0].trade.trade_id, 1U);
    EXPECT_EQ(filled.trades[1].trade.trade_id, 2U);
    EXPECT_TRUE(engine.get_snapshot(7).asks.empty());
    expect_invariants(engine);
}

TEST(MultiInstrumentEngine, PostOnlyRejectsCrossingOrdersWithoutTrading) {
    MultiInstrumentEngine engine;
    ASSERT_EQ(engine.submit_order(order(1, 3, Side::Sell, AdvancedOrderType::Limit, 100, 1)).status,
              OrderStatus::New);
    const auto result = engine.submit_order(order(2, 3, Side::Buy, AdvancedOrderType::PostOnly, 100, 1));
    EXPECT_EQ(result.status, OrderStatus::Rejected);
    EXPECT_TRUE(result.trades.empty());
    EXPECT_EQ(engine.get_snapshot(3).asks.front().total_volume, 1U);
    expect_invariants(engine);
}

TEST(MultiInstrumentEngine, StopMarketAndStopLimitTriggerFromReferenceTrades) {
    MultiInstrumentEngine market_engine;
    ASSERT_EQ(market_engine.submit_order(order(1, 9, Side::Sell, AdvancedOrderType::Limit, 100, 2)).status,
              OrderStatus::New);
    auto stop_market = order(2, 9, Side::Buy, AdvancedOrderType::StopMarket, 0, 1);
    stop_market.stop_price = 105;
    ASSERT_EQ(market_engine.submit_order(stop_market).status, OrderStatus::New);
    EXPECT_TRUE(market_engine.process_reference_trade(9, 104, 10).value().empty());
    const auto triggered = market_engine.process_reference_trade(9, 105, 11);
    ASSERT_TRUE(triggered);
    ASSERT_EQ(triggered.value().size(), 1U);
    EXPECT_EQ(triggered.value()[0].status, OrderStatus::Filled);
    EXPECT_EQ(triggered.value()[0].trades[0].trade.price, 100);
    expect_invariants(market_engine);

    MultiInstrumentEngine limit_engine;
    auto stop_limit = order(3, 10, Side::Buy, AdvancedOrderType::StopLimit, 99, 1);
    stop_limit.stop_price = 105;
    ASSERT_EQ(limit_engine.submit_order(stop_limit).status, OrderStatus::New);
    const auto activated = limit_engine.process_reference_trade(10, 105, 12);
    ASSERT_TRUE(activated);
    ASSERT_EQ(activated.value().size(), 1U);
    EXPECT_EQ(activated.value()[0].status, OrderStatus::New);
    EXPECT_EQ(limit_engine.get_snapshot(10).bids.front().price, 99);
    expect_invariants(limit_engine);
}

TEST(MultiInstrumentEngine, ModifyDecreaseKeepsPriorityWhileIncreaseLosesPriority) {
    MultiInstrumentEngine decrease_engine;
    ASSERT_EQ(decrease_engine.submit_order(order(1, 11, Side::Buy, AdvancedOrderType::Limit, 100, 10)).status,
              OrderStatus::New);
    ASSERT_EQ(decrease_engine.submit_order(order(2, 11, Side::Buy, AdvancedOrderType::Limit, 100, 10)).status,
              OrderStatus::New);
    const auto decrease = decrease_engine.modify_order({11, 1, 100, 5});
    EXPECT_TRUE(decrease.success);
    EXPECT_FALSE(decrease.priority_reset);
    auto sell = order(3, 11, Side::Sell, AdvancedOrderType::Ioc, 100, 7);
    const auto first = decrease_engine.submit_order(sell);
    ASSERT_EQ(first.trades.size(), 2U);
    EXPECT_EQ(first.trades[0].trade.resting_order_id, 1U);
    EXPECT_EQ(first.trades[1].trade.resting_order_id, 2U);
    expect_invariants(decrease_engine);

    MultiInstrumentEngine increase_engine;
    ASSERT_EQ(increase_engine.submit_order(order(4, 12, Side::Buy, AdvancedOrderType::Limit, 100, 5)).status,
              OrderStatus::New);
    ASSERT_EQ(increase_engine.submit_order(order(5, 12, Side::Buy, AdvancedOrderType::Limit, 100, 5)).status,
              OrderStatus::New);
    const auto increase = increase_engine.modify_order({12, 4, 100, 8});
    EXPECT_TRUE(increase.success);
    EXPECT_TRUE(increase.priority_reset);
    const auto second = increase_engine.submit_order(order(6, 12, Side::Sell, AdvancedOrderType::Ioc, 100, 3));
    ASSERT_EQ(second.trades.size(), 1U);
    EXPECT_EQ(second.trades[0].trade.resting_order_id, 5U);
    expect_invariants(increase_engine);
}

TEST(MultiInstrumentEngine, PriceChangeLosesPriorityAndCrossingModifyIsRejected) {
    MultiInstrumentEngine engine;
    ASSERT_EQ(engine.submit_order(order(1, 13, Side::Buy, AdvancedOrderType::Limit, 100, 2)).status,
              OrderStatus::New);
    ASSERT_EQ(engine.submit_order(order(2, 13, Side::Buy, AdvancedOrderType::Limit, 100, 2)).status,
              OrderStatus::New);
    ASSERT_EQ(engine.submit_order(order(3, 13, Side::Buy, AdvancedOrderType::Limit, 99, 2)).status,
              OrderStatus::New);
    const auto moved = engine.modify_order({13, 1, 99, 2});
    EXPECT_TRUE(moved.success);
    EXPECT_TRUE(moved.priority_reset);
    const auto sell = engine.submit_order(order(4, 13, Side::Sell, AdvancedOrderType::Ioc, 99, 5));
    ASSERT_EQ(sell.status, OrderStatus::Filled);
    ASSERT_EQ(sell.trades.size(), 3U);
    EXPECT_EQ(sell.trades[0].trade.resting_order_id, 2U);
    EXPECT_EQ(sell.trades[1].trade.resting_order_id, 3U);
    EXPECT_EQ(sell.trades[2].trade.resting_order_id, 1U);
    ASSERT_EQ(engine.submit_order(order(5, 13, Side::Sell, AdvancedOrderType::Limit, 110, 1)).status,
              OrderStatus::New);
    ASSERT_EQ(engine.submit_order(order(6, 13, Side::Buy, AdvancedOrderType::Limit, 100, 1)).status,
              OrderStatus::New);
    const auto cross = engine.modify_order({13, 6, 110, 1});
    EXPECT_FALSE(cross.success);
    expect_invariants(engine);
}

TEST(MultiInstrumentEngine, AppliesAllSelfTradePreventionPolicies) {
    for (const auto policy : {STPPolicy::CancelNewest, STPPolicy::CancelOldest, STPPolicy::CancelBoth}) {
        MultiInstrumentEngine engine;
        auto resting = order(1, 14, Side::Buy, AdvancedOrderType::Limit, 100, 2, "acct", "desk");
        ASSERT_EQ(engine.submit_order(resting).status, OrderStatus::New);
        const bool participant_match = policy == STPPolicy::CancelNewest;
        auto incoming = order(2, 14, Side::Sell, AdvancedOrderType::Limit, 100, 1,
                              participant_match ? "other-account" : "acct",
                              participant_match ? "desk" : "other-desk");
        incoming.stp_policy = policy;
        const auto result = engine.submit_order(incoming);
        EXPECT_TRUE(result.trades.empty());
        if (policy == STPPolicy::CancelNewest) {
            EXPECT_EQ(result.remaining_quantity, 1U);
            EXPECT_EQ(engine.get_snapshot(14).bids.size(), 1U);
            EXPECT_EQ(engine.active_order_count(), 1U);
        } else if (policy == STPPolicy::CancelOldest) {
            EXPECT_EQ(engine.get_snapshot(14).bids.size(), 0U);
            EXPECT_EQ(engine.get_snapshot(14).asks.size(), 1U);
            EXPECT_EQ(engine.active_order_count(), 1U);
        } else {
            EXPECT_EQ(engine.active_order_count(), 0U);
        }
        expect_invariants(engine);
    }
}

TEST(MultiInstrumentEngine, CancelsPendingStopsAndRejectsWrongInstrumentCancellation) {
    MultiInstrumentEngine engine;
    auto stop = order(1, 15, Side::Sell, AdvancedOrderType::StopMarket, 0, 2);
    stop.stop_price = 90;
    ASSERT_EQ(engine.submit_order(stop).status, OrderStatus::New);
    EXPECT_FALSE(engine.cancel_order(16, 1).success);
    const auto cancelled = engine.cancel_order(15, 1);
    EXPECT_TRUE(cancelled.success);
    EXPECT_EQ(cancelled.cancelled_quantity, 2U);
    EXPECT_EQ(engine.active_order_count(), 0U);
    expect_invariants(engine);
}

} // namespace basalt

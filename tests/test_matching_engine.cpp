#include "basalt/core/matching_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace basalt;

OrderResult submit(MatchingEngine& engine, const OrderId id, const Side side,
                   const Quantity quantity, const Price price,
                   const OrderType type = OrderType::Limit) {
    return engine.submit_order(OrderRequest{id, side, type, price, quantity});
}

void expect_invariants(const MatchingEngine& engine) {
    std::string error;
    EXPECT_TRUE(engine.book().verify_invariants(&error)) << error;
}

std::vector<Trade> run_replay(MatchingEngine& engine, std::mt19937_64& rng,
                              const std::size_t count) {
    std::vector<Trade> all_trades;
    OrderId next_id = 1;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t choice = rng() % 100;
        if (choice < 82) {
            const Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            const bool market = (rng() % 100) < 8;
            const Price price = static_cast<Price>(9900 + (rng() % 201));
            const Quantity quantity = static_cast<Quantity>(1 + (rng() % 100));
            const OrderResult result = submit(engine, next_id++, side, quantity, price,
                                              market ? OrderType::Market : OrderType::Limit);
            all_trades.insert(all_trades.end(), result.trades.begin(), result.trades.end());
        } else {
            const OrderId cancel_id = next_id > 1 ? 1 + (rng() % (next_id - 1)) : 1;
            (void)engine.cancel_order(cancel_id);
        }
    }
    return all_trades;
}

} // namespace

TEST(MatchingEngine, EmptyBookBuyResting) {
    MatchingEngine engine;
    const auto result = submit(engine, 1, Side::Buy, 50, 10000);
    EXPECT_EQ(result.status, OrderStatus::New);
    EXPECT_TRUE(result.trades.empty());
    EXPECT_EQ(engine.get_snapshot().best_bid, 10000);
    EXPECT_EQ(engine.get_snapshot().best_bid_volume, 50);
    expect_invariants(engine);
}

TEST(MatchingEngine, EmptyBookSellResting) {
    MatchingEngine engine;
    const auto result = submit(engine, 1, Side::Sell, 50, 10100);
    EXPECT_EQ(result.status, OrderStatus::New);
    EXPECT_TRUE(result.trades.empty());
    EXPECT_EQ(engine.get_snapshot().best_ask, 10100);
    EXPECT_EQ(engine.get_snapshot().best_ask_volume, 50);
    expect_invariants(engine);
}

TEST(MatchingEngine, IncompatibleOrdersDoNotMatch) {
    MatchingEngine engine;
    const auto bid = submit(engine, 1, Side::Buy, 100, 10000);
    const auto ask = submit(engine, 2, Side::Sell, 100, 10050);
    EXPECT_EQ(bid.status, OrderStatus::New);
    EXPECT_EQ(ask.status, OrderStatus::New);
    EXPECT_TRUE(ask.trades.empty());
    EXPECT_EQ(engine.get_snapshot().spread, 50);
    expect_invariants(engine);
}

TEST(MatchingEngine, ExactCrossCompleteFill) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Buy, 100, 10000);
    const auto result = submit(engine, 2, Side::Sell, 100, 10000);
    EXPECT_EQ(result.status, OrderStatus::Filled);
    ASSERT_EQ(result.trades.size(), 1U);
    EXPECT_EQ(result.trades[0].quantity, 100U);
    EXPECT_EQ(result.trades[0].price, 10000);
    EXPECT_EQ(engine.book().active_order_count(), 0U);
    EXPECT_EQ(engine.stats().total_trades_executed, 1U);
    expect_invariants(engine);
}

TEST(MatchingEngine, ExecutionAtRestingOrderPrice) {
    {
        MatchingEngine engine;
        (void)submit(engine, 1, Side::Sell, 100, 10000);
        const auto result = submit(engine, 2, Side::Buy, 100, 10500);
        ASSERT_EQ(result.trades.size(), 1U);
        EXPECT_EQ(result.trades[0].price, 10000);
        expect_invariants(engine);
    }
    {
        MatchingEngine engine;
        (void)submit(engine, 1, Side::Buy, 100, 10500);
        const auto result = submit(engine, 2, Side::Sell, 100, 10000);
        ASSERT_EQ(result.trades.size(), 1U);
        EXPECT_EQ(result.trades[0].price, 10500);
        expect_invariants(engine);
    }
}

TEST(MatchingEngine, PricePriorityEnforcement) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Sell, 100, 10100);
    (void)submit(engine, 2, Side::Sell, 100, 9900);
    (void)submit(engine, 3, Side::Sell, 100, 10000);
    const auto result = submit(engine, 4, Side::Buy, 250, 10100);
    ASSERT_EQ(result.trades.size(), 3U);
    EXPECT_EQ(result.trades[0].price, 9900);
    EXPECT_EQ(result.trades[1].price, 10000);
    EXPECT_EQ(result.trades[2].price, 10100);
    EXPECT_EQ(result.trades[2].quantity, 50U);
    expect_invariants(engine);
}

TEST(MatchingEngine, TimePriorityFIFOEnforcement) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Buy, 50, 10000);
    (void)submit(engine, 2, Side::Buy, 50, 10000);
    (void)submit(engine, 3, Side::Buy, 50, 10000);
    const auto result = submit(engine, 4, Side::Sell, 120, 10000);
    ASSERT_EQ(result.trades.size(), 3U);
    EXPECT_EQ(result.trades[0].resting_order_id, 1U);
    EXPECT_EQ(result.trades[1].resting_order_id, 2U);
    EXPECT_EQ(result.trades[2].resting_order_id, 3U);
    EXPECT_EQ(result.trades[2].quantity, 20U);
    ASSERT_NE(engine.book().get_order(3), nullptr);
    EXPECT_EQ(engine.book().get_order(3)->remaining_quantity, 30U);
    expect_invariants(engine);
}

TEST(MatchingEngine, PartialFillPassiveResting) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Sell, 100, 10000);
    const auto result = submit(engine, 2, Side::Buy, 40, 10000);
    ASSERT_EQ(result.trades.size(), 1U);
    EXPECT_EQ(result.trades[0].quantity, 40U);
    ASSERT_NE(engine.book().get_order(1), nullptr);
    EXPECT_EQ(engine.book().get_order(1)->remaining_quantity, 60U);
    expect_invariants(engine);
}

TEST(MatchingEngine, PartialFillAggressiveRestsRemainder) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Sell, 40, 10000);
    const auto result = submit(engine, 2, Side::Buy, 100, 10000);
    EXPECT_EQ(result.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(result.filled_quantity, 40U);
    EXPECT_EQ(result.remaining_quantity, 60U);
    EXPECT_FALSE(engine.get_snapshot().has_best_ask);
    EXPECT_EQ(engine.get_snapshot().best_bid, 10000);
    ASSERT_NE(engine.book().get_order(2), nullptr);
    EXPECT_EQ(engine.book().get_order(2)->remaining_quantity, 60U);
    expect_invariants(engine);
}

TEST(MatchingEngine, MultiLevelSweepSingleOrder) {
    MatchingEngine engine;
    for (OrderId i = 0; i < 5; ++i) {
        (void)submit(engine, i + 1, Side::Sell, 50, 10000 + static_cast<Price>(i * 10));
    }
    const auto result = submit(engine, 6, Side::Buy, 220, 10050);
    ASSERT_EQ(result.trades.size(), 5U);
    EXPECT_EQ(engine.book().ask_level_count(), 1U);
    EXPECT_EQ(engine.get_snapshot().best_ask, 10040);
    EXPECT_EQ(engine.get_snapshot().best_ask_volume, 30U);
    expect_invariants(engine);
}

TEST(MatchingEngine, CancelExistingRestingOrder) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Buy, 50, 10000);
    (void)submit(engine, 2, Side::Buy, 60, 10000);
    (void)submit(engine, 3, Side::Buy, 70, 10000);
    const auto cancelled = engine.cancel_order(2);
    EXPECT_TRUE(cancelled.success);
    EXPECT_EQ(cancelled.cancelled_quantity, 60U);
    const auto snapshot = engine.get_snapshot();
    ASSERT_EQ(snapshot.bids.size(), 1U);
    EXPECT_EQ(snapshot.bids[0].total_volume, 120U);
    ASSERT_EQ(snapshot.resting_orders.size(), 2U);
    EXPECT_EQ(snapshot.resting_orders[0].id, 1U);
    EXPECT_EQ(snapshot.resting_orders[1].id, 3U);
    EXPECT_TRUE(engine.cancel_order(1).success);
    EXPECT_TRUE(engine.cancel_order(3).success);
    EXPECT_EQ(engine.book().bid_level_count(), 0U);
    expect_invariants(engine);
}

TEST(MatchingEngine, CancelNonExistentOrAlreadyFilledOrder) {
    MatchingEngine engine;
    const auto missing = engine.cancel_order(9999);
    EXPECT_FALSE(missing.success);
    (void)submit(engine, 1, Side::Buy, 10, 10000);
    (void)submit(engine, 2, Side::Sell, 10, 10000);
    const auto filled = engine.cancel_order(1);
    EXPECT_FALSE(filled.success);
    EXPECT_EQ(engine.book().active_order_count(), 0U);
    expect_invariants(engine);
}

TEST(MatchingEngine, MarketOrderFullFillAcrossLevels) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Sell, 50, 10000);
    (void)submit(engine, 2, Side::Sell, 50, 10100);
    const auto result = submit(engine, 3, Side::Buy, 80, 0, OrderType::Market);
    EXPECT_EQ(result.status, OrderStatus::Filled);
    ASSERT_EQ(result.trades.size(), 2U);
    EXPECT_EQ(result.trades[0].quantity, 50U);
    EXPECT_EQ(result.trades[1].quantity, 30U);
    EXPECT_EQ(engine.book().get_order(2)->remaining_quantity, 20U);
    expect_invariants(engine);
}

TEST(MatchingEngine, MarketOrderInsufficientLiquidityCancelsRemainder) {
    MatchingEngine engine;
    (void)submit(engine, 1, Side::Sell, 40, 10000);
    const auto result = submit(engine, 2, Side::Buy, 100, 0, OrderType::Market);
    EXPECT_EQ(result.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(result.filled_quantity, 40U);
    EXPECT_EQ(result.remaining_quantity, 60U);
    EXPECT_EQ(engine.book().active_order_count(), 0U);
    expect_invariants(engine);
}

TEST(MatchingEngine, MarketOrderOnEmptyBookCancelsImmediately) {
    MatchingEngine engine;
    const auto buy = submit(engine, 1, Side::Buy, 10, 0, OrderType::Market);
    const auto sell = submit(engine, 2, Side::Sell, 10, 0, OrderType::Market);
    EXPECT_EQ(buy.status, OrderStatus::Cancelled);
    EXPECT_EQ(sell.status, OrderStatus::Cancelled);
    EXPECT_TRUE(buy.trades.empty());
    EXPECT_TRUE(sell.trades.empty());
    EXPECT_EQ(engine.book().active_order_count(), 0U);
    expect_invariants(engine);
}

TEST(MatchingEngine, ValidationRejectsInvalidOrders) {
    MatchingEngine engine;
    EXPECT_EQ(submit(engine, 1, Side::Buy, 0, 10000).status, OrderStatus::Rejected);
    EXPECT_EQ(submit(engine, 2, Side::Buy, 1, 0).status, OrderStatus::Rejected);
    EXPECT_EQ(submit(engine, 3, Side::Buy, 1, -1).status, OrderStatus::Rejected);
    EXPECT_EQ(submit(engine, 0, Side::Buy, 1, 10000).status, OrderStatus::Rejected);
    EXPECT_EQ(submit(engine, 4, Side::Buy, MAX_ORDER_QUANTITY + 1, 10000).status,
              OrderStatus::Rejected);
    EXPECT_EQ(submit(engine, 5, Side::Buy, 1, MAX_ORDER_PRICE + 1).status,
              OrderStatus::Rejected);
    (void)submit(engine, 6, Side::Buy, 1, 10000);
    EXPECT_EQ(submit(engine, 6, Side::Sell, 1, 10100).status, OrderStatus::Rejected);
    EXPECT_EQ(engine.stats().total_orders_rejected, 7U);
    EXPECT_EQ(engine.book().active_order_count(), 1U);
    expect_invariants(engine);
}

TEST(MatchingEngine, QuantityReconciliationAudit) {
    struct Ledger {
        Quantity initial{0};
        Quantity traded{0};
        Quantity cancelled{0};
    };
    MatchingEngine engine;
    std::unordered_map<OrderId, Ledger> ledger;
    std::vector<OrderId> ids;
    std::mt19937_64 rng(1234567);
    OrderId next_id = 1;

    for (std::size_t i = 0; i < 1000; ++i) {
        if (!ids.empty() && (rng() % 100) < 28) {
            const OrderId id = ids[static_cast<std::size_t>(rng() % ids.size())];
            const auto cancelled = engine.cancel_order(id);
            if (cancelled.success) ledger[id].cancelled += cancelled.cancelled_quantity;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        } else {
            const Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderType type = (rng() % 100 < 4) ? OrderType::Market : OrderType::Limit;
            const Quantity quantity = 1 + rng() % 50;
            const Price price = static_cast<Price>(9800 + rng() % 401);
            const OrderId id = next_id++;
            const auto result = submit(engine, id, side, quantity, price, type);
            ASSERT_NE(result.status, OrderStatus::Rejected);
            ledger[id].initial = quantity;
            for (const Trade& trade : result.trades) {
                ledger[trade.buy_order_id].traded += trade.quantity;
                ledger[trade.sell_order_id].traded += trade.quantity;
            }
            if (type == OrderType::Market) {
                ledger[id].cancelled += result.remaining_quantity;
            }
            if (result.status == OrderStatus::New || result.status == OrderStatus::PartiallyFilled) {
                if (engine.book().has_order(id)) ids.push_back(id);
            }
        }
    }

    for (const auto& [id, entry] : ledger) {
        const Order* resting = engine.book().get_order(id);
        const Quantity resting_quantity = resting == nullptr ? 0 : resting->remaining_quantity;
        EXPECT_EQ(entry.initial, entry.traded + entry.cancelled + resting_quantity) << "order " << id;
    }
    expect_invariants(engine);
}

TEST(MatchingEngine, DeterministicReplayTest) {
    MatchingEngine first;
    MatchingEngine second;
    std::mt19937_64 first_rng(42);
    std::mt19937_64 second_rng(42);
    const auto first_trades = run_replay(first, first_rng, 5000);
    const auto second_trades = run_replay(second, second_rng, 5000);
    EXPECT_EQ(first_trades, second_trades);
    EXPECT_EQ(first.get_snapshot(), second.get_snapshot());
    expect_invariants(first);
    expect_invariants(second);
}

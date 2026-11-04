#include "atlas/core/order_book.hpp"

#include <iostream>

namespace {
bool invariant(const atlas::OrderBook& book) {
    std::string error;
    if (!book.verify_invariants(&error)) {
        std::cerr << "Invariant failure: " << error << '\n';
        return false;
    }
    return true;
}
}

int main() {
    using namespace atlas;
    OrderBook book;
    const Order bid_a{1, Side::Buy, OrderType::Limit, 10000, 50, 50, 1, 1};
    const Order bid_b{2, Side::Buy, OrderType::Limit, 10000, 60, 60, 2, 2};
    const Order ask{3, Side::Sell, OrderType::Limit, 10100, 40, 40, 3, 3};
    if (!book.insert_resting_order(bid_a) || !invariant(book) ||
        !book.insert_resting_order(bid_b) || !invariant(book) ||
        !book.insert_resting_order(ask) || !invariant(book)) return 1;
    if (book.best_bid() != 10000 || book.best_ask() != 10100 || book.spread() != 100) return 2;

    const CancelResult cancelled = book.cancel_order(1);
    if (!cancelled.success || cancelled.cancelled_quantity != 50 || !invariant(book)) return 3;

    Order incoming{4, Side::Buy, OrderType::Limit, 10100, 20, 20, 4, 4};
    SequenceNumber sequence = 4;
    TradeId trade_id = 0;
    std::vector<Trade> trades;
    book.match_against_book(incoming, sequence, trade_id, 4, trades);
    if (trades.size() != 1 || trades.front().price != 10100 || trades.front().quantity != 20 ||
        book.get_order(3) == nullptr || book.get_order(3)->remaining_quantity != 20 ||
        !invariant(book)) return 4;

    std::cout << "Atlas order book insert/cancel/match invariants: OK\n";
    return 0;
}

#include "atlas/core/trade.hpp"

#include <iostream>

int main() {
    const atlas::OrderRequest request{1, atlas::Side::Buy, atlas::OrderType::Limit, 10000, 1};
    const atlas::Order order{request.id, request.side, request.type, request.price,
                             request.quantity, request.quantity, 1, 0};
    if (order.is_filled() || order.filled_quantity() != 0 || atlas::to_string(order.side) != "BUY") {
        return 1;
    }
    std::cout << "Atlas domain types: OK\n";
    return 0;
}

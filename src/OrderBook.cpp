#include "lob/OrderBook.h"

#include <cstdint>
#include <iostream>
#include <iterator>

#include "lob/CoreTypes.h"

namespace lob {

namespace {

template <typename Book>
void erase_from_level(Book& book, const OrderLocation& loc) {
    auto level = book.find(loc.price);
    level->second.erase(loc.iter);
    if (level->second.empty()) {
        book.erase(level);
    }
}

}  // namespace

void OrderBook::add(const Order& order) {
    if (!checkOrder(order)) {
        return;
    }

    if (order.side == Side::Buy) {
        bid_map[order.price].push_back(order);
        auto iter = std::prev(bid_map[order.price].end());
        order_lookup[order.id] = {
            .side = order.side, .price = order.price, .tif = order.tif, .iter = iter};
    } else {
        ask_map[order.price].push_back(order);
        auto iter = std::prev(ask_map[order.price].end());
        order_lookup[order.id] = {
            .side = order.side, .price = order.price, .tif = order.tif, .iter = iter};
    }
}

void OrderBook::cancel(const OrderId id) { remove(id); }

void OrderBook::modify(const OrderId id, const Price newPrice, const Qty newQty) {
    auto iter = order_lookup.find(id);

    if (iter == order_lookup.end()) {
        std::cout << "Error: Order ID " << id << " doesn't exist.\n";
        return;
    }

    if (newQty <= 0) {
        erase_order(iter);
        return;
    }

    Order& resting = *iter->second.iter;

    // Same price and no size increase keeps time priority; anything else requeues.
    if (newPrice == resting.price && newQty <= resting.qty) {
        resting.qty = newQty;
        return;
    }

    Order updated = resting;
    updated.price = newPrice;
    updated.qty = newQty;

    erase_order(iter);
    add(updated);
}

Price OrderBook::best_bid() {
    if (bid_map.empty()) {
        std::cout << "Bids are empty. Returning INT64_MIN.\n";
        return INT64_MIN;
    }

    return bid_map.begin()->first;
}

Price OrderBook::best_ask() {
    if (ask_map.empty()) {
        std::cout << "Asks are empty. Returning INT64_MAX.\n";
        return INT64_MAX;
    }

    return ask_map.begin()->first;
}

Price OrderBook::spread() { return best_ask() - best_bid(); }

void OrderBook::end_session() {
    for (auto iter = order_lookup.begin(); iter != order_lookup.end();) {
        if (iter->second.tif == Tif::Day) {
            iter = erase_order(iter);
        } else {
            ++iter;
        }
    }
}

void OrderBook::remove(const OrderId id) {
    auto iter = order_lookup.find(id);

    if (iter == order_lookup.end()) {
        std::cout << "Error: Order ID " << id << " doesn't exist.\n";
        return;
    }

    erase_order(iter);
}

OrderBook::LookupIter OrderBook::erase_order(LookupIter iter) {
    const auto& loc = iter->second;

    if (loc.side == Side::Buy) {
        erase_from_level(bid_map, loc);
    } else {
        erase_from_level(ask_map, loc);
    }

    return order_lookup.erase(iter);
}

bool OrderBook::checkOrder(const Order& order) {
    if (order_lookup.find(order.id) != order_lookup.end()) {
        std::cout << "Error: Order ID " << order.id << " already exists.\n";
        return false;
    }

    if (order.qty <= 0) {
        std::cout << "Error: Order Id " << order.id
                  << " Can't create an order with quantity smaller or equal to zero.\n";
        return false;
    }

    return true;
}

}  // namespace lob

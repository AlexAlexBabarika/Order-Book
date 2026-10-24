#include "lob/OrderBook.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <unordered_map>

#include "lob/CoreTypes.h"

using namespace lob;
class OrderBook {
   public:
    void add(const Order& order) {
        if (order_lookup.find(order.id) != order_lookup.end()) {
            std::cout << "Error: Order ID " << order.id << " already exists.\n";
            return;
        }

        if (order.side == Side::Buy) {
            bid_map[order.price].push_back(order);
            auto iter = std::prev(bid_map[order.price].end());
            order_lookup[order.id] = {.side = order.side, .price = order.price, .iter = iter};
        } else {
            ask_map[order.price].push_back(order);
            auto iter = std::prev(ask_map[order.price].end());
            order_lookup[order.id] = {.side = order.side, .price = order.price, .iter = iter};
        }
    }

    void cancel(const OrderId id) { remove(id); }

    Price best_bid() {
        if (bid_map.empty()) {
            std::cout << "Bids are empty. Returning INT64_MIN.\n";
            return INT64_MIN;
        }

        return bid_map.begin()->first;
    }

    Price best_ask() {
        if (ask_map.empty()) {
            std::cout << "Asks are empty. Returning INT64_MAX.\n";
            return INT64_MAX;
        }

        return ask_map.begin()->first;
    }

    Price spread() { return best_ask() - best_bid(); }

    void end_session() { return; }

   private:
    std::unordered_map<OrderId, OrderLocation> order_lookup;
    std::map<Price, std::list<Order>, std::greater<Price>> bid_map;
    std::map<Price, std::list<Order>, std::less<Price>> ask_map;

    void remove(const OrderId id) {
        auto iter = order_lookup.find(id);

        if (iter == order_lookup.end()) {
            std::cout << "Error: Order ID " << id << " doesn't exist.\n";
            return;
        }

        const auto& loc = iter->second;

        if (loc.side == Side::Buy) {
            bid_map[loc.price].erase(loc.iter);
            if (bid_map[loc.price].empty()) {
                bid_map.erase(loc.price);
            }
        } else {
            ask_map[loc.price].erase(loc.iter);
            if (ask_map[loc.price].empty()) {
                ask_map.erase(loc.price);
            }
        }

        order_lookup.erase(iter);
    }
};

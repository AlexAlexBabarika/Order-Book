#pragma once
#include <lob/CoreTypes.h>

#include <functional>
#include <list>
#include <map>
#include <unordered_map>

namespace lob {

class OrderBook {
   public:
    void add(const Order& order);
    void cancel(const OrderId id);
    void modify(const OrderId id, const Price newPrice, const Qty newQty);
    Price best_bid();
    Price best_ask();
    Price spread();
    void end_session();

   private:
    using LookupIter = std::unordered_map<OrderId, OrderLocation>::iterator;

    std::unordered_map<OrderId, OrderLocation> order_lookup;
    std::map<Price, std::list<Order>, std::greater<Price>> bid_map;
    std::map<Price, std::list<Order>, std::less<Price>> ask_map;

    void remove(const OrderId id);
    LookupIter erase_order(LookupIter iter);
    bool checkOrder(const Order& order);
};

}  // namespace lob

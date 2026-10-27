#pragma once
#include <lob/CoreTypes.h>

#include <cstddef>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

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

    std::size_t size() const;
    bool contains(OrderId id) const;
    Qty level_qty(Side side, Price price) const;
    std::vector<OrderId> orders_at(Side side, Price price) const;

   private:
    using LookupIter = std::unordered_map<OrderId, OrderLocation>::iterator;
    std::unordered_map<OrderId, OrderLocation> order_lookup;
    std::map<Price, Level, std::greater<Price>> bid_map;
    std::map<Price, Level, std::less<Price>> ask_map;

    void remove(const OrderId id);
    LookupIter erase_order(LookupIter iter);
    const Level* level_at(const Side side, const Price price) const;
    bool checkOrder(const Order& order);
    static bool crosses(const Order& incoming, const Price price);
};

}  // namespace lob

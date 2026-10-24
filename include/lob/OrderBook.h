#pragma once
#include <lob/CoreTypes.h>

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
};

}  // namespace lob

#include "lob/OrderBook.h"

#include <unordered_map>

#include "lob/CoreTypes.h"
class lob::OrderBook {
   public:
    void add(const lob::Order& order) { order_map[] }

   private:
    std::unordered_map<lob::OrderId, lob::Order> order_map;
}

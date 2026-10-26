#pragma once
#include <cmath>
#include <cstdint>
#include <list>
#include <string>

namespace lob {
typedef std::int64_t Price;
typedef std::uint64_t OrderId;
typedef std::int64_t Qty;

const Price MAX_TICK_LEN = 2;
const Price TICK_FACTOR = pow(10, MAX_TICK_LEN);

enum class Side { Buy, Sell };
enum class Tif : std::uint8_t { Gtc, Ioc, Fok, Day };
enum class OrderType : std::uint8_t { Market, Limit };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty original;
    // A new order is unfilled, so filled = original - remaining = 0
    Qty remaining = original;
    Tif tif;
    OrderType type = OrderType::Limit;
};

struct Level {
    std::list<Order> orders;
    Qty total = 0;
};

struct OrderLocation {
    Side side;
    Price price;
    Tif tif;
    Level* level;
    std::list<Order>::iterator iter;
};
//
// struct CancelOrder {
//     OrderId id;
// };

std::string formatRawPrice(Price price);

}  // namespace lob

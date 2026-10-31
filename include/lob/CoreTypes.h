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

enum class Side : std::uint8_t { Buy, Sell };
enum class Tif : std::uint8_t { Gtc, Ioc, Fok, Day };
enum class OrderType : std::uint8_t { Market, Limit };

enum class Result : std::uint8_t { Ok, DuplicateId, UnknownId, BadQty, BookFull };

struct Order {
    OrderId id;
    Price price;
    Qty original;
    Qty remaining = original;
    Side side;
    Tif tif;
    OrderType type = OrderType::Limit;
};
static_assert(sizeof(Order) == 40);

struct Fill {
    OrderId maker;
    OrderId taker;
    Price price;
    Qty qty;

    bool operator==(const Fill&) const = default;
};

using Slot = std::uint32_t;
constexpr Slot NIL = UINT32_MAX;

struct OrderNode {
    OrderId id;
    Price price;
    Qty remaining;
    Slot prev = NIL, next = NIL;
    Side side;
    Tif tif;
};

// struct Level {
//     Slot head = NIL, tail = NIL;
//     Qty total = 0;
//     std::uint32_t count = 0;
// };

struct Level {
    std::list<Order> orders;
    Qty total = 0;
};

struct OrderLocation {
    Price price;
    Level* level;
    std::list<Order>::iterator iter;
    Side side;
    Tif tif;
};
static_assert(sizeof(OrderLocation) == 32);

std::string formatRawPrice(Price price);

}  // namespace lob

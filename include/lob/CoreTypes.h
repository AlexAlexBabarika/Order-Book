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

enum class Result : std::uint8_t { Ok, DuplicateId, UnknownId, BadQty, BookFull };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty original;
    Qty remaining = original;
    Tif tif;
    OrderType type = OrderType::Limit;
};

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
    Side side;
    Price price;
    Tif tif;
    Level* level;
    std::list<Order>::iterator iter;
};

std::string formatRawPrice(Price price);

}  // namespace lob

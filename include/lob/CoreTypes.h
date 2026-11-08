#pragma once
#include <cmath>
#include <cstdint>
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

enum class Result : std::uint8_t { Ok, DuplicateId, UnknownId, BadQty, BookFull, OutOfBand };

struct PriceBand {
    Price lo;
    Price hi;

    constexpr bool contains(const Price price) const { return lo <= price && price < hi; }
};

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
using LevelSlot = std::uint32_t;
constexpr Slot NIL = UINT32_MAX;

struct Level {
    Price price;
    Qty total = 0;
    Slot head = NIL;  // also links the free list while the level is free
    Slot tail = NIL;
    std::uint32_t count = 0;
};
static_assert(sizeof(Level) == 32);

// Matching reads the first two fields on every fill
struct OrderNode {
    OrderId id;
    Qty remaining;
    Qty original;
    Slot prev = NIL;
    Slot next = NIL;  // also links the free list while the slot is free
    LevelSlot level = NIL;
    Side side;
    Tif tif;
};
static_assert(sizeof(OrderNode) == 40);

std::string formatRawPrice(Price price);

}  // namespace lob

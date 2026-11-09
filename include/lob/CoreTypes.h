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

enum class Result : std::uint8_t { Ok, UnknownId, BadQty, BookFull, OutOfBand };

// The book hands out order ids. No id it hands out is ever NO_ORDER
constexpr OrderId NO_ORDER = UINT64_MAX;

// What add() and submit() return. id is NO_ORDER only if the order was turned away before it
// got one. submit() gives the id before matching, so fills can name it, and returns it even
// when the order then doesn't rest
struct Placed {
    Result result;
    OrderId id = NO_ORDER;

    bool operator==(const Placed&) const = default;
};

struct PriceBand {
    Price lo;
    Price hi;

    constexpr bool contains(const Price price) const { return lo <= price && price < hi; }
};

// What a client sends. It has no id: the book assigns one
struct Order {
    Price price;
    Qty original;
    Qty remaining = original;
    Side side;
    Tif tif;
    OrderType type = OrderType::Limit;
};
static_assert(sizeof(Order) == 32);

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

// Matching reads the fields before original on every fill
struct OrderNode {
    Qty remaining;
    // Goes up by one each time the slot is taken or freed, so it's odd while the slot holds a
    // live order. An order's id is its slot plus this, so the id dies with the order
    std::uint32_t generation = 0;
    Slot prev = NIL;
    Slot next = NIL;  // also links the free list while the slot is free
    LevelSlot level = NIL;
    Qty original;
    Side side;
    Tif tif;
};
static_assert(sizeof(OrderNode) == 40);

std::string formatRawPrice(Price price);

}  // namespace lob

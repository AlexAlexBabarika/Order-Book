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

struct Level {
    Price price;
    Qty total = 0;
    Slot head = NIL;
    Slot tail = NIL;
    std::uint32_t count = 0;
};
static_assert(sizeof(Level) == 32);

// Step 7 swaps Level* for a 4-byte level slot, which brings this to 40 bytes
struct OrderNode {
    OrderId id;
    Qty remaining;
    Qty original;
    Level* level = nullptr;
    Slot prev = NIL;
    Slot next = NIL;  // also links the free list while the slot is free
    Side side;
    Tif tif;
};
static_assert(sizeof(OrderNode) == 48);

std::string formatRawPrice(Price price);

}  // namespace lob

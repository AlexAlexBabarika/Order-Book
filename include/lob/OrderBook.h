#pragma once
#include <lob/CoreTypes.h>
#include <lob/Pool.h>
#include <lob/PriceArray.h>
#include <lob/PriceLadder.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace lob {

// The array won the replay in step 8. The CMake option LOB_PRICE_LADDER puts the ladder back.
// Both sides use one type, ordered so that a lower key is better: see OrderBook::key()
#ifdef LOB_PRICE_LADDER
using PriceLevels = PriceLadder<std::less<Price>>;
#else
using PriceLevels = PriceArray<std::less<Price>>;
#endif

// levels is indexed by side
static_assert(static_cast<int>(Side::Buy) == 0 && static_cast<int>(Side::Sell) == 1);

class OrderBook {
   public:
    static constexpr std::uint32_t DEFAULT_MAX_ORDERS = 1 << 16;
    static constexpr PriceBand DEFAULT_BAND{.lo = 0, .hi = 1 << 16};

    // At most max_orders orders rest at once
    explicit OrderBook(std::uint32_t max_orders = DEFAULT_MAX_ORDERS,
                       PriceBand band = DEFAULT_BAND);
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    // Rests the order without matching it
    Placed add(const Order& order);
    Result cancel(OrderId id);
    Result modify(OrderId id, Price newPrice, Qty newQty);
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> spread() const;
    void end_session();

    std::size_t size() const;
    bool contains(OrderId id) const;
    Qty level_qty(Side side, Price price) const;
    std::vector<OrderId> orders_at(Side side, Price price) const;

    // Matches the order, then rests what's left if its type and tif allow it
    template <typename OnFill>
    Placed submit(Order order, OnFill&& on_fill);

   private:
    // The pool holds one slot more than max_orders, so submit always has a slot for the
    // incoming order while it matches, even when the book is full
    OrderPool order_pool;
    LevelPool level_pool;
    PriceBand band;
    // Bids, then asks. Indexing by side, not branching on it, matters because sides are
    // random: the branch was mispredicted on about half of all submits (step 11)
    std::array<PriceLevels, 2> levels;
    std::uint32_t max_orders;
    std::uint32_t resting_orders = 0;

    // A level's place in its side. Bids are keyed by -price and asks by price, so on both
    // sides a lower key is better. key() is its own inverse: key(side, key(side, p)) == p
    static constexpr Price key(const Side side, const Price price) {
        return side == Side::Buy ? -price : price;
    }
    static constexpr Side opposite(const Side side) {
        return side == Side::Buy ? Side::Sell : Side::Buy;
    }
    PriceLevels& levels_of(const Side side) { return levels[static_cast<std::size_t>(side)]; }
    const PriceLevels& levels_of(const Side side) const {
        return levels[static_cast<std::size_t>(side)];
    }
    // The highest maker key the order trades with
    static Price limit_key(const Order& incoming) {
        return incoming.type == OrderType::Market ? std::numeric_limits<Price>::max()
                                                  : key(opposite(incoming.side), incoming.price);
    }

    // An id is the slot in the low 32 bits and the slot's generation in the high 32
    static OrderId make_id(const Slot slot, const std::uint32_t generation) {
        return (static_cast<OrderId>(generation) << 32) | slot;
    }
    OrderId id_of(const Slot slot) const { return make_id(slot, order_pool[slot].generation); }
    // NIL unless the id belongs to an order in the book
    Slot find(OrderId id) const;
    bool full() const { return resting_orders == max_orders; }

    Slot open_slot();
    void close_slot(Slot slot);
    void rest(Slot slot, const Order& order);
    void erase_order(Slot slot);
    void link_back(LevelSlot level, Slot slot);
    void unlink(Slot slot);
    LevelSlot level_for(Side side, Price price);
    void erase_if_empty(LevelSlot level, Side side);
    const Level* level_at(const Side side, const Price price) const;
    static Result validate(const Order& order);

    template <typename OnFill>
    void match(Order& incoming, OrderId taker, OnFill& on_fill);

    bool can_fill(const Order& incoming) const;

    static bool should_rest(const Order& order);
};

template <typename OnFill>
void OrderBook::match(Order& incoming, const OrderId taker, OnFill& on_fill) {
    PriceLevels& book = levels_of(opposite(incoming.side));
    const Price limit = limit_key(incoming);
    while (incoming.remaining > 0 && !book.empty()) {
        if (book.best_price() > limit) {
            return;
        }

        const LevelSlot level_slot = book.best_slot();
        Level& level = level_pool[level_slot];
        while (incoming.remaining > 0 && level.head != NIL) {
            const Slot slot = level.head;
            OrderNode& maker = order_pool[slot];
            const Qty qty = std::min(incoming.remaining, maker.remaining);
            maker.remaining -= qty;
            incoming.remaining -= qty;
            level.total -= qty;
            on_fill(Fill{.maker = make_id(slot, maker.generation),
                         .taker = taker,
                         .price = level.price,
                         .qty = qty});

            if (maker.remaining == 0) {
                unlink(slot);
                close_slot(slot);
                --resting_orders;
            }
        }

        if (level.head == NIL) {
            book.pop_best();
            level_pool.release(level_slot);
        }
    }
}

template <typename OnFill>
Placed OrderBook::submit(Order order, OnFill&& on_fill) {
    if (const Result result = validate(order); result != Result::Ok) {
        return {.result = result};
    }

    if (order.type == OrderType::Limit && !band.contains(order.price)) {
        return {.result = Result::OutOfBand};
    }

    // Fills name the taker, so the order takes its slot, and with it its id, before it trades
    const Slot slot = open_slot();
    const OrderId id = id_of(slot);

    if (order.tif == Tif::Fok && !can_fill(order)) {
        close_slot(slot);
        return {.result = Result::Ok, .id = id};
    }

    match(order, id, on_fill);

    if (!should_rest(order)) {
        close_slot(slot);
        return {.result = Result::Ok, .id = id};
    }
    // Checked after matching, because filled makers free room for the remainder
    if (full()) {
        close_slot(slot);
        return {.result = Result::BookFull, .id = id};
    }
    rest(slot, order);
    return {.result = Result::Ok, .id = id};
}
}  // namespace lob

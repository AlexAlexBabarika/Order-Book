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
    // Less at the same price keeps the order's place in its queue. Any other change is a
    // cancel and replace under the same id
    template <typename OnFill>
    Result modify(OrderId id, Price newPrice, Qty newQty, OnFill&& on_fill);
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
    // Takes the order out of its level. It keeps its slot, and with it its id. Forced inline:
    // left to itself, clang kept it out of line, and cancel paid for extra register saves
    // around the call (step 15)
    [[gnu::always_inline]] void take_off_book(const Slot slot) {
        unlink(slot);
        erase_if_empty(order_pool[slot].level, order_pool[slot].side);
        --resting_orders;
    }
    void erase_order(Slot slot);

    // The part of modify() that can't trade. It lives in the .cpp, so a modify costs the
    // caller one call. If the new price crosses, it changes nothing and hands back the
    // order's slot for trade_amended()
    struct Amended {
        Result result;
        Slot crossing = NIL;
    };
    Amended amend(OrderId id, Price newPrice, Qty newQty);
    // Shrinks the order where it stands, if the change allows it
    bool shrink_in_place(Slot slot, Price newPrice, Qty newQty);
    // The order as it stands after a modify. It keeps its side, its tif and what it filled
    Order replacement(Slot slot, Price newPrice, Qty newQty) const;
    // The cancel and replace of a modify that crosses. Not inlined, since it's rare and holds
    // a copy of match()
    template <typename OnFill>
    [[gnu::noinline]] void trade_amended(Slot slot, Price newPrice, Qty newQty, OnFill& on_fill);

    void link_back(LevelSlot level, Slot slot);
    void unlink(Slot slot);
    LevelSlot level_for(Side side, Price price);
    void erase_if_empty(LevelSlot level, Side side);
    const Level* level_at(const Side side, const Price price) const;
    static Result validate(const Order& order);

    // Forced inline, since it's submit()'s inner loop. Once modify() called it too, the
    // compiler stopped inlining it, and every submit paid a call (step 15)
    template <typename OnFill>
    [[gnu::always_inline]] inline void match(Order& incoming, OrderId taker, OnFill& on_fill);

    // Whether the order would trade with the best level on the other side
    bool crosses(const Order& incoming) const {
        const PriceLevels& book = levels_of(opposite(incoming.side));
        return !book.empty() && book.best_price() <= limit_key(incoming);
    }
    bool can_fill(const Order& incoming) const;

    static bool should_rest(const Order& order);
};

template <typename OnFill>
inline void OrderBook::match(Order& incoming, const OrderId taker, OnFill& on_fill) {
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

template <typename OnFill>
Result OrderBook::modify(const OrderId id, const Price newPrice, const Qty newQty,
                         OnFill&& on_fill) {
    const Amended amended = amend(id, newPrice, newQty);
    if (amended.crossing != NIL) {
        trade_amended(amended.crossing, newPrice, newQty, on_fill);
    }
    return amended.result;
}

// Like a new submit, but the order keeps its slot and id, and what it filled before
template <typename OnFill>
void OrderBook::trade_amended(const Slot slot, const Price newPrice, const Qty newQty,
                              OnFill& on_fill) {
    Order order = replacement(slot, newPrice, newQty);
    // The order gives up its place before it trades, so a full book doesn't block it
    take_off_book(slot);
    match(order, id_of(slot), on_fill);
    if (order.remaining == 0) {
        close_slot(slot);
        return;
    }
    // Only GTC and Day orders rest, so the rest of a resting order may rest again, and the
    // book has room: this order's own place was freed above
    rest(slot, order);
}
}  // namespace lob

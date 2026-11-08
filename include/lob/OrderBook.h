#pragma once
#include <ankerl/unordered_dense.h>
#include <lob/CoreTypes.h>
#include <lob/Pool.h>
#include <lob/PriceArray.h>
#include <lob/PriceLadder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace lob {

// The array won the replay in step 8. The CMake option LOB_PRICE_LADDER puts the ladder back
#ifdef LOB_PRICE_LADDER
template <typename Better>
using PriceLevels = PriceLadder<Better>;
#else
template <typename Better>
using PriceLevels = PriceArray<Better>;
#endif

class OrderBook {
   public:
    static constexpr std::uint32_t DEFAULT_MAX_ORDERS = 1 << 16;
    static constexpr PriceBand DEFAULT_BAND{.lo = 0, .hi = 1 << 16};

    explicit OrderBook(std::uint32_t max_orders = DEFAULT_MAX_ORDERS,
                       PriceBand band = DEFAULT_BAND);
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    Result add(const Order& order);
    Result cancel(const OrderId id);
    Result modify(const OrderId id, const Price newPrice, const Qty newQty);
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> spread() const;
    void end_session();

    std::size_t size() const;
    bool contains(OrderId id) const;
    Qty level_qty(Side side, Price price) const;
    std::vector<OrderId> orders_at(Side side, Price price) const;

    template <typename OnFill>
    void match(Order& incoming, OnFill&& on_fill);

    template <typename OnFill>
    Result submit(Order order, OnFill&& on_fill);

   private:
    using OrderIndex = ankerl::unordered_dense::map<OrderId, Slot>;
    using LookupIter = OrderIndex::iterator;

    OrderPool order_pool;
    LevelPool level_pool;
    OrderIndex order_lookup;
    PriceBand band;
    PriceLevels<std::greater<Price>> bid_levels;
    PriceLevels<std::less<Price>> ask_levels;

    LookupIter erase_order(LookupIter iter);
    void link_back(LevelSlot level, Slot slot);
    void unlink(Slot slot);
    LevelSlot level_for(Side side, Price price);
    void erase_if_empty(LevelSlot level, Side side);
    const Level* level_at(const Side side, const Price price) const;
    static Result validate(const Order& order);
    static bool crosses(const Order& incoming, const Price price);

    template <typename Book, typename OnFill>
    void match_against(Book& book, Order& incoming, OnFill& on_fill);

    bool can_fill(const Order& incoming) const;

    template <typename Book>
    bool can_fill_from(const Book& book, const Order& incoming) const;

    static bool should_rest(const Order& order);
};

template <typename Book, typename OnFill>
void OrderBook::match_against(Book& book, Order& incoming, OnFill& on_fill) {
    while (incoming.remaining > 0 && !book.empty()) {
        const Price price = book.best_price();
        if (!crosses(incoming, price)) {
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
            on_fill(Fill{.maker = maker.id, .taker = incoming.id, .price = price, .qty = qty});

            if (maker.remaining == 0) {
                order_lookup.erase(maker.id);
                unlink(slot);
                order_pool.release(slot);
            }
        }

        if (level.head == NIL) {
            book.pop_best();
            level_pool.release(level_slot);
        }
    }
}

template <typename OnFill>
void OrderBook::match(Order& incoming, OnFill&& on_fill) {
    if (incoming.side == Side::Buy) {
        match_against(ask_levels, incoming, on_fill);
    } else {
        match_against(bid_levels, incoming, on_fill);
    }
}

template <typename OnFill>
Result OrderBook::submit(Order order, OnFill&& on_fill) {
    if (const Result result = validate(order); result != Result::Ok) {
        return result;
    }

    if (order.type == OrderType::Limit && !band.contains(order.price)) {
        return Result::OutOfBand;
    }

    if (contains(order.id)) {
        return Result::DuplicateId;
    }

    if (order.tif == Tif::Fok && !can_fill(order)) {
        return Result::Ok;
    }

    match(order, std::forward<OnFill>(on_fill));

    if (should_rest(order)) {
        return add(order);
    }
    return Result::Ok;
}
}  // namespace lob

#pragma once
#include <lob/CoreTypes.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lob {

class OrderBook {
   public:
    OrderBook() = default;
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
    using LookupIter = std::unordered_map<OrderId, OrderLocation>::iterator;

    std::unordered_map<OrderId, OrderLocation> order_lookup;
    std::map<Price, Level, std::greater<Price>> bid_map;
    std::map<Price, Level, std::less<Price>> ask_map;

    LookupIter erase_order(LookupIter iter);
    const Level* level_at(const Side side, const Price price) const;
    static Result validate(const Order& order);
    static bool crosses(const Order& incoming, const Price price);

    template <typename Book, typename OnFill>
    void match_against(Book& book, Order& incoming, OnFill& on_fill);

    bool can_fill(const Order& incoming) const;

    template <typename Book>
    static bool can_fill_from(const Book& book, const Order& incoming);

    static bool should_rest(const Order& order);
};

template <typename Book, typename OnFill>
void OrderBook::match_against(Book& book, Order& incoming, OnFill& on_fill) {
    while (incoming.remaining > 0 && !book.empty()) {
        const auto best = book.begin();
        const Price price = best->first;
        if (!crosses(incoming, price)) {
            return;
        }

        Level& level = best->second;
        while (incoming.remaining > 0 && !level.orders.empty()) {
            Order& maker = level.orders.front();
            const Qty qty = std::min(incoming.remaining, maker.remaining);
            maker.remaining -= qty;
            incoming.remaining -= qty;
            level.total -= qty;
            on_fill(Fill{.maker = maker.id, .taker = incoming.id, .price = price, .qty = qty});

            if (maker.remaining == 0) {
                order_lookup.erase(maker.id);
                level.orders.pop_front();
            }
        }

        if (level.orders.empty()) {
            book.erase(best);
        }
    }
}

template <typename OnFill>
void OrderBook::match(Order& incoming, OnFill&& on_fill) {
    if (incoming.side == Side::Buy) {
        match_against(ask_map, incoming, on_fill);
    } else {
        match_against(bid_map, incoming, on_fill);
    }
}

template <typename OnFill>
Result OrderBook::submit(Order order, OnFill&& on_fill) {
    if (const Result result = validate(order); result != Result::Ok) {
        return result;
    }

    if (contains(order.id)) {
        return Result::DuplicateId;
    }

    if (order.tif == Tif::Fok && !can_fill(order)) {
        return Result::Ok;
    }

    match(order, std::forward<OnFill>(on_fill));

    if (should_rest(order)) {
        add(order);
    }
    return Result::Ok;
}
}  // namespace lob

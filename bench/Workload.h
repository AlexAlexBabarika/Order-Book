#pragma once
#include <lob/CoreTypes.h>
#include <lob/OrderBook.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob::bench {

// The generator turns adds into cancels once the book holds this many orders
constexpr std::size_t TARGET_DEPTH = 10'000;

enum class MsgType : std::uint8_t { Submit, Cancel, Modify };

struct Message {
    MsgType type;
    Order order;      // Submit
    OrderId id;       // Cancel, Modify
    Price new_price;  // Modify
    Qty new_qty;      // Modify
};

// Replayed in order on an empty book, every cancel and modify targets a live order
std::vector<Message> make_workload(std::size_t count, std::uint64_t seed);

template <typename OnFill>
void apply(OrderBook& book, const Message& msg, OnFill& on_fill) {
    switch (msg.type) {
        case MsgType::Submit:
            book.submit(msg.order, on_fill);
            return;
        case MsgType::Cancel:
            book.cancel(msg.id);
            return;
        case MsgType::Modify:
            book.modify(msg.id, msg.new_price, msg.new_qty);
            return;
    }
}

}  // namespace lob::bench

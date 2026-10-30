#include "Workload.h"

#include <algorithm>
#include <random>

namespace lob::bench {

namespace {

// Resting buys sit below MID and resting sells above it, so resting adds and modifies never cross
constexpr Price MID = 10'000;
constexpr Price MAX_OFFSET = 100;

struct LiveOrder {
    OrderId id;
    Side side;
};

// Builds each message against a scratch book, so it always knows which orders are live
class Generator {
   public:
    explicit Generator(std::uint64_t seed) : rng(seed) {}

    Message next() {
        const int roll = percent(rng);
        if (roll < 10) {
            return take();
        }
        if (roll < 55) {
            return scratch.size() < TARGET_DEPTH ? rest() : cancel();
        }
        // Every resting order is as likely to be cancelled or modified as any other, so these scale
        // with the depth. At TARGET_DEPTH the mix is 45% rest, 40% cancel, 5% modify, 10% take.
        if (depth(rng) >= scratch.size()) {
            return rest();
        }
        return roll < 95 ? cancel() : modify();
    }

   private:
    std::mt19937_64 rng;
    std::uniform_int_distribution<int> percent{0, 99};
    std::uniform_int_distribution<std::size_t> depth{0, TARGET_DEPTH - 1};
    std::bernoulli_distribution coin;
    std::geometric_distribution<Price> offset{0.15};
    std::uniform_int_distribution<Qty> qty{1, 100};
    std::uniform_int_distribution<Price> cross{1, 6};

    OrderBook scratch;
    std::vector<LiveOrder> live;
    OrderId next_id = 0;

    Side side() { return coin(rng) ? Side::Buy : Side::Sell; }

    Price passive_price(const Side side) {
        const Price ticks = 1 + std::min(offset(rng), MAX_OFFSET - 1);
        return side == Side::Buy ? MID - ticks : MID + ticks;
    }

    Message submit(const Order& order) {
        scratch.submit(order, [](const Fill&) {});
        return {.type = MsgType::Submit, .order = order};
    }

    Message rest() {
        const Side s = side();
        const Order order{.id = next_id++,
                          .side = s,
                          .price = passive_price(s),
                          .original = qty(rng),
                          .tif = Tif::Gtc};
        live.push_back({order.id, s});
        return submit(order);
    }

    // An IOC a few ticks through the touch, which may sweep more than one level
    Message take() {
        const Side s = side();
        const Price ticks = cross(rng);
        const Order order{.id = next_id++,
                          .side = s,
                          .price = s == Side::Buy ? MID + ticks : MID - ticks,
                          .original = qty(rng),
                          .tif = Tif::Ioc};
        return submit(order);
    }

    Message cancel() {
        const std::size_t i = pick_live();
        const OrderId id = live[i].id;
        drop(i);
        scratch.cancel(id);
        return {.type = MsgType::Cancel, .order = {}, .id = id};
    }

    Message modify() {
        const LiveOrder order = live[pick_live()];
        const Message msg{.type = MsgType::Modify,
                          .order = {},
                          .id = order.id,
                          .new_price = passive_price(order.side),
                          .new_qty = qty(rng)};
        scratch.modify(msg.id, msg.new_price, msg.new_qty);
        return msg;
    }

    // Orders a taker filled are still in `live`. They're dropped here when a pick lands on one.
    // Needs a non-empty book: every resting order is in `live`, so the loop then ends.
    std::size_t pick_live() {
        for (;;) {
            const std::size_t i =
                std::uniform_int_distribution<std::size_t>{0, live.size() - 1}(rng);
            if (scratch.contains(live[i].id)) {
                return i;
            }
            drop(i);
        }
    }

    void drop(const std::size_t i) {
        live[i] = live.back();
        live.pop_back();
    }
};

}  // namespace

std::vector<Message> make_workload(const std::size_t count, const std::uint64_t seed) {
    Generator generator(seed);
    std::vector<Message> messages;
    messages.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        messages.push_back(generator.next());
    }
    return messages;
}

}  // namespace lob::bench

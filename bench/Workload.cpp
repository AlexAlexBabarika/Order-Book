#include "Workload.h"

#include <algorithm>
#include <random>

namespace lob::bench {

namespace {

constexpr Price MID = 10'000;
constexpr Price MAX_OFFSET = 100;

struct LiveOrder {
    OrderId id;
    Side side;
};

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
                          .price = passive_price(s),
                          .original = qty(rng),
                          .side = s,
                          .tif = Tif::Gtc};
        live.push_back({order.id, s});
        return submit(order);
    }

    Message take() {
        const Side s = side();
        const Price ticks = cross(rng);
        const Order order{.id = next_id++,
                          .price = s == Side::Buy ? MID + ticks : MID - ticks,
                          .original = qty(rng),
                          .side = s,
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

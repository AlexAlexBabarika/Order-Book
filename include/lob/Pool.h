#pragma once
#include <lob/CoreTypes.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace lob {

template <typename Node, Slot Node::* Link>
class Pool {
   public:
    explicit Pool(std::uint32_t capacity) : nodes_(capacity) {
        for (Slot s = 0; s < capacity; ++s) {
            nodes_[s].*Link = (s + 1 == capacity) ? NIL : s + 1;
        }
        free_head_ = capacity == 0 ? NIL : 0;
    }

    Pool(Pool&& other) noexcept
        : nodes_(std::move(other.nodes_)), free_head_(std::exchange(other.free_head_, NIL)) {}
    Pool& operator=(Pool&& other) noexcept {
        nodes_ = std::move(other.nodes_);
        free_head_ = std::exchange(other.free_head_, NIL);
        return *this;
    }

    bool full() const { return free_head_ == NIL; }

    Slot acquire() {
        const Slot s = free_head_;
        if (s != NIL) {
            free_head_ = nodes_[s].*Link;
        }
        return s;
    }

    void release(Slot s) {
        nodes_[s].*Link = free_head_;
        free_head_ = s;
    }

    Node& operator[](Slot s) { return nodes_[s]; }
    const Node& operator[](Slot s) const { return nodes_[s]; }

   private:
    std::vector<Node> nodes_;
    Slot free_head_ = NIL;
};

using OrderPool = Pool<OrderNode, &OrderNode::next>;
using LevelPool = Pool<Level, &Level::head>;

}  // namespace lob

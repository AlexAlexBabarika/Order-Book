#pragma once
#include <lob/CoreTypes.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace lob {

class OrderPool {
   public:
    explicit OrderPool(std::uint32_t capacity) : nodes_(capacity) {
        for (Slot s = 0; s < capacity; ++s) {
            nodes_[s].next = (s + 1 == capacity) ? NIL : s + 1;
        }
        free_head_ = capacity == 0 ? NIL : 0;
    }

    OrderPool(OrderPool&& other) noexcept
        : nodes_(std::move(other.nodes_)), free_head_(std::exchange(other.free_head_, NIL)) {}
    OrderPool& operator=(OrderPool&& other) noexcept {
        nodes_ = std::move(other.nodes_);
        free_head_ = std::exchange(other.free_head_, NIL);
        return *this;
    }

    bool full() const { return free_head_ == NIL; }

    Slot acquire() {
        const Slot s = free_head_;
        if (s != NIL) {
            free_head_ = nodes_[s].next;
        }
        return s;
    }

    void release(Slot s) {
        nodes_[s].next = free_head_;
        free_head_ = s;
    }

    OrderNode& operator[](Slot s) { return nodes_[s]; }
    const OrderNode& operator[](Slot s) const { return nodes_[s]; }

   private:
    std::vector<OrderNode> nodes_;
    Slot free_head_ = NIL;
};

}  // namespace lob

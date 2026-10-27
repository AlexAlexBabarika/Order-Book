#pragma once

#include "lob/CoreTypes.h"

using Slot = std::uint32_t;
constexpr Slot NIL = UINT32_MAX;

class OrderPool {
   public:
    explicit OrderPool(std::uint32_t capacity) : nodes_(capacity) {
        for (Slot s = 0; s < capacity; ++s) {
            nodes_[s].next = (s + 1 == capacity) ? NIL : s + 1;
        }
        free_head_ = capacity == 0 ? NIL : 0;
    }

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

    lob::OrderNode& operator[](Slot s) { return nodes_[s]; }

   private:
    std::vector<lob::OrderNode> nodes_;
    Slot free_head_ = NIL;
};

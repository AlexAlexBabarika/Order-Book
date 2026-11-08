#pragma once
#include <lob/CoreTypes.h>

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace lob {

template <typename Better>
class PriceArray {
   public:
    PriceArray(const PriceBand band, std::size_t)
        : base_(band.lo), slots_(static_cast<std::size_t>(band.hi - band.lo), NIL) {}

    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }

    template <typename Visit>
    void for_each_from_best(Visit visit) const {
        std::size_t left = count_;
        for (Price i = best_; left > 0; i += WORSE) {
            const LevelSlot slot = at(i);
            if (slot == NIL) {
                continue;
            }
            --left;
            if (!visit(base_ + i, slot)) {
                return;
            }
        }
    }

    Price best_price() const { return base_ + best_; }
    LevelSlot best_slot() const { return at(best_); }

    void pop_best() { remove(best_); }

    LevelSlot find(const Price price) const { return in_band(price) ? at(price - base_) : NIL; }

    std::pair<LevelSlot&, bool> try_emplace(const Price price) {
        assert(in_band(price) && "the book checks the band first");
        const Price i = price - base_;
        LevelSlot& slot = slots_[static_cast<std::size_t>(i)];
        if (slot != NIL) {
            return {slot, false};
        }
        if (count_ == 0 || Better{}(price, best_price())) {
            best_ = i;
        }
        ++count_;
        return {slot, true};
    }

    void erase(const Price price) {
        if (in_band(price) && at(price - base_) != NIL) {
            remove(price - base_);
        }
    }

   private:
    static constexpr Price WORSE = Better{}(Price{1}, Price{0}) ? -1 : 1;

    LevelSlot at(const Price i) const { return slots_[static_cast<std::size_t>(i)]; }

    bool in_band(const Price price) const {
        return price >= base_ && price - base_ < static_cast<Price>(slots_.size());
    }

    void remove(const Price i) {
        slots_[static_cast<std::size_t>(i)] = NIL;
        --count_;
        if (i != best_ || count_ == 0) {
            return;
        }
        do {
            best_ += WORSE;
        } while (at(best_) == NIL);
    }

    Price base_;
    Price best_ = 0;  // offset from base_; meaningless while empty
    std::size_t count_ = 0;
    std::vector<LevelSlot> slots_;
};

}  // namespace lob

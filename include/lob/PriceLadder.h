#pragma once
#include <lob/CoreTypes.h>

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace lob {

template <typename Better>
class PriceLadder {
   public:
    PriceLadder(PriceBand, const std::size_t max_levels) {
        prices_.resize(max_levels);
        prices_.clear();
        slots_.resize(max_levels);
        slots_.clear();
    }

    bool empty() const { return prices_.empty(); }
    std::size_t size() const { return prices_.size(); }

    template <typename Visit>
    void for_each_from_best(Visit visit) const {
        for (std::size_t i = prices_.size(); i-- > 0;) {
            if (!visit(prices_[i], slots_[i])) {
                return;
            }
        }
    }

    Price best_price() const { return prices_.back(); }
    LevelSlot best_slot() const { return slots_.back(); }

    void pop_best() {
        prices_.pop_back();
        slots_.pop_back();
    }

    LevelSlot find(const Price price) const {
        const std::size_t i = past_better(price);
        return holds(i, price) ? slots_[i - 1] : NIL;
    }

    std::pair<LevelSlot&, bool> try_emplace(const Price price) {
        const std::size_t i = past_better(price);
        if (holds(i, price)) {
            return {slots_[i - 1], false};
        }
        prices_.insert(prices_.begin() + static_cast<std::ptrdiff_t>(i), price);
        slots_.insert(slots_.begin() + static_cast<std::ptrdiff_t>(i), NIL);
        return {slots_[i], true};
    }

    void erase(const Price price) {
        const std::size_t i = past_better(price);
        if (!holds(i, price)) {
            return;
        }
        prices_.erase(prices_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }

   private:
    std::size_t past_better(const Price price) const {
        std::size_t i = prices_.size();
        while (i > 0 && Better{}(prices_[i - 1], price)) {
            --i;
        }
        return i;
    }

    bool holds(const std::size_t i, const Price price) const {
        return i > 0 && prices_[i - 1] == price;
    }

    std::vector<Price> prices_;
    std::vector<LevelSlot> slots_;
};

}  // namespace lob

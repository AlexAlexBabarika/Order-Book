#include <gtest/gtest.h>

#include "lob/OrderBook.h"

namespace {

TEST(OrderBook, EmptyBookHasNoOrders) {
    lob::OrderBook book;
    EXPECT_EQ(book.size(), 0u);
}

}  // namespace

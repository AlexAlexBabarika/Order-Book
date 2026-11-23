#pragma once
#include <lob/CoreTypes.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace lob::feed {

// The event types of a LOBSTER message file, by their numbers there
enum class EventType : std::uint8_t {
    Add = 1,
    Reduce = 2,  // part of the order is cancelled
    Delete = 3,
    Execute = 4,  // a visible order trades, in part or in full
    HiddenExecute = 5,
    Cross = 6,
    Halt = 7,
};

// One line of a message file, without its time. The price is as the file has it: dollars
// times 10,000. The side is the resting order's, also when the event is an execution
struct Event {
    std::uint64_t id;
    Qty size;
    Price price;
    EventType type;
    Side side;
};

struct Quote {
    Price price;
    Qty qty;
};

// One line of an orderbook file: the book after the message on the same line of the message
// file. Best first, with the levels the file marks as unoccupied left out
struct BookRow {
    std::vector<Quote> asks;
    std::vector<Quote> bids;
};

// Both throw std::runtime_error, naming the line, on a line they can't read
std::vector<Event> read_events(std::istream& in);

class RowReader {
   public:
    explicit RowReader(std::istream& in) : in_(in) {}
    // False at the end of the file
    bool next(BookRow& row);

   private:
    std::istream& in_;
    std::string line_;
    std::size_t line_no_ = 0;
};

}  // namespace lob::feed

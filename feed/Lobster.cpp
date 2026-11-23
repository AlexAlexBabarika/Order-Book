#include "Lobster.h"

#include <charconv>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace lob::feed {

namespace {

// What an orderbook file puts in the price of an unoccupied level, negated for bids
constexpr Price NO_LEVEL = 9'999'999'999;

[[noreturn]] void fail(const std::size_t line_no, const std::string& what) {
    throw std::runtime_error("line " + std::to_string(line_no) + ": " + what);
}

std::string_view next_field(std::string_view& line) {
    const std::size_t comma = line.find(',');
    const std::string_view field = line.substr(0, comma);
    line = comma == std::string_view::npos ? std::string_view{} : line.substr(comma + 1);
    return field;
}

std::int64_t to_int(const std::string_view field, const std::size_t line_no) {
    std::int64_t value = 0;
    const char* const last = field.data() + field.size();
    const auto [end, ec] = std::from_chars(field.data(), last, value);
    if (ec != std::errc{} || end != last) {
        fail(line_no, "bad number '" + std::string(field) + "'");
    }
    return value;
}

// False at the end of the file. Skips blank lines and drops a trailing carriage return
bool next_line(std::istream& in, std::string& line, std::size_t& line_no) {
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            return true;
        }
    }
    return false;
}

Event to_event(std::string_view line, const std::size_t line_no) {
    next_field(line);  // the time
    const std::int64_t type = to_int(next_field(line), line_no);
    const std::int64_t id = to_int(next_field(line), line_no);
    const std::int64_t size = to_int(next_field(line), line_no);
    const std::int64_t price = to_int(next_field(line), line_no);
    const std::int64_t direction = to_int(next_field(line), line_no);
    if (type < 1 || type > 7) {
        fail(line_no, "unknown event type " + std::to_string(type));
    }
    if (direction != 1 && direction != -1) {
        fail(line_no, "direction is neither 1 nor -1");
    }
    return {.id = static_cast<std::uint64_t>(id),
            .size = size,
            .price = price,
            .type = static_cast<EventType>(type),
            .side = direction == 1 ? Side::Buy : Side::Sell};
}

}  // namespace

std::vector<Event> read_events(std::istream& in) {
    std::vector<Event> events;
    std::string line;
    std::size_t line_no = 0;
    while (next_line(in, line, line_no)) {
        events.push_back(to_event(line, line_no));
    }
    return events;
}

// Each level is four fields: ask price, ask size, bid price, bid size
bool RowReader::next(BookRow& row) {
    if (!next_line(in_, line_, line_no_)) {
        return false;
    }
    row.asks.clear();
    row.bids.clear();
    std::string_view rest = line_;
    while (!rest.empty()) {
        for (std::vector<Quote>* side : {&row.asks, &row.bids}) {
            const Price price = to_int(next_field(rest), line_no_);
            if (rest.empty()) {
                fail(line_no_, "a level is cut short");
            }
            const Qty qty = to_int(next_field(rest), line_no_);
            if (qty > 0 && price != NO_LEVEL && price != -NO_LEVEL) {
                side->push_back({.price = price, .qty = qty});
            }
        }
    }
    return true;
}

}  // namespace lob::feed

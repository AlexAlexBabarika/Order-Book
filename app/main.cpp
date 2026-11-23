#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Checker.h"
#include "FeedBook.h"
#include "Lobster.h"
#include "lob/CoreTypes.h"

namespace {
using namespace lob;

constexpr std::uint32_t MAX_ORDERS = 1 << 18;
constexpr Price BAND_WIDTH = 1 << 16;
constexpr Price TICK = 100;
constexpr int TIMED_RUNS = 5;

std::ifstream open(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("can't open " + path);
    }
    return in;
}

// As wide as the default band, with the first order in the middle
PriceBand band_around(const std::vector<feed::Event>& events) {
    for (const feed::Event& event : events) {
        if (event.type == feed::EventType::Add) {
            const Price lo = event.price / TICK - BAND_WIDTH / 2;
            return {.lo = lo, .hi = lo + BAND_WIDTH};
        }
    }
    throw std::runtime_error("the message file adds no orders");
}

const char* name(const Side side) { return side == Side::Buy ? "bid" : "ask"; }

void print(const feed::Stats& stats, const OrderBook& book) {
    std::cout << "applied " << stats.applied << ", ignored " << stats.ignored << ", unknown id "
              << stats.unknown << ", rejected " << stats.rejected << "; " << book.size()
              << " orders resting at the end\n";
}

// False if the book ever differed from the file
bool check(const std::vector<feed::Event>& events, const PriceBand band,
           const std::string& book_path) {
    std::ifstream in = open(book_path);
    feed::RowReader rows(in);
    feed::FeedBook book(MAX_ORDERS, band, TICK);
    feed::Checker checker(book);
    feed::BookRow row;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!rows.next(row)) {
            throw std::runtime_error(book_path + " has fewer lines than the message file");
        }
        // The first row seeds the book, and it already holds the first event
        if (i > 0) {
            book.apply(events[i]);
        }
        checker.check(row);
    }

    std::cout << "checked " << checker.rows() << " rows, " << checker.levels() << " levels ("
              << checker.synced() << " synced to the file): " << checker.mismatches()
              << " mismatches\n";
    if (const std::optional<feed::Mismatch>& first = checker.first_mismatch()) {
        std::cout << "first mismatch: row " << first->row << ", " << name(first->side) << " at "
                  << first->price << ": file has " << first->expected << ", book has "
                  << first->actual << "\n";
    }
    print(book.stats(), book.book());
    return checker.mismatches() == 0;
}

// Nanoseconds per event. With an orderbook file, the book starts from its first row, so the
// orders older than the feed are there to be cancelled and traded
double time_replay(const std::vector<feed::Event>& events, const PriceBand band,
                   const std::optional<std::string>& book_path, const bool report) {
    feed::FeedBook book(MAX_ORDERS, band, TICK);
    std::size_t from = 0;
    if (book_path) {
        std::ifstream in = open(*book_path);
        feed::BookRow row;
        if (feed::RowReader(in).next(row)) {
            feed::Checker(book).check(row);
            from = 1;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = from; i < events.size(); ++i) {
        book.apply(events[i]);
    }
    const std::chrono::duration<double, std::nano> elapsed =
        std::chrono::steady_clock::now() - start;
    if (report) {
        print(book.stats(), book.book());
    }
    return elapsed.count() / static_cast<double>(events.size() - from);
}

int run(const std::string& message_path, const std::optional<std::string>& book_path) {
    std::ifstream in = open(message_path);
    const std::vector<feed::Event> events = feed::read_events(in);
    const PriceBand band = band_around(events);

    std::array<std::size_t, 8> by_type{};
    for (const feed::Event& event : events) {
        ++by_type[static_cast<std::size_t>(event.type)];
    }
    std::cout << events.size() << " events: " << by_type[1] << " adds, " << by_type[2]
              << " reductions, " << by_type[3] << " deletions, " << by_type[4] << " executions, "
              << by_type[5] + by_type[6] + by_type[7] << " other\n";

    const bool matches = !book_path || check(events, band, *book_path);

    std::array<double, TIMED_RUNS> runs{};
    for (int i = 0; i < TIMED_RUNS; ++i) {
        // check() has printed the book's counts if there's an orderbook file
        runs[static_cast<std::size_t>(i)] =
            time_replay(events, band, book_path, !book_path && i == 0);
    }
    std::sort(runs.begin(), runs.end());
    std::cout << "replay: " << runs[TIMED_RUNS / 2] << " ns per event (median of " << TIMED_RUNS
              << " runs, best " << runs.front() << ")\n";
    return matches ? 0 : 1;
}
}  // namespace

int main(const int argc, const char* const argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: " << argv[0] << " <message.csv> [<orderbook.csv>]\n";
        return 2;
    }
    try {
        return run(argv[1], argc == 3 ? std::optional<std::string>(argv[2]) : std::nullopt);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    }
}

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
#include <utility>
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

// What a replay did with the events, and what it left in the book
struct Outcome {
    feed::Stats stats;
    std::size_t resting = 0;

    bool operator==(const Outcome&) const = default;
};

Outcome outcome_of(const feed::FeedBook& book) {
    return {.stats = book.stats(), .resting = book.book().size()};
}

void print(const Outcome& outcome) {
    const feed::Stats& stats = outcome.stats;
    std::cout << "applied " << stats.applied << ", ignored " << stats.ignored << ", unknown id "
              << stats.unknown << ", rejected " << stats.rejected << "; " << outcome.resting
              << " orders resting at the end\n";
}

struct Checked {
    bool matches;  // false if the book ever differed from the file
    Outcome outcome;
    // What the checker changed to keep the book in line with the file, for the timed runs
    feed::Corrections corrections;
};

Checked check(const std::vector<feed::Event>& events, const PriceBand band,
              const std::string& book_path) {
    std::ifstream in = open(book_path);
    feed::RowReader rows(in);
    feed::FeedBook book(MAX_ORDERS, band, TICK);
    feed::Corrections corrections;
    feed::Checker checker(book, &corrections);
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
    const Outcome outcome = outcome_of(book);
    print(outcome);
    return {.matches = checker.mismatches() == 0,
            .outcome = outcome,
            .corrections = std::move(corrections)};
}

struct Timed {
    double ns;  // for the whole replay
    Outcome outcome;
};

// The events from first on, with the corrections of a checked run in among them. Those are
// timed too: without them the book drifts from the file wherever a level changed out of view
Timed time_replay(const std::vector<feed::Event>& events, const PriceBand band,
                  const std::size_t first, const feed::Corrections& corrections) {
    feed::FeedBook book(MAX_ORDERS, band, TICK);
    const auto start = std::chrono::steady_clock::now();
    feed::replay(book, events, first, corrections);
    const std::chrono::duration<double, std::nano> elapsed =
        std::chrono::steady_clock::now() - start;
    return {.ns = elapsed.count(), .outcome = outcome_of(book)};
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

    const std::optional<Checked> checked =
        book_path ? std::optional<Checked>(check(events, band, *book_path)) : std::nullopt;
    const feed::Corrections none;
    const feed::Corrections& corrections = checked ? checked->corrections : none;
    // The first row of an orderbook file seeds the book, and it already holds the first event
    const std::size_t first = checked ? 1 : 0;
    // A step is an event, an order a correction takes out, or the level it leaves
    const std::size_t corrected = corrections.levels.size() + corrections.dropped.size();
    const auto steps = static_cast<double>(events.size() - first + corrected);

    std::array<double, TIMED_RUNS> runs{};
    for (int i = 0; i < TIMED_RUNS; ++i) {
        const Timed timed = time_replay(events, band, first, corrections);
        runs[static_cast<std::size_t>(i)] = timed.ns / steps;
        if (checked && timed.outcome != checked->outcome) {
            throw std::runtime_error("the timed replay left a different book from the checked one");
        }
        // check() has printed the book's counts if there's an orderbook file
        if (!checked && i == 0) {
            print(timed.outcome);
        }
    }
    std::sort(runs.begin(), runs.end());
    std::cout << "replay: " << events.size() - first << " events and " << corrected
              << " corrections, " << runs[TIMED_RUNS / 2] << " ns each (median of " << TIMED_RUNS
              << " runs, best " << runs.front() << ")\n";
    return !checked || checked->matches ? 0 : 1;
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

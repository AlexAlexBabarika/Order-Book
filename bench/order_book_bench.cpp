#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Workload.h"

namespace {
using namespace lob;

constexpr std::size_t WARM_UP = 100'000;
constexpr std::size_t MEASURED = 1'000'000;
constexpr std::uint64_t SEED = 42;

const std::vector<bench::Message>& workload() {
    static const std::vector<bench::Message> messages =
        bench::make_workload(WARM_UP + MEASURED, SEED);
    return messages;
}

// A fresh book that has replayed the warm-up part of the stream. Built outside the timed region.
std::unique_ptr<OrderBook> warmed_up_book(benchmark::State& state, auto& on_fill) {
    state.PauseTiming();
    auto book = std::make_unique<OrderBook>();
    for (std::size_t i = 0; i < WARM_UP; ++i) {
        bench::apply(*book, workload()[i], on_fill);
    }
    state.ResumeTiming();
    return book;
}

// Freeing ~10,000 orders isn't part of the workload, so it happens outside the timed region
void tear_down(benchmark::State& state, std::unique_ptr<OrderBook>& book) {
    state.PauseTiming();
    book.reset();
    state.ResumeTiming();
}

// The mean. Nothing but the book is in the timed loop.
void BM_Replay(benchmark::State& state) {
    const std::vector<bench::Message>& messages = workload();
    std::size_t fills = 0;
    auto on_fill = [&fills](const Fill&) { ++fills; };

    for (auto _ : state) {
        auto book = warmed_up_book(state, on_fill);
        for (std::size_t i = WARM_UP; i < messages.size(); ++i) {
            bench::apply(*book, messages[i], on_fill);
        }
        tear_down(state, book);
    }

    benchmark::DoNotOptimize(fills);
    state.counters["per_msg"] = benchmark::Counter(
        static_cast<double>(MEASURED),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

// The tail. Reading the clock around every message adds its own cost, so take the mean from
// BM_Replay. On Apple silicon steady_clock ticks every 41.67 ns, so these come in steps of that.
void BM_ReplayLatency(benchmark::State& state) {
    using Clock = std::chrono::steady_clock;
    const std::vector<bench::Message>& messages = workload();
    std::vector<std::int64_t> latencies(MEASURED);
    std::size_t fills = 0;
    auto on_fill = [&fills](const Fill&) { ++fills; };

    for (auto _ : state) {
        auto book = warmed_up_book(state, on_fill);
        for (std::size_t i = 0; i < MEASURED; ++i) {
            const auto start = Clock::now();
            bench::apply(*book, messages[WARM_UP + i], on_fill);
            latencies[i] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        }
        tear_down(state, book);
    }

    benchmark::DoNotOptimize(fills);
    // From the last iteration
    std::sort(latencies.begin(), latencies.end());
    const auto at = [&latencies](double q) {
        return static_cast<double>(latencies[static_cast<std::size_t>(q * (MEASURED - 1))]);
    };
    state.counters["p50_ns"] = at(0.50);
    state.counters["p99_ns"] = at(0.99);
    state.counters["p99.9_ns"] = at(0.999);
    state.counters["max_ns"] = at(1.0);
}

BENCHMARK(BM_Replay)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ReplayLatency)->Unit(benchmark::kMillisecond);
}  // namespace

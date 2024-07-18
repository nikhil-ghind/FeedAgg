/**
 * feed_simulator.cpp
 *
 * Simulates 15 exchange feeds.  Each feed runs in its own thread and generates
 * random ticks for 20 symbols at approximately 1000 ticks/second.  Raw JSON
 * bytes are written to a per-feed POSIX shared-memory ring buffer.
 */

#include "feed_agg/shm_channel.hpp"
#include "feed_agg/feed_simulator.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace feed {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static const std::vector<std::string> kExchangeNames = {
    "NASDAQ", "NYSE",  "CBOE",   "BATS",    "IEX",
    "EDGA",   "EDGX",  "BYX",    "BX",      "PHLX",
    "ARCA",   "AMEX",  "MIAX",   "GEMINI",  "C2"
};

static const std::vector<std::string> kSymbols = {
    "AAPL", "MSFT", "GOOGL", "AMZN", "META",
    "NVDA", "TSLA", "JPM",   "BAC",  "GS",
    "SPY",  "QQQ",  "IWM",   "VTI",  "GLD",
    "SLV",  "TLT",  "HYG",   "XLF",  "XLK"
};

static constexpr int    kTicksPerSec    = 1000;
static constexpr std::size_t kShmCapacity = 4096; // per feed ring buffer

// Baseline prices for each symbol
static const double kBasePrices[] = {
    182.0, 378.0, 140.0, 178.0,  480.0,
    580.0, 250.0, 198.0,  38.0,  380.0,
    480.0, 380.0, 195.0, 220.0,  185.0,
     24.0,  96.0,  77.0,  38.0,  185.0
};

// ─────────────────────────────────────────────────────────────────────────────
// FeedSimulator implementation
// ─────────────────────────────────────────────────────────────────────────────

FeedSimulator::FeedSimulator() = default;

FeedSimulator::~FeedSimulator() {
    stop();
}

void FeedSimulator::start() {
    running_.store(true, std::memory_order_release);

    for (std::size_t i = 0; i < kExchangeNames.size(); ++i) {
        const std::string& name = kExchangeNames[i];
        std::string shm_name = "feed_" + name;

        // Create SHM channel for this feed
        producers_.push_back(ShmProducer::create(shm_name, kShmCapacity));
        shm_names_.push_back(shm_name);

        // Launch feed thread
        threads_.emplace_back([this, i, name]() {
            run_feed(i, name);
        });
    }

    std::cout << "[FeedSimulator] Started " << kExchangeNames.size()
              << " exchange feeds\n";
}

void FeedSimulator::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    // Cleanup SHM
    for (auto& p : producers_) p.unlink();
    producers_.clear();
}

const std::vector<std::string>& FeedSimulator::shm_names() const {
    return shm_names_;
}

void FeedSimulator::run_feed(std::size_t feed_idx, const std::string& exchange) {
    std::mt19937_64 rng(std::hash<std::string>{}(exchange) ^ feed_idx);
    std::uniform_real_distribution<double> noise(-0.5, 0.5);
    std::uniform_real_distribution<double> qty_dist(1.0, 1000.0);
    std::uniform_int_distribution<int>     sym_dist(0, static_cast<int>(kSymbols.size()) - 1);
    std::uniform_int_distribution<int>     side_dist(0, 1);

    // Per-symbol mid prices (walk them randomly)
    std::vector<double> mid(kSymbols.size());
    for (std::size_t s = 0; s < kSymbols.size(); ++s)
        mid[s] = kBasePrices[s];

    const auto interval = std::chrono::microseconds(1'000'000 / kTicksPerSec);
    auto next_tick = std::chrono::steady_clock::now();

    ShmProducer& prod = producers_[feed_idx];

    while (running_.load(std::memory_order_relaxed)) {
        // Sleep until next tick
        std::this_thread::sleep_until(next_tick);
        next_tick += interval;

        // Pick a random symbol and update its mid price
        int sym_idx = sym_dist(rng);
        mid[sym_idx] += noise(rng);
        if (mid[sym_idx] <= 0.0) mid[sym_idx] = kBasePrices[sym_idx];

        double spread = mid[sym_idx] * 0.0002; // 2 bps spread
        bool is_ask   = side_dist(rng) == 1;
        double price  = is_ask ? mid[sym_idx] + spread / 2.0
                               : mid[sym_idx] - spread / 2.0;
        double qty    = std::round(qty_dist(rng));

        Tick t;
        std::strncpy(t.symbol, kSymbols[sym_idx].c_str(), sizeof(t.symbol) - 1);
        t.price        = price;
        t.qty          = qty;
        t.side         = is_ask ? 1 : 0;
        t.timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        // Write to SHM (non-blocking; drop on full buffer)
        prod.write(t);
    }
}

} // namespace feed

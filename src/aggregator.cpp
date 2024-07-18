/**
 * aggregator.cpp
 *
 * Core market-data aggregator:
 *   1. Opens per-feed SHM consumers.
 *   2. Parses incoming Ticks via parse_tick_simd.
 *   3. Merges updates into per-symbol consolidated OrderBooks.
 *   4. Publishes NBBO updates to a shared-memory channel named "nbbo".
 */

#include "feed_agg/order_book.hpp"
#include "feed_agg/shm_channel.hpp"
#include "feed_agg/simd_parser.hpp"
#include "feed_agg/aggregator.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace feed {

static constexpr std::size_t kNbboCap = 8192;

// ─────────────────────────────────────────────────────────────────────────────
// Aggregator implementation
// ─────────────────────────────────────────────────────────────────────────────

Aggregator::Aggregator(const std::vector<std::string>& feed_shm_names)
    : feed_names_(feed_shm_names)
    , nbbo_producer_(ShmProducer::create("nbbo", kNbboCap))
{}

Aggregator::~Aggregator() {
    stop();
    nbbo_producer_.unlink();
}

void Aggregator::start() {
    running_.store(true, std::memory_order_release);
    agg_thread_ = std::thread([this]() { run(); });
    std::cout << "[Aggregator] Started, reading from "
              << feed_names_.size() << " feeds\n";
}

void Aggregator::stop() {
    running_.store(false, std::memory_order_release);
    if (agg_thread_.joinable()) agg_thread_.join();
}

std::shared_ptr<OrderBook> Aggregator::book_for(const std::string& symbol) {
    std::lock_guard lock(books_mu_);
    auto it = books_.find(symbol);
    if (it == books_.end()) return nullptr;
    return it->second;
}

uint64_t Aggregator::ticks_processed() const {
    return ticks_processed_.load(std::memory_order_relaxed);
}

void Aggregator::run() {
    // Open SHM consumers for each feed
    std::vector<ShmConsumer> consumers;
    consumers.reserve(feed_names_.size());
    for (const auto& name : feed_names_) {
        try {
            consumers.push_back(ShmConsumer::open(name));
        } catch (const std::exception& e) {
            std::cerr << "[Aggregator] Warning: cannot open SHM '" << name
                      << "': " << e.what() << "\n";
        }
    }

    Tick t;
    while (running_.load(std::memory_order_relaxed)) {
        bool any = false;
        for (auto& c : consumers) {
            while (c.read(t)) {
                any = true;
                process_tick(t);
            }
        }
        if (!any) {
            struct timespec ts{0, 50'000}; // 50 µs
            nanosleep(&ts, nullptr);
        }
    }
}

void Aggregator::process_tick(const Tick& t) {
    ticks_processed_.fetch_add(1, std::memory_order_relaxed);

    std::string sym(t.symbol);
    if (sym.empty()) return;

    // Get or create order book
    std::shared_ptr<OrderBook> book;
    {
        std::lock_guard lock(books_mu_);
        auto& entry = books_[sym];
        if (!entry) entry = std::make_shared<OrderBook>(sym);
        book = entry;
    }

    // Apply the update
    Side side = (t.side == 0) ? Side::Bid : Side::Ask;
    book->apply_update(side, t.price, t.qty);

    // Compute new NBBO and publish
    BBO bbo = book->best_bid_offer();
    if (bbo.valid) {
        Tick nbbo_tick;
        std::strncpy(nbbo_tick.symbol, t.symbol, sizeof(nbbo_tick.symbol) - 1);
        nbbo_tick.price = bbo.bid_price; // use bid as representative price
        nbbo_tick.qty   = bbo.bid_qty;
        nbbo_tick.side  = 0; // bid side of NBBO
        nbbo_tick.timestamp_ns = t.timestamp_ns;
        nbbo_producer_.write(nbbo_tick);
    }
}

} // namespace feed

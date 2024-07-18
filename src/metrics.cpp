/**
 * metrics.cpp
 *
 * Tracks per-feed ticks/second, parse latency histogram, and book-update
 * latency.  Prints a summary every kReportIntervalSec seconds.
 */

#include "feed_agg/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace feed {

static constexpr int kReportIntervalSec = 5;

// ─────────────────────────────────────────────────────────────────────────────
// Histogram helpers
// ─────────────────────────────────────────────────────────────────────────────

static double percentile(std::vector<uint64_t>& v, double pct) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>(pct / 100.0 * (v.size() - 1));
    return static_cast<double>(v[idx]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Metrics impl
// ─────────────────────────────────────────────────────────────────────────────

Metrics::Metrics(std::size_t n_feeds) : n_feeds_(n_feeds) {
    per_feed_ticks_.resize(n_feeds, 0);
}

void Metrics::record_tick(std::size_t feed_idx) {
    if (feed_idx < per_feed_ticks_.size())
        per_feed_ticks_[feed_idx].fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_parse_latency_ns(uint64_t ns) {
    // Sample 1 in 10
    static thread_local uint64_t counter = 0;
    if (++counter % 10 == 0) {
        std::lock_guard lock(mu_);
        parse_lat_.push_back(ns);
    }
}

void Metrics::record_book_update_latency_ns(uint64_t ns) {
    static thread_local uint64_t counter = 0;
    if (++counter % 10 == 0) {
        std::lock_guard lock(mu_);
        book_lat_.push_back(ns);
    }
}

void Metrics::start_reporter() {
    reporter_ = std::thread([this]() {
        while (!stop_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(
                std::chrono::seconds(kReportIntervalSec));
            if (!stop_.load(std::memory_order_relaxed)) print_report();
        }
    });
}

void Metrics::stop_reporter() {
    stop_.store(true, std::memory_order_release);
    if (reporter_.joinable()) reporter_.join();
}

void Metrics::print_report() {
    std::lock_guard lock(mu_);

    std::cout << "\n────────────────────────────────────────────────\n"
              << "  FeedAgg Metrics Report\n"
              << "────────────────────────────────────────────────\n";

    uint64_t total = 0;
    for (std::size_t i = 0; i < n_feeds_; ++i) {
        uint64_t c = per_feed_ticks_[i].exchange(0, std::memory_order_relaxed);
        total += c;
        if (i < 15) {
            std::cout << "  Feed[" << std::setw(2) << i << "]: "
                      << c / kReportIntervalSec << " ticks/sec\n";
        }
    }
    std::cout << "  Total:    " << total / kReportIntervalSec << " ticks/sec\n";

    if (!parse_lat_.empty()) {
        std::cout << "\n  Parse latency (ns):\n"
                  << "    p50: " << percentile(parse_lat_, 50.0) << "\n"
                  << "    p95: " << percentile(parse_lat_, 95.0) << "\n"
                  << "    p99: " << percentile(parse_lat_, 99.0) << "\n";
        parse_lat_.clear();
    }

    if (!book_lat_.empty()) {
        std::cout << "\n  Book update latency (ns):\n"
                  << "    p50: " << percentile(book_lat_, 50.0) << "\n"
                  << "    p95: " << percentile(book_lat_, 95.0) << "\n"
                  << "    p99: " << percentile(book_lat_, 99.0) << "\n";
        book_lat_.clear();
    }

    std::cout << "────────────────────────────────────────────────\n\n";
}

} // namespace feed

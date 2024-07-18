/**
 * main.cpp — FeedAgg entry point
 *
 * Starts:
 *   1. FeedSimulator — 15 exchange threads writing to per-feed SHM channels
 *   2. Aggregator    — reads all feeds, merges order books, publishes NBBO
 *   3. Metrics       — periodic stats reporter
 *
 * Runs until SIGINT / SIGTERM.
 */

#include "feed_agg/aggregator.hpp"
#include "feed_agg/feed_simulator.hpp"
#include "feed_agg/metrics.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

static std::atomic<bool> g_running{true};

static void handle_sig(int) {
    g_running.store(false, std::memory_order_release);
}

int main() {
    std::signal(SIGINT,  handle_sig);
    std::signal(SIGTERM, handle_sig);

    std::cout << "╔══════════════════════════════════════╗\n"
              << "║  FeedAgg — Market Data Aggregator    ║\n"
              << "╚══════════════════════════════════════╝\n\n";

    // 1. Start feed simulator
    feed::FeedSimulator simulator;
    simulator.start();

    // Brief pause so SHM segments are visible to consumers
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 2. Start aggregator
    feed::Aggregator aggregator(simulator.shm_names());
    aggregator.start();

    // 3. Start metrics reporter
    feed::Metrics metrics(simulator.shm_names().size());
    metrics.start_reporter();

    std::cout << "Running... Press Ctrl+C to stop.\n\n";

    // Main loop: periodically print top-level stats
    auto t_last = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now     = std::chrono::steady_clock::now();
        double secs  = std::chrono::duration<double>(now - t_last).count();
        t_last       = now;

        uint64_t total = aggregator.ticks_processed();
        static uint64_t last_total = 0;
        uint64_t delta = total - last_total;
        last_total = total;

        std::cout << "[Main] Ticks processed: " << total
                  << " (~" << static_cast<uint64_t>(delta / secs) << "/sec)\n";
    }

    std::cout << "\nShutting down...\n";
    metrics.stop_reporter();
    aggregator.stop();
    simulator.stop();

    std::cout << "Total ticks aggregated: " << aggregator.ticks_processed() << "\n";
    return 0;
}

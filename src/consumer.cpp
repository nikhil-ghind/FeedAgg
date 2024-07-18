/**
 * consumer.cpp
 *
 * Example shared-memory consumer.  Opens the "nbbo" SHM channel written by
 * the aggregator and prints NBBO updates as they arrive.
 */

#include "feed_agg/shm_channel.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

static std::atomic<bool> g_running{true};

static void handle_sig(int) {
    g_running.store(false, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  handle_sig);
    std::signal(SIGTERM, handle_sig);

    std::string shm_name = "nbbo";
    if (argc > 1) shm_name = argv[1];

    feed::ShmConsumer consumer = feed::ShmConsumer::open(shm_name);
    std::cout << "[Consumer] Listening on SHM channel: " << shm_name << "\n";
    std::cout << std::left
              << std::setw(10) << "Symbol"
              << std::setw(14) << "Bid Price"
              << std::setw(12) << "Bid Qty"
              << "Timestamp (ns)\n"
              << std::string(56, '-') << "\n";

    uint64_t count   = 0;
    uint64_t dropped = 0;
    auto     t_start = std::chrono::steady_clock::now();
    auto     t_next_report = t_start + std::chrono::seconds(5);

    feed::Tick t;
    while (g_running.load(std::memory_order_relaxed)) {
        if (consumer.read(t)) {
            ++count;
            // Print at most 20 per second to keep output readable
            if (count % 50 == 0) {
                std::cout << std::left
                          << std::setw(10) << t.symbol
                          << std::setw(14) << std::fixed << std::setprecision(4) << t.price
                          << std::setw(12) << t.qty
                          << t.timestamp_ns << "\n";
            }
        } else {
            struct timespec ts{0, 100'000}; // 100 µs
            nanosleep(&ts, nullptr);
        }

        // Report stats every 5 seconds
        auto now = std::chrono::steady_clock::now();
        if (now >= t_next_report) {
            double elapsed = std::chrono::duration<double>(now - t_start).count();
            std::cout << "\n[Consumer] Stats: "
                      << count << " ticks received in "
                      << std::fixed << std::setprecision(1) << elapsed << "s  ("
                      << static_cast<uint64_t>(count / elapsed) << " ticks/sec)\n\n";
            t_next_report = now + std::chrono::seconds(5);
        }
    }

    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    std::cout << "\n[Consumer] Final: " << count << " ticks in "
              << std::fixed << std::setprecision(2) << elapsed << "s ("
              << static_cast<uint64_t>(count / elapsed) << " ticks/sec)\n";
    return 0;
}

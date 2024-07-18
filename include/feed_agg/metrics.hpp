#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace feed {

class Metrics {
public:
    explicit Metrics(std::size_t n_feeds);

    void record_tick(std::size_t feed_idx);
    void record_parse_latency_ns(uint64_t ns);
    void record_book_update_latency_ns(uint64_t ns);

    void start_reporter();
    void stop_reporter();
    void print_report();

private:
    std::size_t                          n_feeds_;
    std::vector<std::atomic<uint64_t>>   per_feed_ticks_;
    std::mutex                           mu_;
    std::vector<uint64_t>                parse_lat_;
    std::vector<uint64_t>                book_lat_;
    std::thread                          reporter_;
    std::atomic<bool>                    stop_{false};
};

} // namespace feed

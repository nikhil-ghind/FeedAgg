#pragma once

#include "feed_agg/order_book.hpp"
#include "feed_agg/shm_channel.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace feed {

class Aggregator {
public:
    explicit Aggregator(const std::vector<std::string>& feed_shm_names);
    ~Aggregator();

    Aggregator(const Aggregator&) = delete;
    Aggregator& operator=(const Aggregator&) = delete;

    void start();
    void stop();

    std::shared_ptr<OrderBook> book_for(const std::string& symbol);
    uint64_t ticks_processed() const;

private:
    void run();
    void process_tick(const Tick& t);

    std::vector<std::string>                              feed_names_;
    ShmProducer                                           nbbo_producer_;
    std::atomic<bool>                                     running_{false};
    std::thread                                           agg_thread_;
    std::mutex                                            books_mu_;
    std::unordered_map<std::string, std::shared_ptr<OrderBook>> books_;
    std::atomic<uint64_t>                                 ticks_processed_{0};
};

} // namespace feed

#pragma once

#include "feed_agg/shm_channel.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace feed {

class FeedSimulator {
public:
    FeedSimulator();
    ~FeedSimulator();

    FeedSimulator(const FeedSimulator&) = delete;
    FeedSimulator& operator=(const FeedSimulator&) = delete;

    void start();
    void stop();

    const std::vector<std::string>& shm_names() const;

private:
    void run_feed(std::size_t feed_idx, const std::string& exchange);

    std::atomic<bool>           running_{false};
    std::vector<std::thread>    threads_;
    std::vector<ShmProducer>    producers_;
    std::vector<std::string>    shm_names_;
};

} // namespace feed

#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace feed {

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

enum class Side { Bid, Ask };

struct Level {
    double price;
    double qty;
};

struct BBO {
    double bid_price{0.0};
    double bid_qty{0.0};
    double ask_price{0.0};
    double ask_qty{0.0};
    bool   valid{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// OrderBook
//
// Maintains bid and ask sides as price → qty maps.  Thread-safe via a
// shared_mutex: many readers can call best_bid_offer() / top_n() concurrently;
// updates via apply_update() take an exclusive write lock.
// ─────────────────────────────────────────────────────────────────────────────

class OrderBook {
public:
    // Bids: highest price first
    using BidMap = std::map<double, double, std::greater<double>>;
    // Asks: lowest price first
    using AskMap = std::map<double, double>;

    explicit OrderBook(std::string symbol = "") : symbol_(std::move(symbol)) {}

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    /**
     * Apply a price-level update.
     * qty == 0.0 → remove the level.
     * qty >  0.0 → set (or overwrite) the level.
     */
    void apply_update(Side side, double price, double qty) {
        std::unique_lock lock(mu_);
        if (side == Side::Bid) {
            if (qty == 0.0) {
                bids_.erase(price);
            } else {
                bids_[price] = qty;
            }
        } else {
            if (qty == 0.0) {
                asks_.erase(price);
            } else {
                asks_[price] = qty;
            }
        }
    }

    /**
     * Return the best bid and best ask with their quantities.
     * Returns BBO{valid=false} if either side is empty.
     */
    BBO best_bid_offer() const {
        std::shared_lock lock(mu_);
        BBO bbo;
        if (!bids_.empty() && !asks_.empty()) {
            auto bid_it = bids_.begin();
            auto ask_it = asks_.begin();
            bbo.bid_price = bid_it->first;
            bbo.bid_qty   = bid_it->second;
            bbo.ask_price = ask_it->first;
            bbo.ask_qty   = ask_it->second;
            bbo.valid     = true;
        }
        return bbo;
    }

    /**
     * Return the top N price levels for the given side.
     * If fewer than N levels exist, returns all of them.
     */
    std::vector<Level> top_n(Side side, int n) const {
        std::shared_lock lock(mu_);
        std::vector<Level> result;
        result.reserve(static_cast<std::size_t>(n));

        if (side == Side::Bid) {
            for (auto it = bids_.begin(); it != bids_.end() && n-- > 0; ++it) {
                result.push_back({it->first, it->second});
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && n-- > 0; ++it) {
                result.push_back({it->first, it->second});
            }
        }
        return result;
    }

    std::size_t bid_depth() const {
        std::shared_lock lock(mu_);
        return bids_.size();
    }

    std::size_t ask_depth() const {
        std::shared_lock lock(mu_);
        return asks_.size();
    }

    const std::string& symbol() const noexcept { return symbol_; }

private:
    mutable std::shared_mutex mu_;
    BidMap      bids_;
    AskMap      asks_;
    std::string symbol_;
};

} // namespace feed

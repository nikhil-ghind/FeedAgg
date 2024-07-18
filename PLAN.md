# Feed Agg

## Project Overview
Scalable C++ market-data aggregator processing real-time feeds from 15+ exchanges. Multi-threaded architecture with SIMD optimizations for parsing and shared-memory IPC for low-latency distribution to downstream services. Designed for sub-millisecond processing latency per market data update.

## Tech Stack
- **Language:** C++17/C++20
- **Networking:** Boost.Asio (async TCP/UDP), raw sockets
- **SIMD:** SSE4.2/AVX2 intrinsics
- **IPC:** POSIX shared memory (`shm_open`), memory-mapped files
- **Build:** CMake 3.20+
- **Testing:** Google Test, Google Benchmark
- **Serialization:** FlatBuffers (zero-copy)
- **Profiling:** perf, Intel VTune

## Architecture Overview
```
┌──────────────────────────────────────────────────────────────┐
│                    Feed Handlers (per exchange)               │
│   ┌─────────┐ ┌─────────┐ ┌─────────┐      ┌─────────┐     │
│   │ NYSE    │ │ NASDAQ  │ │ CME     │ ...  │ Exchange │     │
│   │ Handler │ │ Handler │ │ Handler │      │ N        │     │
│   └────┬────┘ └────┬────┘ └────┬────┘      └────┬────┘     │
│        └────────────┴──────────┴─────────────────┘          │
│                           │                                  │
│                    ┌──────▼───────┐                          │
│                    │ Normalizer   │  (SIMD-optimized parse)  │
│                    │ + Aggregator │                          │
│                    └──────┬───────┘                          │
│                           │                                  │
│                    ┌──────▼───────┐                          │
│                    │ Book Builder │  (L2 order book)         │
│                    └──────┬───────┘                          │
│                           │                                  │
│                    ┌──────▼───────┐                          │
│                    │ Shared Memory│  (IPC to consumers)      │
│                    │ Publisher    │                          │
│                    └──────────────┘                          │
└──────────────────────────────────────────────────────────────┘
```

## Phase 1: Core Data Structures & Feed Protocol
**Goal:** Define market data types and a simulated exchange feed protocol.

### Tasks
1. Project structure:
   ```
   cppMarketDataAggregator/
   ├── CMakeLists.txt
   ├── include/
   │   ├── types.h              # MarketUpdate, OrderBookLevel, NBBO
   │   ├── feed_handler.h       # Abstract FeedHandler
   │   ├── parser.h             # SIMD-optimized message parser
   │   ├── order_book.h         # L2 OrderBook
   │   ├── aggregator.h         # Multi-exchange aggregator
   │   ├── shm_publisher.h      # Shared memory publisher
   │   └── config.h             # Configuration
   ├── src/
   │   ├── feed_handler.cpp
   │   ├── parser.cpp
   │   ├── order_book.cpp
   │   ├── aggregator.cpp
   │   └── shm_publisher.cpp
   ├── feeds/
   │   ├── simulated_feed.h     # Simulated exchange feed generator
   │   └── simulated_feed.cpp
   ├── consumers/
   │   └── shm_consumer.cpp     # Example shared memory consumer
   ├── tests/
   │   ├── test_order_book.cpp
   │   ├── test_parser.cpp
   │   └── test_aggregator.cpp
   ├── benchmarks/
   │   ├── bench_parser.cpp
   │   └── bench_aggregator.cpp
   └── tools/
       ├── feed_simulator.cpp   # Multi-exchange feed simulator
       └── latency_checker.cpp  # End-to-end latency measurement
   ```
2. `include/types.h`:
   ```cpp
   struct MarketUpdate {
       uint64_t timestamp_ns;
       uint32_t exchange_id;
       char symbol[8];          // fixed-size, null-padded
       enum class Type : uint8_t { TRADE, QUOTE, BBO_UPDATE } type;
       double bid_price, ask_price;
       uint32_t bid_size, ask_size;
       double last_price;
       uint32_t last_size;
   };

   struct OrderBookLevel {
       double price;
       uint32_t size;
       uint32_t order_count;
   };

   struct NBBO {  // National Best Bid/Offer
       char symbol[8];
       double best_bid, best_ask;
       uint32_t best_bid_size, best_ask_size;
       uint32_t best_bid_exchange, best_ask_exchange;
       uint64_t timestamp_ns;
   };
   ```
3. `feeds/simulated_feed.h`:
   - `class SimulatedFeed`:
     - Generates realistic market data: random walks for prices, Poisson-distributed trade arrivals
     - Configurable: symbols, update rate (msg/sec), trade/quote ratio
     - `void start(std::function<void(const uint8_t*, size_t)> callback)` — calls callback with raw feed bytes
     - Sends data over UDP to specified port
4. Wire protocol: simple binary format — 2-byte length prefix + MarketUpdate struct (fixed size for SIMD alignment)

## Phase 2: SIMD-Optimized Parser
**Goal:** Parse raw exchange feed bytes into MarketUpdate structs using SIMD intrinsics.

### Tasks
1. `include/parser.h` / `src/parser.cpp`:
   - `class FeedParser`:
     - `size_t parse_batch(const uint8_t* data, size_t len, MarketUpdate* out, size_t max_out)` — returns number of updates parsed
     - SIMD implementation for fixed-format messages:
       - Use `_mm256_loadu_si256` to load 32 bytes at a time
       - Use `_mm_cmpeq_epi8` for delimiter/header scanning
       - Vectorized byte-to-double conversion for price fields
     - Fallback scalar implementation for non-SIMD platforms
   - Compile-time dispatch: `#ifdef __AVX2__` for AVX2 path, `#ifdef __SSE4_2__` for SSE path
2. `class PriceParser`:
   - `static double parse_price_simd(const char* str, size_t len)` — SIMD fixed-point decimal parsing
   - `static void parse_prices_batch(const char** strs, double* out, size_t count)` — batch conversion
3. Benchmarks (`benchmarks/bench_parser.cpp`):
   - Compare SIMD vs scalar parse throughput (messages/sec)
   - Measure at various message sizes and batch sizes
   - Target: 10M+ messages/sec parse throughput
4. Tests:
   - Parse known message → verify all fields match expected values
   - Parse batch of 1000 messages → verify all parsed correctly
   - Malformed message handling: truncated, invalid fields

## Phase 3: Order Book & Aggregator
**Goal:** Build L2 order book per symbol per exchange and aggregate NBBO across exchanges.

### Tasks
1. `include/order_book.h` / `src/order_book.cpp`:
   - `class OrderBook`:
     - `OrderBook(const std::string& symbol, uint32_t exchange_id, size_t max_levels = 10)`
     - `void apply_update(const MarketUpdate& update)` — insert/update/delete price level
     - `OrderBookLevel get_best_bid() const` / `get_best_ask() const`
     - `std::vector<OrderBookLevel> get_bids(size_t depth) const` / `get_asks(size_t depth)`
     - `NBBO get_nbbo() const`
   - Internal: `std::map<double, OrderBookLevel, std::greater<>>` for bids (descending), `std::map<double, OrderBookLevel>` for asks (ascending)
   - Or flat sorted array for L2 (better cache performance at small depths)
2. `include/aggregator.h` / `src/aggregator.cpp`:
   - `class MarketDataAggregator`:
     - `MarketDataAggregator(std::vector<std::string> symbols, size_t num_exchanges)`
     - `void on_update(const MarketUpdate& update)` — routes to correct OrderBook, recomputes NBBO
     - `NBBO get_nbbo(const std::string& symbol) const` — best bid/ask across all exchanges
     - `void register_nbbo_callback(std::function<void(const NBBO&)> cb)` — called when NBBO changes
   - Internal: `std::unordered_map<std::string, std::array<OrderBook, MAX_EXCHANGES>> books_`
3. Tests:
   - Apply sequence of updates → verify book state matches expected
   - NBBO: best bid from exchange A, best ask from exchange B
   - NBBO callback fires only when NBBO actually changes

## Phase 4: Feed Handlers & Networking
**Goal:** Implement per-exchange feed handlers with async I/O.

### Tasks
1. `include/feed_handler.h` / `src/feed_handler.cpp`:
   - `class FeedHandler`:
     - `FeedHandler(boost::asio::io_context& io, const FeedConfig& config, MarketDataAggregator& aggregator)`
     - `void connect()` — async UDP bind or TCP connect to exchange feed
     - `void start()` — begin async_receive loop
     - Internal: `async_receive` → parse batch → `aggregator.on_update()` for each
     - Reconnection logic with exponential backoff
   - `struct FeedConfig { std::string exchange_name; std::string host; uint16_t port; Protocol protocol; /* UDP/TCP */ }`
2. Create concrete feed handlers for simulated exchanges:
   - `class SimulatedFeedHandler : public FeedHandler` — connects to local SimulatedFeed
3. Thread model:
   - One `io_context` per feed handler thread
   - Pin each feed thread to a dedicated CPU core
   - Feed handler threads write to thread-safe aggregator
4. `src/main.cpp`:
   - Load config (JSON): list of exchanges with connection details
   - Create `io_context` per feed
   - Create `FeedHandler` per exchange
   - Start all handlers, run event loops
5. Integration test: start 4 simulated feeds → verify aggregator receives from all, NBBO updates correctly

## Phase 5: Shared Memory Publisher & Consumer
**Goal:** Publish aggregated market data to downstream services via shared memory IPC.

### Tasks
1. `include/shm_publisher.h` / `src/shm_publisher.cpp`:
   - `class SharedMemoryPublisher`:
     - `SharedMemoryPublisher(const std::string& shm_name, size_t ring_buffer_size)`
     - `void publish(const NBBO& nbbo)` — writes to shared memory ring buffer
     - Uses `shm_open`, `mmap`, `ftruncate` for POSIX shared memory
     - Ring buffer header: `{ atomic<uint64_t> write_pos; atomic<uint64_t> read_pos; }`
     - Zero-copy: writes NBBO struct directly into mapped memory
   - Memory layout: header (cache-line aligned) + ring buffer of fixed-size NBBO entries
2. `consumers/shm_consumer.cpp`:
   - `class SharedMemoryConsumer`:
     - Attaches to same shared memory segment
     - Spin-waits on `write_pos` advancement
     - Reads NBBO entries without locks
   - Example consumer binary: prints NBBO updates to stdout with latency measurement
3. `tools/latency_checker.cpp`:
   - Measures end-to-end latency: feed timestamp → consumer read timestamp
   - Histogram output: min, p50, p95, p99, max
4. Tests:
   - Publisher writes 1M entries, consumer reads all — verify no loss
   - Multi-consumer: 4 consumers read independently from same shm
5. Final benchmark:
   - 15 simulated feeds → aggregator → shm publisher → consumer
   - Measure throughput (updates/sec) and latency distribution
   - Target: < 1ms end-to-end p99

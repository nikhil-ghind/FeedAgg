#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace feed {

// ─────────────────────────────────────────────────────────────────────────────
// Tick — the unit of data exchanged over shared memory
// ─────────────────────────────────────────────────────────────────────────────

struct Tick {
    char     symbol[16]{};
    double   price{0.0};
    double   qty{0.0};
    int      side{0}; // 0 = bid, 1 = ask
    uint64_t timestamp_ns{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// Shared memory ring-buffer layout
//
//  ┌────────────────────────────────────────────────────────────┐
//  │  ShmHeader (64 bytes, cache-line aligned)                  │
//  │    head : atomic<uint64_t>  – next write position          │
//  │    tail : atomic<uint64_t>  – next read position           │
//  │    capacity : uint64_t                                     │
//  ├────────────────────────────────────────────────────────────┤
//  │  Slot[0] … Slot[capacity-1]                                │
//  │    Each slot: sequence (atomic<uint64_t>) + Tick data      │
//  └────────────────────────────────────────────────────────────┘
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(64) ShmHeader {
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    uint64_t              capacity{0};
    uint8_t               _pad[64 - 3 * sizeof(uint64_t)];
};
static_assert(sizeof(ShmHeader) == 64, "ShmHeader must be 64 bytes");

struct alignas(64) ShmSlot {
    std::atomic<uint64_t> sequence{0};
    Tick                  data;
    // Pad to cache-line boundary (Tick ≈ 48 bytes; seq = 8 bytes → 56 total → pad 8)
    uint8_t _pad[64 - sizeof(std::atomic<uint64_t>) - sizeof(Tick)];
};

static constexpr std::size_t kShmSlotSize = sizeof(ShmSlot);

inline std::size_t shm_total_size(uint64_t capacity) {
    return sizeof(ShmHeader) + capacity * sizeof(ShmSlot);
}

// ─────────────────────────────────────────────────────────────────────────────
// ShmProducer
// ─────────────────────────────────────────────────────────────────────────────

class ShmProducer {
public:
    ShmProducer() = default;
    ~ShmProducer() { close(); }

    ShmProducer(const ShmProducer&) = delete;
    ShmProducer& operator=(const ShmProducer&) = delete;

    /**
     * Create (or re-create) a shared memory segment of given capacity.
     * capacity must be a power of two.
     */
    static ShmProducer create(const std::string& name, uint64_t capacity) {
        if ((capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("capacity must be power of two");

        std::string shm_name = "/" + name;
        std::size_t size = shm_total_size(capacity);

        int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (fd < 0) throw std::runtime_error("shm_open failed: " + shm_name);

        if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
            ::close(fd);
            throw std::runtime_error("ftruncate failed");
        }

        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);
        if (ptr == MAP_FAILED) throw std::runtime_error("mmap failed");

        // Initialise header
        auto* hdr = new (ptr) ShmHeader{};
        hdr->capacity.store(capacity, std::memory_order_relaxed);

        // Initialise slots
        auto* slots = reinterpret_cast<ShmSlot*>(
            static_cast<char*>(ptr) + sizeof(ShmHeader));
        for (uint64_t i = 0; i < capacity; ++i) {
            new (&slots[i]) ShmSlot{};
            slots[i].sequence.store(i, std::memory_order_relaxed);
        }

        ShmProducer p;
        p.ptr_      = ptr;
        p.size_     = size;
        p.capacity_ = capacity;
        p.mask_     = capacity - 1;
        p.name_     = shm_name;
        p.header_   = hdr;
        p.slots_    = slots;
        return p;
    }

    /**
     * Write a Tick into the ring buffer.
     * Returns false if the buffer is full (non-blocking).
     */
    bool write(const Tick& t) noexcept {
        uint64_t head = header_->head.load(std::memory_order_relaxed);
        for (;;) {
            ShmSlot& slot = slots_[head & mask_];
            uint64_t seq  = slot.sequence.load(std::memory_order_acquire);
            int64_t  diff = static_cast<int64_t>(seq) - static_cast<int64_t>(head);
            if (diff == 0) {
                if (header_->head.compare_exchange_weak(head, head + 1,
                                                        std::memory_order_relaxed)) {
                    std::memcpy(&slot.data, &t, sizeof(Tick));
                    slot.sequence.store(head + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                head = header_->head.load(std::memory_order_relaxed);
            }
        }
    }

    void close() {
        if (ptr_ && ptr_ != MAP_FAILED) {
            munmap(ptr_, size_);
            ptr_ = nullptr;
        }
    }

    void unlink() {
        if (!name_.empty()) shm_unlink(name_.c_str());
    }

private:
    void*       ptr_{nullptr};
    std::size_t size_{0};
    uint64_t    capacity_{0};
    uint64_t    mask_{0};
    std::string name_;
    ShmHeader*  header_{nullptr};
    ShmSlot*    slots_{nullptr};
};

// ─────────────────────────────────────────────────────────────────────────────
// ShmConsumer
// ─────────────────────────────────────────────────────────────────────────────

class ShmConsumer {
public:
    ShmConsumer() = default;
    ~ShmConsumer() { close(); }

    ShmConsumer(const ShmConsumer&) = delete;
    ShmConsumer& operator=(const ShmConsumer&) = delete;

    /**
     * Open an existing shared memory segment by name.
     */
    static ShmConsumer open(const std::string& name) {
        std::string shm_name = "/" + name;

        int fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
        if (fd < 0) throw std::runtime_error("shm_open failed: " + shm_name);

        // Read header to get capacity
        ShmHeader tmp_hdr;
        if (pread(fd, &tmp_hdr, sizeof(ShmHeader), 0) < 0) {
            ::close(fd);
            throw std::runtime_error("pread header failed");
        }
        uint64_t capacity = tmp_hdr.capacity.load(std::memory_order_relaxed);
        std::size_t size  = shm_total_size(capacity);

        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);
        if (ptr == MAP_FAILED) throw std::runtime_error("mmap failed");

        ShmConsumer c;
        c.ptr_      = ptr;
        c.size_     = size;
        c.capacity_ = capacity;
        c.mask_     = capacity - 1;
        c.header_   = reinterpret_cast<ShmHeader*>(ptr);
        c.slots_    = reinterpret_cast<ShmSlot*>(
            static_cast<char*>(ptr) + sizeof(ShmHeader));
        // Consumer tracks its own local tail
        c.local_tail_ = c.header_->tail.load(std::memory_order_relaxed);
        return c;
    }

    /**
     * Read the next Tick from the ring buffer.
     * Returns false if no new data is available (non-blocking).
     */
    bool read(Tick& t) noexcept {
        uint64_t tail = local_tail_;
        ShmSlot& slot = slots_[tail & mask_];
        uint64_t seq  = slot.sequence.load(std::memory_order_acquire);
        int64_t  diff = static_cast<int64_t>(seq) - static_cast<int64_t>(tail + 1);
        if (diff == 0) {
            std::memcpy(&t, &slot.data, sizeof(Tick));
            slot.sequence.store(tail + capacity_, std::memory_order_release);
            // Advance global tail atomically then local
            header_->tail.fetch_add(1, std::memory_order_relaxed);
            ++local_tail_;
            return true;
        }
        return false;
    }

    void close() {
        if (ptr_ && ptr_ != MAP_FAILED) {
            munmap(ptr_, size_);
            ptr_ = nullptr;
        }
    }

private:
    void*       ptr_{nullptr};
    std::size_t size_{0};
    uint64_t    capacity_{0};
    uint64_t    mask_{0};
    ShmHeader*  header_{nullptr};
    ShmSlot*    slots_{nullptr};
    uint64_t    local_tail_{0};
};

} // namespace feed

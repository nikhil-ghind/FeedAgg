#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <charconv>

// Detect SIMD support at compile time
#if defined(__SSE4_2__)
#  include <nmmintrin.h>
#  define FEED_HAS_SSE42 1
#else
#  define FEED_HAS_SSE42 0
#endif

#if defined(__AVX2__)
#  include <immintrin.h>
#  define FEED_HAS_AVX2 1
#else
#  define FEED_HAS_AVX2 0
#endif

#include "order_book.hpp" // for feed::Side

namespace feed {

// ─────────────────────────────────────────────────────────────────────────────
// Output struct
// ─────────────────────────────────────────────────────────────────────────────

struct ParsedTick {
    char   symbol[16]{};
    double price{0.0};
    double qty{0.0};
    Side   side{Side::Bid};
    bool   valid{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// Find first occurrence of needle (<=16 bytes) in buf using SSE4.2 PCMPESTRI.
// Returns pointer to match, or nullptr.
#if FEED_HAS_SSE42
inline const char* simd_find_str_sse42(const char* buf, std::size_t buf_len,
                                       const char* needle, int needle_len) {
    if (buf_len < static_cast<std::size_t>(needle_len)) return nullptr;
    __m128i n128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(needle));
    for (std::size_t i = 0; i + 16 <= buf_len; i += 16) {
        __m128i b128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + i));
        // PCMPESTRI: explicit lengths, find equal ordered substring
        int idx = _mm_cmpestri(n128, needle_len, b128, 16,
                               _SIDD_CMP_EQUAL_ORDERED | _SIDD_UBYTE_OPS);
        if (idx < 16) {
            // Verify we didn't straddle a boundary
            if (i + idx + static_cast<std::size_t>(needle_len) <= buf_len) {
                if (std::memcmp(buf + i + idx, needle, needle_len) == 0)
                    return buf + i + idx;
            }
        }
    }
    // Scalar fallback for tail
    const char* end = buf + buf_len - needle_len + 1;
    for (const char* p = buf; p < end; ++p) {
        if (std::memcmp(p, needle, needle_len) == 0) return p;
    }
    return nullptr;
}
#endif // FEED_HAS_SSE42

// Scalar fallback: find needle in buf
inline const char* scalar_find_str(const char* buf, std::size_t buf_len,
                                   const char* needle, std::size_t needle_len) {
    if (buf_len < needle_len) return nullptr;
    const char* end = buf + buf_len - needle_len + 1;
    for (const char* p = buf; p < end; ++p) {
        if (std::memcmp(p, needle, needle_len) == 0) return p;
    }
    return nullptr;
}

// Parse a JSON string value starting just after the opening '"'
inline std::size_t parse_json_string(const char* p, std::size_t max_len,
                                     char* out, std::size_t out_cap) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < max_len && n < out_cap - 1; ++i) {
        if (p[i] == '"') break;
        out[n++] = p[i];
    }
    out[n] = '\0';
    return n;
}

// Parse a JSON number (double) starting at p
inline bool parse_double(const char* p, std::size_t max_len, double& out) {
    // Skip whitespace
    while (max_len > 0 && (*p == ' ' || *p == '\t')) { ++p; --max_len; }
    auto [endp, ec] = std::from_chars(p, p + max_len, out);
    return ec == std::errc{};
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Parse a JSON market-data tick message.
 *
 * Expected JSON format (field order flexible, extra fields ignored):
 *   {"symbol":"AAPL","price":182.34,"qty":100.0,"side":"bid"}
 *
 * When SSE4.2 is available the price/qty field markers are located using
 * PCMPESTRI; otherwise falls back to a scalar byte-scan.
 *
 * Returns a ParsedTick with valid=true on success.
 */
inline ParsedTick parse_tick_simd(const char* buf, std::size_t len) {
    ParsedTick tick;

    if (!buf || len == 0) return tick;

    // ── Locate "symbol" ──────────────────────────────────────────────────────
    static const char kSymbol[] = "\"symbol\":\"";
    const char* sym_ptr = nullptr;
#if FEED_HAS_SSE42
    sym_ptr = detail::simd_find_str_sse42(buf, len, kSymbol, 10);
#else
    sym_ptr = detail::scalar_find_str(buf, len, kSymbol, 10);
#endif
    if (sym_ptr) {
        const char* val = sym_ptr + 10;
        std::size_t rem = len - static_cast<std::size_t>(val - buf);
        detail::parse_json_string(val, rem, tick.symbol, sizeof(tick.symbol));
    }

    // ── Locate "price" ───────────────────────────────────────────────────────
    static const char kPrice[] = "\"price\":";
    const char* price_ptr = nullptr;
#if FEED_HAS_SSE42
    price_ptr = detail::simd_find_str_sse42(buf, len, kPrice, 8);
#else
    price_ptr = detail::scalar_find_str(buf, len, kPrice, 8);
#endif
    if (price_ptr) {
        const char* val = price_ptr + 8;
        std::size_t rem = len - static_cast<std::size_t>(val - buf);
        detail::parse_double(val, rem, tick.price);
    } else {
        return tick; // price is mandatory
    }

    // ── Locate "qty" ─────────────────────────────────────────────────────────
    static const char kQty[] = "\"qty\":";
    const char* qty_ptr = nullptr;
#if FEED_HAS_SSE42
    qty_ptr = detail::simd_find_str_sse42(buf, len, kQty, 6);
#else
    qty_ptr = detail::scalar_find_str(buf, len, kQty, 6);
#endif
    if (qty_ptr) {
        const char* val = qty_ptr + 6;
        std::size_t rem = len - static_cast<std::size_t>(val - buf);
        detail::parse_double(val, rem, tick.qty);
    } else {
        return tick; // qty is mandatory
    }

    // ── Locate "side" ────────────────────────────────────────────────────────
    static const char kSide[] = "\"side\":\"";
    const char* side_ptr = nullptr;
#if FEED_HAS_SSE42
    side_ptr = detail::simd_find_str_sse42(buf, len, kSide, 8);
#else
    side_ptr = detail::scalar_find_str(buf, len, kSide, 8);
#endif
    if (side_ptr) {
        const char* val = side_ptr + 8;
        // "ask" or "bid"
        if (val < buf + len && val[0] == 'a') {
            tick.side = Side::Ask;
        } else {
            tick.side = Side::Bid;
        }
    }

    tick.valid = (tick.price > 0.0 && tick.qty > 0.0);
    return tick;
}

} // namespace feed

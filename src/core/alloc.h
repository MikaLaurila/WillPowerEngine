/**
 * @file alloc.h
 * @brief Tracked heap allocation. All engine heap memory goes through these functions.
 *
 * Every allocation carries a small header recording its size, which feeds global statistics used
 * for leak detection (the test runner fails any test that changes the live allocation count) and
 * memory budgeting. Returned memory is aligned to alignof(max_align_t).
 *
 * Ownership: a pointer returned by wpe_alloc*() / wpe_realloc() is owned by the caller and must be
 * released with wpe_free() exactly once. Never mix with malloc()/free().
 *
 * - Dependencies: core/log, core/debug
 * - Thread affinity: any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_ALLOC_H
#define WPE_CORE_ALLOC_H

#include <stddef.h>
#include <stdint.h>

/** Snapshot of global allocation counters. */
typedef struct WPE_AllocStats {
    uint64_t live_allocations;  /* Allocations not yet freed. */
    uint64_t live_bytes;        /* User-requested bytes not yet freed (headers excluded). */
    uint64_t peak_live_bytes;   /* Highest live_bytes observed since process start. */
    uint64_t total_allocations; /* Allocations ever made, including reallocs that moved. */
} WPE_AllocStats;

/**
 * Allocates uninitialized memory.
 *
 * @param size Byte count. Zero returns NULL without allocating.
 * @return Owned pointer, or NULL on failure (an ERROR is logged).
 *
 * @thread_safety Any thread
 */
[[nodiscard]] void* wpe_alloc(size_t size);

/**
 * Allocates zero-initialized memory for `count` elements of `size` bytes.
 *
 * @return Owned pointer, or NULL if either argument is zero, the product overflows, or
 *         allocation fails (the latter two log an ERROR).
 *
 * @thread_safety Any thread
 */
[[nodiscard]] void* wpe_alloc_zeroed(size_t count, size_t size);

/**
 * Resizes an allocation, preserving its contents up to the smaller of the old and new sizes.
 *
 * @param ptr Owned pointer from wpe_alloc*(), or NULL (behaves like wpe_alloc()). On success
 *            ownership moves to the returned pointer; on failure `ptr` remains valid and owned.
 * @param new_size New byte count. Zero frees `ptr` and returns NULL.
 * @return Owned pointer, or NULL on failure (an ERROR is logged) or when new_size is zero.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] void* wpe_realloc(void* ptr, size_t new_size);

/**
 * Releases memory from wpe_alloc*() / wpe_realloc().
 *
 * @param ptr Owned pointer, or NULL (no-op). Ownership is consumed.
 *
 * @thread_safety Any thread
 */
void wpe_free(void* ptr);

/**
 * Reads the global allocation counters.
 *
 * @param out_stats Receives the snapshot; must not be NULL. Counters are read individually, so a
 *                  snapshot taken while other threads allocate may be slightly inconsistent.
 *
 * @thread_safety Any thread
 */
void wpe_alloc_get_stats(WPE_AllocStats* out_stats);

#endif /* WPE_CORE_ALLOC_H */

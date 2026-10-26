#define WPE_LOG_MODULE "core"
#include "core/alloc.h"

#include <stdatomic.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/debug.h"
#include "core/log.h"

/* Header placed in front of every allocation; padded so user memory keeps max_align_t alignment. */
typedef struct wpe__AllocHeader {
    alignas(max_align_t) size_t size;
    uint32_t magic;
} wpe__AllocHeader;

static_assert(sizeof(wpe__AllocHeader) % alignof(max_align_t) == 0,
              "header must preserve max_align_t alignment of user memory");

/* Distinct live/freed markers let debug builds catch double frees and foreign pointers. */
#define WPE__ALLOC_MAGIC_LIVE 0x57504541u  /* "WPEA" */
#define WPE__ALLOC_MAGIC_FREED 0x46524545u /* "FREE" */

static struct {
    atomic_uint_fast64_t live_allocations;
    atomic_uint_fast64_t live_bytes;
    atomic_uint_fast64_t peak_live_bytes;
    atomic_uint_fast64_t total_allocations;
} wpe__g_alloc_stats;

static wpe__AllocHeader* wpe__alloc_header_of(void* ptr) {
    wpe__AllocHeader* header = (wpe__AllocHeader*)ptr - 1;
    wpe_assert(header->magic == WPE__ALLOC_MAGIC_LIVE);
    return header;
}

static void wpe__alloc_track_add(size_t size) {
    atomic_fetch_add_explicit(&wpe__g_alloc_stats.live_allocations, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&wpe__g_alloc_stats.total_allocations, 1, memory_order_relaxed);
    const uint64_t live =
        atomic_fetch_add_explicit(&wpe__g_alloc_stats.live_bytes, size, memory_order_relaxed) + size;

    uint64_t peak = atomic_load_explicit(&wpe__g_alloc_stats.peak_live_bytes, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&wpe__g_alloc_stats.peak_live_bytes, &peak, live,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void wpe__alloc_track_remove(size_t size) {
    atomic_fetch_sub_explicit(&wpe__g_alloc_stats.live_allocations, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&wpe__g_alloc_stats.live_bytes, size, memory_order_relaxed);
}

static bool wpe__alloc_total_size(size_t size, size_t* out_total) {
    if (ckd_add(out_total, size, sizeof(wpe__AllocHeader))) {
        wpe_log_error("allocation of %zu bytes overflows size_t", size);
        return false;
    }
    return true;
}

void* wpe_alloc(size_t size) {
    if (size == 0) return NULL;

    size_t total;
    if (!wpe__alloc_total_size(size, &total)) return NULL;

    wpe__AllocHeader* header = malloc(total);
    if (!header) {
        wpe_log_error("out of memory allocating %zu bytes", size);
        return NULL;
    }
    header->size = size;
    header->magic = WPE__ALLOC_MAGIC_LIVE;
    wpe__alloc_track_add(size);
    return header + 1;
}

void* wpe_alloc_zeroed(size_t count, size_t size) {
    size_t bytes;
    if (ckd_mul(&bytes, count, size)) {
        wpe_log_error("zeroed allocation of %zu x %zu bytes overflows size_t", count, size);
        return NULL;
    }
    void* ptr = wpe_alloc(bytes);
    if (ptr) memset(ptr, 0, bytes);
    return ptr;
}

void* wpe_realloc(void* ptr, size_t new_size) {
    if (!ptr) return wpe_alloc(new_size);
    if (new_size == 0) {
        wpe_free(ptr);
        return NULL;
    }

    size_t total;
    if (!wpe__alloc_total_size(new_size, &total)) return NULL;

    wpe__AllocHeader* header = wpe__alloc_header_of(ptr);
    const size_t old_size = header->size;
    wpe__AllocHeader* resized = realloc(header, total);
    if (!resized) {
        wpe_log_error("out of memory reallocating %zu -> %zu bytes", old_size, new_size);
        return NULL;
    }
    resized->size = new_size;
    wpe__alloc_track_remove(old_size);
    wpe__alloc_track_add(new_size);
    return resized + 1;
}

void wpe_free(void* ptr) {
    if (!ptr) return;

    wpe__AllocHeader* header = wpe__alloc_header_of(ptr);
    wpe__alloc_track_remove(header->size);
    header->magic = WPE__ALLOC_MAGIC_FREED;
    free(header);
}

void wpe_alloc_get_stats(WPE_AllocStats* out_stats) {
    if (!wpe_check(out_stats != NULL)) return;

    *out_stats = (WPE_AllocStats){
        .live_allocations = atomic_load_explicit(&wpe__g_alloc_stats.live_allocations, memory_order_relaxed),
        .live_bytes = atomic_load_explicit(&wpe__g_alloc_stats.live_bytes, memory_order_relaxed),
        .peak_live_bytes = atomic_load_explicit(&wpe__g_alloc_stats.peak_live_bytes, memory_order_relaxed),
        .total_allocations =
            atomic_load_explicit(&wpe__g_alloc_stats.total_allocations, memory_order_relaxed),
    };
}

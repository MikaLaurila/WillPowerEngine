#include "wpe_test.h"

static WPE_AllocStats stats_now(void) {
    WPE_AllocStats stats;
    wpe_alloc_get_stats(&stats);
    return stats;
}

static void test_alloc_tracks_live_allocations_and_bytes(void) {
    const WPE_AllocStats before = stats_now();

    void* a = wpe_alloc(100);
    void* b = wpe_alloc(28);
    WPE_TEST_ASSERT(a != NULL && b != NULL);

    const WPE_AllocStats during = stats_now();
    WPE_TEST_ASSERT_EQ_UINT(before.live_allocations + 2, during.live_allocations);
    WPE_TEST_ASSERT_EQ_UINT(before.live_bytes + 128, during.live_bytes);
    WPE_TEST_ASSERT_EQ_UINT(before.total_allocations + 2, during.total_allocations);
    WPE_TEST_ASSERT(during.peak_live_bytes >= during.live_bytes);

    wpe_free(a);
    wpe_free(b);

    const WPE_AllocStats after = stats_now();
    WPE_TEST_ASSERT_EQ_UINT(before.live_allocations, after.live_allocations);
    WPE_TEST_ASSERT_EQ_UINT(before.live_bytes, after.live_bytes);
}

static void test_alloc_returns_max_aligned_memory(void) {
    void* ptrs[16];
    for (size_t i = 0; i < 16; ++i) {
        ptrs[i] = wpe_alloc(i * 7 + 1);
        WPE_TEST_ASSERT(ptrs[i] != NULL);
        WPE_TEST_ASSERT_EQ_UINT(0, (uintptr_t)ptrs[i] % alignof(max_align_t));
    }
    for (size_t i = 0; i < 16; ++i) wpe_free(ptrs[i]);
}

static void test_alloc_zero_size_returns_null(void) {
    const WPE_AllocStats before = stats_now();
    WPE_TEST_ASSERT(wpe_alloc(0) == NULL);
    WPE_TEST_ASSERT(wpe_alloc_zeroed(0, 8) == NULL);
    WPE_TEST_ASSERT(wpe_alloc_zeroed(8, 0) == NULL);
    WPE_TEST_ASSERT_EQ_UINT(before.total_allocations, stats_now().total_allocations);
}

static void test_alloc_zeroed_clears_memory(void) {
    unsigned char* bytes = wpe_alloc_zeroed(64, 4);
    WPE_TEST_ASSERT(bytes != NULL);
    for (size_t i = 0; i < 256; ++i) WPE_TEST_ASSERT_EQ_UINT(0, bytes[i]);
    wpe_free(bytes);
}

static void test_alloc_rejects_overflowing_sizes(void) {
    wpe_log_set_stderr_enabled(false);
    void* product = wpe_alloc_zeroed(SIZE_MAX / 2, 4);
    void* sum = wpe_alloc(SIZE_MAX - 4);
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(product == NULL);
    WPE_TEST_ASSERT(sum == NULL);
}

static void test_alloc_realloc_preserves_contents(void) {
    unsigned char* bytes = wpe_alloc(16);
    WPE_TEST_ASSERT(bytes != NULL);
    for (unsigned char i = 0; i < 16; ++i) bytes[i] = i;

    const uint64_t live_bytes_before = stats_now().live_bytes;
    unsigned char* grown = wpe_realloc(bytes, 4096);
    WPE_TEST_ASSERT(grown != NULL);
    for (unsigned char i = 0; i < 16; ++i) WPE_TEST_ASSERT_EQ_UINT(i, grown[i]);
    WPE_TEST_ASSERT_EQ_UINT(live_bytes_before - 16 + 4096, stats_now().live_bytes);

    unsigned char* shrunk = wpe_realloc(grown, 8);
    WPE_TEST_ASSERT(shrunk != NULL);
    for (unsigned char i = 0; i < 8; ++i) WPE_TEST_ASSERT_EQ_UINT(i, shrunk[i]);
    wpe_free(shrunk);
}

static void test_alloc_realloc_null_and_zero(void) {
    const uint64_t live_before = stats_now().live_allocations;
    void* ptr = wpe_realloc(NULL, 32);
    WPE_TEST_ASSERT(ptr != NULL);
    WPE_TEST_ASSERT_EQ_UINT(live_before + 1, stats_now().live_allocations);
    WPE_TEST_ASSERT(wpe_realloc(ptr, 0) == NULL);
    WPE_TEST_ASSERT_EQ_UINT(live_before, stats_now().live_allocations);
}

static void test_alloc_realloc_overflow_keeps_original(void) {
    void* ptr = wpe_alloc(8);
    WPE_TEST_ASSERT(ptr != NULL);
    wpe_log_set_stderr_enabled(false);
    void* failed = wpe_realloc(ptr, SIZE_MAX);
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(failed == NULL);
    wpe_free(ptr);  /* Still owned after a failed realloc. */
}

static void test_alloc_free_null_is_noop(void) {
    const WPE_AllocStats before = stats_now();
    wpe_free(NULL);
    WPE_TEST_ASSERT_EQ_UINT(before.live_allocations, stats_now().live_allocations);
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_alloc_tracks_live_allocations_and_bytes),
    WPE_TEST_CASE(test_alloc_returns_max_aligned_memory),
    WPE_TEST_CASE(test_alloc_zero_size_returns_null),
    WPE_TEST_CASE(test_alloc_zeroed_clears_memory),
    WPE_TEST_CASE(test_alloc_rejects_overflowing_sizes),
    WPE_TEST_CASE(test_alloc_realloc_preserves_contents),
    WPE_TEST_CASE(test_alloc_realloc_null_and_zero),
    WPE_TEST_CASE(test_alloc_realloc_overflow_keeps_original),
    WPE_TEST_CASE(test_alloc_free_null_is_noop))

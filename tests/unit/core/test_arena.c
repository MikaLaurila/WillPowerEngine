#include "wpe_test.h"

static void test_arena_alignment_and_distinct_allocations(void) {
    WPE_Arena arena;
    wpe_arena_init(&arena, 256);
    unsigned char* prev = NULL;
    for (size_t align = 1; align <= alignof(max_align_t); align *= 2) {
        unsigned char* p = wpe_arena_alloc(&arena, 3, align);
        WPE_TEST_ASSERT(p != NULL);
        WPE_TEST_ASSERT_EQ_UINT(0, (uintptr_t)p % align);
        WPE_TEST_ASSERT(p != prev);
        p[0] = p[1] = p[2] = 0xAB;
        prev = p;
    }
    WPE_TEST_ASSERT(wpe_arena_alloc(&arena, 0, 0) == NULL);
    wpe_arena_release(&arena);
}

static void test_arena_grows_and_reset_coalesces(void) {
    WPE_Arena arena;
    wpe_arena_init(&arena, 128);
    for (int i = 0; i < 100; ++i)
        WPE_TEST_ASSERT(wpe_arena_alloc(&arena, 100, 0) != NULL);
    const size_t grown = wpe_arena_capacity(&arena);
    WPE_TEST_ASSERT(grown >= 100 * 100);

    WPE_AllocStats before;
    wpe_alloc_get_stats(&before);
    wpe_arena_reset(&arena);
    WPE_AllocStats after;
    wpe_alloc_get_stats(&after);
    /* Many blocks were replaced by a single one of the combined capacity. */
    WPE_TEST_ASSERT(after.live_allocations < before.live_allocations);
    WPE_TEST_ASSERT_EQ_UINT(grown, wpe_arena_capacity(&arena));

    /* After coalescing, the same workload fits without new heap allocations. */
    wpe_alloc_get_stats(&before);
    for (int i = 0; i < 100; ++i)
        WPE_TEST_ASSERT(wpe_arena_alloc(&arena, 100, 0) != NULL);
    wpe_alloc_get_stats(&after);
    WPE_TEST_ASSERT_EQ_UINT(before.total_allocations, after.total_allocations);
    wpe_arena_release(&arena);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_arena_capacity(&arena));
}

static void test_arena_large_allocation_and_zeroed(void) {
    WPE_Arena arena;
    wpe_arena_init(&arena, 64);
    unsigned char* big = wpe_arena_alloc_zeroed(&arena, 1000, 10, 0);
    WPE_TEST_ASSERT(big != NULL);
    for (int i = 0; i < 10000; ++i)
        WPE_TEST_ASSERT_EQ_UINT(0, big[i]);

    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT(wpe_arena_alloc_zeroed(&arena, SIZE_MAX / 2, 4, 0) == NULL);
    wpe_log_set_stderr_enabled(true);
    wpe_arena_release(&arena);
}

static void test_arena_zero_initialized_struct_is_usable(void) {
    WPE_Arena arena = {0};
    WPE_TEST_ASSERT(wpe_arena_alloc(&arena, 16, 0) != NULL);
    wpe_arena_reset(&arena);
    wpe_arena_release(&arena);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_arena_alignment_and_distinct_allocations),
              WPE_TEST_CASE(test_arena_grows_and_reset_coalesces),
              WPE_TEST_CASE(test_arena_large_allocation_and_zeroed),
              WPE_TEST_CASE(test_arena_zero_initialized_struct_is_usable))

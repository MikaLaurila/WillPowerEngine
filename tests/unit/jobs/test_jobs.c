#include "wpe_test.h"

#include <stdatomic.h>

#include "jobs/jobs.h"

enum { ITEMS = 10007 }; /* Prime, so chunks never divide evenly. */

typedef struct Coverage {
    atomic_uint hits[ITEMS];
    atomic_uint max_thread_index;
    atomic_uint calls;
} Coverage;

static Coverage g_coverage;

static void record_items(void* context, uint32_t begin, uint32_t end, uint32_t thread_index) {
    Coverage* coverage = context;
    atomic_fetch_add(&coverage->calls, 1);
    unsigned prev = atomic_load(&coverage->max_thread_index);
    while (thread_index > prev &&
           !atomic_compare_exchange_weak(&coverage->max_thread_index, &prev, thread_index)) {
    }
    for (uint32_t i = begin; i < end; ++i)
        atomic_fetch_add(&coverage->hits[i], 1);
}

static void reset_coverage(void) {
    for (int i = 0; i < ITEMS; ++i)
        atomic_store(&g_coverage.hits[i], 0);
    atomic_store(&g_coverage.max_thread_index, 0);
    atomic_store(&g_coverage.calls, 0);
}

static void assert_each_item_once(uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        WPE_TEST_ASSERT_MSG(atomic_load(&g_coverage.hits[i]) == 1, "item %u hit %u times", i,
                            atomic_load(&g_coverage.hits[i]));
    }
}

static void test_jobs_parallel_for_covers_every_item_once(void) {
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(4, &jobs));
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_jobs_thread_count(jobs));

    const uint32_t grains[] = {1, 7, 64, 1000, ITEMS, ITEMS * 2};
    for (size_t g = 0; g < sizeof grains / sizeof grains[0]; ++g) {
        reset_coverage();
        wpe_jobs_parallel_for(jobs, ITEMS, grains[g], record_items, &g_coverage);
        assert_each_item_once(ITEMS);
        WPE_TEST_ASSERT(atomic_load(&g_coverage.max_thread_index) < wpe_jobs_thread_count(jobs));
        WPE_TEST_ASSERT_EQ_UINT((ITEMS + grains[g] - 1) / grains[g], atomic_load(&g_coverage.calls));
    }
    wpe_jobs_destroy(jobs);
}

static void test_jobs_zero_workers_and_null_run_inline(void) {
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(0, &jobs));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_jobs_thread_count(jobs));

    reset_coverage();
    wpe_jobs_parallel_for(jobs, ITEMS, 13, record_items, &g_coverage);
    assert_each_item_once(ITEMS);
    WPE_TEST_ASSERT_EQ_UINT(0, atomic_load(&g_coverage.max_thread_index));
    wpe_jobs_destroy(jobs);

    reset_coverage();
    wpe_jobs_parallel_for(NULL, 100, 0, record_items, &g_coverage);
    assert_each_item_once(100);
    WPE_TEST_ASSERT_EQ_UINT(100, atomic_load(&g_coverage.calls)); /* grain 0 behaves as 1 */
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_jobs_thread_count(NULL));
}

static void test_jobs_zero_count_does_nothing(void) {
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(2, &jobs));
    reset_coverage();
    wpe_jobs_parallel_for(jobs, 0, 1, record_items, &g_coverage);
    WPE_TEST_ASSERT_EQ_UINT(0, atomic_load(&g_coverage.calls));
    wpe_jobs_destroy(jobs);
}

static void test_jobs_many_consecutive_batches(void) {
    /* Repeated small batches exercise the wake/done handshake for races and lost wake-ups. */
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(WPE_JOBS_AUTO_WORKERS, &jobs));
    WPE_TEST_ASSERT(wpe_jobs_thread_count(jobs) >= 1);
    for (int batch = 0; batch < 2000; ++batch) {
        reset_coverage();
        const uint32_t count = (uint32_t)(batch % 97) + 1;
        wpe_jobs_parallel_for(jobs, count, 1, record_items, &g_coverage);
        assert_each_item_once(count);
    }
    wpe_jobs_destroy(jobs);
}

static void test_jobs_worker_count_is_clamped(void) {
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(1000, &jobs));
    WPE_TEST_ASSERT_EQ_UINT(WPE_JOBS_MAX_WORKERS + 1, wpe_jobs_thread_count(jobs));
    reset_coverage();
    wpe_jobs_parallel_for(jobs, ITEMS, 1, record_items, &g_coverage);
    assert_each_item_once(ITEMS);
    wpe_jobs_destroy(jobs);

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_jobs_create(1, NULL));
    wpe_jobs_destroy(NULL);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_jobs_parallel_for_covers_every_item_once),
              WPE_TEST_CASE(test_jobs_zero_workers_and_null_run_inline),
              WPE_TEST_CASE(test_jobs_zero_count_does_nothing),
              WPE_TEST_CASE(test_jobs_many_consecutive_batches),
              WPE_TEST_CASE(test_jobs_worker_count_is_clamped))

#define WPE_LOG_MODULE "jobs"
#include "jobs/jobs.h"

#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

/*
 * Synchronization protocol for one batch:
 * 1. The owner publishes the batch fields, resets the counters, and signals `wake` once per
 *    worker. The semaphore signal/wait pair orders the batch fields before any worker reads them.
 * 2. Every consumed wake token (whichever worker takes it) runs the claim loop, then decrements
 *    `pending_tokens`; the thread taking it to zero signals `done`.
 * 3. The owner runs the claim loop as thread 0, then waits on `done`. After that no worker
 *    touches the batch, so the next batch may overwrite it.
 * A worker may consume more than one token of the same batch; it then finds no chunks left.
 * Token-based thread indices stay unique because each token gets a fresh index and a worker
 * processes its tokens sequentially.
 */
struct WPE_JobSystem {
    SDL_Thread* threads[WPE_JOBS_MAX_WORKERS];
    uint32_t worker_count;
    SDL_Semaphore* wake;
    SDL_Semaphore* done;
    atomic_bool shutdown;

    /* Current batch (written by the owner before signaling `wake`). */
    WPE_ParallelForFn fn;
    void* context;
    uint32_t count;
    uint32_t grain;
    uint32_t chunk_count;

    atomic_uint next_chunk;
    atomic_uint next_thread_index;
    atomic_uint pending_tokens;
    bool in_batch; /* Owner-only reentrancy guard. */
};

/* Claims and processes chunks until none are left. */
static void wpe__jobs_run_chunks(WPE_JobSystem* jobs, uint32_t thread_index) {
    for (;;) {
        const uint32_t chunk = atomic_fetch_add_explicit(&jobs->next_chunk, 1, memory_order_relaxed);
        if (chunk >= jobs->chunk_count) return;
        const uint32_t begin = chunk * jobs->grain;
        const uint32_t end = begin + jobs->grain < jobs->count ? begin + jobs->grain : jobs->count;
        jobs->fn(jobs->context, begin, end, thread_index);
    }
}

static int SDLCALL wpe__jobs_worker_main(void* data) {
    WPE_JobSystem* jobs = data;
    for (;;) {
        SDL_WaitSemaphore(jobs->wake);
        if (atomic_load_explicit(&jobs->shutdown, memory_order_acquire)) return 0;

        const uint32_t thread_index =
            atomic_fetch_add_explicit(&jobs->next_thread_index, 1, memory_order_relaxed);
        wpe__jobs_run_chunks(jobs, thread_index);
        if (atomic_fetch_sub_explicit(&jobs->pending_tokens, 1, memory_order_acq_rel) == 1) {
            SDL_SignalSemaphore(jobs->done);
        }
    }
}

WPE_Result wpe_jobs_create(uint32_t worker_count, WPE_JobSystem** out_jobs) {
    if (!out_jobs) return WPE_RESULT_ERROR_NULL_ARG;
    *out_jobs = NULL;

    if (worker_count == WPE_JOBS_AUTO_WORKERS) {
        const int cores = SDL_GetNumLogicalCPUCores();
        worker_count = cores > 1 ? (uint32_t)cores - 1 : 0;
    }
    if (worker_count > WPE_JOBS_MAX_WORKERS) worker_count = WPE_JOBS_MAX_WORKERS;

    WPE_JobSystem* jobs = wpe_alloc_zeroed(1, sizeof *jobs);
    if (!jobs) return WPE_RESULT_ERROR_OUT_OF_MEMORY;

    jobs->wake = SDL_CreateSemaphore(0);
    jobs->done = SDL_CreateSemaphore(0);
    if (!jobs->wake || !jobs->done) {
        wpe_log_error("failed to create semaphores: %s", SDL_GetError());
        wpe_jobs_destroy(jobs);
        return WPE_RESULT_ERROR;
    }

    for (uint32_t i = 0; i < worker_count; ++i) {
        char name[32];
        snprintf(name, sizeof name, "wpe_worker_%u", i + 1);
        jobs->threads[i] = SDL_CreateThread(wpe__jobs_worker_main, name, jobs);
        if (!jobs->threads[i]) {
            wpe_log_error("failed to create worker thread %u: %s", i + 1, SDL_GetError());
            wpe_jobs_destroy(jobs);
            return WPE_RESULT_ERROR;
        }
        jobs->worker_count = i + 1;
    }

    wpe_log_debug("job system started with %u worker thread(s)", jobs->worker_count);
    *out_jobs = jobs;
    return WPE_RESULT_OK;
}

void wpe_jobs_destroy(WPE_JobSystem* jobs) {
    if (!jobs) return;
    wpe_assert(!jobs->in_batch);

    atomic_store_explicit(&jobs->shutdown, true, memory_order_release);
    for (uint32_t i = 0; i < jobs->worker_count; ++i)
        SDL_SignalSemaphore(jobs->wake);
    for (uint32_t i = 0; i < jobs->worker_count; ++i)
        SDL_WaitThread(jobs->threads[i], NULL);

    if (jobs->wake) SDL_DestroySemaphore(jobs->wake);
    if (jobs->done) SDL_DestroySemaphore(jobs->done);
    wpe_free(jobs);
}

uint32_t wpe_jobs_thread_count(const WPE_JobSystem* jobs) {
    return jobs ? jobs->worker_count + 1 : 1;
}

void wpe_jobs_parallel_for(WPE_JobSystem* jobs, uint32_t count, uint32_t grain, WPE_ParallelForFn fn,
                           void* context) {
    if (!wpe_check(fn != NULL) || count == 0) return;
    if (grain == 0) grain = 1;
    const uint32_t chunk_count = (uint32_t)(((uint64_t)count + grain - 1) / grain);

    if (!jobs || jobs->worker_count == 0 || chunk_count == 1) {
        for (uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
            const uint32_t begin = chunk * grain;
            fn(context, begin, begin + grain < count ? begin + grain : count, 0);
        }
        return;
    }

    wpe_assert(!jobs->in_batch);
    jobs->in_batch = true;

    jobs->fn = fn;
    jobs->context = context;
    jobs->count = count;
    jobs->grain = grain;
    jobs->chunk_count = chunk_count;
    atomic_store_explicit(&jobs->next_chunk, 0, memory_order_relaxed);
    atomic_store_explicit(&jobs->next_thread_index, 1, memory_order_relaxed);

    /* Waking more workers than there are chunks only adds wake-up latency. */
    const uint32_t tokens = chunk_count - 1 < jobs->worker_count ? chunk_count - 1 : jobs->worker_count;
    atomic_store_explicit(&jobs->pending_tokens, tokens, memory_order_relaxed);
    for (uint32_t i = 0; i < tokens; ++i)
        SDL_SignalSemaphore(jobs->wake);

    wpe__jobs_run_chunks(jobs, 0);
    SDL_WaitSemaphore(jobs->done);

    jobs->in_batch = false;
}

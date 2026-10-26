/**
 * @file jobs.h
 * @brief Worker thread pool with a blocking, data-parallel `parallel_for` primitive.
 *
 * The pool runs one batch at a time. wpe_jobs_parallel_for() splits [0, count) into chunks of
 * `grain` items; workers and the calling thread claim chunks through an atomic counter until all
 * are done, then the call returns. Every invocation of the callback receives a `thread_index` in
 * [0, wpe_jobs_thread_count()) that is unique among concurrently running invocations, so callers
 * can index per-thread scratch memory without locking. The calling thread always uses index 0.
 *
 * Results must not depend on which thread processes which chunk: chunk boundaries are fixed by
 * `grain`, so code that writes per-chunk outputs produces identical results for any worker count.
 *
 * - Dependencies: core, SDL3 (threads and semaphores; private)
 * - Thread affinity: create/destroy/parallel_for from one owning thread. parallel_for is not
 *   reentrant: callbacks must not call it.
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_JOBS_JOBS_H
#define WPE_JOBS_JOBS_H

#include <stdint.h>

#include "core/result.h"

/** Upper bound on worker threads (excluding the calling thread). */
#define WPE_JOBS_MAX_WORKERS 63u
/** Pass as worker_count to use one worker per logical CPU core minus the calling thread. */
#define WPE_JOBS_AUTO_WORKERS UINT32_MAX

typedef struct WPE_JobSystem WPE_JobSystem;

/**
 * Processes items [begin, end) of a parallel_for.
 *
 * @param context The pointer given to wpe_jobs_parallel_for().
 * @param begin First item index. @param end One past the last item index.
 * @param thread_index Unique among concurrent invocations, in [0, wpe_jobs_thread_count()).
 */
typedef void (*WPE_ParallelForFn)(void* context, uint32_t begin, uint32_t end, uint32_t thread_index);

/**
 * Creates a job system and starts its worker threads.
 *
 * @param worker_count Number of worker threads (0 = run everything on the calling thread), or
 *                     WPE_JOBS_AUTO_WORKERS. Clamped to WPE_JOBS_MAX_WORKERS.
 * @param out_jobs Receives the job system; must not be NULL. Owned by the caller; release with
 *                 wpe_jobs_destroy().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY or
 *         WPE_RESULT_ERROR (thread/semaphore creation failed; an ERROR is logged).
 *
 * @thread_safety Any thread; that thread becomes the owner
 */
[[nodiscard]] WPE_Result wpe_jobs_create(uint32_t worker_count, WPE_JobSystem** out_jobs);

/**
 * Stops and joins all workers and frees the job system.
 *
 * @param jobs Job system (ownership consumed), or NULL (no-op).
 *
 * @thread_safety Owning thread; no parallel_for may be running
 */
void wpe_jobs_destroy(WPE_JobSystem* jobs);

/** @return Number of threads that can run callbacks concurrently (workers + caller). @thread_safety Any
 * thread */
uint32_t wpe_jobs_thread_count(const WPE_JobSystem* jobs);

/**
 * Runs `fn` over [0, count) in chunks of `grain` items and waits for completion.
 *
 * @param jobs Job system; NULL runs everything on the calling thread with thread_index 0.
 * @param count Number of items; 0 returns immediately.
 * @param grain Items per chunk; 0 is treated as 1.
 * @param fn Callback; must not be NULL.
 * @param context Passed to fn; caller retains ownership.
 *
 * @thread_safety Owning thread only; not reentrant
 */
void wpe_jobs_parallel_for(WPE_JobSystem* jobs, uint32_t count, uint32_t grain, WPE_ParallelForFn fn,
                           void* context);

#endif /* WPE_JOBS_JOBS_H */

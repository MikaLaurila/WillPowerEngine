/**
 * @file arena.h
 * @brief Linear (bump) allocator for short-lived memory such as per-frame data.
 *
 * Allocations are released all at once by wpe_arena_reset(). When the arena had to grow into
 * several blocks, reset replaces them with one block of the combined capacity, so a workload
 * with stable per-frame memory needs reach zero heap allocations per frame after warm-up.
 *
 * - Dependencies: core/alloc
 * - Thread affinity: not synchronized; one thread at a time per arena
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_ARENA_H
#define WPE_CORE_ARENA_H

#include <stddef.h>

typedef struct wpe__ArenaBlock wpe__ArenaBlock;

/** Zero-initialize (or use wpe_arena_init) before use. Fields are private. */
typedef struct WPE_Arena {
    wpe__ArenaBlock* wpe__head; /* Current block; older blocks follow via `next`. */
    size_t wpe__min_block_size;
} WPE_Arena;

/**
 * Prepares an arena. No memory is allocated until the first wpe_arena_alloc().
 *
 * @param arena Arena to initialize; must not be NULL.
 * @param min_block_size Minimum size of each block (0 selects 64 KiB).
 *
 * @thread_safety Owning thread
 */
void wpe_arena_init(WPE_Arena* arena, size_t min_block_size);

/**
 * Allocates uninitialized memory valid until the next reset or release.
 *
 * @param size Byte count; 0 returns NULL.
 * @param alignment Power of two up to alignof(max_align_t); 0 selects alignof(max_align_t).
 * @return Pointer owned by the arena, or NULL on failure (an ERROR is logged).
 *
 * @thread_safety Owning thread
 */
[[nodiscard]] void* wpe_arena_alloc(WPE_Arena* arena, size_t size, size_t alignment);

/** Like wpe_arena_alloc() but zero-fills `count * size` bytes (overflow-checked). @thread_safety Owning
 * thread */
[[nodiscard]] void* wpe_arena_alloc_zeroed(WPE_Arena* arena, size_t count, size_t size, size_t alignment);

/** Invalidates all allocations and coalesces blocks for reuse. @thread_safety Owning thread */
void wpe_arena_reset(WPE_Arena* arena);

/** Frees all memory; the arena may be reused afterwards. @thread_safety Owning thread */
void wpe_arena_release(WPE_Arena* arena);

/** @return Total bytes of block capacity currently held. @thread_safety Owning thread */
size_t wpe_arena_capacity(const WPE_Arena* arena);

#endif /* WPE_CORE_ARENA_H */

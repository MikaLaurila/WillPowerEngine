#define WPE_LOG_MODULE "core"
#include "core/arena.h"

#include <stdckdint.h>
#include <stdint.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

#define WPE__ARENA_DEFAULT_BLOCK_SIZE ((size_t)64 * 1024)

struct wpe__ArenaBlock {
    wpe__ArenaBlock* next;
    size_t capacity;
    size_t used;
    alignas(max_align_t) unsigned char data[];
};

static wpe__ArenaBlock* wpe__arena_new_block(size_t capacity) {
    size_t total;
    if (ckd_add(&total, sizeof(wpe__ArenaBlock), capacity)) return NULL;
    wpe__ArenaBlock* block = wpe_alloc(total);
    if (!block) return NULL;
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    return block;
}

void wpe_arena_init(WPE_Arena* arena, size_t min_block_size) {
    if (!wpe_check(arena != NULL)) return;
    arena->wpe__head = NULL;
    arena->wpe__min_block_size = min_block_size ? min_block_size : WPE__ARENA_DEFAULT_BLOCK_SIZE;
}

void* wpe_arena_alloc(WPE_Arena* arena, size_t size, size_t alignment) {
    if (!wpe_check(arena != NULL) || size == 0) return NULL;
    if (alignment == 0) alignment = alignof(max_align_t);
    wpe_assert((alignment & (alignment - 1)) == 0 && alignment <= alignof(max_align_t));
    if (arena->wpe__min_block_size == 0) arena->wpe__min_block_size = WPE__ARENA_DEFAULT_BLOCK_SIZE;

    wpe__ArenaBlock* block = arena->wpe__head;
    if (block) {
        const size_t offset = (block->used + alignment - 1) & ~(alignment - 1);
        if (offset <= block->capacity && size <= block->capacity - offset) {
            block->used = offset + size;
            return block->data + offset;
        }
    }

    /* Grow geometrically so the number of blocks stays logarithmic in the peak usage. */
    size_t capacity = arena->wpe__min_block_size;
    if (block && block->capacity <= SIZE_MAX / 2 && block->capacity * 2 > capacity)
        capacity = block->capacity * 2;
    if (capacity < size) capacity = size;

    wpe__ArenaBlock* fresh = wpe__arena_new_block(capacity);
    if (!fresh) {
        wpe_log_error("arena out of memory allocating %zu bytes", size);
        return NULL;
    }
    fresh->next = block;
    fresh->used = size;
    arena->wpe__head = fresh;
    return fresh->data;
}

void* wpe_arena_alloc_zeroed(WPE_Arena* arena, size_t count, size_t size, size_t alignment) {
    size_t bytes;
    if (ckd_mul(&bytes, count, size)) {
        wpe_log_error("arena allocation of %zu x %zu bytes overflows size_t", count, size);
        return NULL;
    }
    void* ptr = wpe_arena_alloc(arena, bytes, alignment);
    if (ptr) memset(ptr, 0, bytes);
    return ptr;
}

void wpe_arena_reset(WPE_Arena* arena) {
    if (!wpe_check(arena != NULL) || !arena->wpe__head) return;

    wpe__ArenaBlock* head = arena->wpe__head;
    if (!head->next) {
        head->used = 0;
        return;
    }

    const size_t total = wpe_arena_capacity(arena);
    wpe_arena_release(arena);
    /* On failure the arena simply starts empty and grows again on demand. */
    arena->wpe__head = wpe__arena_new_block(total);
}

void wpe_arena_release(WPE_Arena* arena) {
    if (!wpe_check(arena != NULL)) return;
    wpe__ArenaBlock* block = arena->wpe__head;
    while (block) {
        wpe__ArenaBlock* next = block->next;
        wpe_free(block);
        block = next;
    }
    arena->wpe__head = NULL;
}

size_t wpe_arena_capacity(const WPE_Arena* arena) {
    if (!arena) return 0;
    size_t total = 0;
    for (const wpe__ArenaBlock* block = arena->wpe__head; block; block = block->next)
        total += block->capacity;
    return total;
}

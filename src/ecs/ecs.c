#define WPE_LOG_MODULE "ecs"
#include "ecs/ecs.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

#define WPE__ECS_INDEX_MASK (WPE_ECS_MAX_ENTITIES - 1u)
#define WPE__ECS_INVALID UINT32_MAX
#define WPE__ECS_MIN_CAPACITY 64u

static_assert(WPE_ECS_MAX_COMPONENT_TYPES <= 64, "component masks are 64-bit");
static_assert(WPE_COMP_USER_FIRST < WPE_ECS_MAX_COMPONENT_TYPES, "user component ids must fit the mask");

typedef struct wpe__EcsSlot {
    uint64_t mask;         /* Components currently attached. */
    uint64_t spawn_serial; /* Monotonic spawn counter value of the current occupant. */
    uint32_t generation;   /* Generation of the current (or next) occupant; >= 1 once used. */
    uint32_t next_free;    /* Free-list link, valid while the slot is free. */
    bool alive;
} wpe__EcsSlot;

typedef struct wpe__EcsPool {
    bool registered;
    char name[WPE_ECS_COMPONENT_NAME_MAX];
    uint32_t size;
    WPE_ComponentHookFn on_add;
    WPE_ComponentHookFn on_remove;
    void* hook_user_data;

    uint32_t* sparse; /* Entity slot index -> dense index, WPE__ECS_INVALID when absent. */
    uint32_t sparse_capacity;
    WPE_Entity* dense_entities;
    unsigned char* data;
    uint32_t count;
    uint32_t capacity;
} wpe__EcsPool;

struct WPE_Ecs {
    wpe__EcsSlot* slots;
    uint32_t slot_count;
    uint32_t slot_capacity;
    uint32_t free_head;
    uint32_t alive_count;
    uint64_t next_spawn_serial;

    /* Incremented on every component removal / despawn; running iterators detect changes with it. */
    uint64_t removal_version;
    /* Non-zero while a component hook runs; structural changes are rejected then. */
    uint32_t hook_depth;

    WPE_Entity* deferred;
    uint32_t deferred_count;
    uint32_t deferred_capacity;

    wpe__EcsPool pools[WPE_ECS_MAX_COMPONENT_TYPES];
};

/* ---- Helpers ----------------------------------------------------------------------------------- */

static uint32_t wpe__ecs_index(WPE_Entity entity) {
    return entity & WPE__ECS_INDEX_MASK;
}

static uint32_t wpe__ecs_generation(WPE_Entity entity) {
    return entity >> WPE_ECS_ENTITY_INDEX_BITS;
}

static WPE_Entity wpe__ecs_make_entity(uint32_t index, uint32_t generation) {
    return (generation << WPE_ECS_ENTITY_INDEX_BITS) | index;
}

/* Returns the slot for an alive entity, or NULL for dead/stale/NULL handles. */
static wpe__EcsSlot* wpe__ecs_live_slot(const WPE_Ecs* ecs, WPE_Entity entity) {
    const uint32_t index = wpe__ecs_index(entity);
    if (index >= ecs->slot_count) return NULL;
    wpe__EcsSlot* slot = &ecs->slots[index];
    if (!slot->alive || slot->generation != wpe__ecs_generation(entity)) return NULL;
    return slot;
}

static const wpe__EcsPool* wpe__ecs_registered_pool(const WPE_Ecs* ecs, WPE_ComponentType type) {
    if ((unsigned)type == WPE_COMP_NONE || (unsigned)type >= WPE_ECS_MAX_COMPONENT_TYPES) return NULL;
    const wpe__EcsPool* pool = &ecs->pools[type];
    return pool->registered ? pool : NULL;
}

/*
 * Computes the doubled capacity needed to hold `required` elements, capped at `limit`.
 * Returns false when `required` exceeds `limit`.
 */
static bool wpe__ecs_next_capacity(uint32_t current, uint32_t required, uint32_t limit, uint32_t* out) {
    if (required > limit) return false;
    uint64_t capacity = current < WPE__ECS_MIN_CAPACITY ? WPE__ECS_MIN_CAPACITY : current;
    while (capacity < required)
        capacity *= 2;
    *out = capacity > limit ? limit : (uint32_t)capacity;
    return true;
}

static bool wpe__ecs_reserve_slots(WPE_Ecs* ecs, uint32_t required) {
    if (required <= ecs->slot_capacity) return true;

    uint32_t capacity;
    if (!wpe__ecs_next_capacity(ecs->slot_capacity, required, WPE_ECS_MAX_ENTITIES, &capacity)) return false;
    wpe__EcsSlot* slots = wpe_realloc(ecs->slots, (size_t)capacity * sizeof *slots);
    if (!slots) return false;
    ecs->slots = slots;
    ecs->slot_capacity = capacity;
    return true;
}

static bool wpe__ecs_reserve_sparse(wpe__EcsPool* pool, uint32_t index) {
    if (index < pool->sparse_capacity) return true;

    uint32_t capacity;
    if (!wpe__ecs_next_capacity(pool->sparse_capacity, index + 1, WPE_ECS_MAX_ENTITIES, &capacity))
        return false;
    uint32_t* sparse = wpe_realloc(pool->sparse, (size_t)capacity * sizeof *sparse);
    if (!sparse) return false;
    for (uint32_t i = pool->sparse_capacity; i < capacity; ++i)
        sparse[i] = WPE__ECS_INVALID;
    pool->sparse = sparse;
    pool->sparse_capacity = capacity;
    return true;
}

static bool wpe__ecs_reserve_dense(wpe__EcsPool* pool, uint32_t required) {
    if (required <= pool->capacity) return true;

    uint32_t capacity;
    if (!wpe__ecs_next_capacity(pool->capacity, required, WPE_ECS_MAX_ENTITIES, &capacity)) return false;

    /* Both arrays must grow before capacity is updated; a partial success only over-allocates. */
    WPE_Entity* entities = wpe_realloc(pool->dense_entities, (size_t)capacity * sizeof *entities);
    if (!entities) return false;
    pool->dense_entities = entities;

    unsigned char* data = wpe_realloc(pool->data, (size_t)capacity * pool->size);
    if (!data) return false;
    pool->data = data;

    pool->capacity = capacity;
    return true;
}

static void* wpe__ecs_pool_at(const wpe__EcsPool* pool, uint32_t dense_index) {
    return pool->data + (size_t)dense_index * pool->size;
}

static void wpe__ecs_run_hook(WPE_Ecs* ecs, const wpe__EcsPool* pool, WPE_ComponentHookFn hook,
                              WPE_Entity entity, uint32_t dense_index) {
    if (!hook) return;
    ++ecs->hook_depth;
    hook(ecs, entity, wpe__ecs_pool_at(pool, dense_index), pool->hook_user_data);
    --ecs->hook_depth;
}

/* Removes the entity's entry from the pool with swap-remove. Hooks must have run already. */
static void wpe__ecs_pool_erase(wpe__EcsPool* pool, uint32_t entity_index) {
    const uint32_t dense = pool->sparse[entity_index];
    const uint32_t last = pool->count - 1;
    if (dense != last) {
        memcpy(wpe__ecs_pool_at(pool, dense), wpe__ecs_pool_at(pool, last), pool->size);
        const WPE_Entity moved = pool->dense_entities[last];
        pool->dense_entities[dense] = moved;
        pool->sparse[wpe__ecs_index(moved)] = dense;
    }
    pool->sparse[entity_index] = WPE__ECS_INVALID;
    pool->count = last;
}

static bool wpe__ecs_reject_in_hook(const WPE_Ecs* ecs, const char* operation) {
    if (ecs->hook_depth == 0) return false;
    wpe_log_error("%s called from a component hook; structural changes are not allowed there", operation);
    wpe_assert(ecs->hook_depth == 0);
    return true;
}

/* ---- World lifecycle --------------------------------------------------------------------------- */

WPE_Result wpe_ecs_create(WPE_Ecs** out_ecs) {
    if (!out_ecs) return WPE_RESULT_ERROR_NULL_ARG;
    *out_ecs = NULL;

    WPE_Ecs* ecs = wpe_alloc_zeroed(1, sizeof *ecs);
    if (!ecs) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    ecs->free_head = WPE__ECS_INVALID;
    ecs->next_spawn_serial = 1;
    *out_ecs = ecs;
    return WPE_RESULT_OK;
}

void wpe_ecs_destroy(WPE_Ecs* ecs) {
    if (!ecs) return;

    /* Same descending order as despawn so higher-level hooks see lower-level components intact. */
    for (uint32_t type = WPE_ECS_MAX_COMPONENT_TYPES - 1; type >= 1; --type) {
        const wpe__EcsPool* pool = &ecs->pools[type];
        if (!pool->registered || !pool->on_remove) continue;
        for (uint32_t i = 0; i < pool->count; ++i) {
            wpe__ecs_run_hook(ecs, pool, pool->on_remove, pool->dense_entities[i], i);
        }
    }

    for (uint32_t type = 0; type < WPE_ECS_MAX_COMPONENT_TYPES; ++type) {
        wpe__EcsPool* pool = &ecs->pools[type];
        wpe_free(pool->sparse);
        wpe_free(pool->dense_entities);
        wpe_free(pool->data);
    }
    wpe_free(ecs->deferred);
    wpe_free(ecs->slots);
    wpe_free(ecs);
}

WPE_Result wpe_ecs_register_component(WPE_Ecs* ecs, WPE_ComponentType type, const WPE_ComponentDesc* desc) {
    if (!ecs || !desc) return WPE_RESULT_ERROR_NULL_ARG;

    if ((unsigned)type == WPE_COMP_NONE || (unsigned)type >= WPE_ECS_MAX_COMPONENT_TYPES) {
        wpe_log_error("component type id %u is out of range", (unsigned)type);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (!desc->name || desc->name[0] == '\0') {
        wpe_log_error("component type %u has no name", (unsigned)type);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const uint32_t align = desc->alignment;
    if (desc->size == 0 || align == 0 || (align & (align - 1)) != 0 || align > alignof(max_align_t) ||
        desc->size % align != 0) {
        wpe_log_error("component '%s' has invalid layout (size %u, alignment %u)", desc->name, desc->size,
                      align);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    wpe__EcsPool* pool = &ecs->pools[type];
    if (pool->registered) {
        wpe_log_error("component type %u is already registered as '%s'", (unsigned)type, pool->name);
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }

    *pool = (wpe__EcsPool){
        .registered = true,
        .size = desc->size,
        .on_add = desc->on_add,
        .on_remove = desc->on_remove,
        .hook_user_data = desc->hook_user_data,
    };
    snprintf(pool->name, sizeof pool->name, "%s", desc->name);
    return WPE_RESULT_OK;
}

WPE_Result wpe_ecs_unregister_component(WPE_Ecs* ecs, WPE_ComponentType type) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (wpe__ecs_reject_in_hook(ecs, "wpe_ecs_unregister_component")) return WPE_RESULT_ERROR_INVALID_STATE;
    if (!wpe__ecs_registered_pool(ecs, type)) return WPE_RESULT_ERROR_NOT_FOUND;
    wpe__EcsPool* pool = &ecs->pools[type];
    if (pool->count != 0) {
        wpe_log_error("cannot unregister component '%s': %u instance(s) still exist", pool->name,
                      pool->count);
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    wpe_free(pool->sparse);
    wpe_free(pool->dense_entities);
    wpe_free(pool->data);
    *pool = (wpe__EcsPool){0};
    return WPE_RESULT_OK;
}

bool wpe_ecs_is_registered(const WPE_Ecs* ecs, WPE_ComponentType type) {
    return ecs && wpe__ecs_registered_pool(ecs, type) != NULL;
}

const char* wpe_ecs_component_name(const WPE_Ecs* ecs, WPE_ComponentType type) {
    if (!ecs) return NULL;
    const wpe__EcsPool* pool = wpe__ecs_registered_pool(ecs, type);
    return pool ? pool->name : NULL;
}

/* ---- Entities ------------------------------------------------------------------------------------ */

WPE_Entity wpe_ecs_spawn(WPE_Ecs* ecs) {
    if (!wpe_check(ecs != NULL)) return WPE_ENTITY_NULL;
    if (wpe__ecs_reject_in_hook(ecs, "wpe_ecs_spawn")) return WPE_ENTITY_NULL;

    uint32_t index;
    if (ecs->free_head != WPE__ECS_INVALID) {
        index = ecs->free_head;
        ecs->free_head = ecs->slots[index].next_free;
    } else {
        if (ecs->slot_count >= WPE_ECS_MAX_ENTITIES || !wpe__ecs_reserve_slots(ecs, ecs->slot_count + 1)) {
            wpe_log_error("cannot spawn entity: limit of %u entity slots reached or out of memory",
                          WPE_ECS_MAX_ENTITIES);
            return WPE_ENTITY_NULL;
        }
        index = ecs->slot_count++;
        ecs->slots[index] = (wpe__EcsSlot){.generation = 1};
    }

    wpe__EcsSlot* slot = &ecs->slots[index];
    slot->alive = true;
    slot->mask = 0;
    slot->spawn_serial = ecs->next_spawn_serial++;
    slot->next_free = WPE__ECS_INVALID;
    ++ecs->alive_count;
    return wpe__ecs_make_entity(index, slot->generation);
}

WPE_Result wpe_ecs_despawn(WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (wpe__ecs_reject_in_hook(ecs, "wpe_ecs_despawn")) return WPE_RESULT_ERROR_INVALID_STATE;

    wpe__EcsSlot* slot = wpe__ecs_live_slot(ecs, entity);
    if (!slot) return WPE_RESULT_ERROR_NOT_FOUND;
    const uint32_t index = wpe__ecs_index(entity);

    /*
     * All on_remove hooks run before any storage is released, in descending type id order:
     * higher-level components (scripts, game components) are torn down while lower-level ones
     * (transform, hierarchy) are still fully intact, since those hooks may edit shared state.
     */
    for (uint32_t type = WPE_ECS_MAX_COMPONENT_TYPES - 1; type >= 1; --type) {
        if (!(slot->mask & WPE_ECS_MASK(type))) continue;
        const wpe__EcsPool* pool = &ecs->pools[type];
        wpe__ecs_run_hook(ecs, pool, pool->on_remove, entity, pool->sparse[index]);
    }
    for (uint32_t type = 1; type < WPE_ECS_MAX_COMPONENT_TYPES; ++type) {
        if (slot->mask & WPE_ECS_MASK(type)) wpe__ecs_pool_erase(&ecs->pools[type], index);
    }

    slot->mask = 0;
    slot->alive = false;
    --ecs->alive_count;
    ++ecs->removal_version;

    if (slot->generation >= WPE_ECS_MAX_GENERATION) {
        /* Retire the slot: reusing it would wrap the generation and let stale handles alias. */
        slot->next_free = WPE__ECS_INVALID;
    } else {
        ++slot->generation;
        slot->next_free = ecs->free_head;
        ecs->free_head = index;
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_ecs_despawn_deferred(WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe__ecs_live_slot(ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;

    if (ecs->deferred_count == ecs->deferred_capacity) {
        uint32_t capacity;
        if (!wpe__ecs_next_capacity(ecs->deferred_capacity, ecs->deferred_count + 1, UINT32_MAX / 2,
                                    &capacity)) {
            return WPE_RESULT_ERROR_CAPACITY;
        }
        WPE_Entity* deferred = wpe_realloc(ecs->deferred, (size_t)capacity * sizeof *deferred);
        if (!deferred) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        ecs->deferred = deferred;
        ecs->deferred_capacity = capacity;
    }
    ecs->deferred[ecs->deferred_count++] = entity;
    return WPE_RESULT_OK;
}

uint32_t wpe_ecs_flush_deferred(WPE_Ecs* ecs) {
    if (!wpe_check(ecs != NULL)) return 0;
    if (wpe__ecs_reject_in_hook(ecs, "wpe_ecs_flush_deferred")) return 0;

    /* Hooks may queue more entities while flushing; the count is re-read every iteration. */
    uint32_t despawned = 0;
    for (uint32_t i = 0; i < ecs->deferred_count; ++i) {
        if (wpe_ecs_despawn(ecs, ecs->deferred[i]) == WPE_RESULT_OK) ++despawned;
    }
    ecs->deferred_count = 0;
    return despawned;
}

bool wpe_ecs_is_alive(const WPE_Ecs* ecs, WPE_Entity entity) {
    return ecs && wpe__ecs_live_slot(ecs, entity) != NULL;
}

uint32_t wpe_ecs_entity_count(const WPE_Ecs* ecs) {
    return ecs ? ecs->alive_count : 0;
}

/* ---- Components ---------------------------------------------------------------------------------- */

WPE_Result wpe_ecs_add_component(WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type, const void* initial,
                                 void** out_component) {
    if (out_component) *out_component = NULL;
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (wpe__ecs_reject_in_hook(ecs, "wpe_ecs_add_component")) return WPE_RESULT_ERROR_INVALID_STATE;

    wpe__EcsSlot* slot = wpe__ecs_live_slot(ecs, entity);
    if (!slot) return WPE_RESULT_ERROR_NOT_FOUND;
    if (!wpe__ecs_registered_pool(ecs, type)) {
        wpe_log_error("cannot add unregistered component type %u", (unsigned)type);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (slot->mask & WPE_ECS_MASK(type)) return WPE_RESULT_ERROR_ALREADY_EXISTS;

    wpe__EcsPool* pool = &ecs->pools[type];
    const uint32_t index = wpe__ecs_index(entity);
    if (!wpe__ecs_reserve_sparse(pool, index) || !wpe__ecs_reserve_dense(pool, pool->count + 1)) {
        wpe_log_error("out of memory adding component '%s'", pool->name);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    const uint32_t dense = pool->count++;
    void* component = wpe__ecs_pool_at(pool, dense);
    if (initial) {
        memcpy(component, initial, pool->size);
    } else {
        memset(component, 0, pool->size);
    }
    pool->dense_entities[dense] = entity;
    pool->sparse[index] = dense;
    slot->mask |= WPE_ECS_MASK(type);

    wpe__ecs_run_hook(ecs, pool, pool->on_add, entity, dense);
    if (out_component) *out_component = component;
    return WPE_RESULT_OK;
}

WPE_Result wpe_ecs_remove_component(WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    if (wpe__ecs_reject_in_hook(ecs, "wpe_ecs_remove_component")) return WPE_RESULT_ERROR_INVALID_STATE;

    wpe__EcsSlot* slot = wpe__ecs_live_slot(ecs, entity);
    if (!slot) return WPE_RESULT_ERROR_NOT_FOUND;
    if (!wpe__ecs_registered_pool(ecs, type)) return WPE_RESULT_ERROR_INVALID_ARG;
    if (!(slot->mask & WPE_ECS_MASK(type))) return WPE_RESULT_ERROR_NOT_FOUND;

    wpe__EcsPool* pool = &ecs->pools[type];
    const uint32_t index = wpe__ecs_index(entity);
    wpe__ecs_run_hook(ecs, pool, pool->on_remove, entity, pool->sparse[index]);
    wpe__ecs_pool_erase(pool, index);
    slot->mask &= ~WPE_ECS_MASK(type);
    ++ecs->removal_version;
    return WPE_RESULT_OK;
}

void* wpe_ecs_get_component(const WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type) {
    if (!ecs) return NULL;
    const wpe__EcsPool* pool = wpe__ecs_registered_pool(ecs, type);
    if (!pool) return NULL;
    const wpe__EcsSlot* slot = wpe__ecs_live_slot(ecs, entity);
    if (!slot || !(slot->mask & WPE_ECS_MASK(type))) return NULL;
    return wpe__ecs_pool_at(pool, pool->sparse[wpe__ecs_index(entity)]);
}

bool wpe_ecs_has_component(const WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type) {
    if ((unsigned)type == WPE_COMP_NONE || (unsigned)type >= WPE_ECS_MAX_COMPONENT_TYPES) return false;
    return (wpe_ecs_component_mask(ecs, entity) & WPE_ECS_MASK(type)) != 0;
}

uint64_t wpe_ecs_component_mask(const WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs) return 0;
    const wpe__EcsSlot* slot = wpe__ecs_live_slot(ecs, entity);
    return slot ? slot->mask : 0;
}

uint32_t wpe_ecs_component_count(const WPE_Ecs* ecs, WPE_ComponentType type) {
    if (!ecs) return 0;
    const wpe__EcsPool* pool = wpe__ecs_registered_pool(ecs, type);
    return pool ? pool->count : 0;
}

WPE_Result wpe_ecs_pool_view(const WPE_Ecs* ecs, WPE_ComponentType type, WPE_EcsPoolView* out_view) {
    if (!ecs || !out_view) return WPE_RESULT_ERROR_NULL_ARG;
    *out_view = (WPE_EcsPoolView){0};
    const wpe__EcsPool* pool = wpe__ecs_registered_pool(ecs, type);
    if (!pool) return WPE_RESULT_ERROR_INVALID_ARG;
    *out_view = (WPE_EcsPoolView){
        .entities = pool->dense_entities,
        .data = pool->data,
        .count = pool->count,
        .stride = pool->size,
    };
    return WPE_RESULT_OK;
}

/* ---- Queries ------------------------------------------------------------------------------------- */

WPE_EcsIter wpe_ecs_query(WPE_Ecs* ecs, WPE_EcsQuery query) {
    WPE_EcsIter it = {.wpe__ecs = ecs, .wpe__query = query};
    if (!wpe_check(ecs != NULL)) return it;

    it.wpe__removal_version = ecs->removal_version;
    if (query.all == 0) {
        it.wpe__driver = WPE_COMP_NONE;
        it.wpe__end = ecs->slot_count;
        it.wpe__spawn_serial_end = ecs->next_spawn_serial;
        return it;
    }

    /* Drive iteration from the smallest required pool; an unregistered requirement matches nothing. */
    uint32_t best_count = UINT32_MAX;
    for (uint32_t type = 0; type < WPE_ECS_MAX_COMPONENT_TYPES; ++type) {
        if (!(query.all & WPE_ECS_MASK(type))) continue;
        const wpe__EcsPool* pool = wpe__ecs_registered_pool(ecs, (WPE_ComponentType)type);
        if (!pool) {
            it.wpe__end = 0;
            return it;
        }
        if (pool->count < best_count) {
            best_count = pool->count;
            it.wpe__driver = type;
        }
    }
    it.wpe__end = best_count;
    return it;
}

bool wpe_ecs_iter_next(WPE_EcsIter* it) {
    if (!it || !it->wpe__ecs) return false;
    const WPE_Ecs* ecs = it->wpe__ecs;

    if (it->wpe__removal_version != ecs->removal_version) {
        wpe_log_error("components were removed or entities despawned during query iteration; stopping");
        wpe_assert(it->wpe__removal_version == ecs->removal_version);
        it->wpe__cursor = it->wpe__end;
        it->entity = WPE_ENTITY_NULL;
        return false;
    }

    const uint64_t all = it->wpe__query.all;
    const uint64_t none = it->wpe__query.none;
    while (it->wpe__cursor < it->wpe__end) {
        const uint32_t cursor = it->wpe__cursor++;
        WPE_Entity entity;
        uint64_t mask;
        if (it->wpe__driver == WPE_COMP_NONE) {
            const wpe__EcsSlot* slot = &ecs->slots[cursor];
            if (!slot->alive || slot->spawn_serial >= it->wpe__spawn_serial_end) continue;
            entity = wpe__ecs_make_entity(cursor, slot->generation);
            mask = slot->mask;
        } else {
            entity = ecs->pools[it->wpe__driver].dense_entities[cursor];
            mask = ecs->slots[wpe__ecs_index(entity)].mask;
        }

        if ((mask & all) == all && (mask & none) == 0) {
            it->entity = entity;
            it->wpe__dense_index = cursor;
            return true;
        }
    }
    it->entity = WPE_ENTITY_NULL;
    return false;
}

void* wpe_ecs_iter_get(const WPE_EcsIter* it, WPE_ComponentType type) {
    if (!it || !it->wpe__ecs || it->entity == WPE_ENTITY_NULL) return NULL;
    if (it->wpe__driver != WPE_COMP_NONE && (uint32_t)type == it->wpe__driver) {
        return wpe__ecs_pool_at(&it->wpe__ecs->pools[type], it->wpe__dense_index);
    }
    return wpe_ecs_get_component(it->wpe__ecs, it->entity, type);
}

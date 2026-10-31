#define WPE_LOG_MODULE "graphics"
#include "graphics/anim_states.h"

#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "graphics/animation.h"

/* ---- Graphs --------------------------------------------------------------------------------------- */

static size_t wpe__align(size_t size) {
    return (size + 15u) & ~(size_t)15u;
}

/* One allocation with the arrays after the header, their pointers set and contents zeroed. */
static WPE_AnimGraph* wpe__graph_alloc(uint32_t params, uint32_t states, uint32_t transitions,
                                       uint32_t conditions, uint32_t events) {
    const size_t sizes[5] = {params * sizeof(WPE_AnimParam), states * sizeof(WPE_AnimState),
                             transitions * sizeof(WPE_AnimTransition), conditions * sizeof(WPE_AnimCondition),
                             events * sizeof(WPE_AnimEvent)};
    size_t total = wpe__align(sizeof(WPE_AnimGraph));
    for (int i = 0; i < 5; ++i)
        total += wpe__align(sizes[i]);
    WPE_AnimGraph* graph = wpe_alloc_zeroed(1, total);
    if (!graph) return NULL;
    unsigned char* at = (unsigned char*)graph + wpe__align(sizeof(WPE_AnimGraph));
    void* arrays[5];
    for (int i = 0; i < 5; ++i) {
        arrays[i] = sizes[i] ? at : NULL;
        at += wpe__align(sizes[i]);
    }
    graph->params = arrays[0];
    graph->states = arrays[1];
    graph->transitions = arrays[2];
    graph->conditions = arrays[3];
    graph->events = arrays[4];
    graph->param_count = params;
    graph->state_count = states;
    graph->transition_count = transitions;
    graph->condition_count = conditions;
    graph->event_count = events;
    return graph;
}

static bool wpe__name_ok(const char* name) {
    return name[0] != '\0' && memchr(name, '\0', WPE_ANIM_NAME_MAX) != NULL;
}

/* Logs why a description is invalid; returns false. */
static bool wpe__invalid(const char* what) {
    wpe_log_error("animation states: %s", what);
    return false;
}

static bool wpe__check_desc(const WPE_AnimGraphDesc* d) {
    if (d->param_count > WPE_ANIM_MAX_PARAMS) return wpe__invalid("too many parameters");
    if (d->state_count == 0 || !d->states) return wpe__invalid("a graph needs a state");
    if (d->start >= d->state_count) return wpe__invalid("the start state is not one of its states");
    if ((d->param_count && !d->params) || (d->transition_count && !d->transitions) ||
        (d->condition_count && !d->conditions) || (d->event_count && !d->events))
        return wpe__invalid("an array is missing");
    if (!(d->trigger_buffer >= 0.0f) || !isfinite(d->trigger_buffer))
        return wpe__invalid("bad trigger buffer");
    for (uint32_t i = 0; i < d->param_count; ++i) {
        if (!wpe__name_ok(d->params[i].name) || (unsigned)d->params[i].type > WPE_ANIM_PARAM_TRIGGER ||
            !isfinite(d->params[i].value))
            return wpe__invalid("a parameter has no name, a bad type or a bad value");
        for (uint32_t k = 0; k < i; ++k)
            if (strcmp(d->params[k].name, d->params[i].name) == 0)
                return wpe__invalid("two parameters share a name");
    }
    for (uint32_t i = 0; i < d->state_count; ++i) {
        const WPE_AnimState* s = &d->states[i];
        if (!wpe__name_ok(s->name)) return wpe__invalid("a state has no name");
        for (uint32_t k = 0; k < i; ++k)
            if (strcmp(d->states[k].name, s->name) == 0) return wpe__invalid("two states share a name");
        if (!isfinite(s->speed)) return wpe__invalid("a state's speed is not finite");
        if (s->speed_param != WPE_ANIM_ANY &&
            (s->speed_param >= d->param_count || d->params[s->speed_param].type != WPE_ANIM_PARAM_NUMBER))
            return wpe__invalid("a state's speed parameter is not a number parameter");
        if (s->first_event > d->event_count || s->event_count > d->event_count - s->first_event)
            return wpe__invalid("a state's events are out of range");
    }
    for (uint32_t i = 0; i < d->event_count; ++i)
        if (!wpe__name_ok(d->events[i].name) || !(d->events[i].time >= 0.0f) || !isfinite(d->events[i].time))
            return wpe__invalid("an event has no name or a bad time");
    for (uint32_t i = 0; i < d->transition_count; ++i) {
        const WPE_AnimTransition* t = &d->transitions[i];
        if ((t->from != WPE_ANIM_ANY && t->from >= d->state_count) || t->to >= d->state_count)
            return wpe__invalid("a transition names a state that isn't there");
        if (!(t->fade >= 0.0f) || !isfinite(t->fade) || !isfinite(t->exit) || !isfinite(t->window_start) ||
            !isfinite(t->window_end) || t->window_start > t->window_end)
            return wpe__invalid("a transition has a bad fade, exit or window");
        if (t->first_condition > d->condition_count ||
            t->condition_count > d->condition_count - t->first_condition)
            return wpe__invalid("a transition's conditions are out of range");
    }
    for (uint32_t i = 0; i < d->condition_count; ++i)
        if (d->conditions[i].param >= d->param_count ||
            (unsigned)d->conditions[i].compare > WPE_ANIM_NOT_EQUAL || !isfinite(d->conditions[i].value))
            return wpe__invalid("a condition names a parameter that isn't there");
    return true;
}

WPE_Result wpe_anim_graph_create(const WPE_AnimGraphDesc* desc, WPE_AnimGraph** out_graph) {
    if (!desc || !out_graph) return WPE_RESULT_ERROR_NULL_ARG;
    *out_graph = NULL;
    if (!wpe__check_desc(desc)) return WPE_RESULT_ERROR_INVALID_ARG;
    WPE_AnimGraph* g = wpe__graph_alloc(desc->param_count, desc->state_count, desc->transition_count,
                                        desc->condition_count, desc->event_count);
    if (!g) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (desc->param_count) memcpy(g->params, desc->params, desc->param_count * sizeof *g->params);
    memcpy(g->states, desc->states, desc->state_count * sizeof *g->states);
    if (desc->transition_count)
        memcpy(g->transitions, desc->transitions, desc->transition_count * sizeof *g->transitions);
    if (desc->condition_count)
        memcpy(g->conditions, desc->conditions, desc->condition_count * sizeof *g->conditions);
    if (desc->event_count) memcpy(g->events, desc->events, desc->event_count * sizeof *g->events);
    g->start = desc->start;
    g->trigger_buffer = desc->trigger_buffer;
    /* Each state's events in time order (insertion sort: they are few). */
    for (uint32_t s = 0; s < g->state_count && g->events; ++s) {
        WPE_AnimEvent* events = g->events + g->states[s].first_event;
        for (uint32_t i = 1; i < g->states[s].event_count; ++i) {
            const WPE_AnimEvent e = events[i];
            uint32_t k = i;
            while (k > 0 && events[k - 1].time > e.time) {
                events[k] = events[k - 1];
                --k;
            }
            events[k] = e;
        }
    }
    *out_graph = g;
    return WPE_RESULT_OK;
}

WPE_AnimGraph* wpe_anim_graph_clone(const WPE_AnimGraph* graph) {
    if (!graph) return NULL;
    WPE_AnimGraph* g = wpe__graph_alloc(graph->param_count, graph->state_count, graph->transition_count,
                                        graph->condition_count, graph->event_count);
    if (!g) return NULL;
    if (g->param_count) memcpy(g->params, graph->params, g->param_count * sizeof *g->params);
    if (g->state_count) memcpy(g->states, graph->states, g->state_count * sizeof *g->states);
    if (g->transition_count)
        memcpy(g->transitions, graph->transitions, g->transition_count * sizeof *g->transitions);
    if (g->condition_count)
        memcpy(g->conditions, graph->conditions, g->condition_count * sizeof *g->conditions);
    if (g->event_count) memcpy(g->events, graph->events, g->event_count * sizeof *g->events);
    g->start = graph->start;
    g->trigger_buffer = graph->trigger_buffer;
    return g;
}

void wpe_anim_graph_destroy(WPE_AnimGraph* graph) {
    wpe_free(graph);
}

uint32_t wpe_anim_graph_find_param(const WPE_AnimGraph* graph, const char* name) {
    for (uint32_t i = 0; graph && name && i < graph->param_count; ++i)
        if (strcmp(graph->params[i].name, name) == 0) return i;
    return WPE_ANIM_ANY;
}

uint32_t wpe_anim_graph_find_state(const WPE_AnimGraph* graph, const char* name) {
    for (uint32_t i = 0; graph && name && i < graph->state_count; ++i)
        if (strcmp(graph->states[i].name, name) == 0) return i;
    return WPE_ANIM_ANY;
}

/* ---- Running ---------------------------------------------------------------------------------------- */

static void wpe__notice(WPE_AnimStates* st, WPE_AnimNoticeKind kind, uint32_t index) {
    if (st->queue_count < WPE_ANIM_QUEUE_MAX) {
        st->queue[st->queue_count++] = (WPE_AnimNotice){.kind = kind, .index = index};
    } else {
        wpe_log_warn("animation states: more than %u events in a frame; some were dropped",
                     WPE_ANIM_QUEUE_MAX);
    }
}

/* The state's playback rate: its speed, times its speed parameter if it has one. */
static float wpe__state_speed(const WPE_AnimStates* st, const WPE_AnimState* s) {
    return s->speed_param == WPE_ANIM_ANY ? s->speed : s->speed * st->values[s->speed_param];
}

static void wpe__enter(WPE_AnimStates* st, WPE_Animator* animator, uint32_t state, float fade) {
    const WPE_AnimState* s = &st->graph->states[state];
    st->state = state;
    st->state_time = 0.0f;
    st->clip_time = -1.0f; /* So events at time 0 fire. */
    animator->speed = wpe__state_speed(st, s);
    if (wpe_animator_play(animator, s->animation, fade, s->loop) != WPE_RESULT_OK)
        wpe_log_warn("animation state '%s' plays animation %u, which the model doesn't have", s->name,
                     s->animation);
    wpe__notice(st, WPE_ANIM_NOTICE_STATE, state);
}

/* How far through the state's clip the animator is (0..1). A clip without length counts as done. */
static float wpe__progress(const WPE_AnimStates* st, const WPE_Animator* animator) {
    const WPE_AnimState* s = &st->graph->states[st->state];
    if (animator->animation != s->animation || s->animation >= animator->model->animation_count) return 1.0f;
    const float duration = animator->model->animations[s->animation].duration;
    if (!(duration > 0.0f)) return 1.0f;
    const float t = s->loop ? fmodf(animator->time, duration) : animator->time;
    return t <= 0.0f ? 0.0f : (t >= duration ? 1.0f : t / duration);
}

static bool wpe__holds(const WPE_AnimStates* st, const WPE_AnimCondition* c) {
    const float v = st->values[c->param];
    const bool set = st->graph->params[c->param].type == WPE_ANIM_PARAM_TRIGGER ? v > 0.0f : v != 0.0f;
    switch (c->compare) {
    case WPE_ANIM_IS_SET:
        return set;
    case WPE_ANIM_IS_NOT_SET:
        return !set;
    case WPE_ANIM_GREATER:
        return v > c->value;
    case WPE_ANIM_LESS:
        return v < c->value;
    case WPE_ANIM_GREATER_EQUAL:
        return v >= c->value;
    case WPE_ANIM_LESS_EQUAL:
        return v <= c->value;
    case WPE_ANIM_EQUAL:
        return v == c->value;
    case WPE_ANIM_NOT_EQUAL:
    default:
        return v != c->value;
    }
}

/* The first transition that may fire now, or NULL. */
static const WPE_AnimTransition* wpe__ready(const WPE_AnimStates* st, const WPE_Animator* animator) {
    const WPE_AnimGraph* g = st->graph;
    const float progress = wpe__progress(st, animator);
    for (uint32_t i = 0; i < g->transition_count; ++i) {
        const WPE_AnimTransition* t = &g->transitions[i];
        if (t->from == WPE_ANIM_ANY ? (t->to == st->state && !t->restart) : t->from != st->state) continue;
        if (progress < t->window_start || progress > t->window_end) continue;
        if (t->exit >= 0.0f && progress < t->exit) continue;
        bool all = true;
        for (uint32_t c = 0; c < t->condition_count && all; ++c)
            all = wpe__holds(st, &g->conditions[t->first_condition + c]);
        if (all) return t;
    }
    return NULL;
}

void wpe_anim_states_update(WPE_Ecs* ecs, float dt) {
    if (!ecs || !(dt >= 0.0f)) return;
    WPE_EcsIter it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_ANIM_STATES) | WPE_ECS_MASK(WPE_COMP_ANIMATOR)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_AnimStates* st = wpe_ecs_iter_get(&it, WPE_COMP_ANIM_STATES);
        WPE_Animator* animator = wpe_ecs_iter_get(&it, WPE_COMP_ANIMATOR);
        if (!st->graph || !animator->model) continue;
        if (st->state >= st->graph->state_count) {
            wpe__enter(st, animator, st->graph->start, 0.0f);
            continue;
        }
        st->state_time += dt;
        const WPE_AnimTransition* t = wpe__ready(st, animator);
        if (t) {
            /* The triggers it fired on are used up. */
            for (uint32_t c = 0; c < t->condition_count; ++c) {
                const WPE_AnimCondition* cond = &st->graph->conditions[t->first_condition + c];
                if (st->graph->params[cond->param].type == WPE_ANIM_PARAM_TRIGGER)
                    st->values[cond->param] = 0.0f;
            }
            wpe__enter(st, animator, t->to, t->fade);
        } else {
            animator->speed = wpe__state_speed(st, &st->graph->states[st->state]);
        }
        /* Triggers lapse after the transitions had their chance this update. */
        for (uint32_t p = 0; p < st->graph->param_count; ++p) {
            if (st->graph->params[p].type == WPE_ANIM_PARAM_TRIGGER && st->values[p] > 0.0f)
                st->values[p] = fmaxf(st->values[p] - dt, 0.0f);
        }
    }
}

void wpe_anim_states_scan(WPE_Ecs* ecs) {
    if (!ecs) return;
    WPE_EcsIter it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_ANIM_STATES) | WPE_ECS_MASK(WPE_COMP_ANIMATOR)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_AnimStates* st = wpe_ecs_iter_get(&it, WPE_COMP_ANIM_STATES);
        const WPE_Animator* animator = wpe_ecs_iter_get(&it, WPE_COMP_ANIMATOR);
        if (!st->graph || !animator->model || st->state >= st->graph->state_count) continue;
        const WPE_AnimState* s = &st->graph->states[st->state];
        if (animator->animation != s->animation) continue; /* Something else plays it now. */
        const float previous = st->clip_time, now = animator->time;
        st->clip_time = now;
        if (animator->speed < 0.0f) continue; /* NOTE(limitation): events fire only when playing forwards. */
        const WPE_AnimEvent* events = st->graph->events + s->first_event;
        const bool wrapped = s->loop && now < previous; /* A loop went round since the last scan. */
        for (uint32_t i = 0; i < s->event_count; ++i) {
            const float t = events[i].time;
            const bool passed = wrapped ? (t > previous || t <= now) : (t > previous && t <= now);
            if (passed) wpe__notice(st, WPE_ANIM_NOTICE_EVENT, s->first_event + i);
        }
    }
}

bool wpe_anim_states_set(WPE_AnimStates* states, const char* name, float value) {
    const uint32_t p = states ? wpe_anim_graph_find_param(states->graph, name) : WPE_ANIM_ANY;
    if (p == WPE_ANIM_ANY || !isfinite(value)) return false;
    switch (states->graph->params[p].type) {
    case WPE_ANIM_PARAM_TRIGGER:
        return false;
    case WPE_ANIM_PARAM_BOOL:
        states->values[p] = value != 0.0f ? 1.0f : 0.0f;
        return true;
    case WPE_ANIM_PARAM_NUMBER:
    default:
        states->values[p] = value;
        return true;
    }
}

bool wpe_anim_states_trigger(WPE_AnimStates* states, const char* name) {
    const uint32_t p = states ? wpe_anim_graph_find_param(states->graph, name) : WPE_ANIM_ANY;
    if (p == WPE_ANIM_ANY || states->graph->params[p].type != WPE_ANIM_PARAM_TRIGGER) return false;
    /* Set for the buffer time; a zero buffer still lasts for the next update. */
    states->values[p] = states->graph->trigger_buffer > 0.0f ? states->graph->trigger_buffer : 1e-6f;
    return true;
}

bool wpe_anim_states_enter(WPE_Ecs* ecs, WPE_Entity entity, const char* state, float fade) {
    WPE_AnimStates* st = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_ANIM_STATES) : NULL;
    WPE_Animator* animator = ecs ? wpe_ecs_get_component(ecs, entity, WPE_COMP_ANIMATOR) : NULL;
    const uint32_t s = st ? wpe_anim_graph_find_state(st->graph, state) : WPE_ANIM_ANY;
    if (s == WPE_ANIM_ANY || !animator || !animator->model || !(fade >= 0.0f) || !isfinite(fade))
        return false;
    wpe__enter(st, animator, s, fade);
    return true;
}

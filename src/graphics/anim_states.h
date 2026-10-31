/**
 * @file anim_states.h
 * @brief Animation state machines: which of a model's animations an animator plays, chosen by parameters.
 *
 * A graph (WPE_AnimGraph, immutable) has parameters, states and transitions:
 * - Parameters are numbers, bools or triggers. A trigger (an attack press) stays set for the graph's
 *   `trigger_buffer` seconds, and a transition that fires on it uses it up, so an input pressed a little
 * early still counts (combo buffering).
 * - A state plays one animation (looping or not) at a speed, optionally times a parameter (a run cycle that
 *   keeps pace with the movement), and has events at times in it ("hit" when the blade connects).
 * - Transitions are tried in order each update: the first whose conditions all hold fires, crossfading
 *   into its state. A transition can leave from one state or from any, can wait until a point of the clip
 *   (`exit`, as a fraction of it: 1 when it ends) and can be allowed only within a span of it (`window`: a
 *   combo's next attack is accepted late in the swing).
 *
 * The WPE_AnimStates component (WPE_COMP_ANIM_STATES) runs a graph on the entity's WPE_Animator. Its add hook
 * copies the graph, so whoever adds the component keeps their own; its remove hook frees the copy. Scripts
 * build it (wpe.set_anim_states); it is not saved in scene files.
 *
 * Per frame: wpe_anim_states_update() (after game code set the parameters, before wpe_animation_update())
 * fires transitions; wpe_anim_states_scan() (after it) finds the events the clips passed. Both queue what
 * happened in the component (WPE_AnimStates::queue) for the listener's script.
 *
 * - Dependencies: graphics (animator), assets (animations)
 * - Thread affinity: the thread owning the world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_ANIM_STATES_H
#define WPE_GRAPHICS_ANIM_STATES_H

#include "graphics/graphics.h"

#define WPE_ANIM_NAME_MAX 32u /* Names of parameters, states and events, with the terminator. */
#define WPE_ANIM_MAX_PARAMS 16u
#define WPE_ANIM_ANY UINT32_MAX /* A transition's `from`: any state. */
#define WPE_ANIM_QUEUE_MAX 16u  /* Events and state changes kept for the listener between dispatches. */

typedef enum WPE_AnimParamType {
    WPE_ANIM_PARAM_NUMBER = 0,
    WPE_ANIM_PARAM_BOOL,    /* 0 or 1. */
    WPE_ANIM_PARAM_TRIGGER, /* Set: seconds left before it lapses. */
} WPE_AnimParamType;

typedef enum WPE_AnimCompare {
    WPE_ANIM_IS_SET = 0, /* A bool true, a trigger set, a number not 0. */
    WPE_ANIM_IS_NOT_SET,
    WPE_ANIM_GREATER,
    WPE_ANIM_LESS,
    WPE_ANIM_GREATER_EQUAL,
    WPE_ANIM_LESS_EQUAL,
    WPE_ANIM_EQUAL,
    WPE_ANIM_NOT_EQUAL,
} WPE_AnimCompare;

typedef struct WPE_AnimParam {
    char name[WPE_ANIM_NAME_MAX];
    WPE_AnimParamType type;
    float value; /* The starting value (triggers start unset). */
} WPE_AnimParam;

typedef struct WPE_AnimEvent {
    char name[WPE_ANIM_NAME_MAX];
    float time; /* Seconds into the state's animation. */
} WPE_AnimEvent;

typedef struct WPE_AnimState {
    char name[WPE_ANIM_NAME_MAX];
    uint32_t animation; /* Index into the model's animations. */
    bool loop;
    float speed;          /* Playback rate (1 normal). */
    uint32_t speed_param; /* A number parameter the rate is multiplied by (times speed), or WPE_ANIM_ANY. */
    uint32_t first_event; /* Its events in WPE_AnimGraph::events (sorted by time). */
    uint32_t event_count;
} WPE_AnimState;

typedef struct WPE_AnimCondition {
    uint32_t param;
    WPE_AnimCompare compare;
    float value;
} WPE_AnimCondition;

typedef struct WPE_AnimTransition {
    uint32_t from; /* A state, or WPE_ANIM_ANY. */
    uint32_t to;
    float fade;         /* Crossfade seconds. */
    float exit;         /* Fires once the clip is this far through (fraction; 1: at its end); < 0: no wait. */
    float window_start; /* Allowed only while the clip is between these fractions of its length. */
    float window_end;
    bool restart; /* From any state: also when already in `to` (playing it again from the start). */
    uint32_t first_condition; /* In WPE_AnimGraph::conditions; all must hold. */
    uint32_t condition_count;
} WPE_AnimTransition;

/** A graph: everything lives in this one allocation (wpe_anim_graph_create). */
typedef struct WPE_AnimGraph {
    WPE_AnimParam* params;
    uint32_t param_count;
    WPE_AnimState* states;
    uint32_t state_count;
    WPE_AnimTransition* transitions;
    uint32_t transition_count;
    WPE_AnimCondition* conditions;
    uint32_t condition_count;
    WPE_AnimEvent* events;
    uint32_t event_count;
    uint32_t start;       /* The state it begins in. */
    float trigger_buffer; /* Seconds a trigger stays set (0.2 by default). */
} WPE_AnimGraph;

/** What a graph is made from (arrays are copied). Indices refer to the arrays given. */
typedef struct WPE_AnimGraphDesc {
    const WPE_AnimParam* params;
    uint32_t param_count;
    const WPE_AnimState* states; /* Their event ranges index `events`. */
    uint32_t state_count;
    const WPE_AnimTransition* transitions; /* Their condition ranges index `conditions`. */
    uint32_t transition_count;
    const WPE_AnimCondition* conditions;
    uint32_t condition_count;
    const WPE_AnimEvent* events;
    uint32_t event_count;
    uint32_t start;
    float trigger_buffer;
} WPE_AnimGraphDesc;

/**
 * Builds a graph, checking every index and range (a state's animation is checked against the model when the
 * graph runs). Events are sorted by time within each state.
 *
 * @param out_graph Receives the graph; free it with wpe_anim_graph_destroy().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (logged) or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_anim_graph_create(const WPE_AnimGraphDesc* desc, WPE_AnimGraph** out_graph);

/** @return A copy (one allocation, like the original), or NULL out of memory. @thread_safety Any thread */
WPE_AnimGraph* wpe_anim_graph_clone(const WPE_AnimGraph* graph);

/** Frees a graph; NULL is a no-op. @thread_safety Any thread */
void wpe_anim_graph_destroy(WPE_AnimGraph* graph);

/** @return The index of the parameter or state called `name`, or WPE_ANIM_ANY. @thread_safety Any thread */
uint32_t wpe_anim_graph_find_param(const WPE_AnimGraph* graph, const char* name);
uint32_t wpe_anim_graph_find_state(const WPE_AnimGraph* graph, const char* name);

typedef enum WPE_AnimNoticeKind {
    WPE_ANIM_NOTICE_STATE = 0, /* A state was entered. */
    WPE_ANIM_NOTICE_EVENT,     /* An event's time was passed. */
} WPE_AnimNoticeKind;

typedef struct WPE_AnimNotice {
    WPE_AnimNoticeKind kind;
    uint32_t index; /* The state, or the event (in WPE_AnimGraph::events). */
} WPE_AnimNotice;

typedef struct WPE_AnimStates {
    WPE_AnimGraph* graph; /* The component's own copy (made by the add hook, freed by the remove hook). */
    WPE_Entity
        listener; /* Whose script hears on_animation_state / on_animation_event (WPE_ENTITY_NULL: none). */
    /* Runtime (the add hook resets these). */
    uint32_t state;   /* Index into graph->states; entered by the first update (WPE_ANIM_ANY before it). */
    float state_time; /* Seconds since it was entered. */
    float clip_time;  /* The animator's time at the last scan (events after it fire next). */
    float values[WPE_ANIM_MAX_PARAMS];
    WPE_AnimNotice queue[WPE_ANIM_QUEUE_MAX];
    uint32_t queue_count;
} WPE_AnimStates;

/**
 * Fires transitions on every entity with WPE_AnimStates and WPE_Animator: lets triggers lapse by `dt`, tries
 * the transitions (one per update), plays the state entered and keeps each animator's speed in step with its
 * state's parameter. The first update enters the start state.
 *
 * @thread_safety Thread owning the world
 */
void wpe_anim_states_update(WPE_Ecs* ecs, float dt);

/** Queues the events each state's clip passed since the last scan (call after wpe_animation_update()).
 * @thread_safety Thread owning the world */
void wpe_anim_states_scan(WPE_Ecs* ecs);

/** Sets a number or bool parameter (bools: non-zero is true). @return false for an unknown name or a trigger.
 * @thread_safety Thread owning the world */
bool wpe_anim_states_set(WPE_AnimStates* states, const char* name, float value);

/** Sets a trigger for the graph's buffer time. @return false for an unknown name or one that isn't a trigger.
 * @thread_safety Thread owning the world */
bool wpe_anim_states_trigger(WPE_AnimStates* states, const char* name);

/** Enters a state now (as a transition would, with `fade` seconds). @return false for an unknown state.
 * @thread_safety Thread owning the world */
bool wpe_anim_states_enter(WPE_Ecs* ecs, WPE_Entity entity, const char* state, float fade);

#endif /* WPE_GRAPHICS_ANIM_STATES_H */

/**
 * @file profile.h
 * @brief The profiler's numbers: one entry per engine system, script file or script section, as the engine
 *        collects them (wpe_engine_profile) and shows them (`wpe_runtime --profile`, the editor's Performance
 *        panel, wpe.get_profile).
 *
 * - Dependencies: none
 * - Thread affinity: any thread (plain data)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_PROFILE_H
#define WPE_CORE_PROFILE_H

#include <stdint.h>

#define WPE_PROFILE_NAME_MAX 48u /* Including the terminator. */
/** How many entries the engine's profiler keeps: its stages first, then scripts and sections. */
#define WPE_PROFILE_MAX_ENTRIES 64u

typedef enum WPE_ProfileKind {
    WPE_PROFILE_SYSTEM = 0, /* An engine stage ("physics", "render"); stages include the scripts they call. */
    WPE_PROFILE_SCRIPT,  /* A script file's callbacks, timers and tweens: its own time (not nested calls). */
    WPE_PROFILE_SECTION, /* A script's own section (wpe.profile_begin/end), counted in its script too. */
} WPE_ProfileKind;

typedef struct WPE_ProfileEntry {
    char name[WPE_PROFILE_NAME_MAX];
    WPE_ProfileKind kind;
    double ms;         /* The last frame. */
    double average_ms; /* Smoothed over about half a second. */
    double peak_ms;    /* The highest frame within the last one to two seconds. */
    uint32_t calls;    /* Calls in the last frame (scripts and sections). */
} WPE_ProfileEntry;

#endif /* WPE_CORE_PROFILE_H */

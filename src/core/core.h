/**
 * @file core.h
 * @brief Public interface of the core module: results, logging, assertions, tracked allocation.
 *
 * Core is the dependency root of the engine; every other module may depend on it and it depends
 * on nothing but the C standard library.
 *
 * - Dependencies: none
 * - Thread affinity: see the individual headers
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_CORE_H
#define WPE_CORE_CORE_H

#include "core/alloc.h"
#include "core/arena.h"
#include "core/debug.h"
#include "core/fixed_step.h"
#include "core/log.h"
#include "core/result.h"
#include "core/version.h"

#endif /* WPE_CORE_CORE_H */

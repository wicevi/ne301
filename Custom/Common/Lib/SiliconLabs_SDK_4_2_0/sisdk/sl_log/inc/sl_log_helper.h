/***************************************************************************/ /**
 * @file  sl_log_helper.h
 * @brief Minimal stand-in for the Gecko SDK sl_log_helper.h (external-host
 *        build without the GSDK logging component).
 *
 * The 4.2.x platform_core sources (sl_slist.c, sl_mem_pool.c, ...) pull this
 * header in for SL_LOG_DEBUG_ASSERT only. The full GSDK header chains into
 * sl_log.h / sl_log_common_config.h (the logging subsystem we do not vendor),
 * so this stub keeps the same macro surface with asserts routed to the
 * standard C assert. Matches the approach of the 4.1.1 port's
 * sl_log_helper_si91x.h no-op stubs.
 *
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/

#ifndef SL_LOG_HELPER_H
#define SL_LOG_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

// Arguments formatting helpers used by trace macros (kept for completeness)
#define SL_LOG_U32_ARG(x) ((uint32_t)(uintptr_t)(x))
#define SL_LOG_I32_ARG(x) ((int32_t)(uintptr_t)(x))
#define SL_LOG_PTR_ARG(x) ((void *)(uintptr_t)(x))

// The platform sources only use SL_LOG_DEBUG_ASSERT: compile it to the
// standard C assert so contract violations still trap in debug builds
// (NDEBUG builds compile it out, same as the upstream SL_LOG_DEBUG=0 path).
#define SL_LOG_CRASH_ASSERT(condition) assert(condition)
#define SL_LOG_DEBUG_ASSERT(condition) assert(condition)

#ifdef __cplusplus
}
#endif

#endif /* SL_LOG_HELPER_H */

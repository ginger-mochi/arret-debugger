/*
 * trace.h: Execution trace logging
 *
 * Records every instruction executed on selected CPUs into a ring buffer
 * (for the Qt UI) and optionally to a file.  Lines include disassembly,
 * optional bank prefix, optional register state, and optional SP-based
 * indentation.
 *
 * The trace module manages its own retrodebug execution subscriptions
 * (broad, all addresses) for each enabled CPU.
 */

#ifndef AR_TRACE_H
#define AR_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "retrodebug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRACE_LINE_SIZE 256

/* Start tracing.  If path is non-NULL and non-empty, also write to file.
 * Subscribes for execution events on all enabled CPUs.
 * Returns true on success. */
bool ar_trace_start(const char *path);

/* Stop tracing.  Unsubscribes all trace subscriptions, closes file. */
void ar_trace_stop(void);

/* Is tracing currently active? */
bool ar_trace_active(void);

/* Enable/disable a CPU by id.  NULL or "" = primary CPU.
 * Settings persist across trace sessions. */
bool ar_trace_cpu_enable(const char *cpu_id, bool enable);

/* Query whether a CPU is enabled for tracing. */
bool ar_trace_cpu_enabled(const char *cpu_id);

/* Enable/disable instruction tracing (default: true). */
void ar_trace_set_instructions(bool enable);
bool ar_trace_get_instructions(void);

/* Enable/disable interrupt tracing (default: true). */
void ar_trace_set_interrupts(bool enable);
bool ar_trace_get_interrupts(void);

/* Enable/disable register logging after "; ". */
void ar_trace_set_registers(bool enable);
bool ar_trace_get_registers(void);

/* Enable/disable SP-based indentation. */
void ar_trace_set_indent(bool enable);
bool ar_trace_get_indent(void);

/* System-specific trace options (driven by sys::TraceOption).
 * Options are identified by index (0-based).
 * Settings persist across trace sessions. */
unsigned ar_trace_sys_option_count(void);
const char *ar_trace_sys_option_label(unsigned idx);
void ar_trace_sys_option_enable(unsigned idx, bool enable);
bool ar_trace_sys_option_enabled(unsigned idx);

/* Get the trace file path (empty string if none). */
const char *ar_trace_file_path(void);

/* Read new trace lines since last call.
 * out must point to a buffer of at least max_lines * TRACE_LINE_SIZE bytes.
 * Each line is a null-terminated string.
 * Returns the number of lines copied. */
unsigned ar_trace_read_new(char *out, unsigned max_lines);

/* Total number of lines traced (monotonic, may exceed ring size). */
uint64_t ar_trace_total_lines(void);

/* Called from handle_event for execution events.
 * Returns false (trace never halts the core). */
bool ar_trace_on_event(rd_SubscriptionID sub_id, rd_Event const *event);

/* Check if sub_id belongs to a trace subscription. */
bool ar_trace_is_sub(rd_SubscriptionID sub_id);

#ifdef __cplusplus
}
#endif

#endif /* AR_TRACE_H */

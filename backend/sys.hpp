/*
 * sys.hpp: System descriptor module
 *
 * Maps system description strings (from rd_System.v1.description) to
 * system-specific metadata -- interrupt names, trace options, etc.
 */

#ifndef AR_SYS_H
#define AR_SYS_H

#include "retrodebug.h"

namespace sys {

/* Function pointer type for logging a trace line.
 * The sys module calls this from its event handler to write a line
 * to the trace ring buffer and optional file. */
typedef void (*trace_log_fn)(const char *line);

struct TraceOption {
    const char *label;           // displayed in the UI
};

struct Sys {
    const char *description;          // matches rd_System.v1.description
    const char *const *int_names;     // indexed by interrupt kind
    unsigned num_int_names;

    const TraceOption *trace_options;
    unsigned num_trace_options;

    /* Start a trace option.  The sys module should subscribe to whatever
     * events it needs via dif, and log output by calling log_fn.
     * Called on the core thread (from ar_trace_start or handle_event context). */
    bool (*trace_option_start)(unsigned option_idx, rd_DebuggerIf *dif,
                               trace_log_fn log_fn);

    /* Stop a trace option.  Unsubscribe all events, release resources.
     * Called on content unload or trace stop. */
    void (*trace_option_stop)(unsigned option_idx, rd_DebuggerIf *dif);

    /* Return true if sub_id belongs to this system's trace options. */
    bool (*trace_option_is_sub)(rd_SubscriptionID sub_id);

    /* Handle an event for a trace option subscription.
     * Called from the main handle_event when trace_option_is_sub returns true.
     * Must return false (trace never halts). */
    bool (*trace_option_on_event)(rd_SubscriptionID sub_id,
                                  rd_Event const *event);
};

const Sys *sys_for_desc(const char *description);

} // namespace sys

#endif // AR_SYS_H

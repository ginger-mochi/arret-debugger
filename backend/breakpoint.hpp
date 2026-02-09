/*
 * breakpoint.h: Breakpoint management API
 *
 * Breakpoints have an address, enabled state, execute/read/write flags,
 * and a condition string (future use).  Stored in a map keyed by
 * monotonically increasing public ID (starts at 1, never reused).
 */

#ifndef AR_BREAKPOINT_H
#define AR_BREAKPOINT_H

#include <stdint.h>
#include <stdbool.h>

#include "retrodebug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AR_BP_EXECUTE  0x1
#define AR_BP_READ     0x2
#define AR_BP_WRITE    0x4

typedef struct ar_breakpoint {
    int      id;
    uint64_t address;
    bool     enabled;
    bool     temporary;      /* auto-delete on first hit */
    unsigned flags;          /* bitmask of AR_BP_* */
    char     condition[256]; /* future */
    char     cpu_id[64];     /* empty string = primary CPU */
} ar_breakpoint;

int      ar_bp_add(uint64_t addr, unsigned flags, bool enabled, bool temporary, const char *cond, const char *cpu_id);
bool     ar_bp_delete(int id);
bool     ar_bp_enable(int id, bool enabled);
bool     ar_bp_set_temporary(int id, bool temporary);
bool     ar_bp_replace(int id, uint64_t addr, unsigned flags, bool enabled, bool temporary, const char *cond, const char *cpu_id);
const ar_breakpoint *ar_bp_get(int id);
unsigned ar_bp_list(ar_breakpoint *out, unsigned max);
unsigned ar_bp_count(void);
void     ar_bp_clear(void);

/* Returns true if the given subscription ID belongs to a breakpoint */
bool     ar_bp_sub_is_breakpoint(rd_SubscriptionID sub_id);

/* Returns breakpoint ID for a given subscription ID, or -1 if not found */
int      ar_bp_sub_to_id(rd_SubscriptionID sub_id);

/* Persistence */
bool     ar_bp_save(const char *path);
bool     ar_bp_load(const char *path);
void     ar_bp_set_auto(bool on);
void     ar_bp_auto_load(void);

/* Breakpoint hit — returns bp id (>= 0) or -1 if none; cleared by ack */
int      ar_bp_hit(void);
void     ar_bp_ack_hit(void);

/* Deferred temp-breakpoint deletion (safe to call from event handler) */
void     ar_bp_defer_delete(int id);
void     ar_bp_flush_deferred(void);

#ifdef __cplusplus
}
#endif

#endif /* AR_BREAKPOINT_H */

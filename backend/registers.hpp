/*
 * registers.h: CPU-agnostic register name ↔ index mapping
 */

#ifndef AI_RETRO_REGISTERS_H
#define AI_RETRO_REGISTERS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns lowercase canonical name, or NULL if index is out of range */
const char *ar_reg_name(unsigned cpu_type, unsigned reg_index);

/* Case-insensitive lookup; returns -1 if not found */
int ar_reg_from_name(unsigned cpu_type, const char *name);

/* Number of hex digits for display (2 = 8-bit, 4 = 16-bit, 8 = 32-bit) */
int ar_reg_digits(unsigned cpu_type, unsigned reg_index);

/* Total number of named registers for this CPU type */
unsigned ar_reg_count(unsigned cpu_type);

/* Returns the rd_* index of the Nth register (0-based order), or -1 */
int ar_reg_by_order(unsigned cpu_type, unsigned n);

/* Returns the PC register index for a CPU type, or -1 if unknown */
int ar_reg_pc(unsigned cpu_type);

#ifdef __cplusplus
}
#endif

#endif /* AI_RETRO_REGISTERS_H */

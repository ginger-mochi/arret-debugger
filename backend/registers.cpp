/*
 * registers.cpp: CPU-agnostic register name ↔ index mapping
 *
 * Each CPU's registers are defined once via X-macro tables.
 * Adding a register = one X(...) line.  The RD_* constants come from
 * retrodebug.h, so the compiler catches typos or stale entries.
 */

#include "registers.hpp"
#include "retrodebug.h"

#include <strings.h>

/* ========================================================================
 * X-macro register tables
 *
 * Format: X(rd_constant, name_string, hex_digits)
 * ======================================================================== */

#define RD_LR35902_REGS(X) \
    X(RD_LR35902_A,   "a",   2) \
    X(RD_LR35902_F,   "f",   2) \
    X(RD_LR35902_B,   "b",   2) \
    X(RD_LR35902_C,   "c",   2) \
    X(RD_LR35902_D,   "d",   2) \
    X(RD_LR35902_E,   "e",   2) \
    X(RD_LR35902_H,   "h",   2) \
    X(RD_LR35902_L,   "l",   2) \
    X(RD_LR35902_SP,  "sp",  4) \
    X(RD_LR35902_PC,  "pc",  4) \
    X(RD_LR35902_AF,  "af",  4) \
    X(RD_LR35902_BC,  "bc",  4) \
    X(RD_LR35902_DE,  "de",  4) \
    X(RD_LR35902_HL,  "hl",  4) \
    X(RD_LR35902_IME, "ime", 2)

#define RD_Z80_REGS(X) \
    X(RD_Z80_A,   "a",   2) \
    X(RD_Z80_F,   "f",   2) \
    X(RD_Z80_BC,  "bc",  4) \
    X(RD_Z80_DE,  "de",  4) \
    X(RD_Z80_HL,  "hl",  4) \
    X(RD_Z80_IX,  "ix",  4) \
    X(RD_Z80_IY,  "iy",  4) \
    X(RD_Z80_AF2, "af'", 4) \
    X(RD_Z80_BC2, "bc'", 4) \
    X(RD_Z80_DE2, "de'", 4) \
    X(RD_Z80_HL2, "hl'", 4) \
    X(RD_Z80_I,   "i",   2) \
    X(RD_Z80_R,   "r",   2) \
    X(RD_Z80_SP,  "sp",  4) \
    X(RD_Z80_PC,  "pc",  4) \
    X(RD_Z80_IFF, "iff", 2) \
    X(RD_Z80_IM,  "im",  2) \
    X(RD_Z80_WZ,  "wz",  4)

#define RD_6502_REGS(X) \
    X(RD_6502_A,  "a",  2) \
    X(RD_6502_X,  "x",  2) \
    X(RD_6502_Y,  "y",  2) \
    X(RD_6502_S,  "s",  2) \
    X(RD_6502_PC, "pc", 4) \
    X(RD_6502_P,  "p",  2)

#define RD_65816_REGS(X) \
    X(RD_65816_A,   "a",   4) \
    X(RD_65816_X,   "x",   4) \
    X(RD_65816_Y,   "y",   4) \
    X(RD_65816_S,   "s",   4) \
    X(RD_65816_PC,  "pc",  4) \
    X(RD_65816_P,   "p",   2) \
    X(RD_65816_DB,  "db",  2) \
    X(RD_65816_D,   "d",   4) \
    X(RD_65816_PB,  "pb",  2) \
    X(RD_65816_EMU, "emu", 2)

#define RD_R3000A_REGS(X) \
    X(RD_R3000A_R0, "r0", 8) \
    X(RD_R3000A_AT, "at", 8) \
    X(RD_R3000A_V0, "v0", 8) \
    X(RD_R3000A_V1, "v1", 8) \
    X(RD_R3000A_A0, "a0", 8) \
    X(RD_R3000A_A1, "a1", 8) \
    X(RD_R3000A_A2, "a2", 8) \
    X(RD_R3000A_A3, "a3", 8) \
    X(RD_R3000A_T0, "t0", 8) \
    X(RD_R3000A_T1, "t1", 8) \
    X(RD_R3000A_T2, "t2", 8) \
    X(RD_R3000A_T3, "t3", 8) \
    X(RD_R3000A_T4, "t4", 8) \
    X(RD_R3000A_T5, "t5", 8) \
    X(RD_R3000A_T6, "t6", 8) \
    X(RD_R3000A_T7, "t7", 8) \
    X(RD_R3000A_S0, "s0", 8) \
    X(RD_R3000A_S1, "s1", 8) \
    X(RD_R3000A_S2, "s2", 8) \
    X(RD_R3000A_S3, "s3", 8) \
    X(RD_R3000A_S4, "s4", 8) \
    X(RD_R3000A_S5, "s5", 8) \
    X(RD_R3000A_S6, "s6", 8) \
    X(RD_R3000A_S7, "s7", 8) \
    X(RD_R3000A_T8, "t8", 8) \
    X(RD_R3000A_T9, "t9", 8) \
    X(RD_R3000A_K0, "k0", 8) \
    X(RD_R3000A_K1, "k1", 8) \
    X(RD_R3000A_GP, "gp", 8) \
    X(RD_R3000A_SP, "sp", 8) \
    X(RD_R3000A_FP, "fp", 8) \
    X(RD_R3000A_RA, "ra", 8) \
    X(RD_R3000A_PC, "pc", 8) \
    X(RD_R3000A_LO, "lo", 8) \
    X(RD_R3000A_HI, "hi", 8)

/* ========================================================================
 * Table generation
 * ======================================================================== */

struct RegEntry {
    unsigned index;
    const char *name;
    int digits;
};

#define REG_ENTRY(id, n, d) { (id), (n), (d) },

static const RegEntry lr35902_regs[] = { RD_LR35902_REGS(REG_ENTRY) };
static const RegEntry z80_regs[]     = { RD_Z80_REGS(REG_ENTRY) };
static const RegEntry r6502_regs[]   = { RD_6502_REGS(REG_ENTRY) };
static const RegEntry r65816_regs[]  = { RD_65816_REGS(REG_ENTRY) };
static const RegEntry r3000a_regs[]  = { RD_R3000A_REGS(REG_ENTRY) };

#undef REG_ENTRY

/* ========================================================================
 * Dispatch helper
 * ======================================================================== */

template<unsigned N>
static constexpr unsigned arrsize(const RegEntry (&)[N]) { return N; }

static const RegEntry *table_for_cpu(unsigned cpu_type, unsigned &count) {
    switch (cpu_type) {
    case RD_CPU_LR35902: count = arrsize(lr35902_regs); return lr35902_regs;
    case RD_CPU_Z80:     count = arrsize(z80_regs);     return z80_regs;
    case RD_CPU_6502:    count = arrsize(r6502_regs);   return r6502_regs;
    case RD_CPU_65816:   count = arrsize(r65816_regs);  return r65816_regs;
    case RD_CPU_R3000A:  count = arrsize(r3000a_regs);  return r3000a_regs;
    default: count = 0; return nullptr;
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

const char *ar_reg_name(unsigned cpu_type, unsigned reg_index) {
    unsigned count;
    const RegEntry *tbl = table_for_cpu(cpu_type, count);
    if (!tbl) return nullptr;
    for (unsigned i = 0; i < count; i++)
        if (tbl[i].index == reg_index) return tbl[i].name;
    return nullptr;
}

int ar_reg_from_name(unsigned cpu_type, const char *name) {
    if (!name) return -1;
    unsigned count;
    const RegEntry *tbl = table_for_cpu(cpu_type, count);
    if (!tbl) return -1;
    for (unsigned i = 0; i < count; i++)
        if (strcasecmp(name, tbl[i].name) == 0) return (int)tbl[i].index;
    return -1;
}

int ar_reg_digits(unsigned cpu_type, unsigned reg_index) {
    unsigned count;
    const RegEntry *tbl = table_for_cpu(cpu_type, count);
    if (!tbl) return 2;
    for (unsigned i = 0; i < count; i++)
        if (tbl[i].index == reg_index) return tbl[i].digits;
    return 2;
}

unsigned ar_reg_count(unsigned cpu_type) {
    unsigned count;
    table_for_cpu(cpu_type, count);
    return count;
}

int ar_reg_by_order(unsigned cpu_type, unsigned n) {
    unsigned count;
    const RegEntry *tbl = table_for_cpu(cpu_type, count);
    if (!tbl || n >= count) return -1;
    return (int)tbl[n].index;
}

int ar_reg_pc(unsigned cpu_type) {
    switch (cpu_type) {
    case RD_CPU_LR35902: return RD_LR35902_PC;
    case RD_CPU_Z80:     return RD_Z80_PC;
    case RD_CPU_6502:    return RD_6502_PC;
    case RD_CPU_65816:   return RD_65816_PC;
    case RD_CPU_R3000A:  return RD_R3000A_PC;
    default: return -1;
    }
}

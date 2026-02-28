/*
 * arch.hpp: Architecture module
 *
 * Table-driven disassembler plus register layout and trace register
 * descriptors, starting with Sharp LR35902 (Game Boy CPU).
 * Designed to be extensible to other architectures.
 */

#ifndef AR_ARCH_H
#define AR_ARCH_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct rd_Cpu;

namespace arch {

struct Instruction {
    uint64_t    address;      // Address of this instruction
    uint8_t     length;       // Byte length
    std::string text;         // e.g. "LD BC,$1234"

    bool     breaks_flow;     // Unconditional non-sequential flow (JP, JR uncond, RET, RETI)
    bool     has_target;      // True if target is valid
    uint64_t target;          // Computed jump destination (JP/JR variants only)
    bool     is_error;        // Invalid/undefined opcode
};

/* ---- Register layout descriptors (for Qt register pane) ---- */

struct RegFlag {
    unsigned bit;
    const char *name;       // NULL = show bit number
};

enum RegLayoutType { REG_HEX, REG_FLAGS };

struct RegLayoutEntry {
    RegLayoutType type;
    const char *label;          // REG_HEX: field label (e.g. "A", "BC")
    unsigned reg_index;         // register index (REG_HEX + REG_FLAGS)
    unsigned bits;              // REG_HEX: register width in bits (8/16/32)
    const RegFlag *flags;       // REG_FLAGS: flag definitions
    unsigned num_flags;         // REG_FLAGS: count
};

/* ---- Trace register descriptors ---- */

struct TraceReg {
    unsigned reg_index;
    const char *name;           // uppercase display name
    unsigned bits;              // register width in bits
};

/* ---- Stack trace types ---- */

struct StackFrame {
    uint64_t pc;            // Return address (caller's PC)
    uint64_t sp;            // Stack pointer at this frame
    uint64_t func_addr;     // Estimated function start (UINT64_MAX if unknown)
};

enum class StackTraceStatus {
    OK,                     // Completed normally (hit end of chain)
    MAX_DEPTH,              // Hit max_depth limit
    SCAN_LIMIT,             // Couldn't find function prologue within scan window
    INVALID_SP,             // SP invalid or moved wrong direction
    INVALID_RA,             // RA pointed somewhere absurd
    READ_ERROR,             // Memory read failed
};

struct StackTrace {
    std::vector<StackFrame> frames;
    StackTraceStatus        status;
};

/* ---- Architecture descriptor ---- */

struct Arch {
    unsigned cpu_type;              // RD_MAKE_CPU_TYPE value from retrodebug.h
    unsigned max_insn_size;         // Maximum instruction size in bytes
    unsigned alignment;             // Instruction alignment in bytes
    const RegLayoutEntry *reg_layout;   // NULL = generic fallback
    unsigned num_reg_layout;
    const TraceReg *trace_regs;         // NULL = log all registers
    unsigned num_trace_regs;
    unsigned branch_delay_slots;    // 0 = no delay slot; 1 = MIPS-style (branch takes effect after next insn)

    // Stack trace support (NULL if not supported for this arch)
    const char *const *calling_conventions;  // nullptr-terminated list; first is default
    StackTrace (*stack_trace_fn)(rd_Cpu const *cpu, unsigned max_depth,
                                 unsigned cc_index);
};

const Arch *arch_for_cpu(unsigned cpu_type);

std::vector<Instruction> disassemble(
    std::span<const uint8_t> data,
    uint64_t base_addr,
    unsigned cpu_type,
    uint32_t flags = 0);

std::span<const char *const> stack_trace_conventions(unsigned cpu_type);
StackTrace stack_trace(rd_Cpu const *cpu, unsigned max_depth = 64,
                       unsigned cc_index = 0);

} // namespace arch

#endif // AR_ARCH_H

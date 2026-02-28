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
};

const Arch *arch_for_cpu(unsigned cpu_type);

std::vector<Instruction> disassemble(
    std::span<const uint8_t> data,
    uint64_t base_addr,
    unsigned cpu_type,
    uint32_t flags = 0);

} // namespace arch

#endif // AR_ARCH_H

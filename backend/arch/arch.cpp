/*
 * arch.cpp: Architecture dispatcher
 */

#include "arch.hpp"
#include "retrodebug.h"

namespace arch {

// Forward declarations for arch-specific disassemblers
std::vector<Instruction> dis_lr35902(std::span<const uint8_t> data,
                                     uint64_t base_addr, uint32_t flags);
std::vector<Instruction> dis_6502(std::span<const uint8_t> data,
                                  uint64_t base_addr, uint32_t flags);
std::vector<Instruction> dis_r3000a(std::span<const uint8_t> data,
                                    uint64_t base_addr, uint32_t flags);

// Forward declarations for arch-specific layout/trace data
extern const RegLayoutEntry lr35902_reg_layout[];
extern const unsigned lr35902_num_reg_layout;
extern const TraceReg lr35902_trace_regs[];
extern const unsigned lr35902_num_trace_regs;

extern const RegLayoutEntry mos6502_reg_layout[];
extern const unsigned mos6502_num_reg_layout;

extern const RegLayoutEntry r3000a_reg_layout[];
extern const unsigned r3000a_num_reg_layout;
extern const TraceReg r3000a_trace_regs[];
extern const unsigned r3000a_num_trace_regs;

// Forward declarations for R3000A stack trace
extern const char *r3000a_cc_names[];
StackTrace r3000a_stack_trace(rd_Cpu const *cpu, unsigned max_depth,
                              unsigned cc_index);

struct ArchEntry {
    Arch arch;
    std::vector<Instruction> (*disassemble_fn)(std::span<const uint8_t>,
                                               uint64_t, uint32_t);
};

static const ArchEntry arch_table[] = {
    { { RD_CPU_LR35902, 3, 1,
        lr35902_reg_layout, lr35902_num_reg_layout,
        lr35902_trace_regs, lr35902_num_trace_regs, 0,
        nullptr, nullptr },
      dis_lr35902 },
    { { RD_CPU_6502, 3, 1,
        mos6502_reg_layout, mos6502_num_reg_layout,
        nullptr, 0, 0,
        nullptr, nullptr },
      dis_6502 },
    { { RD_CPU_R3000A, 4, 4,
        r3000a_reg_layout, r3000a_num_reg_layout,
        r3000a_trace_regs, r3000a_num_trace_regs, 1,
        r3000a_cc_names, r3000a_stack_trace },
      dis_r3000a },
};

const Arch *arch_for_cpu(unsigned cpu_type)
{
    for (auto &e : arch_table)
        if (e.arch.cpu_type == cpu_type)
            return &e.arch;
    return nullptr;
}

std::vector<Instruction> disassemble(std::span<const uint8_t> data,
                                     uint64_t base_addr,
                                     unsigned cpu_type,
                                     uint32_t flags)
{
    for (auto &e : arch_table)
        if (e.arch.cpu_type == cpu_type)
            return e.disassemble_fn(data, base_addr, flags);
    return {};
}

std::span<const char *const> stack_trace_conventions(unsigned cpu_type)
{
    const Arch *a = arch_for_cpu(cpu_type);
    if (!a || !a->calling_conventions) return {};
    unsigned count = 0;
    while (a->calling_conventions[count]) count++;
    return { a->calling_conventions, count };
}

StackTrace stack_trace(rd_Cpu const *cpu, unsigned max_depth, unsigned cc_index)
{
    if (!cpu) return { {}, StackTraceStatus::READ_ERROR };
    const Arch *a = arch_for_cpu(cpu->v1.type);
    if (!a || !a->stack_trace_fn) return { {}, StackTraceStatus::READ_ERROR };
    return a->stack_trace_fn(cpu, max_depth, cc_index);
}

} // namespace arch

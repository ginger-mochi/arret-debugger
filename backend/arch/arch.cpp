/*
 * arch.cpp: Architecture dispatcher
 */

#include "arch.hpp"
#include "retrodebug.h"

namespace arch {

// Forward declarations for arch-specific disassemblers
std::vector<Instruction> dis_lr35902(std::span<const uint8_t> data,
                                     uint64_t base_addr, uint32_t flags);

// Forward declarations for arch-specific layout/trace data
extern const RegLayoutEntry lr35902_reg_layout[];
extern const unsigned lr35902_num_reg_layout;
extern const TraceReg lr35902_trace_regs[];
extern const unsigned lr35902_num_trace_regs;

struct ArchEntry {
    Arch arch;
    std::vector<Instruction> (*disassemble_fn)(std::span<const uint8_t>,
                                               uint64_t, uint32_t);
};

static const ArchEntry arch_table[] = {
    { { RD_CPU_LR35902, 3, 1,
        lr35902_reg_layout, lr35902_num_reg_layout,
        lr35902_trace_regs, lr35902_num_trace_regs },
      dis_lr35902 },
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

} // namespace arch

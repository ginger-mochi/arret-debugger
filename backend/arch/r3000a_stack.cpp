/*
 * r3000a_stack.cpp: R3000A (PSX) stack trace via prologue scanning
 *
 * Unwinds the call stack by scanning backward from each return address
 * looking for the function prologue pattern:
 *   addiu sp, sp, -N   (0x27BDxxxx, imm16 negative)
 *   sw ra, offset(sp)  (0xAFBFxxxx)
 */

#include "arch.hpp"
#include "retrodebug.h"

#include <cstdint>

namespace arch {

const char *r3000a_cc_names[] = { "o32", nullptr };

// PSX RAM: 2MB mirrored at KUSEG 0x00000000, KSEG0 0x80000000, KSEG1 0xA0000000
static constexpr uint32_t RAM_SIZE  = 0x200000;
static constexpr uint32_t KUSEG_BASE = 0x00000000;
static constexpr uint32_t KSEG0_BASE = 0x80000000;
static constexpr uint32_t KSEG1_BASE = 0xA0000000;

static constexpr unsigned MAX_SCAN_INSNS = 2000;
static constexpr uint32_t MAX_FRAME_SIZE = 0x10000; // 64KB

static bool is_ram_addr(uint32_t addr) {
    uint32_t off;
    if (addr < KUSEG_BASE + RAM_SIZE) off = addr - KUSEG_BASE;
    else if (addr >= KSEG0_BASE && addr < KSEG0_BASE + RAM_SIZE) off = addr - KSEG0_BASE;
    else if (addr >= KSEG1_BASE && addr < KSEG1_BASE + RAM_SIZE) off = addr - KSEG1_BASE;
    else return false;
    (void)off;
    return true;
}

static uint32_t read32(rd_Memory const *mem, uint32_t addr) {
    uint8_t b0 = mem->v1.peek(mem, addr + 0, false);
    uint8_t b1 = mem->v1.peek(mem, addr + 1, false);
    uint8_t b2 = mem->v1.peek(mem, addr + 2, false);
    uint8_t b3 = mem->v1.peek(mem, addr + 3, false);
    return (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
}

StackTrace r3000a_stack_trace(rd_Cpu const *cpu, unsigned max_depth,
                              unsigned /*cc_index*/) {
    StackTrace result;
    result.status = StackTraceStatus::OK;

    rd_Memory const *mem = cpu->v1.memory_region;
    if (!mem) {
        result.status = StackTraceStatus::READ_ERROR;
        return result;
    }

    uint32_t pc = (uint32_t)cpu->v1.get_register(cpu, RD_R3000A_PC);
    uint32_t sp = (uint32_t)cpu->v1.get_register(cpu, RD_R3000A_SP);
    uint32_t ra = (uint32_t)cpu->v1.get_register(cpu, RD_R3000A_RA);

    // Push current frame
    result.frames.push_back({ pc, sp, UINT64_MAX });

    for (unsigned depth = 0; depth < max_depth; depth++) {
        if (ra == 0) {
            result.status = StackTraceStatus::OK;
            return result;
        }
        if (ra & 3) {
            result.status = StackTraceStatus::INVALID_RA;
            return result;
        }
        if (!is_ram_addr(ra)) {
            result.status = StackTraceStatus::INVALID_RA;
            return result;
        }

        // Scan backward from pc looking for prologue
        uint32_t scan_pc = pc;
        uint32_t frame_size = 0;
        uint32_t ra_offset = UINT32_MAX;
        uint32_t func_start = UINT32_MAX;
        bool found_addiu_sp = false;

        // Compute scan lower bound (KSEG0 base or 2000 insns back)
        uint32_t scan_limit_addr = KSEG0_BASE;
        if (scan_pc - KSEG0_BASE > MAX_SCAN_INSNS * 4)
            scan_limit_addr = scan_pc - MAX_SCAN_INSNS * 4;
        // For KUSEG addresses, bound similarly
        if (scan_pc < KSEG0_BASE) {
            scan_limit_addr = 0;
            if (scan_pc > MAX_SCAN_INSNS * 4)
                scan_limit_addr = scan_pc - MAX_SCAN_INSNS * 4;
        }

        for (uint32_t addr = scan_pc; addr > scan_limit_addr; addr -= 4) {
            if (addr < 4) break;
            uint32_t insn = read32(mem, addr - 4);

            // addiu sp, sp, imm16  =  0x27BD____
            if ((insn & 0xFFFF0000) == 0x27BD0000) {
                int16_t imm = (int16_t)(insn & 0xFFFF);
                if (imm < 0) {
                    frame_size = (uint32_t)(-imm);
                    func_start = addr - 4;
                    found_addiu_sp = true;
                    break;
                }
            }
        }

        if (found_addiu_sp && frame_size > MAX_FRAME_SIZE) {
            result.status = StackTraceStatus::SCAN_LIMIT;
            return result;
        }

        // Look for sw ra, offset(sp) near func_start
        if (found_addiu_sp) {
            // Scan forward from func_start for sw ra
            for (uint32_t addr = func_start; addr < func_start + 40 && addr < scan_pc; addr += 4) {
                uint32_t insn = read32(mem, addr);
                // sw ra, offset(sp) = 0xAFBF____
                if ((insn & 0xFFFF0000) == 0xAFBF0000) {
                    ra_offset = (uint16_t)(insn & 0xFFFF);
                    break;
                }
            }
        }

        // Determine next RA
        uint32_t next_ra;
        if (ra_offset != UINT32_MAX && found_addiu_sp) {
            // RA was saved to stack — read it
            next_ra = read32(mem, sp + ra_offset);
        } else if (depth == 0) {
            // Leaf function: RA is still in register, use it directly
            next_ra = ra;
            // For leaf, frame_size might be 0 (no prologue found yet is ok)
            if (!found_addiu_sp)
                frame_size = 0;
        } else {
            // Non-leaf, non-first frame, no sw ra found — can't continue
            result.status = StackTraceStatus::SCAN_LIMIT;
            return result;
        }

        // Advance SP
        uint32_t next_sp = sp + frame_size;

        // Validate next SP
        if (frame_size > 0) {
            if (next_sp < sp) { // wrapped
                result.status = StackTraceStatus::INVALID_SP;
                return result;
            }
            if ((next_sp & 3) != 0) {
                result.status = StackTraceStatus::INVALID_SP;
                return result;
            }
        }

        // Push frame for the caller
        result.frames.push_back({
            next_ra,
            next_sp,
            found_addiu_sp ? (uint64_t)func_start : UINT64_MAX
        });

        if (next_ra == 0) {
            result.status = StackTraceStatus::OK;
            return result;
        }

        // Set up for next iteration
        pc = next_ra;
        sp = next_sp;
        ra = next_ra; // will be overwritten by stack read in next iteration
    }

    result.status = StackTraceStatus::MAX_DEPTH;
    return result;
}

} // namespace arch

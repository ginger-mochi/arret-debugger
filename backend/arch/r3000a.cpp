/*
 * r3000a.cpp: MIPS R3000A architecture data
 *
 * Disassembler covering MIPS I base instructions, COP0 (system control),
 * and COP2/GTE (Geometry Transformation Engine) for PlayStation.
 *
 * Register layout and trace register descriptors for the Qt frontend
 * and trace engine.
 */

#include "arch.hpp"
#include "retrodebug.h"

#include <cstdio>
#include <cstring>

namespace arch {

/* ======================================================================== */
/* GPR names                                                                 */
/* ======================================================================== */

static const char *const gpr_name[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

/* ======================================================================== */
/* COP0 register names                                                       */
/* ======================================================================== */

static const char *cop0_reg_name(unsigned r)
{
    switch (r) {
    case  3: return "BPC";
    case  5: return "BDA";
    case  7: return "DCIC";
    case  8: return "BadVaddr";
    case  9: return "BDAM";
    case 11: return "BPCM";
    case 12: return "Status";
    case 13: return "Cause";
    case 14: return "EPC";
    case 15: return "PRId";
    default: {
        static thread_local char buf[8];
        snprintf(buf, sizeof(buf), "cop0r%u", r);
        return buf;
    }
    }
}

/* ======================================================================== */
/* Field extraction                                                          */
/* ======================================================================== */

static inline unsigned field_op(uint32_t w)     { return (w >> 26) & 0x3F; }
static inline unsigned field_rs(uint32_t w)     { return (w >> 21) & 0x1F; }
static inline unsigned field_rt(uint32_t w)     { return (w >> 16) & 0x1F; }
static inline unsigned field_rd(uint32_t w)     { return (w >> 11) & 0x1F; }
static inline unsigned field_shamt(uint32_t w)  { return (w >>  6) & 0x1F; }
static inline unsigned field_funct(uint32_t w)  { return  w        & 0x3F; }
static inline uint16_t field_imm16(uint32_t w)  { return (uint16_t)w; }
static inline uint32_t field_target(uint32_t w) { return  w & 0x03FFFFFF; }

/* ======================================================================== */
/* GTE command table                                                         */
/* ======================================================================== */

struct GTECmd {
    uint8_t  funct;
    const char *name;
};

static const GTECmd gte_cmds[] = {
    { 0x01, "RTPS"  }, { 0x06, "NCLIP" }, { 0x0C, "OP"    },
    { 0x10, "DPCS"  }, { 0x11, "INTPL" }, { 0x12, "MVMVA" },
    { 0x13, "NCDS"  }, { 0x14, "CDP"   }, { 0x16, "NCDT"  },
    { 0x1B, "NCCS"  }, { 0x1C, "CC"    }, { 0x1E, "NCS"   },
    { 0x20, "NCT"   }, { 0x28, "SQR"   }, { 0x29, "DCPL"  },
    { 0x2A, "DPCT"  }, { 0x2D, "AVSZ3" }, { 0x2E, "AVSZ4" },
    { 0x30, "RTPT"  }, { 0x3D, "GPF"   }, { 0x3E, "GPL"   },
    { 0x3F, "NCCT"  },
};

static const char *gte_cmd_name(unsigned funct)
{
    for (auto &c : gte_cmds)
        if (c.funct == funct)
            return c.name;
    return nullptr;
}

/* ======================================================================== */
/* Instruction formatting helpers                                            */
/* ======================================================================== */

static void fmt_unknown(char *buf, size_t sz, uint32_t w)
{
    snprintf(buf, sz, "DW %08X", w);
}

/* ======================================================================== */
/* SPECIAL (op=0x00) decoder                                                 */
/* ======================================================================== */

static Instruction decode_special(uint32_t w, uint64_t addr)
{
    char buf[64];
    unsigned rd = field_rd(w);
    unsigned rs = field_rs(w);
    unsigned rt = field_rt(w);
    unsigned shamt = field_shamt(w);
    unsigned funct = field_funct(w);
    bool breaks = false;
    bool has_target = false;
    uint64_t target = 0;

    switch (funct) {
    /* Shift immediate */
    case 0x00: // SLL
        if (rd == 0 && rt == 0 && shamt == 0) {
            snprintf(buf, sizeof(buf), "NOP");
        } else {
            snprintf(buf, sizeof(buf), "SLL %s,%s,%u", gpr_name[rd], gpr_name[rt], shamt);
        }
        break;
    case 0x02:
        snprintf(buf, sizeof(buf), "SRL %s,%s,%u", gpr_name[rd], gpr_name[rt], shamt);
        break;
    case 0x03:
        snprintf(buf, sizeof(buf), "SRA %s,%s,%u", gpr_name[rd], gpr_name[rt], shamt);
        break;

    /* Shift variable */
    case 0x04:
        snprintf(buf, sizeof(buf), "SLLV %s,%s,%s", gpr_name[rd], gpr_name[rt], gpr_name[rs]);
        break;
    case 0x06:
        snprintf(buf, sizeof(buf), "SRLV %s,%s,%s", gpr_name[rd], gpr_name[rt], gpr_name[rs]);
        break;
    case 0x07:
        snprintf(buf, sizeof(buf), "SRAV %s,%s,%s", gpr_name[rd], gpr_name[rt], gpr_name[rs]);
        break;

    /* Jump register */
    case 0x08: // JR
        snprintf(buf, sizeof(buf), "JR %s", gpr_name[rs]);
        breaks = true;
        break;
    case 0x09: // JALR
        if (rd == 31) {
            snprintf(buf, sizeof(buf), "JALR %s", gpr_name[rs]);
        } else {
            snprintf(buf, sizeof(buf), "JALR %s,%s", gpr_name[rd], gpr_name[rs]);
        }
        break;

    /* System */
    case 0x0C:
        snprintf(buf, sizeof(buf), "SYSCALL");
        breaks = true;
        break;
    case 0x0D:
        snprintf(buf, sizeof(buf), "BREAK");
        breaks = true;
        break;

    /* HI/LO */
    case 0x10:
        snprintf(buf, sizeof(buf), "MFHI %s", gpr_name[rd]);
        break;
    case 0x11:
        snprintf(buf, sizeof(buf), "MTHI %s", gpr_name[rs]);
        break;
    case 0x12:
        snprintf(buf, sizeof(buf), "MFLO %s", gpr_name[rd]);
        break;
    case 0x13:
        snprintf(buf, sizeof(buf), "MTLO %s", gpr_name[rs]);
        break;

    /* Multiply/divide */
    case 0x18:
        snprintf(buf, sizeof(buf), "MULT %s,%s", gpr_name[rs], gpr_name[rt]);
        break;
    case 0x19:
        snprintf(buf, sizeof(buf), "MULTU %s,%s", gpr_name[rs], gpr_name[rt]);
        break;
    case 0x1A:
        snprintf(buf, sizeof(buf), "DIV %s,%s", gpr_name[rs], gpr_name[rt]);
        break;
    case 0x1B:
        snprintf(buf, sizeof(buf), "DIVU %s,%s", gpr_name[rs], gpr_name[rt]);
        break;

    /* ALU register-register */
    case 0x20:
        snprintf(buf, sizeof(buf), "ADD %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;
    case 0x21: // ADDU
        if (rs == 0) {
            snprintf(buf, sizeof(buf), "MOVE %s,%s", gpr_name[rd], gpr_name[rt]);
        } else {
            snprintf(buf, sizeof(buf), "ADDU %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        }
        break;
    case 0x22:
        snprintf(buf, sizeof(buf), "SUB %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;
    case 0x23:
        snprintf(buf, sizeof(buf), "SUBU %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;
    case 0x24:
        snprintf(buf, sizeof(buf), "AND %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;
    case 0x25: // OR
        if (rs == 0) {
            snprintf(buf, sizeof(buf), "MOVE %s,%s", gpr_name[rd], gpr_name[rt]);
        } else {
            snprintf(buf, sizeof(buf), "OR %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        }
        break;
    case 0x26:
        snprintf(buf, sizeof(buf), "XOR %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;
    case 0x27:
        snprintf(buf, sizeof(buf), "NOR %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;
    case 0x2A:
        snprintf(buf, sizeof(buf), "SLT %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;
    case 0x2B:
        snprintf(buf, sizeof(buf), "SLTU %s,%s,%s", gpr_name[rd], gpr_name[rs], gpr_name[rt]);
        break;

    default:
        fmt_unknown(buf, sizeof(buf), w);
        return { addr, 4, buf, false, false, 0, true };
    }

    return { addr, 4, buf, breaks, has_target, target, false };
}

/* ======================================================================== */
/* REGIMM (op=0x01) decoder                                                  */
/* ======================================================================== */

static Instruction decode_regimm(uint32_t w, uint64_t addr)
{
    char buf[64];
    unsigned rs = field_rs(w);
    unsigned rt = field_rt(w);
    int16_t offset = (int16_t)field_imm16(w);
    uint64_t target = (addr + 4 + ((int32_t)offset << 2)) & 0xFFFFFFFF;

    switch (rt) {
    case 0x00:
        snprintf(buf, sizeof(buf), "BLTZ %s,$@%08X", gpr_name[rs], (uint32_t)target);
        break;
    case 0x01:
        snprintf(buf, sizeof(buf), "BGEZ %s,$@%08X", gpr_name[rs], (uint32_t)target);
        break;
    case 0x10:
        snprintf(buf, sizeof(buf), "BLTZAL %s,$@%08X", gpr_name[rs], (uint32_t)target);
        break;
    case 0x11:
        snprintf(buf, sizeof(buf), "BGEZAL %s,$@%08X", gpr_name[rs], (uint32_t)target);
        break;
    default:
        fmt_unknown(buf, sizeof(buf), w);
        return { addr, 4, buf, false, false, 0, true };
    }

    return { addr, 4, buf, false, true, target, false };
}

/* ======================================================================== */
/* COP0 (op=0x10) decoder                                                   */
/* ======================================================================== */

static Instruction decode_cop0(uint32_t w, uint64_t addr)
{
    char buf[64];
    unsigned rs = field_rs(w);
    unsigned rt = field_rt(w);
    unsigned rd = field_rd(w);

    switch (rs) {
    case 0x00:
        snprintf(buf, sizeof(buf), "MFC0 %s,%s", gpr_name[rt], cop0_reg_name(rd));
        break;
    case 0x02:
        snprintf(buf, sizeof(buf), "CFC0 %s,%u", gpr_name[rt], rd);
        break;
    case 0x04:
        snprintf(buf, sizeof(buf), "MTC0 %s,%s", gpr_name[rt], cop0_reg_name(rd));
        break;
    case 0x06:
        snprintf(buf, sizeof(buf), "CTC0 %s,%u", gpr_name[rt], rd);
        break;
    case 0x08: // BC0x
        if (rt == 0)
            snprintf(buf, sizeof(buf), "BC0F $@%08X",
                     (uint32_t)((addr + 4 + ((int32_t)(int16_t)field_imm16(w) << 2)) & 0xFFFFFFFF));
        else if (rt == 1)
            snprintf(buf, sizeof(buf), "BC0T $@%08X",
                     (uint32_t)((addr + 4 + ((int32_t)(int16_t)field_imm16(w) << 2)) & 0xFFFFFFFF));
        else {
            fmt_unknown(buf, sizeof(buf), w);
            return { addr, 4, buf, false, false, 0, true };
        }
        return { addr, 4, buf, false, true,
                 (addr + 4 + ((int32_t)(int16_t)field_imm16(w) << 2)) & 0xFFFFFFFF, false };
    case 0x10: // CO (coprocessor operation)
        if (field_funct(w) == 0x10) {
            snprintf(buf, sizeof(buf), "RFE");
        } else {
            fmt_unknown(buf, sizeof(buf), w);
            return { addr, 4, buf, false, false, 0, true };
        }
        break;
    default:
        fmt_unknown(buf, sizeof(buf), w);
        return { addr, 4, buf, false, false, 0, true };
    }

    return { addr, 4, buf, false, false, 0, false };
}

/* ======================================================================== */
/* COP2/GTE (op=0x12) decoder                                               */
/* ======================================================================== */

static Instruction decode_cop2(uint32_t w, uint64_t addr)
{
    char buf[64];
    unsigned rs = field_rs(w);
    unsigned rt = field_rt(w);
    unsigned rd = field_rd(w);

    // GTE command: bit 25 set
    if (w & (1u << 25)) {
        unsigned funct = w & 0x3F;
        const char *name = gte_cmd_name(funct);
        if (name)
            snprintf(buf, sizeof(buf), "%s", name);
        else
            snprintf(buf, sizeof(buf), "COP2 %07X", w & 0x1FFFFFF);
        return { addr, 4, buf, false, false, 0, name == nullptr };
    }

    switch (rs) {
    case 0x00:
        snprintf(buf, sizeof(buf), "MFC2 %s,%u", gpr_name[rt], rd);
        break;
    case 0x02:
        snprintf(buf, sizeof(buf), "CFC2 %s,%u", gpr_name[rt], rd);
        break;
    case 0x04:
        snprintf(buf, sizeof(buf), "MTC2 %s,%u", gpr_name[rt], rd);
        break;
    case 0x06:
        snprintf(buf, sizeof(buf), "CTC2 %s,%u", gpr_name[rt], rd);
        break;
    case 0x08: // BC2x
        if (rt == 0)
            snprintf(buf, sizeof(buf), "BC2F $@%08X",
                     (uint32_t)((addr + 4 + ((int32_t)(int16_t)field_imm16(w) << 2)) & 0xFFFFFFFF));
        else if (rt == 1)
            snprintf(buf, sizeof(buf), "BC2T $@%08X",
                     (uint32_t)((addr + 4 + ((int32_t)(int16_t)field_imm16(w) << 2)) & 0xFFFFFFFF));
        else {
            fmt_unknown(buf, sizeof(buf), w);
            return { addr, 4, buf, false, false, 0, true };
        }
        return { addr, 4, buf, false, true,
                 (addr + 4 + ((int32_t)(int16_t)field_imm16(w) << 2)) & 0xFFFFFFFF, false };
    default:
        fmt_unknown(buf, sizeof(buf), w);
        return { addr, 4, buf, false, false, 0, true };
    }

    return { addr, 4, buf, false, false, 0, false };
}

/* ======================================================================== */
/* Main disassembler                                                         */
/* ======================================================================== */

std::vector<Instruction> dis_r3000a(std::span<const uint8_t> data,
                                    uint64_t base_addr, uint32_t /*flags*/)
{
    std::vector<Instruction> out;
    size_t pos = 0;

    while (pos + 4 <= data.size()) {
        uint64_t addr = base_addr + pos;

        // Read 4 bytes little-endian
        uint32_t w = (uint32_t)data[pos]
                   | ((uint32_t)data[pos + 1] << 8)
                   | ((uint32_t)data[pos + 2] << 16)
                   | ((uint32_t)data[pos + 3] << 24);

        char buf[64];
        bool breaks = false;
        bool has_target = false;
        uint64_t target = 0;
        bool is_error = false;

        unsigned op = field_op(w);

        switch (op) {
        case 0x00: // SPECIAL
            out.push_back(decode_special(w, addr));
            pos += 4;
            continue;

        case 0x01: // REGIMM
            out.push_back(decode_regimm(w, addr));
            pos += 4;
            continue;

        case 0x02: { // J
            uint64_t t = (addr & 0xF0000000) | ((uint64_t)field_target(w) << 2);
            snprintf(buf, sizeof(buf), "J $@%08X", (uint32_t)t);
            breaks = true;
            has_target = true;
            target = t;
            break;
        }
        case 0x03: { // JAL
            uint64_t t = (addr & 0xF0000000) | ((uint64_t)field_target(w) << 2);
            snprintf(buf, sizeof(buf), "JAL $@%08X", (uint32_t)t);
            has_target = true;
            target = t;
            break;
        }

        case 0x04: { // BEQ
            int16_t offset = (int16_t)field_imm16(w);
            target = (addr + 4 + ((int32_t)offset << 2)) & 0xFFFFFFFF;
            unsigned rs = field_rs(w), rt = field_rt(w);
            if (rs == 0 && rt == 0) {
                snprintf(buf, sizeof(buf), "B $@%08X", (uint32_t)target);
            } else if (rt == 0) {
                snprintf(buf, sizeof(buf), "BEQZ %s,$@%08X", gpr_name[rs], (uint32_t)target);
            } else {
                snprintf(buf, sizeof(buf), "BEQ %s,%s,$@%08X", gpr_name[rs], gpr_name[rt], (uint32_t)target);
            }
            has_target = true;
            break;
        }
        case 0x05: { // BNE
            int16_t offset = (int16_t)field_imm16(w);
            target = (addr + 4 + ((int32_t)offset << 2)) & 0xFFFFFFFF;
            unsigned rs = field_rs(w), rt = field_rt(w);
            if (rt == 0) {
                snprintf(buf, sizeof(buf), "BNEZ %s,$@%08X", gpr_name[rs], (uint32_t)target);
            } else {
                snprintf(buf, sizeof(buf), "BNE %s,%s,$@%08X", gpr_name[rs], gpr_name[rt], (uint32_t)target);
            }
            has_target = true;
            break;
        }
        case 0x06: { // BLEZ
            int16_t offset = (int16_t)field_imm16(w);
            target = (addr + 4 + ((int32_t)offset << 2)) & 0xFFFFFFFF;
            snprintf(buf, sizeof(buf), "BLEZ %s,$@%08X", gpr_name[field_rs(w)], (uint32_t)target);
            has_target = true;
            break;
        }
        case 0x07: { // BGTZ
            int16_t offset = (int16_t)field_imm16(w);
            target = (addr + 4 + ((int32_t)offset << 2)) & 0xFFFFFFFF;
            snprintf(buf, sizeof(buf), "BGTZ %s,$@%08X", gpr_name[field_rs(w)], (uint32_t)target);
            has_target = true;
            break;
        }

        /* Arithmetic immediate */
        case 0x08:
            snprintf(buf, sizeof(buf), "ADDI %s,%s,%d",
                     gpr_name[field_rt(w)], gpr_name[field_rs(w)], (int16_t)field_imm16(w));
            break;
        case 0x09: { // ADDIU
            unsigned rt = field_rt(w), rs = field_rs(w);
            int16_t imm = (int16_t)field_imm16(w);
            if (rs == 0) {
                snprintf(buf, sizeof(buf), "LI %s,%d", gpr_name[rt], imm);
            } else {
                snprintf(buf, sizeof(buf), "ADDIU %s,%s,%d", gpr_name[rt], gpr_name[rs], imm);
            }
            break;
        }
        case 0x0A:
            snprintf(buf, sizeof(buf), "SLTI %s,%s,%d",
                     gpr_name[field_rt(w)], gpr_name[field_rs(w)], (int16_t)field_imm16(w));
            break;
        case 0x0B:
            snprintf(buf, sizeof(buf), "SLTIU %s,%s,%d",
                     gpr_name[field_rt(w)], gpr_name[field_rs(w)], (int16_t)field_imm16(w));
            break;

        /* Logical immediate (hex) */
        case 0x0C:
            snprintf(buf, sizeof(buf), "ANDI %s,%s,$%04X",
                     gpr_name[field_rt(w)], gpr_name[field_rs(w)], field_imm16(w));
            break;
        case 0x0D: { // ORI
            unsigned rt = field_rt(w), rs = field_rs(w);
            uint16_t imm = field_imm16(w);
            if (rs == 0) {
                snprintf(buf, sizeof(buf), "LI %s,$%04X", gpr_name[rt], imm);
            } else {
                snprintf(buf, sizeof(buf), "ORI %s,%s,$%04X", gpr_name[rt], gpr_name[rs], imm);
            }
            break;
        }
        case 0x0E:
            snprintf(buf, sizeof(buf), "XORI %s,%s,$%04X",
                     gpr_name[field_rt(w)], gpr_name[field_rs(w)], field_imm16(w));
            break;
        case 0x0F:
            snprintf(buf, sizeof(buf), "LUI %s,$%04X",
                     gpr_name[field_rt(w)], field_imm16(w));
            break;

        /* Coprocessors */
        case 0x10: // COP0
            out.push_back(decode_cop0(w, addr));
            pos += 4;
            continue;
        case 0x12: // COP2/GTE
            out.push_back(decode_cop2(w, addr));
            pos += 4;
            continue;

        /* Loads */
        case 0x20:
            snprintf(buf, sizeof(buf), "LB %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x21:
            snprintf(buf, sizeof(buf), "LH %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x22:
            snprintf(buf, sizeof(buf), "LWL %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x23:
            snprintf(buf, sizeof(buf), "LW %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x24:
            snprintf(buf, sizeof(buf), "LBU %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x25:
            snprintf(buf, sizeof(buf), "LHU %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x26:
            snprintf(buf, sizeof(buf), "LWR %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;

        /* Stores */
        case 0x28:
            snprintf(buf, sizeof(buf), "SB %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x29:
            snprintf(buf, sizeof(buf), "SH %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x2A:
            snprintf(buf, sizeof(buf), "SWL %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x2B:
            snprintf(buf, sizeof(buf), "SW %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x2E:
            snprintf(buf, sizeof(buf), "SWR %s,%d(%s)",
                     gpr_name[field_rt(w)], (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;

        /* Coprocessor load/store */
        case 0x32: // LWC2
            snprintf(buf, sizeof(buf), "LWC2 %u,%d(%s)",
                     field_rt(w), (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;
        case 0x3A: // SWC2
            snprintf(buf, sizeof(buf), "SWC2 %u,%d(%s)",
                     field_rt(w), (int16_t)field_imm16(w), gpr_name[field_rs(w)]);
            break;

        default:
            fmt_unknown(buf, sizeof(buf), w);
            is_error = true;
            break;
        }

        out.push_back({ addr, 4, buf, breaks, has_target, target, is_error });
        pos += 4;
    }

    return out;
}

/* ======================================================================== */
/* Register layout (for Qt register pane)                                    */
/* ======================================================================== */

extern const RegLayoutEntry r3000a_reg_layout[] = {
    { REG_HEX, "zero", RD_R3000A_R0, 32, nullptr, 0 },
    { REG_HEX, "at",   RD_R3000A_AT, 32, nullptr, 0 },
    { REG_HEX, "v0",   RD_R3000A_V0, 32, nullptr, 0 },
    { REG_HEX, "v1",   RD_R3000A_V1, 32, nullptr, 0 },
    { REG_HEX, "a0",   RD_R3000A_A0, 32, nullptr, 0 },
    { REG_HEX, "a1",   RD_R3000A_A1, 32, nullptr, 0 },
    { REG_HEX, "a2",   RD_R3000A_A2, 32, nullptr, 0 },
    { REG_HEX, "a3",   RD_R3000A_A3, 32, nullptr, 0 },
    { REG_HEX, "t0",   RD_R3000A_T0, 32, nullptr, 0 },
    { REG_HEX, "t1",   RD_R3000A_T1, 32, nullptr, 0 },
    { REG_HEX, "t2",   RD_R3000A_T2, 32, nullptr, 0 },
    { REG_HEX, "t3",   RD_R3000A_T3, 32, nullptr, 0 },
    { REG_HEX, "t4",   RD_R3000A_T4, 32, nullptr, 0 },
    { REG_HEX, "t5",   RD_R3000A_T5, 32, nullptr, 0 },
    { REG_HEX, "t6",   RD_R3000A_T6, 32, nullptr, 0 },
    { REG_HEX, "t7",   RD_R3000A_T7, 32, nullptr, 0 },
    { REG_HEX, "s0",   RD_R3000A_S0, 32, nullptr, 0 },
    { REG_HEX, "s1",   RD_R3000A_S1, 32, nullptr, 0 },
    { REG_HEX, "s2",   RD_R3000A_S2, 32, nullptr, 0 },
    { REG_HEX, "s3",   RD_R3000A_S3, 32, nullptr, 0 },
    { REG_HEX, "s4",   RD_R3000A_S4, 32, nullptr, 0 },
    { REG_HEX, "s5",   RD_R3000A_S5, 32, nullptr, 0 },
    { REG_HEX, "s6",   RD_R3000A_S6, 32, nullptr, 0 },
    { REG_HEX, "s7",   RD_R3000A_S7, 32, nullptr, 0 },
    { REG_HEX, "t8",   RD_R3000A_T8, 32, nullptr, 0 },
    { REG_HEX, "t9",   RD_R3000A_T9, 32, nullptr, 0 },
    { REG_HEX, "k0",   RD_R3000A_K0, 32, nullptr, 0 },
    { REG_HEX, "k1",   RD_R3000A_K1, 32, nullptr, 0 },
    { REG_HEX, "gp",   RD_R3000A_GP, 32, nullptr, 0 },
    { REG_HEX, "sp",   RD_R3000A_SP, 32, nullptr, 0 },
    { REG_HEX, "fp",   RD_R3000A_FP, 32, nullptr, 0 },
    { REG_HEX, "ra",   RD_R3000A_RA, 32, nullptr, 0 },
    { REG_HEX, "PC",    RD_R3000A_PC, 32, nullptr, 0 },
    { REG_HEX, "HI",    RD_R3000A_HI, 32, nullptr, 0 },
    { REG_HEX, "LO",    RD_R3000A_LO, 32, nullptr, 0 },
};

extern const unsigned r3000a_num_reg_layout =
    sizeof(r3000a_reg_layout) / sizeof(r3000a_reg_layout[0]);

/* ======================================================================== */
/* Trace registers                                                           */
/* ======================================================================== */

extern const TraceReg r3000a_trace_regs[] = {
    { RD_R3000A_AT, "AT", 32 },
    { RD_R3000A_V0, "V0", 32 }, { RD_R3000A_V1, "V1", 32 },
    { RD_R3000A_A0, "A0", 32 }, { RD_R3000A_A1, "A1", 32 },
    { RD_R3000A_A2, "A2", 32 }, { RD_R3000A_A3, "A3", 32 },
    { RD_R3000A_T0, "T0", 32 }, { RD_R3000A_T1, "T1", 32 },
    { RD_R3000A_T2, "T2", 32 }, { RD_R3000A_T3, "T3", 32 },
    { RD_R3000A_T4, "T4", 32 }, { RD_R3000A_T5, "T5", 32 },
    { RD_R3000A_T6, "T6", 32 }, { RD_R3000A_T7, "T7", 32 },
    { RD_R3000A_S0, "S0", 32 }, { RD_R3000A_S1, "S1", 32 },
    { RD_R3000A_S2, "S2", 32 }, { RD_R3000A_S3, "S3", 32 },
    { RD_R3000A_S4, "S4", 32 }, { RD_R3000A_S5, "S5", 32 },
    { RD_R3000A_S6, "S6", 32 }, { RD_R3000A_S7, "S7", 32 },
    { RD_R3000A_T8, "T8", 32 }, { RD_R3000A_T9, "T9", 32 },
    { RD_R3000A_K0, "K0", 32 }, { RD_R3000A_K1, "K1", 32 },
    { RD_R3000A_GP, "GP", 32 }, { RD_R3000A_SP, "SP", 32 },
    { RD_R3000A_FP, "FP", 32 }, { RD_R3000A_RA, "RA", 32 },
    { RD_R3000A_HI, "HI", 32 }, { RD_R3000A_LO, "LO", 32 },
};

extern const unsigned r3000a_num_trace_regs =
    sizeof(r3000a_trace_regs) / sizeof(r3000a_trace_regs[0]);

} // namespace arch

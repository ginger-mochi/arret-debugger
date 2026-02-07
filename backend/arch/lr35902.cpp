/*
 * lr35902.cpp: Sharp LR35902 (Game Boy CPU) architecture data
 *
 * Disassembler: table-driven, 256-entry base opcode table with format strings.
 * CB-prefix opcodes are computed from regular bit patterns.
 *
 * Register layout and trace register descriptors for the Qt frontend
 * and trace engine.
 */

#include "arch.hpp"
#include "retrodebug.h"

#include <cstdio>
#include <cstring>

namespace arch {

// Flags for OpEntry
enum : uint8_t {
    F_NONE       = 0,
    F_BREAKS     = 1 << 0,   // Unconditional non-sequential flow
    F_TARGET     = 1 << 1,   // has absolute jump target
    F_REL_TARGET = 1 << 2,   // has relative jump target (compute addr + 2 + signed imm)
};

struct OpEntry {
    const char *fmt;       // printf format, e.g. "LD BC,$%04X"
    uint8_t imm_bytes;     // 0, 1, or 2
    uint8_t flags;
};

// Undefined opcodes produce nullptr fmt
#define UND { nullptr, 0, 0 }

static const OpEntry base_ops[256] = {
    // 0x00-0x0F
    { "NOP",             0, F_NONE },       // 00
    { "LD BC,$%04X",     2, F_NONE },       // 01
    { "LD (BC),A",       0, F_NONE },       // 02
    { "INC BC",          0, F_NONE },       // 03
    { "INC B",           0, F_NONE },       // 04
    { "DEC B",           0, F_NONE },       // 05
    { "LD B,$%02X",      1, F_NONE },       // 06
    { "RLCA",            0, F_NONE },       // 07
    { "LD ($@%04X),SP",  2, F_NONE },       // 08
    { "ADD HL,BC",       0, F_NONE },       // 09
    { "LD A,(BC)",       0, F_NONE },       // 0A
    { "DEC BC",          0, F_NONE },       // 0B
    { "INC C",           0, F_NONE },       // 0C
    { "DEC C",           0, F_NONE },       // 0D
    { "LD C,$%02X",      1, F_NONE },       // 0E
    { "RRCA",            0, F_NONE },       // 0F

    // 0x10-0x1F
    { "STOP",            1, F_NONE },       // 10
    { "LD DE,$%04X",     2, F_NONE },       // 11
    { "LD (DE),A",       0, F_NONE },       // 12
    { "INC DE",          0, F_NONE },       // 13
    { "INC D",           0, F_NONE },       // 14
    { "DEC D",           0, F_NONE },       // 15
    { "LD D,$%02X",      1, F_NONE },       // 16
    { "RLA",             0, F_NONE },       // 17
    { "JR $@%04X",       1, F_BREAKS | F_REL_TARGET }, // 18
    { "ADD HL,DE",       0, F_NONE },       // 19
    { "LD A,(DE)",       0, F_NONE },       // 1A
    { "DEC DE",          0, F_NONE },       // 1B
    { "INC E",           0, F_NONE },       // 1C
    { "DEC E",           0, F_NONE },       // 1D
    { "LD E,$%02X",      1, F_NONE },       // 1E
    { "RRA",             0, F_NONE },       // 1F

    // 0x20-0x2F
    { "JR NZ,$@%04X",    1, F_REL_TARGET }, // 20
    { "LD HL,$%04X",     2, F_NONE },       // 21
    { "LD (HL+),A",      0, F_NONE },       // 22
    { "INC HL",          0, F_NONE },       // 23
    { "INC H",           0, F_NONE },       // 24
    { "DEC H",           0, F_NONE },       // 25
    { "LD H,$%02X",      1, F_NONE },       // 26
    { "DAA",             0, F_NONE },       // 27
    { "JR Z,$@%04X",     1, F_REL_TARGET }, // 28
    { "ADD HL,HL",       0, F_NONE },       // 29
    { "LD A,(HL+)",      0, F_NONE },       // 2A
    { "DEC HL",          0, F_NONE },       // 2B
    { "INC L",           0, F_NONE },       // 2C
    { "DEC L",           0, F_NONE },       // 2D
    { "LD L,$%02X",      1, F_NONE },       // 2E
    { "CPL",             0, F_NONE },       // 2F

    // 0x30-0x3F
    { "JR NC,$@%04X",    1, F_REL_TARGET }, // 30
    { "LD SP,$%04X",     2, F_NONE },       // 31
    { "LD (HL-),A",      0, F_NONE },       // 32
    { "INC SP",          0, F_NONE },       // 33
    { "INC (HL)",        0, F_NONE },       // 34
    { "DEC (HL)",        0, F_NONE },       // 35
    { "LD (HL),$%02X",   1, F_NONE },       // 36
    { "SCF",             0, F_NONE },       // 37
    { "JR C,$@%04X",     1, F_REL_TARGET }, // 38
    { "ADD HL,SP",       0, F_NONE },       // 39
    { "LD A,(HL-)",      0, F_NONE },       // 3A
    { "DEC SP",          0, F_NONE },       // 3B
    { "INC A",           0, F_NONE },       // 3C
    { "DEC A",           0, F_NONE },       // 3D
    { "LD A,$%02X",      1, F_NONE },       // 3E
    { "CCF",             0, F_NONE },       // 3F

    // 0x40-0x4F
    { "LD B,B",          0, F_NONE },       // 40
    { "LD B,C",          0, F_NONE },       // 41
    { "LD B,D",          0, F_NONE },       // 42
    { "LD B,E",          0, F_NONE },       // 43
    { "LD B,H",          0, F_NONE },       // 44
    { "LD B,L",          0, F_NONE },       // 45
    { "LD B,(HL)",       0, F_NONE },       // 46
    { "LD B,A",          0, F_NONE },       // 47
    { "LD C,B",          0, F_NONE },       // 48
    { "LD C,C",          0, F_NONE },       // 49
    { "LD C,D",          0, F_NONE },       // 4A
    { "LD C,E",          0, F_NONE },       // 4B
    { "LD C,H",          0, F_NONE },       // 4C
    { "LD C,L",          0, F_NONE },       // 4D
    { "LD C,(HL)",       0, F_NONE },       // 4E
    { "LD C,A",          0, F_NONE },       // 4F

    // 0x50-0x5F
    { "LD D,B",          0, F_NONE },       // 50
    { "LD D,C",          0, F_NONE },       // 51
    { "LD D,D",          0, F_NONE },       // 52
    { "LD D,E",          0, F_NONE },       // 53
    { "LD D,H",          0, F_NONE },       // 54
    { "LD D,L",          0, F_NONE },       // 55
    { "LD D,(HL)",       0, F_NONE },       // 56
    { "LD D,A",          0, F_NONE },       // 57
    { "LD E,B",          0, F_NONE },       // 58
    { "LD E,C",          0, F_NONE },       // 59
    { "LD E,D",          0, F_NONE },       // 5A
    { "LD E,E",          0, F_NONE },       // 5B
    { "LD E,H",          0, F_NONE },       // 5C
    { "LD E,L",          0, F_NONE },       // 5D
    { "LD E,(HL)",       0, F_NONE },       // 5E
    { "LD E,A",          0, F_NONE },       // 5F

    // 0x60-0x6F
    { "LD H,B",          0, F_NONE },       // 60
    { "LD H,C",          0, F_NONE },       // 61
    { "LD H,D",          0, F_NONE },       // 62
    { "LD H,E",          0, F_NONE },       // 63
    { "LD H,H",          0, F_NONE },       // 64
    { "LD H,L",          0, F_NONE },       // 65
    { "LD H,(HL)",       0, F_NONE },       // 66
    { "LD H,A",          0, F_NONE },       // 67
    { "LD L,B",          0, F_NONE },       // 68
    { "LD L,C",          0, F_NONE },       // 69
    { "LD L,D",          0, F_NONE },       // 6A
    { "LD L,E",          0, F_NONE },       // 6B
    { "LD L,H",          0, F_NONE },       // 6C
    { "LD L,L",          0, F_NONE },       // 6D
    { "LD L,(HL)",       0, F_NONE },       // 6E
    { "LD L,A",          0, F_NONE },       // 6F

    // 0x70-0x7F
    { "LD (HL),B",       0, F_NONE },       // 70
    { "LD (HL),C",       0, F_NONE },       // 71
    { "LD (HL),D",       0, F_NONE },       // 72
    { "LD (HL),E",       0, F_NONE },       // 73
    { "LD (HL),H",       0, F_NONE },       // 74
    { "LD (HL),L",       0, F_NONE },       // 75
    { "HALT",            0, F_NONE },       // 76
    { "LD (HL),A",       0, F_NONE },       // 77
    { "LD A,B",          0, F_NONE },       // 78
    { "LD A,C",          0, F_NONE },       // 79
    { "LD A,D",          0, F_NONE },       // 7A
    { "LD A,E",          0, F_NONE },       // 7B
    { "LD A,H",          0, F_NONE },       // 7C
    { "LD A,L",          0, F_NONE },       // 7D
    { "LD A,(HL)",       0, F_NONE },       // 7E
    { "LD A,A",          0, F_NONE },       // 7F

    // 0x80-0x8F
    { "ADD A,B",         0, F_NONE },       // 80
    { "ADD A,C",         0, F_NONE },       // 81
    { "ADD A,D",         0, F_NONE },       // 82
    { "ADD A,E",         0, F_NONE },       // 83
    { "ADD A,H",         0, F_NONE },       // 84
    { "ADD A,L",         0, F_NONE },       // 85
    { "ADD A,(HL)",      0, F_NONE },       // 86
    { "ADD A,A",         0, F_NONE },       // 87
    { "ADC A,B",         0, F_NONE },       // 88
    { "ADC A,C",         0, F_NONE },       // 89
    { "ADC A,D",         0, F_NONE },       // 8A
    { "ADC A,E",         0, F_NONE },       // 8B
    { "ADC A,H",         0, F_NONE },       // 8C
    { "ADC A,L",         0, F_NONE },       // 8D
    { "ADC A,(HL)",      0, F_NONE },       // 8E
    { "ADC A,A",         0, F_NONE },       // 8F

    // 0x90-0x9F
    { "SUB B",           0, F_NONE },       // 90
    { "SUB C",           0, F_NONE },       // 91
    { "SUB D",           0, F_NONE },       // 92
    { "SUB E",           0, F_NONE },       // 93
    { "SUB H",           0, F_NONE },       // 94
    { "SUB L",           0, F_NONE },       // 95
    { "SUB (HL)",        0, F_NONE },       // 96
    { "SUB A",           0, F_NONE },       // 97
    { "SBC A,B",         0, F_NONE },       // 98
    { "SBC A,C",         0, F_NONE },       // 99
    { "SBC A,D",         0, F_NONE },       // 9A
    { "SBC A,E",         0, F_NONE },       // 9B
    { "SBC A,H",         0, F_NONE },       // 9C
    { "SBC A,L",         0, F_NONE },       // 9D
    { "SBC A,(HL)",      0, F_NONE },       // 9E
    { "SBC A,A",         0, F_NONE },       // 9F

    // 0xA0-0xAF
    { "AND B",           0, F_NONE },       // A0
    { "AND C",           0, F_NONE },       // A1
    { "AND D",           0, F_NONE },       // A2
    { "AND E",           0, F_NONE },       // A3
    { "AND H",           0, F_NONE },       // A4
    { "AND L",           0, F_NONE },       // A5
    { "AND (HL)",        0, F_NONE },       // A6
    { "AND A",           0, F_NONE },       // A7
    { "XOR B",           0, F_NONE },       // A8
    { "XOR C",           0, F_NONE },       // A9
    { "XOR D",           0, F_NONE },       // AA
    { "XOR E",           0, F_NONE },       // AB
    { "XOR H",           0, F_NONE },       // AC
    { "XOR L",           0, F_NONE },       // AD
    { "XOR (HL)",        0, F_NONE },       // AE
    { "XOR A",           0, F_NONE },       // AF

    // 0xB0-0xBF
    { "OR B",            0, F_NONE },       // B0
    { "OR C",            0, F_NONE },       // B1
    { "OR D",            0, F_NONE },       // B2
    { "OR E",            0, F_NONE },       // B3
    { "OR H",            0, F_NONE },       // B4
    { "OR L",            0, F_NONE },       // B5
    { "OR (HL)",         0, F_NONE },       // B6
    { "OR A",            0, F_NONE },       // B7
    { "CP B",            0, F_NONE },       // B8
    { "CP C",            0, F_NONE },       // B9
    { "CP D",            0, F_NONE },       // BA
    { "CP E",            0, F_NONE },       // BB
    { "CP H",            0, F_NONE },       // BC
    { "CP L",            0, F_NONE },       // BD
    { "CP (HL)",         0, F_NONE },       // BE
    { "CP A",            0, F_NONE },       // BF

    // 0xC0-0xCF
    { "RET NZ",          0, F_NONE },       // C0
    { "POP BC",          0, F_NONE },       // C1
    { "JP NZ,$@%04X",    2, F_TARGET },     // C2
    { "JP $@%04X",       2, F_BREAKS | F_TARGET }, // C3
    { "CALL NZ,$@%04X",  2, F_NONE },       // C4
    { "PUSH BC",         0, F_NONE },       // C5
    { "ADD A,$%02X",     1, F_NONE },       // C6
    { "RST $00",         0, F_NONE },       // C7
    { "RET Z",           0, F_NONE },       // C8
    { "RET",             0, F_BREAKS },     // C9
    { "JP Z,$@%04X",     2, F_TARGET },     // CA
    { nullptr,           0, 0 },            // CB (prefix, handled separately)
    { "CALL Z,$@%04X",   2, F_NONE },       // CC
    { "CALL $@%04X",     2, F_NONE },       // CD
    { "ADC A,$%02X",     1, F_NONE },       // CE
    { "RST $08",         0, F_NONE },       // CF

    // 0xD0-0xDF
    { "RET NC",          0, F_NONE },       // D0
    { "POP DE",          0, F_NONE },       // D1
    { "JP NC,$@%04X",    2, F_TARGET },     // D2
    UND,                                    // D3
    { "CALL NC,$@%04X",  2, F_NONE },       // D4
    { "PUSH DE",         0, F_NONE },       // D5
    { "SUB $%02X",       1, F_NONE },       // D6
    { "RST $10",         0, F_NONE },       // D7
    { "RET C",           0, F_NONE },       // D8
    { "RETI",            0, F_BREAKS },     // D9
    { "JP C,$@%04X",     2, F_TARGET },     // DA
    UND,                                    // DB
    { "CALL C,$@%04X",   2, F_NONE },       // DC
    UND,                                    // DD
    { "SBC A,$%02X",     1, F_NONE },       // DE
    { "RST $18",         0, F_NONE },       // DF

    // 0xE0-0xEF
    { "LDH ($@FF%02X),A", 1, F_NONE },     // E0
    { "POP HL",          0, F_NONE },       // E1
    { "LD ($FF00+C),A",  0, F_NONE },       // E2
    UND,                                    // E3
    UND,                                    // E4
    { "PUSH HL",         0, F_NONE },       // E5
    { "AND $%02X",       1, F_NONE },       // E6
    { "RST $20",         0, F_NONE },       // E7
    { "ADD SP,$%02X",    1, F_NONE },       // E8
    { "JP HL",           0, F_BREAKS },     // E9
    { "LD ($@%04X),A",   2, F_NONE },       // EA
    UND,                                    // EB
    UND,                                    // EC
    UND,                                    // ED
    { "XOR $%02X",       1, F_NONE },       // EE
    { "RST $28",         0, F_NONE },       // EF

    // 0xF0-0xFF
    { "LDH A,($@FF%02X)", 1, F_NONE },     // F0
    { "POP AF",          0, F_NONE },       // F1
    { "LD A,($FF00+C)",  0, F_NONE },       // F2
    { "DI",              0, F_NONE },       // F3
    UND,                                    // F4
    { "PUSH AF",         0, F_NONE },       // F5
    { "OR $%02X",        1, F_NONE },       // F6
    { "RST $30",         0, F_NONE },       // F7
    { "LD HL,SP+$%02X",  1, F_NONE },      // F8
    { "LD SP,HL",        0, F_NONE },       // F9
    { "LD A,($@%04X)",   2, F_NONE },       // FA
    { "EI",              0, F_NONE },       // FB
    UND,                                    // FC
    UND,                                    // FD
    { "CP $%02X",        1, F_NONE },       // FE
    { "RST $38",         0, F_NONE },       // FF
};

// CB-prefix register names (indexed by low 3 bits)
static const char *const cb_regs[8] = {
    "B", "C", "D", "E", "H", "L", "(HL)", "A"
};

// CB-prefix operation names for 0x00-0x3F (indexed by bits 5-3)
static const char *const cb_ops[8] = {
    "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"
};

// CB-prefix group names for 0x40-0xFF (indexed by bits 7-6: 1=BIT, 2=RES, 3=SET)
static const char *const cb_groups[4] = {
    nullptr, "BIT", "RES", "SET"
};

static Instruction decode_cb(uint8_t op, uint64_t addr)
{
    char buf[32];
    unsigned group = op >> 6;
    unsigned reg = op & 7;

    if (group == 0) {
        // 0x00-0x3F: shift/rotate ops
        unsigned which = (op >> 3) & 7;
        snprintf(buf, sizeof(buf), "%s %s", cb_ops[which], cb_regs[reg]);
    } else {
        // 0x40-0xFF: BIT/RES/SET b,r
        unsigned bit = (op >> 3) & 7;
        snprintf(buf, sizeof(buf), "%s %u,%s", cb_groups[group], bit, cb_regs[reg]);
    }

    return { addr, 2, buf, false, false, 0, false };
}

std::vector<Instruction> dis_lr35902(std::span<const uint8_t> data,
                                     uint64_t base_addr, uint32_t /*flags*/)
{
    std::vector<Instruction> out;
    size_t pos = 0;

    while (pos < data.size()) {
        uint64_t addr = base_addr + pos;
        uint8_t op = data[pos];

        // CB prefix
        if (op == 0xCB) {
            if (pos + 1 >= data.size()) {
                // Truncated CB prefix
                char buf[8];
                snprintf(buf, sizeof(buf), "DB $%02X", op);
                out.push_back({ addr, 1, buf, false, false, 0, true });
                break;
            }
            out.push_back(decode_cb(data[pos + 1], addr));
            pos += 2;
            continue;
        }

        const OpEntry &e = base_ops[op];

        // Undefined opcode
        if (!e.fmt) {
            char buf[8];
            snprintf(buf, sizeof(buf), "DB $%02X", op);
            out.push_back({ addr, 1, buf, false, false, 0, true });
            pos += 1;
            continue;
        }

        uint8_t total = 1 + e.imm_bytes;

        // Truncated instruction
        if (pos + total > data.size()) {
            char buf[8];
            snprintf(buf, sizeof(buf), "DB $%02X", op);
            out.push_back({ addr, 1, buf, false, false, 0, true });
            break;
        }

        // Read immediate value
        uint16_t imm = 0;
        if (e.imm_bytes == 1)
            imm = data[pos + 1];
        else if (e.imm_bytes == 2)
            imm = data[pos + 1] | (data[pos + 2] << 8);

        // Format the mnemonic
        char buf[48];
        bool breaks = (e.flags & F_BREAKS) != 0;
        bool has_target = false;
        uint64_t target = 0;

        if (e.flags & F_REL_TARGET) {
            // Relative jump: target = addr + 2 + signed offset
            int8_t offset = static_cast<int8_t>(imm & 0xFF);
            target = (addr + 2 + offset) & 0xFFFF;
            snprintf(buf, sizeof(buf), e.fmt, (unsigned)target);
            has_target = true;
        } else if (e.flags & F_TARGET) {
            // Absolute jump target
            target = imm;
            snprintf(buf, sizeof(buf), e.fmt, imm);
            has_target = true;
        } else if (e.imm_bytes > 0) {
            snprintf(buf, sizeof(buf), e.fmt, imm);
        } else {
            snprintf(buf, sizeof(buf), "%s", e.fmt);
        }

        out.push_back({ addr, total, buf, breaks, has_target, target, false });
        pos += total;
    }

    return out;
}

/* ======================================================================== */
/* Register layout (for Qt register pane)                                    */
/* ======================================================================== */

static const RegFlag lr35902_named_flags[] = {
    { 7, "Z" }, { 6, "N" }, { 5, "H" }, { 4, "C" },
};

static const RegFlag lr35902_unnamed_flags[] = {
    { 3, nullptr }, { 2, nullptr }, { 1, nullptr }, { 0, nullptr },
};

static const RegFlag lr35902_ime_flag[] = {
    { 0, "IME" },
};

extern const RegLayoutEntry lr35902_reg_layout[] = {
    { REG_HEX,   "A",  RD_LR35902_A,   8, nullptr, 0 },
    { REG_HEX,   "BC", RD_LR35902_BC, 16, nullptr, 0 },
    { REG_HEX,   "DE", RD_LR35902_DE, 16, nullptr, 0 },
    { REG_HEX,   "HL", RD_LR35902_HL, 16, nullptr, 0 },
    { REG_HEX,   "SP", RD_LR35902_SP, 16, nullptr, 0 },
    { REG_HEX,   "PC", RD_LR35902_PC, 16, nullptr, 0 },
    { REG_FLAGS, nullptr, RD_LR35902_F, 0, lr35902_named_flags, 4 },
    { REG_FLAGS, nullptr, RD_LR35902_F, 0, lr35902_unnamed_flags, 4 },
    { REG_FLAGS, nullptr, RD_LR35902_IME, 0, lr35902_ime_flag, 1 },
};

extern const unsigned lr35902_num_reg_layout =
    sizeof(lr35902_reg_layout) / sizeof(lr35902_reg_layout[0]);

/* ======================================================================== */
/* Trace registers                                                           */
/* ======================================================================== */

extern const TraceReg lr35902_trace_regs[] = {
    { RD_LR35902_AF,  "AF",  16 },
    { RD_LR35902_BC,  "BC",  16 },
    { RD_LR35902_DE,  "DE",  16 },
    { RD_LR35902_HL,  "HL",  16 },
    { RD_LR35902_SP,  "SP",  16 },
    { RD_LR35902_PC,  "PC",  16 },
    { RD_LR35902_IME, "IME",  8 },
};

extern const unsigned lr35902_num_trace_regs =
    sizeof(lr35902_trace_regs) / sizeof(lr35902_trace_regs[0]);

} // namespace arch

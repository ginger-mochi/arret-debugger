/*
 * mos6502.cpp: MOS 6502 architecture data
 *
 * Disassembler: table-driven, 256-entry opcode table with format strings.
 * Covers all documented NMOS 6502 opcodes; undocumented opcodes are
 * treated as undefined.
 *
 * Register layout descriptors for the Qt frontend.
 */

#include "arch.hpp"
#include "retrodebug.h"

#include <cstdio>

namespace arch {

// Flags for OpEntry
enum : uint8_t {
    F_NONE       = 0,
    F_BREAKS     = 1 << 0,   // Unconditional non-sequential flow
    F_TARGET     = 1 << 1,   // has absolute jump target
    F_REL_TARGET = 1 << 2,   // has relative jump target (compute addr + 2 + signed imm)
};

struct OpEntry {
    const char *fmt;       // printf format, e.g. "LDA #$%02X"
    uint8_t imm_bytes;     // 0, 1, or 2
    uint8_t flags;
};

// Undefined opcodes produce nullptr fmt
#define UND { nullptr, 0, 0 }

static const OpEntry ops_6502[256] = {
    // 0x00-0x0F
    { "BRK",                0, F_BREAKS },     // 00
    { "ORA ($@%02X,X)",     1, F_NONE },       // 01
    UND,                                        // 02
    UND,                                        // 03
    UND,                                        // 04
    { "ORA $@%02X",         1, F_NONE },       // 05
    { "ASL $@%02X",         1, F_NONE },       // 06
    UND,                                        // 07
    { "PHP",                0, F_NONE },       // 08
    { "ORA #$%02X",         1, F_NONE },       // 09
    { "ASL A",              0, F_NONE },       // 0A
    UND,                                        // 0B
    UND,                                        // 0C
    { "ORA $@%04X",         2, F_NONE },       // 0D
    { "ASL $@%04X",         2, F_NONE },       // 0E
    UND,                                        // 0F

    // 0x10-0x1F
    { "BPL $@%04X",         1, F_REL_TARGET }, // 10
    { "ORA ($@%02X),Y",     1, F_NONE },       // 11
    UND,                                        // 12
    UND,                                        // 13
    UND,                                        // 14
    { "ORA $@%02X,X",       1, F_NONE },       // 15
    { "ASL $@%02X,X",       1, F_NONE },       // 16
    UND,                                        // 17
    { "CLC",                0, F_NONE },       // 18
    { "ORA $@%04X,Y",       2, F_NONE },       // 19
    UND,                                        // 1A
    UND,                                        // 1B
    UND,                                        // 1C
    { "ORA $@%04X,X",       2, F_NONE },       // 1D
    { "ASL $@%04X,X",       2, F_NONE },       // 1E
    UND,                                        // 1F

    // 0x20-0x2F
    { "JSR $@%04X",         2, F_NONE },       // 20
    { "AND ($@%02X,X)",     1, F_NONE },       // 21
    UND,                                        // 22
    UND,                                        // 23
    { "BIT $@%02X",         1, F_NONE },       // 24
    { "AND $@%02X",         1, F_NONE },       // 25
    { "ROL $@%02X",         1, F_NONE },       // 26
    UND,                                        // 27
    { "PLP",                0, F_NONE },       // 28
    { "AND #$%02X",         1, F_NONE },       // 29
    { "ROL A",              0, F_NONE },       // 2A
    UND,                                        // 2B
    { "BIT $@%04X",         2, F_NONE },       // 2C
    { "AND $@%04X",         2, F_NONE },       // 2D
    { "ROL $@%04X",         2, F_NONE },       // 2E
    UND,                                        // 2F

    // 0x30-0x3F
    { "BMI $@%04X",         1, F_REL_TARGET }, // 30
    { "AND ($@%02X),Y",     1, F_NONE },       // 31
    UND,                                        // 32
    UND,                                        // 33
    UND,                                        // 34
    { "AND $@%02X,X",       1, F_NONE },       // 35
    { "ROL $@%02X,X",       1, F_NONE },       // 36
    UND,                                        // 37
    { "SEC",                0, F_NONE },       // 38
    { "AND $@%04X,Y",       2, F_NONE },       // 39
    UND,                                        // 3A
    UND,                                        // 3B
    UND,                                        // 3C
    { "AND $@%04X,X",       2, F_NONE },       // 3D
    { "ROL $@%04X,X",       2, F_NONE },       // 3E
    UND,                                        // 3F

    // 0x40-0x4F
    { "RTI",                0, F_BREAKS },     // 40
    { "EOR ($@%02X,X)",     1, F_NONE },       // 41
    UND,                                        // 42
    UND,                                        // 43
    UND,                                        // 44
    { "EOR $@%02X",         1, F_NONE },       // 45
    { "LSR $@%02X",         1, F_NONE },       // 46
    UND,                                        // 47
    { "PHA",                0, F_NONE },       // 48
    { "EOR #$%02X",         1, F_NONE },       // 49
    { "LSR A",              0, F_NONE },       // 4A
    UND,                                        // 4B
    { "JMP $@%04X",         2, F_BREAKS | F_TARGET }, // 4C
    { "EOR $@%04X",         2, F_NONE },       // 4D
    { "LSR $@%04X",         2, F_NONE },       // 4E
    UND,                                        // 4F

    // 0x50-0x5F
    { "BVC $@%04X",         1, F_REL_TARGET }, // 50
    { "EOR ($@%02X),Y",     1, F_NONE },       // 51
    UND,                                        // 52
    UND,                                        // 53
    UND,                                        // 54
    { "EOR $@%02X,X",       1, F_NONE },       // 55
    { "LSR $@%02X,X",       1, F_NONE },       // 56
    UND,                                        // 57
    { "CLI",                0, F_NONE },       // 58
    { "EOR $@%04X,Y",       2, F_NONE },       // 59
    UND,                                        // 5A
    UND,                                        // 5B
    UND,                                        // 5C
    { "EOR $@%04X,X",       2, F_NONE },       // 5D
    { "LSR $@%04X,X",       2, F_NONE },       // 5E
    UND,                                        // 5F

    // 0x60-0x6F
    { "RTS",                0, F_BREAKS },     // 60
    { "ADC ($@%02X,X)",     1, F_NONE },       // 61
    UND,                                        // 62
    UND,                                        // 63
    UND,                                        // 64
    { "ADC $@%02X",         1, F_NONE },       // 65
    { "ROR $@%02X",         1, F_NONE },       // 66
    UND,                                        // 67
    { "PLA",                0, F_NONE },       // 68
    { "ADC #$%02X",         1, F_NONE },       // 69
    { "ROR A",              0, F_NONE },       // 6A
    UND,                                        // 6B
    { "JMP ($@%04X)",       2, F_BREAKS },     // 6C
    { "ADC $@%04X",         2, F_NONE },       // 6D
    { "ROR $@%04X",         2, F_NONE },       // 6E
    UND,                                        // 6F

    // 0x70-0x7F
    { "BVS $@%04X",         1, F_REL_TARGET }, // 70
    { "ADC ($@%02X),Y",     1, F_NONE },       // 71
    UND,                                        // 72
    UND,                                        // 73
    UND,                                        // 74
    { "ADC $@%02X,X",       1, F_NONE },       // 75
    { "ROR $@%02X,X",       1, F_NONE },       // 76
    UND,                                        // 77
    { "SEI",                0, F_NONE },       // 78
    { "ADC $@%04X,Y",       2, F_NONE },       // 79
    UND,                                        // 7A
    UND,                                        // 7B
    UND,                                        // 7C
    { "ADC $@%04X,X",       2, F_NONE },       // 7D
    { "ROR $@%04X,X",       2, F_NONE },       // 7E
    UND,                                        // 7F

    // 0x80-0x8F
    UND,                                        // 80
    { "STA ($@%02X,X)",     1, F_NONE },       // 81
    UND,                                        // 82
    UND,                                        // 83
    { "STY $@%02X",         1, F_NONE },       // 84
    { "STA $@%02X",         1, F_NONE },       // 85
    { "STX $@%02X",         1, F_NONE },       // 86
    UND,                                        // 87
    { "DEY",                0, F_NONE },       // 88
    UND,                                        // 89
    { "TXA",                0, F_NONE },       // 8A
    UND,                                        // 8B
    { "STY $@%04X",         2, F_NONE },       // 8C
    { "STA $@%04X",         2, F_NONE },       // 8D
    { "STX $@%04X",         2, F_NONE },       // 8E
    UND,                                        // 8F

    // 0x90-0x9F
    { "BCC $@%04X",         1, F_REL_TARGET }, // 90
    { "STA ($@%02X),Y",     1, F_NONE },       // 91
    UND,                                        // 92
    UND,                                        // 93
    { "STY $@%02X,X",       1, F_NONE },       // 94
    { "STA $@%02X,X",       1, F_NONE },       // 95
    { "STX $@%02X,Y",       1, F_NONE },       // 96
    UND,                                        // 97
    { "TYA",                0, F_NONE },       // 98
    { "STA $@%04X,Y",       2, F_NONE },       // 99
    { "TXS",                0, F_NONE },       // 9A
    UND,                                        // 9B
    UND,                                        // 9C
    { "STA $@%04X,X",       2, F_NONE },       // 9D
    UND,                                        // 9E
    UND,                                        // 9F

    // 0xA0-0xAF
    { "LDY #$%02X",         1, F_NONE },       // A0
    { "LDA ($@%02X,X)",     1, F_NONE },       // A1
    { "LDX #$%02X",         1, F_NONE },       // A2
    UND,                                        // A3
    { "LDY $@%02X",         1, F_NONE },       // A4
    { "LDA $@%02X",         1, F_NONE },       // A5
    { "LDX $@%02X",         1, F_NONE },       // A6
    UND,                                        // A7
    { "TAY",                0, F_NONE },       // A8
    { "LDA #$%02X",         1, F_NONE },       // A9
    { "TAX",                0, F_NONE },       // AA
    UND,                                        // AB
    { "LDY $@%04X",         2, F_NONE },       // AC
    { "LDA $@%04X",         2, F_NONE },       // AD
    { "LDX $@%04X",         2, F_NONE },       // AE
    UND,                                        // AF

    // 0xB0-0xBF
    { "BCS $@%04X",         1, F_REL_TARGET }, // B0
    { "LDA ($@%02X),Y",     1, F_NONE },       // B1
    UND,                                        // B2
    UND,                                        // B3
    { "LDY $@%02X,X",       1, F_NONE },       // B4
    { "LDA $@%02X,X",       1, F_NONE },       // B5
    { "LDX $@%02X,Y",       1, F_NONE },       // B6
    UND,                                        // B7
    { "CLV",                0, F_NONE },       // B8
    { "LDA $@%04X,Y",       2, F_NONE },       // B9
    { "TSX",                0, F_NONE },       // BA
    UND,                                        // BB
    { "LDY $@%04X,X",       2, F_NONE },       // BC
    { "LDA $@%04X,X",       2, F_NONE },       // BD
    { "LDX $@%04X,Y",       2, F_NONE },       // BE
    UND,                                        // BF

    // 0xC0-0xCF
    { "CPY #$%02X",         1, F_NONE },       // C0
    { "CMP ($@%02X,X)",     1, F_NONE },       // C1
    UND,                                        // C2
    UND,                                        // C3
    { "CPY $@%02X",         1, F_NONE },       // C4
    { "CMP $@%02X",         1, F_NONE },       // C5
    { "DEC $@%02X",         1, F_NONE },       // C6
    UND,                                        // C7
    { "INY",                0, F_NONE },       // C8
    { "CMP #$%02X",         1, F_NONE },       // C9
    { "DEX",                0, F_NONE },       // CA
    UND,                                        // CB
    { "CPY $@%04X",         2, F_NONE },       // CC
    { "CMP $@%04X",         2, F_NONE },       // CD
    { "DEC $@%04X",         2, F_NONE },       // CE
    UND,                                        // CF

    // 0xD0-0xDF
    { "BNE $@%04X",         1, F_REL_TARGET }, // D0
    { "CMP ($@%02X),Y",     1, F_NONE },       // D1
    UND,                                        // D2
    UND,                                        // D3
    UND,                                        // D4
    { "CMP $@%02X,X",       1, F_NONE },       // D5
    { "DEC $@%02X,X",       1, F_NONE },       // D6
    UND,                                        // D7
    { "CLD",                0, F_NONE },       // D8
    { "CMP $@%04X,Y",       2, F_NONE },       // D9
    UND,                                        // DA
    UND,                                        // DB
    UND,                                        // DC
    { "CMP $@%04X,X",       2, F_NONE },       // DD
    { "DEC $@%04X,X",       2, F_NONE },       // DE
    UND,                                        // DF

    // 0xE0-0xEF
    { "CPX #$%02X",         1, F_NONE },       // E0
    { "SBC ($@%02X,X)",     1, F_NONE },       // E1
    UND,                                        // E2
    UND,                                        // E3
    { "CPX $@%02X",         1, F_NONE },       // E4
    { "SBC $@%02X",         1, F_NONE },       // E5
    { "INC $@%02X",         1, F_NONE },       // E6
    UND,                                        // E7
    { "INX",                0, F_NONE },       // E8
    { "SBC #$%02X",         1, F_NONE },       // E9
    { "NOP",                0, F_NONE },       // EA
    UND,                                        // EB
    { "CPX $@%04X",         2, F_NONE },       // EC
    { "SBC $@%04X",         2, F_NONE },       // ED
    { "INC $@%04X",         2, F_NONE },       // EE
    UND,                                        // EF

    // 0xF0-0xFF
    { "BEQ $@%04X",         1, F_REL_TARGET }, // F0
    { "SBC ($@%02X),Y",     1, F_NONE },       // F1
    UND,                                        // F2
    UND,                                        // F3
    UND,                                        // F4
    { "SBC $@%02X,X",       1, F_NONE },       // F5
    { "INC $@%02X,X",       1, F_NONE },       // F6
    UND,                                        // F7
    { "SED",                0, F_NONE },       // F8
    { "SBC $@%04X,Y",       2, F_NONE },       // F9
    UND,                                        // FA
    UND,                                        // FB
    UND,                                        // FC
    { "SBC $@%04X,X",       2, F_NONE },       // FD
    { "INC $@%04X,X",       2, F_NONE },       // FE
    UND,                                        // FF
};

std::vector<Instruction> dis_6502(std::span<const uint8_t> data,
                                  uint64_t base_addr, uint32_t /*flags*/)
{
    std::vector<Instruction> out;
    size_t pos = 0;

    while (pos < data.size()) {
        uint64_t addr = base_addr + pos;
        uint8_t op = data[pos];

        const OpEntry &e = ops_6502[op];

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
            // Relative branch: target = addr + 2 + signed offset
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

static const RegFlag mos6502_p_flags[] = {
    { 7, "N" }, { 6, "V" }, { 5, nullptr }, { 4, "B" },
    { 3, "D" }, { 2, "I" }, { 1, "Z" },    { 0, "C" },
};

extern const RegLayoutEntry mos6502_reg_layout[] = {
    { REG_HEX,   "A",  RD_6502_A,   8, nullptr, 0 },
    { REG_HEX,   "X",  RD_6502_X,   8, nullptr, 0 },
    { REG_HEX,   "Y",  RD_6502_Y,   8, nullptr, 0 },
    { REG_HEX,   "S",  RD_6502_S,   8, nullptr, 0 },
    { REG_HEX,   "PC", RD_6502_PC, 16, nullptr, 0 },
    { REG_FLAGS, nullptr, RD_6502_P,  0, mos6502_p_flags, 8 },
};

extern const unsigned mos6502_num_reg_layout =
    sizeof(mos6502_reg_layout) / sizeof(mos6502_reg_layout[0]);

} // namespace arch

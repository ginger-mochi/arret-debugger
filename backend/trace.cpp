/*
 * trace.cpp: Execution trace logging engine
 *
 * Manages retrodebug broad execution subscriptions for selected CPUs.
 * On each execution event, disassembles the instruction, formats a trace
 * line, and writes it to a ring buffer (for the Qt UI) and optionally to
 * a file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <span>

#include "trace.hpp"
#include "backend.hpp"
#include "arch.hpp"
#include "registers.hpp"

/* ========================================================================
 * Ring buffer
 * ======================================================================== */

#define TRACE_RING_SIZE (1 << 16)  /* 64K entries */

static char *g_ring = nullptr;            /* lazily allocated */
static uint64_t g_ring_head = 0;          /* next write position (monotonic) */
static uint64_t g_ring_read = 0;          /* consumer read position */
static std::mutex g_ring_mutex;
static uint64_t g_total_lines = 0;

static void init_ring(void) {
    if (!g_ring)
        g_ring = (char *)calloc(TRACE_RING_SIZE, TRACE_LINE_SIZE);
}

static void ring_write(const char *line) {
    if (!g_ring) return;
    std::lock_guard lock(g_ring_mutex);
    uint64_t idx = g_ring_head % TRACE_RING_SIZE;
    strncpy(g_ring + idx * TRACE_LINE_SIZE, line, TRACE_LINE_SIZE - 1);
    g_ring[idx * TRACE_LINE_SIZE + TRACE_LINE_SIZE - 1] = '\0';
    g_ring_head++;
    g_total_lines++;
}

/* ========================================================================
 * State
 * ======================================================================== */

static bool g_active = false;
static bool g_registers = false;
static bool g_indent = false;
static FILE *g_file = nullptr;
static char g_file_path[4096] = {0};

/* Per-CPU trace state */
struct TraceCpu {
    rd_Cpu const *cpu;
    std::string id;
    bool enabled;
    rd_SubscriptionID sub_id;
    int sp_reg;         /* register index for SP, or -1 */
};

static std::vector<TraceCpu> g_cpus;
static std::unordered_map<rd_SubscriptionID, int> g_sub_to_cpu;

/* Persistent CPU settings (survive across trace sessions) */
static std::unordered_map<std::string, bool> g_cpu_settings;

/* ========================================================================
 * Memory map cache (for bank display)
 * ======================================================================== */

struct MMapEntry {
    uint64_t base;
    uint64_t size;
    int64_t bank;
};

struct CpuMMap {
    std::vector<MMapEntry> entries;
    int bankWidth;   /* 0 = no banking */
    int addrWidth;   /* hex digits for address */
};

static std::unordered_map<rd_Cpu const *, CpuMMap> g_cpu_mmaps;

static void build_mmap(rd_Cpu const *cpu) {
    CpuMMap cm;
    cm.bankWidth = 0;
    cm.addrWidth = 4;

    rd_Memory const *mem = cpu->v1.memory_region;
    if (!mem) {
        g_cpu_mmaps[cpu] = cm;
        return;
    }

    uint64_t maxAddr = mem->v1.base_address + mem->v1.size;
    if (maxAddr > 0x10000) cm.addrWidth = 8;

    if (mem->v1.get_memory_map_count && mem->v1.get_memory_map) {
        unsigned mc = mem->v1.get_memory_map_count(mem);
        if (mc > 0) {
            auto *maps = (rd_MemoryMap *)malloc(mc * sizeof(rd_MemoryMap));
            if (maps) {
                mem->v1.get_memory_map(mem, maps);
                int64_t maxBank = -1;
                for (unsigned i = 0; i < mc; i++) {
                    cm.entries.push_back({maps[i].base_addr, maps[i].size,
                                          maps[i].bank});
                    if (maps[i].bank > maxBank) maxBank = maps[i].bank;
                }
                if (maxBank >= 0) {
                    cm.bankWidth = 1;
                    for (int64_t v = maxBank; v >= 10; v /= 10) cm.bankWidth++;
                }
                free(maps);
            }
        }
    }

    g_cpu_mmaps[cpu] = cm;
}

static int64_t bank_for_addr(const CpuMMap &cm, uint64_t addr) {
    for (auto &e : cm.entries)
        if (addr >= e.base && addr < e.base + e.size)
            return e.bank;
    return -1;
}

/* ========================================================================
 * Subscription management
 * ======================================================================== */

static void sync_subscriptions(void) {
    rd_DebuggerIf *dif = ar_get_debugger_if();
    if (!dif || !dif->v1.subscribe || !dif->v1.unsubscribe) return;

    /* Unsubscribe all existing trace subs */
    for (auto &tc : g_cpus) {
        if (tc.sub_id >= 0) {
            dif->v1.unsubscribe(tc.sub_id);
            tc.sub_id = -1;
        }
    }
    g_sub_to_cpu.clear();

    if (!g_active) return;

    /* Subscribe for each enabled CPU */
    for (int i = 0; i < (int)g_cpus.size(); i++) {
        TraceCpu &tc = g_cpus[i];
        if (!tc.enabled) continue;

        rd_Subscription sub{};
        sub.type = RD_EVENT_EXECUTION;
        sub.execution.cpu = tc.cpu;
        sub.execution.type = RD_STEP;
        sub.execution.address_range_begin = 0;
        sub.execution.address_range_end = UINT64_MAX;

        tc.sub_id = dif->v1.subscribe(&sub);
        if (tc.sub_id >= 0) {
            g_sub_to_cpu[tc.sub_id] = i;
        } else {
            fprintf(stderr, "[arret] trace: failed to subscribe for CPU %s\n",
                    tc.id.c_str());
        }
    }
}

static void populate_cpus(void) {
    g_cpus.clear();
    g_cpu_mmaps.clear();

    rd_System const *sys = ar_debug_system();
    if (!sys) return;

    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        rd_Cpu const *cpu = sys->v1.cpus[i];
        TraceCpu tc;
        tc.cpu = cpu;
        tc.id = cpu->v1.id;

        /* Apply saved setting, or default to main CPU only */
        auto it = g_cpu_settings.find(tc.id);
        if (it != g_cpu_settings.end())
            tc.enabled = it->second;
        else
            tc.enabled = (cpu->v1.is_main != 0);

        tc.sub_id = -1;
        tc.sp_reg = ar_reg_from_name(cpu->v1.type, "sp");

        g_cpus.push_back(tc);
        build_mmap(cpu);
    }
}

/* ========================================================================
 * Trace line formatting
 * ======================================================================== */

/* Strip '@' address markers from disassembly text (no symbol interpolation) */
static void strip_at_markers(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        if (src[i] != '@')
            dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* ========================================================================
 * Public API
 * ======================================================================== */

bool ar_trace_start(const char *path) {
    if (g_active) ar_trace_stop();
    if (!ar_has_debug()) return false;

    init_ring();
    if (!g_ring) return false;

    /* Reset ring buffer */
    {
        std::lock_guard lock(g_ring_mutex);
        g_ring_head = 0;
        g_ring_read = 0;
        g_total_lines = 0;
    }

    /* Open file if path given */
    g_file_path[0] = '\0';
    if (path && path[0]) {
        g_file = fopen(path, "w");
        if (!g_file) {
            fprintf(stderr, "[arret] trace: cannot open %s\n", path);
            return false;
        }
        strncpy(g_file_path, path, sizeof(g_file_path) - 1);
    }

    populate_cpus();
    g_active = true;
    sync_subscriptions();

    if (g_file)
        fprintf(stderr, "[arret] trace: started (file: %s)\n", g_file_path);
    else
        fprintf(stderr, "[arret] trace: started\n");

    return true;
}

void ar_trace_stop(void) {
    if (!g_active) return;

    g_active = false;
    sync_subscriptions();

    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_file_path[0] = '\0';

    fprintf(stderr, "[arret] trace: stopped (%lu lines)\n",
            (unsigned long)g_total_lines);
}

bool ar_trace_active(void) {
    return g_active;
}

bool ar_trace_cpu_enable(const char *cpu_id, bool enable) {
    /* Resolve default */
    const char *resolved = cpu_id;
    if (!resolved || !resolved[0]) {
        rd_Cpu const *primary = ar_debug_cpu();
        if (!primary) return false;
        resolved = primary->v1.id;
    }

    /* Save persistent setting */
    g_cpu_settings[resolved] = enable;

    /* Apply to live state if active */
    if (g_active) {
        for (auto &tc : g_cpus) {
            if (tc.id == resolved) {
                tc.enabled = enable;
                sync_subscriptions();
                return true;
            }
        }
    }

    /* Verify CPU exists */
    rd_System const *sys = ar_debug_system();
    if (!sys) return false;
    for (unsigned i = 0; i < sys->v1.num_cpus; i++)
        if (strcmp(sys->v1.cpus[i]->v1.id, resolved) == 0)
            return true;
    return false;
}

bool ar_trace_cpu_enabled(const char *cpu_id) {
    const char *resolved = cpu_id;
    if (!resolved || !resolved[0]) {
        rd_Cpu const *primary = ar_debug_cpu();
        if (!primary) return false;
        resolved = primary->v1.id;
    }

    /* Check live state first */
    for (auto &tc : g_cpus)
        if (tc.id == resolved)
            return tc.enabled;

    /* Fall back to saved settings */
    auto it = g_cpu_settings.find(resolved);
    if (it != g_cpu_settings.end())
        return it->second;

    /* Default: main CPU only */
    rd_System const *sys = ar_debug_system();
    if (!sys) return false;
    for (unsigned i = 0; i < sys->v1.num_cpus; i++)
        if (strcmp(sys->v1.cpus[i]->v1.id, resolved) == 0)
            return sys->v1.cpus[i]->v1.is_main != 0;
    return false;
}

void ar_trace_set_registers(bool enable) { g_registers = enable; }
bool ar_trace_get_registers(void) { return g_registers; }

void ar_trace_set_indent(bool enable) { g_indent = enable; }
bool ar_trace_get_indent(void) { return g_indent; }

const char *ar_trace_file_path(void) { return g_file_path; }

unsigned ar_trace_read_new(char *out, unsigned max_lines) {
    std::lock_guard lock(g_ring_mutex);

    uint64_t available = g_ring_head - g_ring_read;
    if (available > TRACE_RING_SIZE) {
        /* Ring wrapped — skip to oldest available */
        g_ring_read = g_ring_head - TRACE_RING_SIZE;
        available = TRACE_RING_SIZE;
    }

    unsigned to_read = (unsigned)std::min((uint64_t)max_lines, available);
    for (unsigned i = 0; i < to_read; i++) {
        uint64_t idx = (g_ring_read + i) % TRACE_RING_SIZE;
        memcpy(out + (size_t)i * TRACE_LINE_SIZE,
               g_ring + idx * TRACE_LINE_SIZE, TRACE_LINE_SIZE);
    }
    g_ring_read += to_read;
    return to_read;
}

uint64_t ar_trace_total_lines(void) {
    return g_total_lines;
}

bool ar_trace_is_sub(rd_SubscriptionID sub_id) {
    return g_sub_to_cpu.find(sub_id) != g_sub_to_cpu.end();
}

bool ar_trace_on_event(rd_SubscriptionID sub_id, rd_Event const *event) {
    if (event->type != RD_EVENT_EXECUTION) return false;

    auto it = g_sub_to_cpu.find(sub_id);
    if (it == g_sub_to_cpu.end()) return false;

    int cpu_idx = it->second;
    if (cpu_idx < 0 || cpu_idx >= (int)g_cpus.size()) return false;
    TraceCpu &tc = g_cpus[cpu_idx];

    rd_Cpu const *cpu = event->execution.cpu;
    uint64_t pc = event->execution.address;

    /* Memory region for instruction bytes */
    rd_Memory const *mem = cpu->v1.memory_region;
    if (!mem) return false;

    /* Memory map for bank display */
    auto mm = g_cpu_mmaps.find(cpu);

    /* Read bytes at PC for disassembly */
    const arch::Arch *arch = arch::arch_for_cpu(cpu->v1.type);
    unsigned maxInsn = arch ? arch->max_insn_size : 4;
    if (maxInsn > 16) maxInsn = 16;
    uint8_t buf[16];
    for (unsigned i = 0; i < maxInsn; i++)
        buf[i] = mem->v1.peek(mem, pc + i, false);

    /* Disassemble one instruction */
    auto insns = arch::disassemble(
        std::span<const uint8_t>(buf, maxInsn), pc, cpu->v1.type);

    /* Format the trace line */
    char line[TRACE_LINE_SIZE];
    int pos = 0;

    /* Indentation based on SP */
    if (g_indent && tc.sp_reg >= 0) {
        uint64_t sp = cpu->v1.get_register(cpu, (unsigned)tc.sp_reg);
        int depth = (int)(sp % 64);
        for (int i = 0; i < depth && pos < TRACE_LINE_SIZE - 2; i++)
            line[pos++] = ' ';
    }

    /* Bank prefix */
    if (mm != g_cpu_mmaps.end() && mm->second.bankWidth > 0) {
        int64_t bank = bank_for_addr(mm->second, pc);
        if (bank >= 0)
            pos += snprintf(line + pos, TRACE_LINE_SIZE - pos,
                            "%*ld:", mm->second.bankWidth, (long)bank);
        else
            pos += snprintf(line + pos, TRACE_LINE_SIZE - pos,
                            "%*s ", mm->second.bankWidth, "");
    }

    /* Address */
    int aw = 4;
    if (mm != g_cpu_mmaps.end()) aw = mm->second.addrWidth;
    pos += snprintf(line + pos, TRACE_LINE_SIZE - pos,
                    "%0*lX: ", aw, (unsigned long)pc);

    /* Instruction text (strip @ markers, no symbol interpolation) */
    if (!insns.empty()) {
        char stripped[128];
        strip_at_markers(insns[0].text.c_str(), stripped, sizeof(stripped));
        pos += snprintf(line + pos, TRACE_LINE_SIZE - pos, "%s", stripped);
    } else {
        pos += snprintf(line + pos, TRACE_LINE_SIZE - pos, "???");
    }

    /* Registers */
    if (g_registers && pos < TRACE_LINE_SIZE - 4) {
        pos += snprintf(line + pos, TRACE_LINE_SIZE - pos, " ; ");

        int pc_reg = ar_reg_pc(cpu->v1.type);
        bool first = true;

        if (arch && arch->trace_regs) {
            /* Arch-specific trace register set */
            for (unsigned i = 0; i < arch->num_trace_regs && pos < TRACE_LINE_SIZE - 2; i++) {
                const arch::TraceReg &tr = arch->trace_regs[i];
                if ((int)tr.reg_index == pc_reg) continue;

                uint64_t val = cpu->v1.get_register(cpu, tr.reg_index);
                int digits = (int)tr.bits / 4;

                if (!first && pos < TRACE_LINE_SIZE - 1)
                    line[pos++] = ' ';
                first = false;

                pos += snprintf(line + pos, TRACE_LINE_SIZE - pos,
                                "%s=%0*lX", tr.name, digits, (unsigned long)val);
            }
        } else {
            /* Fallback: iterate all registers */
            unsigned nregs = ar_reg_count(cpu->v1.type);
            for (unsigned i = 0; i < nregs && pos < TRACE_LINE_SIZE - 2; i++) {
                int idx = ar_reg_by_order(cpu->v1.type, i);
                if (idx < 0) continue;
                if (idx == pc_reg) continue;

                const char *name = ar_reg_name(cpu->v1.type, (unsigned)idx);
                if (!name) continue;
                int digits = ar_reg_digits(cpu->v1.type, (unsigned)idx);
                uint64_t val = cpu->v1.get_register(cpu, (unsigned)idx);

                if (!first && pos < TRACE_LINE_SIZE - 1)
                    line[pos++] = ' ';
                first = false;

                char uname[16];
                int ni = 0;
                for (const char *p = name; *p && ni < 14; p++)
                    uname[ni++] = toupper((unsigned char)*p);
                uname[ni] = '\0';

                pos += snprintf(line + pos, TRACE_LINE_SIZE - pos,
                                "%s=%0*lX", uname, digits, (unsigned long)val);
            }
        }
    }

    if (pos >= TRACE_LINE_SIZE) pos = TRACE_LINE_SIZE - 1;
    line[pos] = '\0';

    /* Write to ring buffer */
    ring_write(line);

    /* Write to file */
    if (g_file) {
        fputs(line, g_file);
        fputc('\n', g_file);
    }

    return false;  /* never halt */
}

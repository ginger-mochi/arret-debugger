/*
 * breakpoint.cpp: Breakpoint storage and retrodebug subscription sync
 *
 * Breakpoints are stored in a std::map keyed by monotonically increasing ID.
 * After any mutation, sync_subscriptions() unsubscribes all existing BP subs
 * and re-subscribes enabled execution breakpoints.
 */

#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <string>
#include <string.h>
#include <stdio.h>

#include "breakpoint.hpp"
#include "backend.hpp"

static std::map<int, ar_breakpoint> g_bps;
static int g_next_id = 1;
static bool g_auto_save = false;

/* Maps retrodebug subscription ID → breakpoint ID */
static std::unordered_map<rd_SubscriptionID, int> g_sub_to_bp;

/* BP IDs whose subscriptions failed during the last sync */
static std::set<int> g_sub_failed;

/* BP IDs to delete after the current frame finishes (deferred from handler) */
static std::vector<int> g_deferred_deletes;

/* Find a CPU by its id string; returns primary CPU if id is NULL or empty */
static rd_Cpu const *find_cpu(const char *id) {
    if (!id || !id[0]) return ar_debug_cpu();
    rd_System const *sys = ar_debug_system();
    if (!sys) return nullptr;
    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        if (strcmp(sys->v1.cpus[i]->v1.id, id) == 0)
            return sys->v1.cpus[i];
    }
    return nullptr;
}

static void sync_subscriptions(void) {
    rd_DebuggerIf *dif = ar_get_debugger_if();
    if (!dif || !dif->v1.subscribe || !dif->v1.unsubscribe) return;

    /* Unsubscribe all existing breakpoint subs */
    for (auto &[sub_id, bp_id] : g_sub_to_bp) {
        (void)bp_id;
        dif->v1.unsubscribe(sub_id);
    }
    g_sub_to_bp.clear();
    g_sub_failed.clear();

    for (auto &[id, bp] : g_bps) {
        if (!bp.enabled) continue;

        rd_Cpu const *cpu = find_cpu(bp.cpu_id);
        if (!cpu) continue;

        /* Execution subscription */
        if (bp.flags & AR_BP_EXECUTE) {
            rd_Subscription sub{};
            sub.type = RD_EVENT_EXECUTION;
            sub.execution.cpu = cpu;
            sub.execution.type = RD_STEP;
            sub.execution.address_range_begin = bp.address;
            sub.execution.address_range_end = bp.address;

            rd_SubscriptionID sid = dif->v1.subscribe(&sub);
            if (sid >= 0)
                g_sub_to_bp[sid] = id;
            else
                g_sub_failed.insert(id);
        }

        /* Memory watchpoint subscriptions (read / write) */
        if (bp.flags & (AR_BP_READ | AR_BP_WRITE)) {
            rd_Memory const *mem = cpu->v1.memory_region;
            if (!mem) { g_sub_failed.insert(id); continue; }

            uint8_t op = 0;
            if (bp.flags & AR_BP_READ)  op |= RD_MEMORY_READ;
            if (bp.flags & AR_BP_WRITE) op |= RD_MEMORY_WRITE;

            rd_Subscription sub{};
            sub.type = RD_EVENT_MEMORY;
            sub.memory.memory = mem;
            sub.memory.address_range_begin = bp.address;
            sub.memory.address_range_end = bp.address;
            sub.memory.operation = op;

            rd_SubscriptionID sid = dif->v1.subscribe(&sub);
            if (sid >= 0)
                g_sub_to_bp[sid] = id;
            else
                g_sub_failed.insert(id);
        }
    }
}

static void auto_save(void) {
    if (!g_auto_save) return;
    const char *base = ar_rompath_base();
    if (!base || !base[0]) return;
    std::string path = std::string(base) + ".bp";
    ar_bp_save(path.c_str());
}

int ar_bp_add(uint64_t addr, unsigned flags, bool enabled, bool temporary, const char *cond, const char *cpu_id) {
    ar_breakpoint bp{};
    bp.id = g_next_id++;
    bp.address = addr;
    bp.enabled = enabled;
    bp.temporary = temporary;
    bp.flags = flags;
    if (cond)
        strncpy(bp.condition, cond, sizeof(bp.condition) - 1);
    if (cpu_id)
        strncpy(bp.cpu_id, cpu_id, sizeof(bp.cpu_id) - 1);

    g_bps[bp.id] = bp;
    sync_subscriptions();

    if (g_sub_failed.count(bp.id)) {
        g_bps.erase(bp.id);
        sync_subscriptions();
        return -1;
    }

    auto_save();
    return bp.id;
}

bool ar_bp_delete(int id) {
    auto it = g_bps.find(id);
    if (it == g_bps.end()) return false;
    g_bps.erase(it);
    sync_subscriptions();
    auto_save();
    return true;
}

bool ar_bp_enable(int id, bool enabled) {
    auto it = g_bps.find(id);
    if (it == g_bps.end()) return false;
    bool old_enabled = it->second.enabled;
    it->second.enabled = enabled;
    sync_subscriptions();

    if (g_sub_failed.count(id)) {
        it->second.enabled = old_enabled;
        sync_subscriptions();
        return false;
    }

    auto_save();
    return true;
}

bool ar_bp_set_temporary(int id, bool temporary) {
    auto it = g_bps.find(id);
    if (it == g_bps.end()) return false;
    it->second.temporary = temporary;
    auto_save();
    return true;
}

bool ar_bp_replace(int id, uint64_t addr, unsigned flags, bool enabled, bool temporary, const char *cond, const char *cpu_id) {
    auto it = g_bps.find(id);
    if (it == g_bps.end()) return false;

    ar_breakpoint old = it->second;

    it->second.address = addr;
    it->second.flags = flags;
    it->second.enabled = enabled;
    it->second.temporary = temporary;
    memset(it->second.condition, 0, sizeof(it->second.condition));
    if (cond)
        strncpy(it->second.condition, cond, sizeof(it->second.condition) - 1);
    memset(it->second.cpu_id, 0, sizeof(it->second.cpu_id));
    if (cpu_id)
        strncpy(it->second.cpu_id, cpu_id, sizeof(it->second.cpu_id) - 1);
    sync_subscriptions();

    if (g_sub_failed.count(id)) {
        it->second = old;
        sync_subscriptions();
        return false;
    }

    auto_save();
    return true;
}

const ar_breakpoint *ar_bp_get(int id) {
    auto it = g_bps.find(id);
    if (it == g_bps.end()) return nullptr;
    return &it->second;
}

unsigned ar_bp_list(ar_breakpoint *out, unsigned max) {
    unsigned i = 0;
    for (auto &[id, bp] : g_bps) {
        if (i >= max) break;
        out[i++] = bp;
    }
    return i;
}

unsigned ar_bp_count(void) {
    return (unsigned)g_bps.size();
}

void ar_bp_clear(void) {
    g_bps.clear();
    sync_subscriptions();
    auto_save();
}

bool ar_bp_sub_is_breakpoint(rd_SubscriptionID sub_id) {
    return g_sub_to_bp.find(sub_id) != g_sub_to_bp.end();
}

int ar_bp_sub_to_id(rd_SubscriptionID sub_id) {
    auto it = g_sub_to_bp.find(sub_id);
    if (it == g_sub_to_bp.end()) return -1;
    return it->second;
}

bool ar_bp_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    for (auto &[id, bp] : g_bps) {
        /* [cpu.]<hex_addr> <flags>[d] [condition] */
        if (bp.cpu_id[0])
            fprintf(f, "%s.", bp.cpu_id);
        fprintf(f, "%04lX ", (unsigned long)bp.address);
        char flags[10] = {0};
        int fi = 0;
        if (bp.flags & AR_BP_EXECUTE) flags[fi++] = 'X';
        if (bp.flags & AR_BP_READ)    flags[fi++] = 'R';
        if (bp.flags & AR_BP_WRITE)   flags[fi++] = 'W';
        if (bp.temporary) flags[fi++] = 't';
        if (!bp.enabled) flags[fi++] = 'd';
        fprintf(f, "%s", flags);
        if (bp.condition[0])
            fprintf(f, " %s", bp.condition);
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "[arret] Saved %u breakpoints to %s\n",
            (unsigned)g_bps.size(), path);
    return true;
}

bool ar_bp_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Clear without triggering auto-save */
    bool was_auto = g_auto_save;
    g_auto_save = false;
    ar_bp_clear();

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        /* Parse: [cpu.]<hex_addr> <flags>[d] [condition] */
        char addr_s[128] = {0}, flags_s[64] = {0}, cond[256] = {0};
        int n = sscanf(line, "%127s %63s %255[^\n]", addr_s, flags_s, cond);
        if (n < 2) continue;

        /* Parse optional cpu_id prefix */
        char cpu_id[64] = {0};
        char *dot = strchr(addr_s, '.');
        if (dot) {
            size_t prefix_len = (size_t)(dot - addr_s);
            if (prefix_len > 0 && prefix_len < sizeof(cpu_id)) {
                memcpy(cpu_id, addr_s, prefix_len);
                cpu_id[prefix_len] = '\0';
            }
            memmove(addr_s, dot + 1, strlen(dot + 1) + 1);
        }

        uint64_t addr = strtoull(addr_s, NULL, 16);

        /* Parse flags and disabled/temporary markers */
        unsigned flags = 0;
        bool enabled = true;
        bool temporary = false;
        for (char *p = flags_s; *p; p++) {
            char c = *p & ~0x20; /* toupper */
            if (c == 'X') flags |= AR_BP_EXECUTE;
            else if (c == 'R') flags |= AR_BP_READ;
            else if (c == 'W') flags |= AR_BP_WRITE;
            else if (c == 'D') enabled = false;
            else if (c == 'T') temporary = true;
        }

        /* Trim trailing whitespace from condition */
        size_t cl = strlen(cond);
        while (cl > 0 && (cond[cl-1] == ' ' || cond[cl-1] == '\t'))
            cond[--cl] = '\0';

        ar_bp_add(addr, flags, enabled, temporary, cond[0] ? cond : NULL,
                  cpu_id[0] ? cpu_id : NULL);
    }
    fclose(f);

    g_auto_save = was_auto;
    fprintf(stderr, "[arret] Loaded %u breakpoints from %s\n",
            (unsigned)g_bps.size(), path);
    return true;
}

void ar_bp_set_auto(bool on) {
    g_auto_save = on;
}

void ar_bp_auto_load(void) {
    const char *base = ar_rompath_base();
    if (!base || !base[0]) return;
    std::string path = std::string(base) + ".bp";
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    fclose(f);
    ar_bp_load(path.c_str());
}

void ar_bp_defer_delete(int id) {
    g_deferred_deletes.push_back(id);
}

void ar_bp_flush_deferred(void) {
    if (g_deferred_deletes.empty()) return;
    auto pending = std::move(g_deferred_deletes);
    g_deferred_deletes.clear();
    for (int id : pending)
        ar_bp_delete(id);
}

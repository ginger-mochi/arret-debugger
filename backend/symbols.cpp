/*
 * symbols.cpp: Label and comment annotation storage
 *
 * Storage: std::map<(region_id, addr), SymEntry>
 * Persistence: JSON array in <rombase>.sym.json
 * Resolution: walks memory maps to deepest backing region
 */

#include <map>
#include <optional>
#include <set>
#include <string>
#include <regex>
#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "symbols.hpp"
#include "backend.hpp"

struct SymEntry {
    std::string label;
    std::string comment;
};

using SymKey = std::pair<std::string, uint64_t>;
static std::map<SymKey, SymEntry> g_syms;

/* ======================================================================== */
/* Label validation                                                          */
/* ======================================================================== */

static bool valid_label(const char *label) {
    if (!label || !label[0]) return false;
    /* Must match [a-zA-Z_][a-zA-Z0-9_]* */
    char c = label[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
        return false;
    for (int i = 1; label[i]; i++) {
        c = label[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

/* ======================================================================== */
/* Resolution                                                                */
/* ======================================================================== */

std::optional<ar_resolved_addr> ar_sym_resolve(const char *region_id,
                                               uint64_t addr)
{
    if (!region_id) return std::nullopt;

    std::set<std::string> visited;
    std::string cur_region = region_id;
    uint64_t cur_addr = addr;

    for (;;) {
        if (visited.count(cur_region))
            return std::nullopt; /* cycle */
        visited.insert(cur_region);

        rd_Memory const *mem = ar_find_memory_by_id(cur_region.c_str());
        if (!mem) {
            /* First iteration: region doesn't exist at all */
            if (visited.size() == 1) return std::nullopt;
            break;
        }

        if (!mem->v1.get_memory_map_count || !mem->v1.get_memory_map)
            break;

        unsigned count = mem->v1.get_memory_map_count(mem);
        if (count == 0) break;

        auto *maps = new rd_MemoryMap[count];
        mem->v1.get_memory_map(mem, maps);

        bool found = false;
        for (unsigned i = 0; i < count; i++) {
            if (maps[i].source &&
                cur_addr >= maps[i].base_addr &&
                cur_addr < maps[i].base_addr + maps[i].size)
            {
                uint64_t new_addr = maps[i].source_base_addr +
                                    (cur_addr - maps[i].base_addr);
                cur_region = maps[i].source->v1.id;
                cur_addr = new_addr;
                found = true;
                break;
            }
        }
        delete[] maps;

        if (!found) break;
    }

    return ar_resolved_addr{ cur_region, cur_addr };
}

std::optional<ar_resolved_addr> ar_sym_resolve_bank(const char *region_id,
                                                    uint64_t addr,
                                                    int64_t bank)
{
    if (!region_id) return std::nullopt;

    rd_Memory const *mem = ar_find_memory_by_id(region_id);
    if (!mem) return std::nullopt;
    if (!mem->v1.get_bank_address) return std::nullopt;

    rd_MemoryMap map;
    if (!mem->v1.get_bank_address(mem, addr, bank, &map))
        return std::nullopt;

    if (map.source) {
        uint64_t new_addr = map.source_base_addr + (addr - map.base_addr);
        return ar_sym_resolve(map.source->v1.id, new_addr);
    }

    /* No source — resolve from the original region */
    return ar_sym_resolve(region_id, addr);
}

/* ======================================================================== */
/* Auto-save helper                                                          */
/* ======================================================================== */

static void auto_save(void) {
    const char *base = ar_rompath_base();
    if (!base || !base[0]) return;
    std::string path = std::string(base) + ".sym.json";
    ar_sym_save(path.c_str());
}

/* ======================================================================== */
/* Label API                                                                 */
/* ======================================================================== */

bool ar_sym_set_label(const char *region_id, uint64_t addr, const char *label) {
    if (!valid_label(label)) return false;
    SymKey key{region_id, addr};
    g_syms[key].label = label;
    auto_save();
    return true;
}

bool ar_sym_delete_label(const char *region_id, uint64_t addr) {
    auto it = g_syms.find({region_id, addr});
    if (it == g_syms.end()) return false;
    it->second.label.clear();
    if (it->second.comment.empty())
        g_syms.erase(it);
    auto_save();
    return true;
}

const char *ar_sym_get_label(const char *region_id, uint64_t addr) {
    auto it = g_syms.find({region_id, addr});
    if (it == g_syms.end() || it->second.label.empty()) return nullptr;
    return it->second.label.c_str();
}

/* ======================================================================== */
/* Comment API                                                               */
/* ======================================================================== */

bool ar_sym_set_comment(const char *region_id, uint64_t addr, const char *comment) {
    if (!comment) return false;
    SymKey key{region_id, addr};
    g_syms[key].comment = comment;
    auto_save();
    return true;
}

bool ar_sym_delete_comment(const char *region_id, uint64_t addr) {
    auto it = g_syms.find({region_id, addr});
    if (it == g_syms.end()) return false;
    it->second.comment.clear();
    if (it->second.label.empty())
        g_syms.erase(it);
    auto_save();
    return true;
}

const char *ar_sym_get_comment(const char *region_id, uint64_t addr) {
    auto it = g_syms.find({region_id, addr});
    if (it == g_syms.end() || it->second.comment.empty()) return nullptr;
    return it->second.comment.c_str();
}

/* ======================================================================== */
/* List / count / clear                                                      */
/* ======================================================================== */

unsigned ar_sym_list(ar_symbol *out, unsigned max) {
    unsigned i = 0;
    for (auto &[key, entry] : g_syms) {
        if (i >= max) break;
        ar_symbol &s = out[i++];
        memset(&s, 0, sizeof(s));
        strncpy(s.region_id, key.first.c_str(), sizeof(s.region_id) - 1);
        s.address = key.second;
        strncpy(s.label, entry.label.c_str(), sizeof(s.label) - 1);
        strncpy(s.comment, entry.comment.c_str(), sizeof(s.comment) - 1);
    }
    return i;
}

unsigned ar_sym_count(void) {
    return (unsigned)g_syms.size();
}

void ar_sym_clear(void) {
    g_syms.clear();
}

bool ar_sym_has_annotation(const char *region_id, uint64_t addr) {
    auto it = g_syms.find({region_id, addr});
    return it != g_syms.end();
}

/* ======================================================================== */
/* JSON persistence                                                          */
/* ======================================================================== */

/* Escape a string for JSON output */
static void json_write_string(FILE *f, const std::string &s) {
    fputc('"', f);
    for (char c : s) {
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if ((unsigned char)c < 0x20)
                fprintf(f, "\\u%04x", (unsigned char)c);
            else
                fputc(c, f);
            break;
        }
    }
    fputc('"', f);
}

bool ar_sym_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fputs("[\n", f);
    bool first = true;
    for (auto &[key, entry] : g_syms) {
        if (!first) fputs(",\n", f);
        first = false;
        fputs("  {\"region\":", f);
        json_write_string(f, key.first);
        fprintf(f, ",\"addr\":%" PRIu64, key.second);
        if (!entry.label.empty()) {
            fputs(",\"label\":", f);
            json_write_string(f, entry.label);
        }
        if (!entry.comment.empty()) {
            fputs(",\"comment\":", f);
            json_write_string(f, entry.comment);
        }
        fputc('}', f);
    }
    fputs("\n]\n", f);
    fclose(f);
    fprintf(stderr, "[arret] Saved %u symbols to %s\n",
            (unsigned)g_syms.size(), path);
    return true;
}

/* Simple JSON parser: read entire file, scan for objects with known keys */

static std::string json_unescape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '"':  out += '"';  i++; break;
            case '\\': out += '\\'; i++; break;
            case 'n':  out += '\n'; i++; break;
            case 'r':  out += '\r'; i++; break;
            case 't':  out += '\t'; i++; break;
            case 'u':
                /* Skip \uXXXX — just drop it for simplicity */
                i += 5;
                break;
            default: out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

/* Extract a JSON string value starting at pos (pos points to opening '"').
   Returns the unescaped string and advances pos past the closing '"'. */
static std::string parse_json_string(const std::string &data, size_t &pos) {
    if (pos >= data.size() || data[pos] != '"') return {};
    pos++; /* skip opening quote */
    std::string raw;
    while (pos < data.size()) {
        if (data[pos] == '\\' && pos + 1 < data.size()) {
            raw += data[pos];
            raw += data[pos + 1];
            pos += 2;
        } else if (data[pos] == '"') {
            pos++; /* skip closing quote */
            return json_unescape(raw);
        } else {
            raw += data[pos++];
        }
    }
    return json_unescape(raw);
}

/* Parse a JSON number at pos, advance past it */
static uint64_t parse_json_number(const std::string &data, size_t &pos) {
    uint64_t val = 0;
    while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
        val = val * 10 + (data[pos] - '0');
        pos++;
    }
    return val;
}

bool ar_sym_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return false; }
    std::string data((size_t)fsize, '\0');
    size_t nread = fread(&data[0], 1, (size_t)fsize, f);
    (void)nread;
    fclose(f);

    g_syms.clear();

    /* Scan for objects */
    size_t pos = 0;
    while (pos < data.size()) {
        /* Find next '{' */
        pos = data.find('{', pos);
        if (pos == std::string::npos) break;
        pos++; /* skip '{' */

        std::string region;
        uint64_t addr = 0;
        std::string label;
        std::string comment;
        bool has_region = false;
        bool has_addr = false;

        /* Parse key-value pairs until '}' */
        while (pos < data.size() && data[pos] != '}') {
            /* Skip whitespace and commas */
            while (pos < data.size() &&
                   (data[pos] == ' ' || data[pos] == '\t' ||
                    data[pos] == '\n' || data[pos] == '\r' ||
                    data[pos] == ','))
                pos++;

            if (pos >= data.size() || data[pos] == '}') break;

            /* Parse key */
            std::string key = parse_json_string(data, pos);
            if (key.empty()) break;

            /* Skip ':' and whitespace */
            while (pos < data.size() &&
                   (data[pos] == ':' || data[pos] == ' ' || data[pos] == '\t'))
                pos++;

            if (pos >= data.size()) break;

            /* Parse value */
            if (data[pos] == '"') {
                std::string val = parse_json_string(data, pos);
                if (key == "region")  { region = val; has_region = true; }
                else if (key == "label")   label = val;
                else if (key == "comment") comment = val;
            } else if (data[pos] >= '0' && data[pos] <= '9') {
                uint64_t val = parse_json_number(data, pos);
                if (key == "addr") { addr = val; has_addr = true; }
            } else {
                /* Skip unknown value */
                pos++;
            }
        }

        if (pos < data.size() && data[pos] == '}')
            pos++;

        if (has_region && has_addr && (!label.empty() || !comment.empty())) {
            SymEntry entry;
            entry.label = label;
            entry.comment = comment;
            g_syms[{region, addr}] = entry;
        }
    }

    fprintf(stderr, "[arret] Loaded %u symbols from %s\n",
            (unsigned)g_syms.size(), path);
    return true;
}

void ar_sym_auto_load(void) {
    const char *base = ar_rompath_base();
    if (!base || !base[0]) return;
    std::string path = std::string(base) + ".sym.json";
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    fclose(f);
    ar_sym_load(path.c_str());
}

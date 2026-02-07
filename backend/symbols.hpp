/*
 * symbols.h: Label and comment annotation API
 *
 * Symbols attach labels and comments to (region_id, address) pairs.
 * Addresses are resolved through memory maps to the deepest backing
 * region before storage.  Persisted as <rombase>.sym.json.
 */

#ifndef AR_SYMBOLS_H
#define AR_SYMBOLS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Labels: must match [a-zA-Z_][a-zA-Z0-9_]*.  Returns false on invalid. */
bool        ar_sym_set_label(const char *region_id, uint64_t addr, const char *label);
bool        ar_sym_delete_label(const char *region_id, uint64_t addr);
const char *ar_sym_get_label(const char *region_id, uint64_t addr);

/* Comments: free-form text. */
bool        ar_sym_set_comment(const char *region_id, uint64_t addr, const char *comment);
bool        ar_sym_delete_comment(const char *region_id, uint64_t addr);
const char *ar_sym_get_comment(const char *region_id, uint64_t addr);

typedef struct ar_symbol {
    char     region_id[64];
    uint64_t address;
    char     label[128];
    char     comment[1024];
} ar_symbol;

unsigned ar_sym_list(ar_symbol *out, unsigned max);
unsigned ar_sym_count(void);

bool ar_sym_save(const char *path);
bool ar_sym_load(const char *path);
void ar_sym_auto_load(void);
void ar_sym_clear(void);

/* True if address has label or comment. */
bool ar_sym_has_annotation(const char *region_id, uint64_t addr);

#ifdef __cplusplus
}

#include <optional>
#include <string>

/* Result of resolving an address through memory maps.
 * region_id: the deepest backing memory region ID
 * addr:      the translated address within that region */
struct ar_resolved_addr {
    std::string region_id;
    uint64_t    addr;
};

/* Resolve address through memory maps to the deepest backing region.
 * Follows rd_MemoryMap chains until no further source exists.
 * Returns std::nullopt on cycle or if region_id is unknown. */
std::optional<ar_resolved_addr> ar_sym_resolve(const char *region_id,
                                               uint64_t addr);

/* Resolve a banked address: looks up get_bank_address on the given region,
 * then continues resolution via ar_sym_resolve.  Returns std::nullopt if
 * the region has no banking support or the bank is out of range. */
std::optional<ar_resolved_addr> ar_sym_resolve_bank(const char *region_id,
                                                    uint64_t addr,
                                                    int64_t bank);

#endif /* __cplusplus */

#endif /* AR_SYMBOLS_H */

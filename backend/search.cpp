/*
 * search.cpp: Memory search engine (cheat finder)
 *
 * Maintains a bitfield of candidate addresses within a memory region.
 * Successive filter operations narrow the set by comparing current values
 * against a target or against previously snapshotted values.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "backend.hpp"

/* ========================================================================
 * State
 * ======================================================================== */

static rd_Memory const *s_mem;
static int       s_data_size;   /* 1, 2, or 4 */
static int       s_alignment;   /* 1, 2, or 4 */
static uint64_t  s_base_addr;
static uint64_t  s_region_size;
static uint64_t  s_num_slots;
static uint8_t  *s_candidates;  /* bitfield, 1 bit per slot */
static uint64_t *s_prev;        /* previous value per slot */
static uint64_t  s_count;

/* ========================================================================
 * Helpers
 * ======================================================================== */

static inline void bit_set(uint8_t *bf, uint64_t i)   { bf[i >> 3] |=  (1u << (i & 7)); }
static inline void bit_clear(uint8_t *bf, uint64_t i) { bf[i >> 3] &= ~(1u << (i & 7)); }
static inline bool bit_test(const uint8_t *bf, uint64_t i) {
    return (bf[i >> 3] >> (i & 7)) & 1;
}

static uint64_t read_value(uint64_t addr, int size) {
    uint64_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint64_t)s_mem->v1.peek(s_mem, addr + (uint64_t)i, false) << (i * 8);
    return v;
}

static inline uint64_t slot_to_addr(uint64_t slot) {
    return s_base_addr + slot * (uint64_t)s_alignment;
}

/* ========================================================================
 * API
 * ======================================================================== */

bool ar_search_reset(const char *region_id, int data_size, int alignment) {
    ar_search_free();

    rd_Memory const *mem = ar_find_memory_by_id(region_id);
    if (!mem) return false;

    if (data_size != 1 && data_size != 2 && data_size != 4) data_size = 1;
    if (alignment != 1 && alignment != 2 && alignment != 4) alignment = 1;
    if (alignment < data_size) alignment = data_size;

    s_mem         = mem;
    s_data_size   = data_size;
    s_alignment   = alignment;
    s_base_addr   = mem->v1.base_address;
    s_region_size = mem->v1.size;
    s_num_slots   = s_region_size / (uint64_t)alignment;

    if (s_num_slots == 0) return false;

    /* Allocate bitfield — all bits set */
    uint64_t bf_bytes = (s_num_slots + 7) / 8;
    s_candidates = (uint8_t *)malloc((size_t)bf_bytes);
    if (!s_candidates) { ar_search_free(); return false; }
    memset(s_candidates, 0xFF, (size_t)bf_bytes);

    /* Clear trailing bits beyond s_num_slots */
    uint64_t tail = bf_bytes * 8 - s_num_slots;
    if (tail > 0) {
        uint8_t mask = (uint8_t)((1u << (s_num_slots & 7)) - 1);
        s_candidates[bf_bytes - 1] &= mask;
    }

    /* Allocate and snapshot previous values */
    s_prev = (uint64_t *)malloc((size_t)(s_num_slots * sizeof(uint64_t)));
    if (!s_prev) { ar_search_free(); return false; }

    for (uint64_t i = 0; i < s_num_slots; i++)
        s_prev[i] = read_value(slot_to_addr(i), s_data_size);

    s_count = s_num_slots;
    return true;
}

uint64_t ar_search_filter(ar_search_op op, uint64_t value) {
    if (!s_candidates || !s_mem) return 0;

    uint64_t bf_bytes = (s_num_slots + 7) / 8;

    for (uint64_t byte_i = 0; byte_i < bf_bytes; byte_i++) {
        uint8_t bits = s_candidates[byte_i];
        if (bits == 0) continue;

        for (int bit = 0; bit < 8; bit++) {
            if (!(bits & (1u << bit))) continue;

            uint64_t slot = byte_i * 8 + (uint64_t)bit;
            if (slot >= s_num_slots) break;

            uint64_t cur = read_value(slot_to_addr(slot), s_data_size);
            uint64_t cmp = (value == AR_SEARCH_VS_PREV) ? s_prev[slot] : value;
            bool keep = false;

            switch (op) {
            case AR_SEARCH_EQ: keep = (cur == cmp); break;
            case AR_SEARCH_NE: keep = (cur != cmp); break;
            case AR_SEARCH_LT: keep = (cur <  cmp); break;
            case AR_SEARCH_GT: keep = (cur >  cmp); break;
            case AR_SEARCH_LE: keep = (cur <= cmp); break;
            case AR_SEARCH_GE: keep = (cur >= cmp); break;
            }

            if (!keep) {
                bit_clear(s_candidates, slot);
                s_count--;
            }
        }
    }

    /* Update prev for surviving candidates */
    for (uint64_t byte_i = 0; byte_i < bf_bytes; byte_i++) {
        uint8_t bits = s_candidates[byte_i];
        if (bits == 0) continue;

        for (int bit = 0; bit < 8; bit++) {
            if (!(bits & (1u << bit))) continue;

            uint64_t slot = byte_i * 8 + (uint64_t)bit;
            if (slot >= s_num_slots) break;

            s_prev[slot] = read_value(slot_to_addr(slot), s_data_size);
        }
    }

    return s_count;
}

unsigned ar_search_results(ar_search_result *out, unsigned max) {
    if (!s_candidates || !s_mem || max == 0) return 0;

    unsigned written = 0;
    uint64_t bf_bytes = (s_num_slots + 7) / 8;

    for (uint64_t byte_i = 0; byte_i < bf_bytes && written < max; byte_i++) {
        uint8_t bits = s_candidates[byte_i];
        if (bits == 0) continue;

        for (int bit = 0; bit < 8 && written < max; bit++) {
            if (!(bits & (1u << bit))) continue;

            uint64_t slot = byte_i * 8 + (uint64_t)bit;
            if (slot >= s_num_slots) break;

            uint64_t addr = slot_to_addr(slot);
            out[written].addr  = addr;
            out[written].value = read_value(addr, s_data_size);
            out[written].prev  = s_prev[slot];
            written++;
        }
    }

    return written;
}

uint64_t ar_search_count(void) {
    return s_count;
}

bool ar_search_active(void) {
    return s_candidates != nullptr;
}

void ar_search_free(void) {
    free(s_candidates);
    s_candidates = nullptr;
    free(s_prev);
    s_prev = nullptr;
    s_mem = nullptr;
    s_data_size = 0;
    s_alignment = 0;
    s_base_addr = 0;
    s_region_size = 0;
    s_num_slots = 0;
    s_count = 0;
}

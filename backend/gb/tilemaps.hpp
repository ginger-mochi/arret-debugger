#ifndef AR_GB_TILEMAPS_HPP
#define AR_GB_TILEMAPS_HPP

#include <cstdint>

struct rd_Memory;

namespace gb {

struct TilemapEntry {
    uint8_t tile_index;   // raw nametable byte
    uint8_t palette;      // GBC attr bits 0-2
    uint8_t vram_bank;    // GBC attr bit 3
    bool h_flip;          // GBC attr bit 5
    bool v_flip;          // GBC attr bit 6
    bool priority;        // GBC attr bit 7
    bool has_attrs;       // true on GBC
};

struct TilemapData {
    TilemapEntry entries[32][32]; // [row][col]
    uint8_t lcdc;                 // FF40 register value
    bool is_gbc;
};

TilemapData read_tilemap(rd_Memory const *mem, const char *system, int map_index);
void read_gb_palette(rd_Memory const *mem, uint32_t out[4]);
bool read_gbc_palette(rd_Memory const *bgpal, uint32_t out[8][4]);

} // namespace gb

#endif // AR_GB_TILEMAPS_HPP

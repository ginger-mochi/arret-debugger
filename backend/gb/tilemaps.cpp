#include "gb/tilemaps.hpp"
#include "retrodebug.h"
#include <cstring>

namespace gb {

static constexpr uint16_t MAP_BASE[2] = { 0x9800, 0x9C00 };

TilemapData read_tilemap(rd_Memory const *mem, const char *system, int map_index) {
    TilemapData data = {};
    if (!mem || !mem->v1.peek) return data;
    if (map_index < 0 || map_index > 1) return data;

    bool is_gbc = system && strcmp(system, "gbc") == 0;
    data.is_gbc = is_gbc;

    // Read LCDC
    data.lcdc = mem->v1.peek(mem, 0xFF40, false);

    uint16_t base = MAP_BASE[map_index];

    // Read tile indices from bank 0
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 32; col++) {
            uint16_t addr = base + row * 32 + col;
            TilemapEntry &e = data.entries[row][col];
            e.tile_index = mem->v1.peek(mem, addr, false);
            e.has_attrs = false;
        }
    }

    // On GBC, read attributes from VRAM bank 1
    if (is_gbc && mem->v1.get_bank_address) {
        rd_MemoryMap mapping;
        if (mem->v1.get_bank_address(mem, base, 1, &mapping) && mapping.source) {
            for (int row = 0; row < 32; row++) {
                for (int col = 0; col < 32; col++) {
                    uint64_t offset = row * 32 + col;
                    uint8_t attr = mapping.source->v1.peek(mapping.source,
                                       mapping.source_base_addr + offset, false);
                    TilemapEntry &e = data.entries[row][col];
                    e.palette = attr & 0x07;
                    e.vram_bank = (attr >> 3) & 1;
                    e.h_flip = (attr >> 5) & 1;
                    e.v_flip = (attr >> 6) & 1;
                    e.priority = (attr >> 7) & 1;
                    e.has_attrs = true;
                }
            }
        }
    }

    return data;
}

static constexpr uint32_t GREY[4] = {
    0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000
};

void read_gb_palette(rd_Memory const *mem, uint32_t out[4]) {
    if (!mem || !mem->v1.peek) {
        for (int i = 0; i < 4; i++) out[i] = GREY[i];
        return;
    }
    uint8_t bgp = mem->v1.peek(mem, 0xFF47, false);
    for (int i = 0; i < 4; i++)
        out[i] = GREY[(bgp >> (i * 2)) & 3];
}

bool read_gbc_palette(rd_Memory const *bgpal, uint32_t out[8][4]) {
    if (!bgpal || !bgpal->v1.peek) return false;
    for (int pal = 0; pal < 8; pal++) {
        for (int col = 0; col < 4; col++) {
            uint64_t addr = pal * 8 + col * 2;
            uint8_t lo = bgpal->v1.peek(bgpal, addr, false);
            uint8_t hi = bgpal->v1.peek(bgpal, addr + 1, false);
            uint16_t rgb555 = lo | (hi << 8);
            uint8_t r = (rgb555 & 0x1F) << 3;
            uint8_t g = ((rgb555 >> 5) & 0x1F) << 3;
            uint8_t b = ((rgb555 >> 10) & 0x1F) << 3;
            out[pal][col] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
    return true;
}

} // namespace gb

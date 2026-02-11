#include "gb/tiles.hpp"
#include "retrodebug.h"
#include <cstring>

namespace gb {

static constexpr uint64_t VRAM_START = 0x8000;
static constexpr uint64_t VRAM_TILE_END = 0x9800; // 0x8000-0x97FF = 6144 bytes = 384 tiles
static constexpr int TILES_PER_BANK = 384;
static constexpr int BYTES_PER_TILE = 16;

static void decode_tile(const uint8_t *data, TileImage &tile) {
    for (int row = 0; row < 8; row++) {
        uint8_t lo = data[row * 2];
        uint8_t hi = data[row * 2 + 1];
        for (int bit = 7; bit >= 0; bit--) {
            int pixel = ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
            tile.pixels[row * 8 + (7 - bit)] = (uint8_t)pixel;
        }
    }
}

static void read_bank(rd_Memory const *mem, int bank, std::vector<TileImage> &tiles) {
    uint8_t raw[TILES_PER_BANK * BYTES_PER_TILE];

    if (bank == 0) {
        // Bank 0: direct peek
        for (uint64_t addr = VRAM_START; addr < VRAM_TILE_END; addr++)
            raw[addr - VRAM_START] = mem->v1.peek(mem, addr, false);
    } else {
        // Bank 1 (GBC): use get_bank_address to resolve backing memory
        if (!mem->v1.get_bank_address)
            return;

        rd_MemoryMap mapping;
        if (!mem->v1.get_bank_address(mem, VRAM_START, bank, &mapping))
            return;

        // Peek through the source region at the resolved offset
        rd_Memory const *src = mapping.source;
        if (!src)
            return;

        for (uint64_t i = 0; i < VRAM_TILE_END - VRAM_START; i++)
            raw[i] = src->v1.peek(src, mapping.source_base_addr + i, false);
    }

    int base = (int)tiles.size();
    tiles.resize(base + TILES_PER_BANK);
    for (int i = 0; i < TILES_PER_BANK; i++)
        decode_tile(&raw[i * BYTES_PER_TILE], tiles[base + i]);
}

TileSet read_tiles(rd_Memory const *mem, const char *system) {
    TileSet ts;
    ts.banks = 1;

    if (!mem || !mem->v1.peek)
        return ts;

    read_bank(mem, 0, ts.tiles);

    // GBC has a second VRAM bank
    if (system && strcmp(system, "gbc") == 0) {
        if (mem->v1.get_bank_address) {
            rd_MemoryMap mapping;
            if (mem->v1.get_bank_address(mem, VRAM_START, 1, &mapping)) {
                ts.banks = 2;
                read_bank(mem, 1, ts.tiles);
            }
        }
    }

    return ts;
}

} // namespace gb

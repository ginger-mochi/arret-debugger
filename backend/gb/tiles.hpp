#ifndef AR_GB_TILES_HPP
#define AR_GB_TILES_HPP

#include <cstdint>
#include <vector>

struct rd_Memory;

namespace gb {

struct TileImage {
    uint8_t pixels[64]; // 8x8, values 0-3 (palette index)
};

struct TileSet {
    std::vector<TileImage> tiles; // 384 tiles per bank
    int banks;                     // 1 for GB, 2 for GBC
};

// Read all tiles from VRAM via retrodebug peek.
// mem = the CPU-addressable "ram" region, system = "gb" or "gbc"
TileSet read_tiles(rd_Memory const *mem, const char *system);

} // namespace gb

#endif // AR_GB_TILES_HPP

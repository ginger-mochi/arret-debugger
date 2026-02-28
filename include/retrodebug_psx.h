/*
 * retrodebug_psx.h: PSX-specific retrodebug data structures
 *
 * Shared between PSX libretro cores and the frontend.
 * Passed via rd_MiscBreakpointEvent.data for GPU Post events.
 */

#ifndef RETRODEBUG_PSX_H
#define RETRODEBUG_PSX_H

#include <stdint.h>

typedef struct rd_psx_gpu_post {
    uint8_t  port;        /* 0 = GP0, 1 = GP1 */
    uint8_t  source;      /* 0 = CPU, 2 = DMA ch2 */
    uint16_t word_count;  /* number of valid entries in words[] */
    uint32_t pc;          /* R3000A program counter */
    uint32_t words[16];   /* command words (up to 16) */
}
rd_psx_gpu_post;

#endif /* RETRODEBUG_PSX_H */

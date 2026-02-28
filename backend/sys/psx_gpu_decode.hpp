/*
 * psx_gpu_decode.hpp: PSX GPU command decode functions
 *
 * Shared between the trace module (psx.cpp) and the VRAM viewer
 * GPU event log (VramViewer.cpp).
 */

#ifndef PSX_GPU_DECODE_HPP
#define PSX_GPU_DECODE_HPP

#include <stdint.h>
#include <stddef.h>

namespace sys {

void decode_gp0(char *out, size_t out_size,
                const uint32_t *words, unsigned count);

void decode_gp1(char *out, size_t out_size,
                const uint32_t *words);

/* Detailed decode for the event detail pane (vertices, texcoords, colors). */
void decode_gp0_detail(char *out, size_t out_size,
                       const uint32_t *words, unsigned count);

} // namespace sys

#endif // PSX_GPU_DECODE_HPP

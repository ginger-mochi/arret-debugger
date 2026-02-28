/*
 * psx_gpu_capture.hpp: PSX GPU event capture
 *
 * Runs on the core thread.  Records GPU commands, computes VRAM diffs
 * (rectangular, bounding-box–sized), and inserts frame boundaries.
 * The Qt VramViewer reads the finished capture for display.
 */

#ifndef PSX_GPU_CAPTURE_HPP
#define PSX_GPU_CAPTURE_HPP

#include "retrodebug.h"
#include <cstdint>
#include <vector>
#include <mutex>

namespace sys {

/* One captured GPU event. */
struct GpuCapEvent {
    enum Type : uint8_t { GPU_COMMAND, FRAME_BOUNDARY };
    Type type;
    uint8_t  port;           // 0=GP0, 1=GP1
    uint8_t  source;         // 0=CPU, 2=DMA ch2
    bool     is_keyframe;
    unsigned word_count;
    uint32_t words[16];
    uint32_t pc;             // R3000A PC
    unsigned frame_number;   // for FRAME_BOUNDARY

    /* Compressed VRAM diff (qCompress'd).
     * Keyframe: full 1MB VRAM.
     * Partial:  diff_w * diff_h * 2 bytes of packed XOR data. */
    std::vector<uint8_t> diff;

    /* Bounding rectangle in VRAM halfword coords.
     * diff_w == 0 && diff_h == 0 means full VRAM (keyframe or fallback). */
    uint16_t diff_x, diff_y, diff_w, diff_h;
};

/* Start capturing GPU Post events.
 * Returns true if capture was started.  Thread-safe (called from UI). */
bool gpu_capture_start(rd_DebuggerIf *dif);

/* Stop capturing.  Thread-safe (called from UI). */
void gpu_capture_stop(rd_DebuggerIf *dif);

/* Insert a frame boundary.  Called from the core thread
 * (or from refresh(), serialised by the capture mutex). */
void gpu_capture_frame_boundary();

/* Returns true if a capture is in progress. */
bool gpu_capture_active();

/* Access captured events.  Only valid after capture stops.
 * The returned reference is stable until the next gpu_capture_start(). */
const std::vector<GpuCapEvent> &gpu_capture_events();

/* Total compressed bytes stored during capture. */
size_t gpu_capture_compressed_bytes();

/* Lock/unlock for reading capture data from the UI thread. */
std::mutex &gpu_capture_mutex();

/* Reconstruct full 1MB VRAM state at event index idx.
 * Writes into out (must be >= 1048576 bytes).
 * Returns true on success. */
bool gpu_capture_reconstruct(unsigned idx, uint8_t *out);

} // namespace sys

#endif // PSX_GPU_CAPTURE_HPP

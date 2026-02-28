/*
 * psx_gpu_capture.cpp: PSX GPU event capture
 *
 * Core-thread capture of GPU commands with rectangular VRAM diffs.
 */

#include "psx_gpu_capture.hpp"
#include "backend.hpp"
#include "retrodebug_psx.h"

#include <cstring>
#include <climits>
#include <algorithm>
#include <zlib.h>

namespace sys {

/* Define to store every VRAM-modifying event as a full keyframe (no XOR diffs).
 * Useful for debugging capture/reconstruct issues at the cost of memory. */
#define GPU_CAPTURE_ALL_KEYFRAMES

static constexpr int VRAM_W     = 1024;   // halfwords per row
static constexpr int VRAM_H     = 512;
static constexpr int VRAM_BYTES = VRAM_W * VRAM_H * 2;  // 1 048 576
static constexpr int KEYFRAME_INTERVAL = 128;

/* ======================================================================== */
/* Compression helpers (zlib, compatible with Qt's qCompress/qUncompress)   */
/* ======================================================================== */

static std::vector<uint8_t> zcompress(const uint8_t *data, size_t len) {
    /* qCompress format: 4-byte big-endian uncompressed size + zlib stream */
    uLongf bound = compressBound((uLong)len);
    std::vector<uint8_t> out(4 + bound);
    out[0] = (uint8_t)((len >> 24) & 0xFF);
    out[1] = (uint8_t)((len >> 16) & 0xFF);
    out[2] = (uint8_t)((len >>  8) & 0xFF);
    out[3] = (uint8_t)((len >>  0) & 0xFF);
    uLongf dest_len = bound;
    int rc = compress2(out.data() + 4, &dest_len, data, (uLong)len, Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) return {};
    out.resize(4 + dest_len);
    return out;
}

static bool zuncompress(const std::vector<uint8_t> &src, uint8_t *out, size_t out_len) {
    if (src.size() < 4) return false;
    uLongf dest_len = (uLongf)out_len;
    int rc = uncompress(out, &dest_len, src.data() + 4, (uLong)(src.size() - 4));
    return rc == Z_OK && dest_len == (uLongf)out_len;
}

/* ======================================================================== */
/* VRAM bounding rectangle                                                   */
/* ======================================================================== */

struct VramRect { int x, y, w, h; };

static inline int sign11(uint32_t v) {
    return (int)(v << 21) >> 21;
}

/* Compute the VRAM rectangle (halfword coords) affected by a GP0 command.
 * Returns false if bounds can't be determined (caller diffs full VRAM). */
static bool gpuCmdVramRect(const uint32_t *words, unsigned count,
                           int off_x, int off_y,
                           int ax1, int ay1, int ax2, int ay2,
                           VramRect &r)
{
    if (count == 0) return false;
    uint8_t op = (uint8_t)(words[0] >> 24);

    int x0, y0, x1, y1;

    if (op == 0x02 && count >= 3) {
        /* FillRect — absolute VRAM coords */
        int x = (int)(words[1] & 0x3F0);
        int y = (int)((words[1] >> 16) & 0x3FF);
        int w = (int)(((words[2] & 0x3FF) + 0xF) & ~0xF);
        int h = (int)((words[2] >> 16) & 0x1FF);
        if (w == 0 || h == 0) return false;
        x0 = x; y0 = y; x1 = x + w - 1; y1 = y + h - 1;
        if (x1 >= VRAM_W || y1 >= VRAM_H) return false;
    } else if (op >= 0x20 && op <= 0x3F) {
        /* Polygon — mednafen fires the post hook per-triangle, so quads
         * arrive as two 3-vertex calls.  Use only the vertices present. */
        bool tex   = op & 0x04;
        bool shade = op & 0x10;
        unsigned stride = 1 + (shade ? 1 : 0) + (tex ? 1 : 0);
        x0 = INT_MAX; y0 = INT_MAX; x1 = INT_MIN; y1 = INT_MIN;
        for (int v = 0; v < 3; v++) {
            unsigned idx = (v == 0) ? 1 : 1 + v * stride;
            if (idx >= count) return false;
            int vx = sign11(words[idx] & 0x7FF) + off_x;
            int vy = sign11((words[idx] >> 16) & 0x7FF) + off_y;
            if (vx < x0) x0 = vx;
            if (vx > x1) x1 = vx;
            if (vy < y0) y0 = vy;
            if (vy > y1) y1 = vy;
        }
        if (x0 < ax1) x0 = ax1;
        if (y0 < ay1) y0 = ay1;
        if (x1 > ax2) x1 = ax2;
        if (y1 > ay2) y1 = ay2;
    } else if (op >= 0x40 && op <= 0x5F) {
        /* Line / Polyline */
        if (op & 0x08) return false;  /* polyline — vertex count unknown */
        bool shade = op & 0x10;
        unsigned v1_idx = shade ? 3 : 2;
        if (v1_idx >= count || count < 2) return false;
        int vx0 = sign11(words[1] & 0x7FF) + off_x;
        int vy0 = sign11((words[1] >> 16) & 0x7FF) + off_y;
        int vx1 = sign11(words[v1_idx] & 0x7FF) + off_x;
        int vy1 = sign11((words[v1_idx] >> 16) & 0x7FF) + off_y;
        x0 = std::min(vx0, vx1);
        x1 = std::max(vx0, vx1);
        y0 = std::min(vy0, vy1);
        y1 = std::max(vy0, vy1);
        if (x0 < ax1) x0 = ax1;
        if (y0 < ay1) y0 = ay1;
        if (x1 > ax2) x1 = ax2;
        if (y1 > ay2) y1 = ay2;
    } else if (op >= 0x60 && op <= 0x7F) {
        /* Rectangle */
        bool tex = op & 0x04;
        unsigned sz = (op >> 3) & 0x03;
        if (count < 2) return false;
        int vx = sign11(words[1] & 0x7FF) + off_x;
        int vy = sign11((words[1] >> 16) & 0x7FF) + off_y;
        int w, h;
        switch (sz) {
        case 1: w = 1; h = 1; break;
        case 2: w = 8; h = 8; break;
        case 3: w = 16; h = 16; break;
        default:
            if (count < (tex ? 4u : 3u)) return false;
            w = (int)(words[tex ? 3 : 2] & 0x3FF);
            h = (int)((words[tex ? 3 : 2] >> 16) & 0x1FF);
            break;
        }
        x0 = vx; y0 = vy; x1 = vx + w - 1; y1 = vy + h - 1;
        if (x0 < ax1) x0 = ax1;
        if (y0 < ay1) y0 = ay1;
        if (x1 > ax2) x1 = ax2;
        if (y1 > ay2) y1 = ay2;
    } else if (op >= 0x80 && op <= 0x9F && count >= 4) {
        /* VRAM-to-VRAM copy — destination */
        int dx = (int)(words[2] & 0x3FF);
        int dy = (int)((words[2] >> 16) & 0x3FF);
        int w  = (int)(words[3] & 0x3FF);
        int h  = (int)((words[3] >> 16) & 0x1FF);
        if (w == 0) w = 0x400;
        if (h == 0) h = 0x200;
        x0 = dx; y0 = dy; x1 = dx + w - 1; y1 = dy + h - 1;
        if (x1 >= VRAM_W || y1 >= VRAM_H) return false;
    } else if (op >= 0xA0 && op <= 0xBF && count >= 3) {
        /* CPU-to-VRAM */
        int x = (int)(words[1] & 0x3FF);
        int y = (int)((words[1] >> 16) & 0x3FF);
        int w = (int)(words[2] & 0x3FF);
        int h = (int)((words[2] >> 16) & 0x1FF);
        if (w == 0) w = 0x400;
        if (h == 0) h = 0x200;
        x0 = x; y0 = y; x1 = x + w - 1; y1 = y + h - 1;
        if (x1 >= VRAM_W || y1 >= VRAM_H) return false;
    } else {
        return false;
    }

    /* 1-halfword margin */
    x0 -= 1; y0 -= 1; x1 += 1; y1 += 1;

    /* Clamp to VRAM */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1;
    if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return false;

    r.x = x0; r.y = y0;
    r.w = x1 - x0 + 1;
    r.h = y1 - y0 + 1;
    return true;
}

/* ======================================================================== */
/* Rectangular VRAM read/extract/patch helpers                               */
/* ======================================================================== */

#if 0   /* Disabled along with partial rect diff */
static void readVramRect(rd_Memory const *mem, const VramRect &r, uint8_t *out) {
    unsigned row_bytes = (unsigned)r.w * 2;
    for (int row = 0; row < r.h; row++) {
        uint64_t addr = ((uint64_t)(r.y + row) * VRAM_W + r.x) * 2;
        if (mem->v1.peek_range)
            mem->v1.peek_range(mem, addr, row_bytes, out);
        else
            for (unsigned b = 0; b < row_bytes; b++)
                out[b] = mem->v1.peek(mem, addr + b, false);
        out += row_bytes;
    }
}

static void extractRect(const uint8_t *vram, const VramRect &r, uint8_t *out) {
    unsigned row_bytes = (unsigned)r.w * 2;
    for (int row = 0; row < r.h; row++) {
        unsigned off = ((unsigned)(r.y + row) * VRAM_W + (unsigned)r.x) * 2;
        memcpy(out, vram + off, row_bytes);
        out += row_bytes;
    }
}

static void patchRect(uint8_t *vram, const VramRect &r, const uint8_t *src) {
    unsigned row_bytes = (unsigned)r.w * 2;
    for (int row = 0; row < r.h; row++) {
        unsigned off = ((unsigned)(r.y + row) * VRAM_W + (unsigned)r.x) * 2;
        memcpy(vram + off, src, row_bytes);
        src += row_bytes;
    }
}

static void xorRect(const uint8_t *vram, const VramRect &r,
                     const uint8_t *prev, uint8_t *xor_out) {
    unsigned row_bytes = (unsigned)r.w * 2;
    for (int row = 0; row < r.h; row++) {
        unsigned off = ((unsigned)(r.y + row) * VRAM_W + (unsigned)r.x) * 2;
        for (unsigned b = 0; b < row_bytes; b++)
            xor_out[row * row_bytes + b] = vram[off + b] ^ prev[off + b];
    }
}
#endif  /* partial rect diff helpers */

/* ======================================================================== */
/* Capture state                                                             */
/* ======================================================================== */

static std::mutex              g_mutex;
static std::vector<GpuCapEvent> g_events;
static std::vector<uint8_t>   g_prevVram;   // 1MB shadow buffer
static rd_Memory const        *g_vramMem = nullptr;
static rd_SubscriptionID      g_sub = -1;
static bool                    g_active = false;
static unsigned                g_frameCounter = 0;
static size_t                  g_compressedBytes = 0;

/* Deferred diff for CPU>VRAM: at post-hook time the transfer hasn't
 * completed yet (InCmd=INCMD_FBWRITE), so we record the event but defer
 * the VRAM read until the start of the next event or frame boundary. */
static bool                    g_deferred = false;
static size_t                  g_deferredIdx = 0;

/* GPU drawing state (core thread only, no lock needed) */
static int g_drawOffX = 0, g_drawOffY = 0;
static int g_drawAreaX1 = 0, g_drawAreaY1 = 0;
static int g_drawAreaX2 = VRAM_W - 1, g_drawAreaY2 = VRAM_H - 1;

/* ======================================================================== */
/* Full VRAM read helper                                                     */
/* ======================================================================== */

static void readFullVram(rd_Memory const *mem, uint8_t *buf) {
    if (mem->v1.peek_range && mem->v1.peek_range(mem, 0, VRAM_BYTES, buf))
        return;
    for (int i = 0; i < VRAM_BYTES; i++)
        buf[i] = mem->v1.peek(mem, i, false);
}

/* ======================================================================== */
/* Deferred diff completion                                                  */
/* ======================================================================== */

/* Complete the deferred VRAM diff for a previous CPU>VRAM event.
 * Must be called with g_mutex held and g_deferred == true. */
static void completeDeferredDiff() {
    if (!g_deferred || !g_vramMem) return;
    g_deferred = false;

    GpuCapEvent &ev = g_events[g_deferredIdx];

    std::vector<uint8_t> cur(VRAM_BYTES);
    readFullVram(g_vramMem, cur.data());

#ifdef GPU_CAPTURE_ALL_KEYFRAMES
    ev.is_keyframe = true;
    ev.diff = zcompress(cur.data(), VRAM_BYTES);
#else
    if (g_deferredIdx % KEYFRAME_INTERVAL == 0) {
        ev.is_keyframe = true;
        ev.diff = zcompress(cur.data(), VRAM_BYTES);
    } else {
        std::vector<uint8_t> xd(VRAM_BYTES);
        for (int i = 0; i < VRAM_BYTES; i++)
            xd[i] = cur[i] ^ g_prevVram[i];
        ev.is_keyframe = false;
        ev.diff = zcompress(xd.data(), VRAM_BYTES);
    }
#endif
    g_prevVram = std::move(cur);
    g_compressedBytes += ev.diff.size();
}

/* ======================================================================== */
/* Event handler (core thread)                                               */
/* ======================================================================== */

static bool onCaptureEvent(rd_SubscriptionID, rd_Event const *event) {
    if (!g_active || event->type != RD_EVENT_MISC) return false;
    if (event->misc.data_size < sizeof(rd_psx_gpu_post)) return false;
    auto *post = (const rd_psx_gpu_post *)event->misc.data;

    unsigned port   = post->port;
    unsigned count  = post->word_count;
    const uint32_t *words = post->words;
    uint32_t pc     = post->pc;
    unsigned source = post->source;

    /* Track GPU drawing state from config commands */
    if (port == 0 && count > 0) {
        uint8_t cfg = (uint8_t)(words[0] >> 24);
        if (cfg == 0xE3) {
            g_drawAreaX1 = words[0] & 0x3FF;
            g_drawAreaY1 = (words[0] >> 10) & 0x1FF;
        } else if (cfg == 0xE4) {
            g_drawAreaX2 = words[0] & 0x3FF;
            g_drawAreaY2 = (words[0] >> 10) & 0x1FF;
        } else if (cfg == 0xE5) {
            g_drawOffX = sign11(words[0] & 0x7FF);
            g_drawOffY = sign11((words[0] >> 11) & 0x7FF);
        }
    }

    /* Build event record */
    GpuCapEvent ev{};
    ev.type = GpuCapEvent::GPU_COMMAND;
    ev.port = (uint8_t)port;
    ev.source = (uint8_t)source;
    ev.pc = pc;
    ev.word_count = count < 16 ? count : 16;
    for (unsigned i = 0; i < ev.word_count; i++)
        ev.words[i] = words[i];

    /* Does this command modify VRAM? */
    bool modifies_vram = false;
    bool is_cpu_to_vram = false;
    if (port == 0 && count > 0) {
        uint8_t op = (uint8_t)(words[0] >> 24);
        if (op == 0x02 ||
            (op >= 0x20 && op <= 0x7F) ||
            (op >= 0x80 && op <= 0xBF))
            modifies_vram = true;
        if (op >= 0xA0 && op <= 0xBF)
            is_cpu_to_vram = true;
    }

    std::lock_guard lock(g_mutex);

    /* Complete any deferred CPU>VRAM diff — by now the transfer has finished */
    if (g_deferred)
        completeDeferredDiff();

    unsigned eventIdx = (unsigned)g_events.size();

    if (modifies_vram && g_vramMem) {
        /* Compute bounding box for UI overlay */
        VramRect rect;
        bool have_rect = gpuCmdVramRect(
            ev.words, ev.word_count,
            g_drawOffX, g_drawOffY,
            g_drawAreaX1, g_drawAreaY1,
            g_drawAreaX2, g_drawAreaY2,
            rect);

        if (have_rect) {
            ev.diff_x = (uint16_t)rect.x;
            ev.diff_y = (uint16_t)rect.y;
            ev.diff_w = (uint16_t)rect.w;
            ev.diff_h = (uint16_t)rect.h;
        }

        if (is_cpu_to_vram) {
            /* CPU>VRAM: defer diff — VRAM hasn't been updated yet at
             * post-hook time (only InCmd=INCMD_FBWRITE is set). */
            g_events.push_back(std::move(ev));
            g_deferred = true;
            g_deferredIdx = eventIdx;
            return false;
        }

        {
            std::vector<uint8_t> cur(VRAM_BYTES);
            readFullVram(g_vramMem, cur.data());

#ifdef GPU_CAPTURE_ALL_KEYFRAMES
            ev.is_keyframe = true;
            ev.diff = zcompress(cur.data(), VRAM_BYTES);
#else
            if (eventIdx % KEYFRAME_INTERVAL == 0) {
                ev.is_keyframe = true;
                ev.diff = zcompress(cur.data(), VRAM_BYTES);
            } else {
                std::vector<uint8_t> xd(VRAM_BYTES);
                for (int i = 0; i < VRAM_BYTES; i++)
                    xd[i] = cur[i] ^ g_prevVram[i];
                ev.is_keyframe = false;
                ev.diff = zcompress(xd.data(), VRAM_BYTES);
            }
#endif
            g_prevVram = std::move(cur);
        }
        g_compressedBytes += ev.diff.size();
    }

    g_events.push_back(std::move(ev));
    return false;
}

static bool isCaptureSubFilter(rd_SubscriptionID sub_id) {
    return g_active && sub_id == g_sub;
}

/* ======================================================================== */
/* Public API                                                                */
/* ======================================================================== */

bool gpu_capture_start(rd_DebuggerIf *dif) {
    if (!dif || !dif->v1.subscribe || !dif->v1.system) return false;
    if (g_active) return false;

    /* Using "GP0" (pre-command hook) for capture */
    rd_System const *sys = dif->v1.system;
    rd_MiscBreakpoint const *bp_post = nullptr;
    for (unsigned i = 0; i < sys->v1.num_break_points; i++) {
        if (strcmp(sys->v1.break_points[i]->v1.description, "GP0") == 0) {
            bp_post = sys->v1.break_points[i];
            break;
        }
    }
    if (!bp_post) return false;

    rd_Subscription sub{};
    sub.type = RD_EVENT_MISC;
    sub.misc.breakpoint = bp_post;
    g_sub = dif->v1.subscribe(&sub);
    if (g_sub < 0) return false;

    ar_set_aux_event_handler(isCaptureSubFilter, onCaptureEvent);
    ar_set_post_frame_hook(gpu_capture_frame_boundary);

    g_vramMem = ar_find_memory_by_id("vram");
    if (!g_vramMem) {
        dif->v1.unsubscribe(g_sub);
        g_sub = -1;
        ar_clear_aux_event_handler();
        ar_clear_post_frame_hook();
        return false;
    }

    /* Reset state */
    {
        std::lock_guard lock(g_mutex);
        g_events.clear();
        g_compressedBytes = 0;
        g_frameCounter = 1;
        g_drawOffX = 0; g_drawOffY = 0;
        g_drawAreaX1 = 0; g_drawAreaY1 = 0;
        g_drawAreaX2 = VRAM_W - 1; g_drawAreaY2 = VRAM_H - 1;
        g_deferred = false;

        /* Initial keyframe */
        g_prevVram.resize(VRAM_BYTES);
        readFullVram(g_vramMem, g_prevVram.data());

        GpuCapEvent ev{};
        ev.type = GpuCapEvent::GPU_COMMAND;
        ev.is_keyframe = true;
        ev.diff = zcompress(g_prevVram.data(), VRAM_BYTES);
        g_compressedBytes += ev.diff.size();
        g_events.push_back(std::move(ev));
    }

    g_active = true;
    return true;
}

void gpu_capture_stop(rd_DebuggerIf *dif) {
    g_active = false;

    if (dif && dif->v1.unsubscribe && g_sub >= 0) {
        dif->v1.unsubscribe(g_sub);
        g_sub = -1;
    }
    ar_clear_aux_event_handler();
    ar_clear_post_frame_hook();

    /* Free shadow buffer */
    g_prevVram.clear();
    g_prevVram.shrink_to_fit();
    g_vramMem = nullptr;
}

void gpu_capture_frame_boundary() {
    if (!g_active) return;
    std::lock_guard lock(g_mutex);

    /* Complete any deferred CPU>VRAM diff before the frame boundary */
    if (g_deferred)
        completeDeferredDiff();

    GpuCapEvent ev{};
    ev.type = GpuCapEvent::FRAME_BOUNDARY;
    ev.frame_number = g_frameCounter++;
    g_events.push_back(std::move(ev));
}

bool gpu_capture_active() {
    return g_active;
}

const std::vector<GpuCapEvent> &gpu_capture_events() {
    return g_events;
}

size_t gpu_capture_compressed_bytes() {
    return g_compressedBytes;
}

std::mutex &gpu_capture_mutex() {
    return g_mutex;
}

bool gpu_capture_reconstruct(unsigned idx, uint8_t *out) {
    if (idx >= g_events.size()) return false;

    /* Walk back to nearest event with VRAM data */
    unsigned target = idx;
    while (target > 0 && (g_events[target].type == GpuCapEvent::FRAME_BOUNDARY ||
                           g_events[target].diff.empty()))
        target--;
    if (g_events[target].diff.empty()) return false;

#ifdef GPU_CAPTURE_ALL_KEYFRAMES
    /* Every VRAM-modifying event is a keyframe — just decompress it */
    return zuncompress(g_events[target].diff, out, VRAM_BYTES);
#else
    /* Find nearest keyframe <= target */
    unsigned kf = target;
    while (kf > 0 && !g_events[kf].is_keyframe) {
        kf--;
        if (g_events[kf].type == GpuCapEvent::FRAME_BOUNDARY || g_events[kf].diff.empty()) {
            if (kf == 0) break;
            continue;
        }
    }
    if (!g_events[kf].is_keyframe || g_events[kf].diff.empty()) return false;

    /* Decompress keyframe */
    if (!zuncompress(g_events[kf].diff, out, VRAM_BYTES)) return false;

    /* Apply subsequent diffs */
    for (unsigned i = kf + 1; i <= target; i++) {
        const GpuCapEvent &ev = g_events[i];
        if (ev.type == GpuCapEvent::FRAME_BOUNDARY || ev.diff.empty() || ev.is_keyframe)
            continue;

        /* Full VRAM diff */
        std::vector<uint8_t> xd(VRAM_BYTES);
        if (!zuncompress(ev.diff, xd.data(), VRAM_BYTES)) continue;
        for (int j = 0; j < VRAM_BYTES; j++)
            out[j] ^= xd[j];
    }

    return true;
#endif
}

} // namespace sys

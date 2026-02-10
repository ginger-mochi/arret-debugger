/*
 * backend.cpp: Arrêt Debugger shared backend
 *
 * Core loading, libretro callbacks, save/load, retrodebug, audio.
 * Command processing is in cmd.cpp.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <unistd.h>
#include <new>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "backend.hpp"
#include "registers.hpp"
#include "trace.hpp"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define MAX_WIDTH     256
#define MAX_HEIGHT    224
#define MAX_PIXELS    (MAX_WIDTH * MAX_HEIGHT)
#define MAX_VARS      64
#define MAX_SAVE_SLOTS 10
#define AUDIO_RING_SIZE (48000 * 2)  /* ~1 second stereo at 48kHz */
#define CORE_AUDIO_RATE 384000
#define DOWNSAMPLE_RATE 48000
#define DOWNSAMPLE_RATIO (CORE_AUDIO_RATE / DOWNSAMPLE_RATE) /* 8 */

/* ========================================================================
 * Core function pointers
 * ======================================================================== */

typedef struct {
    void *handle;

    void (*retro_init)(void);
    void (*retro_deinit)(void);
    unsigned (*retro_api_version)(void);
    void (*retro_get_system_info)(struct retro_system_info *info);
    void (*retro_get_system_av_info)(struct retro_system_av_info *info);
    void (*retro_set_controller_port_device)(unsigned port, unsigned device);
    void (*retro_reset)(void);
    void (*retro_run)(void);
    size_t (*retro_serialize_size)(void);
    bool (*retro_serialize)(void *data, size_t size);
    bool (*retro_unserialize)(const void *data, size_t size);
    bool (*retro_load_game)(const struct retro_game_info *game);
    void (*retro_unload_game)(void);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);

} core_t;

/* ========================================================================
 * Global state
 * ======================================================================== */

static core_t core;
static bool g_running = true;
static bool g_manual_input = false;
static bool g_mute = false;

/* Directories */
static char system_dir[4096] = ".";
static char save_dir[4096]   = ".";

/* ROM path and base */
static char rom_path_saved[4096];
static char rom_base[4096];

/* Video */
static uint32_t frame_buf[MAX_PIXELS];
static unsigned frame_width  = 160;
static unsigned frame_height = 144;
static unsigned frame_pitch  = 160 * sizeof(uint32_t);

/* AV info */
static struct retro_system_av_info av_info;
static struct retro_system_info sys_info;

/* Input */
static int16_t input_state_val[16];
static bool    input_fixed[16]     = {};
static int16_t input_fixed_val[16] = {};
static bool input_bitmasks_supported = false;

/* Analog input */
static int16_t analog_state_val[4] = {};   /* [lx, ly, rx, ry] */
static bool    analog_fixed[4]     = {};
static int16_t analog_fixed_val[4] = {};

/* Controller types (from SET_CONTROLLER_INFO, port 0) */
#define MAX_CONTROLLER_TYPES 16
static struct { char desc[128]; unsigned id; } controller_types[MAX_CONTROLLER_TYPES];
static unsigned num_controller_types = 0;

/* Variables */
static struct {
    char key[128];
    char value[256];
    char desc[512];
} variables[MAX_VARS];
static unsigned num_variables = 0;
static bool variables_updated = false;

/* Audio ring buffer */
static int16_t audio_ring[AUDIO_RING_SIZE];
static volatile unsigned audio_ring_write = 0;
static volatile unsigned audio_ring_read  = 0;
static unsigned audio_downsample_count = 0;

/* Proc address interface (provided by core via SET_PROC_ADDRESS_CALLBACK) */
static retro_get_proc_address_t core_get_proc_address = NULL;

/* Retrodebug (obtained via get_proc_address) */
static rd_Set rd_set_debugger_fn = NULL;
static rd_DebuggerIf *debugger_if_ptr = NULL;
static rd_Cpu const *debug_cpu_ptr = NULL;
static rd_Memory const *debug_mem_ptr = NULL;
static bool g_has_debug = false;

/* Stepping state */
static rd_SubscriptionID g_step_sub_id = -1;
static bool g_step_active = false;
static bool g_step_complete = false;

/* Breakpoint hit — set to bp id by handle_event, cleared by frontend */
static int g_bp_hit_id = -1;

/* Centralized per-CPU skip: suppress pause-worthy events at these addresses.
 * When resuming from a breakpoint or step, the skip address is set to each
 * CPU's current PC.  A temporary broad step subscription ensures the skip
 * entry gets cleared as soon as the CPU advances past that address. */
#include <map>
static std::map<rd_Cpu const*, uint64_t> g_skip_addr;
static std::map<rd_Cpu const*, rd_SubscriptionID> g_skip_temp_subs;

/* Whether core + content are loaded (for split init in Qt) */
static bool g_core_loaded = false;
static bool g_content_loaded = false;

/* Frontend callbacks */
static ar_frontend_cb frontend_cb;

/* Core thread */
static std::thread              g_core_thread;
static std::mutex               g_core_mutex;
static std::condition_variable  g_core_cv;

enum CoreState { CORE_IDLE, CORE_RUNNING, CORE_BLOCKED, CORE_DONE };
static CoreState                g_core_state = CORE_IDLE;
static bool                     g_core_quit = false;

/* For thread blocking (handle_event blocks the core thread) */
static std::mutex               g_block_mutex;
static std::condition_variable  g_block_cv;
static bool                     g_block_resume = false;

/* JSON output fd (saved original stdout) */
static FILE *json_out_saved = NULL;

/* TCP command port (passed to ar_cmd_server_init) */
static int listen_port_g = 2783;

/* ========================================================================
 * stdout redirection
 * ======================================================================== */

static void init_json_output(void) {
    int saved_fd = dup(STDOUT_FILENO);
    json_out_saved = fdopen(saved_fd, "w");
    if (!freopen("/dev/null", "w", stdout))
        perror("freopen");
}

/* ========================================================================
 * Libretro callbacks
 * ======================================================================== */

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool core_environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        auto *fmt = (enum retro_pixel_format *)data;
        return *fmt == RETRO_PIXEL_FORMAT_XRGB8888;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto *cb = (struct retro_log_callback *)data;
        cb->log = core_log;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        auto *vars = (const struct retro_variable *)data;
        num_variables = 0;
        for (; vars && vars->key; vars++) {
            if (num_variables >= MAX_VARS) break;
            strncpy(variables[num_variables].key, vars->key,
                    sizeof(variables[num_variables].key) - 1);
            strncpy(variables[num_variables].desc, vars->value,
                    sizeof(variables[num_variables].desc) - 1);
            const char *semi = strchr(vars->value, ';');
            if (semi) {
                semi++;
                while (*semi == ' ') semi++;
                const char *pipe = strchr(semi, '|');
                size_t vlen = pipe ? (size_t)(pipe - semi) : strlen(semi);
                if (vlen >= sizeof(variables[num_variables].value))
                    vlen = sizeof(variables[num_variables].value) - 1;
                memcpy(variables[num_variables].value, semi, vlen);
                variables[num_variables].value[vlen] = '\0';
            }
            num_variables++;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto *var = (struct retro_variable *)data;
        var->value = NULL;
        for (unsigned i = 0; i < num_variables; i++) {
            if (strcmp(variables[i].key, var->key) == 0) {
                var->value = variables[i].value;
                return true;
            }
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = variables_updated;
        variables_updated = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        input_bitmasks_supported = true;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        auto *geom = (struct retro_game_geometry *)data;
        frame_width  = geom->base_width;
        frame_height = geom->base_height;
        if (frontend_cb.on_geometry_change)
            frontend_cb.on_geometry_change(frontend_cb.user,
                                           frame_width, frame_height);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK: {
        auto *iface = (const struct retro_get_proc_address_interface *)data;
        core_get_proc_address = iface->get_proc_address;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        return false;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
        auto *info = (const struct retro_controller_info *)data;
        num_controller_types = 0;
        if (info && info->types) {
            for (unsigned i = 0; i < info->num_types && num_controller_types < MAX_CONTROLLER_TYPES; i++) {
                auto &ct = controller_types[num_controller_types];
                strncpy(ct.desc, info->types[i].desc ? info->types[i].desc : "",
                        sizeof(ct.desc) - 1);
                ct.desc[sizeof(ct.desc) - 1] = '\0';
                ct.id = info->types[i].id;
                num_controller_types++;
            }
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}

static void core_video_refresh(const void *data, unsigned width,
                                unsigned height, size_t pitch) {
    if (!data) return;
    unsigned capped_w = width  < MAX_WIDTH  ? width  : MAX_WIDTH;
    unsigned capped_h = height < MAX_HEIGHT ? height : MAX_HEIGHT;
    frame_width  = capped_w;
    frame_height = capped_h;
    frame_pitch  = pitch;

    auto *src = (const uint8_t *)data;
    for (unsigned y = 0; y < capped_h; y++) {
        memcpy(&frame_buf[y * capped_w],
               src + y * pitch,
               capped_w * sizeof(uint32_t));
    }
}

static void core_audio_sample(int16_t left, int16_t right) {
    if (g_mute) return;

    audio_downsample_count++;
    if (audio_downsample_count < DOWNSAMPLE_RATIO) return;
    audio_downsample_count = 0;

    unsigned w = audio_ring_write;
    unsigned next = (w + 2) % AUDIO_RING_SIZE;
    if (next == audio_ring_read) return;
    audio_ring[w]     = left;
    audio_ring[w + 1] = right;
    audio_ring_write  = next;
}

static size_t core_audio_sample_batch(const int16_t *data, size_t frames) {
    for (size_t i = 0; i < frames; i++)
        core_audio_sample(data[i * 2], data[i * 2 + 1]);
    return frames;
}

static void core_input_poll(void) {
}

static int16_t core_input_state(unsigned port, unsigned device,
                                 unsigned index, unsigned id) {
    if (port != 0) return 0;

    if ((device & 0xFF) == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            int16_t mask = 0;
            for (int i = 0; i < 16; i++) {
                int16_t val = input_fixed[i] ? input_fixed_val[i] : input_state_val[i];
                if (val) mask |= (1 << i);
            }
            return mask;
        }
        if (id < 16)
            return input_fixed[id] ? input_fixed_val[id] : input_state_val[id];
    }

    if ((device & 0xFF) == RETRO_DEVICE_ANALOG && index <= 1 && id <= 1) {
        unsigned ai = index * 2 + id;
        return analog_fixed[ai] ? analog_fixed_val[ai] : analog_state_val[ai];
    }

    return 0;
}

/* ========================================================================
 * Retrodebug
 * ======================================================================== */

static uint64_t cpu_get_pc(rd_Cpu const *cpu) {
    int pc_reg = ar_reg_pc(cpu->v1.type);
    if (pc_reg < 0) return 0;
    return cpu->v1.get_register(cpu, (unsigned)pc_reg);
}

static bool debug_handle_event(void *user_data, rd_SubscriptionID sub_id,
                                rd_Event const *event) {
    (void)user_data;

    /* --- Update skip map: iterate all CPUs via system struct,
           clear entries where PC has moved past the skip address --- */
    rd_System const *sys = debugger_if_ptr ? debugger_if_ptr->v1.system : nullptr;
    if (sys) {
        for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
            rd_Cpu const *cpu = sys->v1.cpus[i];
            auto it = g_skip_addr.find(cpu);
            if (it != g_skip_addr.end() && cpu_get_pc(cpu) != it->second) {
                g_skip_addr.erase(it);
                auto ts = g_skip_temp_subs.find(cpu);
                if (ts != g_skip_temp_subs.end()) {
                    debugger_if_ptr->v1.unsubscribe(ts->second);
                    g_skip_temp_subs.erase(ts);
                }
            }
        }
    }

    /* Temp subs exist only for cleanup — they never pause */
    for (auto &[cpu, tsub] : g_skip_temp_subs) {
        if (sub_id == tsub) return false;
    }

    /* Trace logging (never halts).  Suppress at skip addresses to avoid
       double-logging the instruction where the previous step halted. */
    if (ar_trace_is_sub(sub_id)) {
        if (event->type == RD_EVENT_EXECUTION) {
            rd_Cpu const *event_cpu = event->execution.cpu;
            auto it = g_skip_addr.find(event_cpu);
            if (it != g_skip_addr.end() && cpu_get_pc(event_cpu) == it->second)
                return false;
        }
        ar_trace_on_event(sub_id, event);
        return false;
    }

    /* Determine if this event would pause (step hit or breakpoint) */
    bool is_step = (g_step_active && sub_id == g_step_sub_id);
    bool is_bp = ar_bp_sub_is_breakpoint(sub_id);
    if (!is_step && !is_bp) return false;

    /* Suppress pause if the event CPU's PC matches its skip address */
    if (event->type == RD_EVENT_EXECUTION) {
        rd_Cpu const *event_cpu = event->execution.cpu;
        auto it = g_skip_addr.find(event_cpu);
        if (it != g_skip_addr.end() && cpu_get_pc(event_cpu) == it->second)
            return false;
    }

    /* Apply side effects */
    if (is_step) g_step_complete = true;
    if (is_bp) {
        int bp_id = ar_bp_sub_to_id(sub_id);
        g_bp_hit_id = bp_id;
        if (event->type == RD_EVENT_EXECUTION)
            fprintf(stderr, "[arret] breakpoint %d hit at 0x%04lx (%s)\n",
                    bp_id, (unsigned long)event->execution.address,
                    event->can_halt ? "core halted" : "thread blocked");
        if (event->type == RD_EVENT_MEMORY)
            fprintf(stderr, "[arret] watchpoint %d hit at 0x%04lx (%s) (%s)\n",
                    bp_id, (unsigned long)event->memory.address,
                    (event->memory.operation & RD_MEMORY_WRITE) ? "write" : "read",
                    event->can_halt ? "core halted" : "thread blocked");

        /* Defer auto-delete of temporary breakpoints until after the
           frame completes.  Deleting inside the handler triggers
           sync_subscriptions() → rd_unsubscribe() → rd_recompute_sub_state()
           which can switch the core from debug mode to normal mode mid-frame,
           causing it to ignore the halt flag. */
        if (bp_id >= 0) {
            const ar_breakpoint *bp = ar_bp_get(bp_id);
            if (bp && bp->temporary)
                ar_bp_defer_delete(bp_id);
        }
    }

    if (event->can_halt) {
        /* Core can halt its run loop and return from retro_run() */
        return true;
    } else {
        /* Core can't halt — block this thread until frontend resumes */
        {
            std::lock_guard lock(g_core_mutex);
            g_core_state = CORE_BLOCKED;
        }
        g_core_cv.notify_all();

        std::unique_lock lock(g_block_mutex);
        g_block_cv.wait(lock, [] { return g_block_resume; });
        g_block_resume = false;
        return false;  /* core continues execution */
    }
}

alignas(rd_DebuggerIf) static char debugger_if_buf[sizeof(rd_DebuggerIf)];

static bool debug_init(void) {
    if (!core_get_proc_address) {
        fprintf(stderr, "[arret] warning: core does not provide get_proc_address\n");
        return false;
    }

    rd_set_debugger_fn = (rd_Set)core_get_proc_address("rd_set_debugger");
    if (!rd_set_debugger_fn) {
        fprintf(stderr, "[arret] warning: core does not provide rd_set_debugger\n");
        return false;
    }

    auto *storage = new (debugger_if_buf) rd_DebuggerIf{
        RD_API_VERSION,     /* frontend_api_version */
        0,                  /* core_api_version */
        {                   /* v1 */
            nullptr,              /* system */
            nullptr,              /* user_data */
            debug_handle_event,   /* handle_event */
            nullptr,              /* subscribe */
            nullptr,              /* unsubscribe */
        }
    };

    rd_set_debugger_fn(storage);
    debugger_if_ptr = storage;

    if (debugger_if_ptr->v1.system &&
        debugger_if_ptr->v1.system->v1.num_cpus > 0) {
        debug_cpu_ptr = debugger_if_ptr->v1.system->v1.cpus[0];
        debug_mem_ptr = debug_cpu_ptr->v1.memory_region;
        g_has_debug = true;
        fprintf(stderr, "[arret] retrodebug: cpu=%s mem=%s (0x%lx bytes)\n",
                debug_cpu_ptr->v1.id, debug_mem_ptr->v1.id,
                (unsigned long)debug_mem_ptr->v1.size);
    }
    return true;
}

/* ========================================================================
 * Core loading
 * ======================================================================== */

#define LOAD_SYM(S) do { \
    core.S = (decltype(core.S))dlsym(core.handle, #S); \
    if (!core.S) { \
        fprintf(stderr, "Failed to load symbol: %s: %s\n", #S, dlerror()); \
        dlclose(core.handle); \
        return false; \
    } \
} while(0)

static bool core_load(const char *path) {
    core.handle = dlopen(path, RTLD_LAZY);
    if (!core.handle) {
        fprintf(stderr, "Failed to load core: %s\n", dlerror());
        return false;
    }

    LOAD_SYM(retro_init);
    LOAD_SYM(retro_deinit);
    LOAD_SYM(retro_api_version);
    LOAD_SYM(retro_get_system_info);
    LOAD_SYM(retro_get_system_av_info);
    LOAD_SYM(retro_set_controller_port_device);
    LOAD_SYM(retro_reset);
    LOAD_SYM(retro_run);
    LOAD_SYM(retro_serialize_size);
    LOAD_SYM(retro_serialize);
    LOAD_SYM(retro_unserialize);
    LOAD_SYM(retro_load_game);
    LOAD_SYM(retro_unload_game);
    LOAD_SYM(retro_set_environment);
    LOAD_SYM(retro_set_video_refresh);
    LOAD_SYM(retro_set_audio_sample);
    LOAD_SYM(retro_set_audio_sample_batch);
    LOAD_SYM(retro_set_input_poll);
    LOAD_SYM(retro_set_input_state);

    return true;
}

/* ========================================================================
 * Save state management
 * ======================================================================== */

bool ar_save_state(int slot) {
    if (slot < 0 || slot >= MAX_SAVE_SLOTS) return false;
    size_t sz = core.retro_serialize_size();
    if (sz == 0) return false;

    void *buf = malloc(sz);
    if (!buf) return false;

    if (!core.retro_serialize(buf, sz)) { free(buf); return false; }

    char path[4352];
    snprintf(path, sizeof(path), "%s.%d.state", rom_base, slot);
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return false; }
    bool ok = fwrite(buf, 1, sz, f) == sz;
    fclose(f);
    free(buf);
    if (ok)
        fprintf(stderr, "[arret] Saved state to slot %d (%s)\n", slot, path);
    else
        fprintf(stderr, "[arret] Failed to save state slot %d\n", slot);
    return ok;
}

bool ar_serialize(void **out_buf, size_t *out_size) {
    size_t sz = core.retro_serialize_size();
    if (sz == 0) return false;
    void *buf = malloc(sz);
    if (!buf) return false;
    if (!core.retro_serialize(buf, sz)) { free(buf); return false; }
    *out_buf = buf;
    *out_size = sz;
    return true;
}

bool ar_load_state(int slot) {
    if (slot < 0 || slot >= MAX_SAVE_SLOTS) return false;

    char path[4352];
    snprintf(path, sizeof(path), "%s.%d.state", rom_base, slot);
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return false; }
    fclose(f);

    bool ok = core.retro_unserialize(buf, (size_t)sz);
    free(buf);
    if (ok) {
        memset(frame_buf, 0, sizeof(frame_buf));
        if (frontend_cb.on_video_refresh)
            frontend_cb.on_video_refresh(frontend_cb.user);
        fprintf(stderr, "[arret] Loaded state from slot %d (%s)\n", slot, path);
    } else
        fprintf(stderr, "[arret] Failed to load state slot %d\n", slot);
    return ok;
}

/* ========================================================================
 * Memory region lookup
 * ======================================================================== */

rd_Memory const *ar_find_memory_by_id(const char *id) {
    if (!g_has_debug) return NULL;
    rd_System const *sys = debugger_if_ptr->v1.system;

    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        rd_Memory const *m = sys->v1.cpus[i]->v1.memory_region;
        if (m && strcmp(m->v1.id, id) == 0) return m;
    }
    for (unsigned i = 0; i < sys->v1.num_memory_regions; i++) {
        if (strcmp(sys->v1.memory_regions[i]->v1.id, id) == 0)
            return sys->v1.memory_regions[i];
    }
    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        rd_Memory const *m = sys->v1.cpus[i]->v1.memory_region;
        if (!m || !m->v1.get_memory_map_count || !m->v1.get_memory_map) continue;
        unsigned count = m->v1.get_memory_map_count(m);
        auto *maps = (rd_MemoryMap *)malloc(count * sizeof(rd_MemoryMap));
        if (!maps) continue;
        m->v1.get_memory_map(m, maps);
        for (unsigned j = 0; j < count; j++) {
            if (maps[j].source && strcmp(maps[j].source->v1.id, id) == 0) {
                rd_Memory const *result = maps[j].source;
                free(maps);
                return result;
            }
        }
        free(maps);
    }
    return NULL;
}

/* ========================================================================
 * Public API: lifecycle
 * ======================================================================== */

void ar_setup(bool mute_flag, int port, const ar_frontend_cb *cb) {
    g_mute = mute_flag;
    listen_port_g = port;
    if (cb) frontend_cb = *cb;

    if (!json_out_saved)
        init_json_output();

    ar_cmd_server_init(listen_port_g);

    g_running = true;
}

bool ar_load_core(const char *core_path) {
    /* Unload previous core if any */
    if (g_core_loaded) {
        ar_debug_step_end();
        if (g_content_loaded) {
            core.retro_unload_game();
            g_content_loaded = false;
        }
        core.retro_deinit();
        dlclose(core.handle);
        g_core_loaded = false;
        g_has_debug = false;
        debug_cpu_ptr = NULL;
        debug_mem_ptr = NULL;
        debugger_if_ptr = NULL;
        rd_set_debugger_fn = NULL;
        core_get_proc_address = NULL;
        num_controller_types = 0;
    }

    if (!core_load(core_path)) return false;

    /* set_environment must be called before retro_init;
       the core calls SET_PROC_ADDRESS_CALLBACK during this. */
    core.retro_set_environment(core_environment);
    core.retro_set_video_refresh(core_video_refresh);
    core.retro_set_audio_sample(core_audio_sample);
    core.retro_set_audio_sample_batch(core_audio_sample_batch);
    core.retro_set_input_poll(core_input_poll);
    core.retro_set_input_state(core_input_state);

    core.retro_init();

    /* Try retrodebug init — non-fatal if core doesn't support it */
    if (!debug_init()) {
        fprintf(stderr, "[arret] warning: core has no retrodebug support; "
                "debug features will be unavailable\n");
    }

    core.retro_get_system_info(&sys_info);
    fprintf(stderr, "[arret] Core: %s %s\n",
            sys_info.library_name, sys_info.library_version);
    fprintf(stderr, "[arret] Extensions: %s, need_fullpath: %s\n",
            sys_info.valid_extensions,
            sys_info.need_fullpath ? "yes" : "no");

    g_core_loaded = true;
    return true;
}

bool ar_load_content(const char *rom_path) {
    if (!g_core_loaded) {
        fprintf(stderr, "[arret] error: no core loaded\n");
        return false;
    }

    /* Unload previous content */
    if (g_content_loaded) {
        core.retro_unload_game();
        g_content_loaded = false;
    }

    /* Save ROM path and derive rom_base / save_dir */
    snprintf(rom_path_saved, sizeof(rom_path_saved), "%s", rom_path);

    snprintf(rom_base, sizeof(rom_base), "%s", rom_path);
    char *dot = strrchr(rom_base, '.');
    char *sep = strrchr(rom_base, '/');
    if (dot && (!sep || dot > sep))
        *dot = '\0';

    snprintf(save_dir, sizeof(save_dir), "%s", rom_path);
    sep = strrchr(save_dir, '/');
    if (sep)
        *sep = '\0';
    else
        snprintf(save_dir, sizeof(save_dir), ".");

    struct retro_game_info game_info = {};
    game_info.path = rom_path;

    if (!core.retro_load_game(&game_info)) {
        fprintf(stderr, "Failed to load ROM: %s\n", rom_path);
        return false;
    }

    core.retro_get_system_av_info(&av_info);
    frame_width  = av_info.geometry.base_width;
    frame_height = av_info.geometry.base_height;
    fprintf(stderr, "[arret] Video: %ux%u @ %.2f fps\n",
            frame_width, frame_height, av_info.timing.fps);
    fprintf(stderr, "[arret] Audio: %.0f Hz\n", av_info.timing.sample_rate);

    g_content_loaded = true;
    return true;
}

bool ar_init(const char *core_path, const char *rom_path,
             bool mute_flag, int port, const ar_frontend_cb *cb) {
    ar_setup(mute_flag, port, cb);
    if (!ar_load_core(core_path)) return false;
    if (!ar_load_content(rom_path)) return false;
    return true;
}

void ar_shutdown(void) {
    ar_core_thread_stop();
    ar_debug_step_end();
    ar_search_free();
    ar_cmd_server_shutdown();
    if (g_content_loaded) { core.retro_unload_game(); g_content_loaded = false; }
    if (g_core_loaded) { core.retro_deinit(); g_core_loaded = false; }
    if (core.handle) { dlclose(core.handle); core.handle = NULL; }
    g_has_debug = false;
    debug_cpu_ptr = NULL;
    debug_mem_ptr = NULL;
    debugger_if_ptr = NULL;
    rd_set_debugger_fn = NULL;
    core_get_proc_address = NULL;
}

/* ======================================================================== */
/* Public API: per-frame                                                     */
/* ======================================================================== */

void ar_run_frame(void) {
    if (!g_content_loaded) return;
    if (g_core_thread.joinable()) {
        /* Threaded mode: signal and wait */
        ar_run_frame_async();
        std::unique_lock lock(g_core_mutex);
        g_core_cv.wait(lock, [] {
            return g_core_state == CORE_DONE || g_core_state == CORE_BLOCKED;
        });
        if (g_core_state == CORE_DONE) g_core_state = CORE_IDLE;
    } else {
        core.retro_run();
    }
}

/* ======================================================================== */
/* Public API: core thread                                                   */
/* ======================================================================== */

static void core_thread_func() {
    while (true) {
        std::unique_lock lock(g_core_mutex);
        g_core_cv.wait(lock, [] { return g_core_state == CORE_RUNNING || g_core_quit; });
        if (g_core_quit) break;
        lock.unlock();

        core.retro_run();

        lock.lock();
        if (g_core_state == CORE_RUNNING)
            g_core_state = CORE_DONE;
        lock.unlock();
        g_core_cv.notify_all();
    }
}

void ar_core_thread_start(void) {
    if (g_core_thread.joinable()) return;
    g_core_quit = false;
    g_core_state = CORE_IDLE;
    g_core_thread = std::thread(core_thread_func);
}

void ar_core_thread_stop(void) {
    if (!g_core_thread.joinable()) return;
    {
        std::lock_guard lock(g_core_mutex);
        g_core_quit = true;
    }
    g_core_cv.notify_all();
    /* Also wake any blocked handler */
    {
        std::lock_guard lock(g_block_mutex);
        g_block_resume = true;
    }
    g_block_cv.notify_all();
    g_core_thread.join();
    g_core_state = CORE_IDLE;
}

bool ar_run_frame_async(void) {
    if (!g_content_loaded || !g_core_thread.joinable()) return false;
    {
        std::lock_guard lock(g_core_mutex);
        if (g_core_state != CORE_IDLE) return false;
        g_core_state = CORE_RUNNING;
    }
    g_core_cv.notify_all();
    return true;
}

int ar_core_state(void) {
    std::lock_guard lock(g_core_mutex);
    return (int)g_core_state;
}

void ar_core_ack_done(void) {
    std::lock_guard lock(g_core_mutex);
    if (g_core_state == CORE_DONE)
        g_core_state = CORE_IDLE;
}

void ar_core_resume_blocked(void) {
    {
        std::lock_guard lock(g_core_mutex);
        if (g_core_state == CORE_BLOCKED)
            g_core_state = CORE_RUNNING;
    }
    {
        std::lock_guard lock(g_block_mutex);
        g_block_resume = true;
    }
    g_block_cv.notify_all();
}

bool ar_core_blocked(void) {
    std::lock_guard lock(g_core_mutex);
    return g_core_state == CORE_BLOCKED;
}


/* ======================================================================== */
/* Public API: state access                                                  */
/* ======================================================================== */

const uint32_t *ar_frame_buf(void)    { return frame_buf; }
unsigned ar_frame_width(void)         { return frame_width; }
unsigned ar_frame_height(void)        { return frame_height; }
const struct retro_system_av_info *ar_av_info(void)  { return &av_info; }
const struct retro_system_info    *ar_sys_info(void)  { return &sys_info; }

/* ======================================================================== */
/* Public API: input                                                         */
/* ======================================================================== */

void ar_set_input(unsigned id, int16_t value) {
    if (id < 16) input_state_val[id] = value;
}

void ar_set_manual_input(bool on) { g_manual_input = on; }
bool ar_manual_input(void)        { return g_manual_input; }

/* Input fix layer */
void ar_input_fix(unsigned id, int16_t value) {
    if (id < 16) { input_fixed[id] = true; input_fixed_val[id] = value; }
}
void ar_input_unfix(unsigned id) {
    if (id < 16) { input_fixed[id] = false; input_fixed_val[id] = 0; }
}
void ar_input_unfix_all(void) {
    memset(input_fixed, 0, sizeof(input_fixed));
    memset(input_fixed_val, 0, sizeof(input_fixed_val));
    memset(analog_fixed, 0, sizeof(analog_fixed));
    memset(analog_fixed_val, 0, sizeof(analog_fixed_val));
}
bool ar_input_is_fixed(unsigned id) {
    return id < 16 && input_fixed[id];
}
int16_t ar_input_fixed_value(unsigned id) {
    return id < 16 ? input_fixed_val[id] : 0;
}

/* Analog input */
void ar_set_analog(unsigned index, unsigned axis, int16_t value) {
    if (index <= 1 && axis <= 1) analog_state_val[index * 2 + axis] = value;
}
void ar_analog_fix(unsigned index, unsigned axis, int16_t value) {
    if (index <= 1 && axis <= 1) {
        unsigned ai = index * 2 + axis;
        analog_fixed[ai] = true;
        analog_fixed_val[ai] = value;
    }
}
void ar_analog_unfix(unsigned index, unsigned axis) {
    if (index <= 1 && axis <= 1) {
        unsigned ai = index * 2 + axis;
        analog_fixed[ai] = false;
        analog_fixed_val[ai] = 0;
    }
}
bool ar_analog_is_fixed(unsigned index, unsigned axis) {
    return index <= 1 && axis <= 1 && analog_fixed[index * 2 + axis];
}
int16_t ar_analog_fixed_value(unsigned index, unsigned axis) {
    return (index <= 1 && axis <= 1) ? analog_fixed_val[index * 2 + axis] : 0;
}

/* Controller info */
bool ar_controller_has_analog(void) {
    for (unsigned i = 0; i < num_controller_types; i++) {
        if ((controller_types[i].id & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG)
            return true;
    }
    return false;
}

/* ======================================================================== */
/* Public API: audio                                                         */
/* ======================================================================== */

unsigned ar_audio_read(int16_t *out, unsigned max_frames) {
    unsigned count = 0;
    for (unsigned i = 0; i < max_frames; i++) {
        unsigned r = audio_ring_read;
        if (r == audio_ring_write) break;
        out[i * 2]     = audio_ring[r];
        out[i * 2 + 1] = audio_ring[r + 1];
        audio_ring_read = (r + 2) % AUDIO_RING_SIZE;
        count++;
    }
    return count;
}

void ar_set_mute(bool muted) { g_mute = muted; }
bool ar_is_mute(void)        { return g_mute; }

/* ======================================================================== */
/* Public API: debug                                                         */
/* ======================================================================== */

bool             ar_has_debug(void)   { return g_has_debug; }
rd_DebuggerIf   *ar_get_debugger_if(void) { return debugger_if_ptr; }
rd_Cpu const    *ar_debug_cpu(void)   { return debug_cpu_ptr; }
rd_Memory const *ar_debug_mem(void)   { return debug_mem_ptr; }

rd_System const *ar_debug_system(void) {
    if (!g_has_debug || !debugger_if_ptr) return NULL;
    return debugger_if_ptr->v1.system;
}

bool ar_debug_step_begin(int type) {
    if (!g_has_debug || !debugger_if_ptr->v1.subscribe || g_step_active)
        return false;

    rd_ExecutionType rd_type;
    switch (type) {
    case AR_STEP_IN:   rd_type = RD_STEP; break;
    case AR_STEP_OVER: rd_type = RD_STEP_CURRENT_SUBROUTINE; break;
    case AR_STEP_OUT:  rd_type = RD_STEP_OUT; break;
    default: return false;
    }

    rd_Subscription sub{};
    sub.type = RD_EVENT_EXECUTION;
    sub.execution.cpu = debug_cpu_ptr;
    sub.execution.type = rd_type;
    sub.execution.address_range_begin = 0;
    sub.execution.address_range_end = UINT64_MAX;

    rd_SubscriptionID id = debugger_if_ptr->v1.subscribe(&sub);
    if (id < 0) return false;

    g_step_sub_id = id;
    g_step_complete = false;
    g_step_active = true;

    /* Skip the first fire at the current PC (broad subscriptions only,
       STEP_OUT is exempt since it only fires after depth goes negative) */
    if (rd_type != RD_STEP_OUT)
        ar_debug_set_skip();

    return true;
}

bool ar_debug_step_complete(void) {
    return g_step_complete;
}

void ar_debug_step_end(void) {
    if (g_step_active && debugger_if_ptr && debugger_if_ptr->v1.unsubscribe)
        debugger_if_ptr->v1.unsubscribe(g_step_sub_id);
    g_step_active = false;
    g_step_complete = false;
    g_step_sub_id = -1;
}

void ar_debug_step_reset(void) {
    g_step_complete = false;
}

int  ar_bp_hit(void) { return g_bp_hit_id; }
void ar_bp_ack_hit(void) { g_bp_hit_id = -1; }

uint64_t ar_debug_pc(void) {
    if (!g_has_debug || !debug_cpu_ptr) return 0;
    return cpu_get_pc(debug_cpu_ptr);
}

void ar_debug_set_skip(void) {
    if (!g_has_debug || !debugger_if_ptr) return;
    rd_System const *sys = debugger_if_ptr->v1.system;
    if (!sys) return;

    /* Clean up any existing temp subs */
    for (auto &[cpu, tsub] : g_skip_temp_subs) {
        if (debugger_if_ptr->v1.unsubscribe)
            debugger_if_ptr->v1.unsubscribe(tsub);
    }
    g_skip_temp_subs.clear();
    g_skip_addr.clear();

    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        rd_Cpu const *cpu = sys->v1.cpus[i];
        uint64_t pc = cpu_get_pc(cpu);
        g_skip_addr[cpu] = pc;

        /* Temp broad step sub: ensures the skip entry gets cleared
           as soon as the CPU advances past the skip address */
        if (debugger_if_ptr->v1.subscribe) {
            rd_Subscription sub{};
            sub.type = RD_EVENT_EXECUTION;
            sub.execution.cpu = cpu;
            sub.execution.type = RD_STEP;
            sub.execution.address_range_begin = 0;
            sub.execution.address_range_end = UINT64_MAX;

            rd_SubscriptionID sid = debugger_if_ptr->v1.subscribe(&sub);
            if (sid >= 0)
                g_skip_temp_subs[cpu] = sid;
        }
    }
}

/* ======================================================================== */
/* Public API: run control                                                   */
/* ======================================================================== */

bool ar_running(void)        { return g_running; }
void ar_set_running(bool r)  { g_running = r; }
void ar_reset(void)          { if (g_content_loaded) core.retro_reset(); }
const ar_frontend_cb *ar_get_frontend_cb(void) { return &frontend_cb; }
bool ar_core_loaded(void)    { return g_core_loaded; }
bool ar_content_loaded(void) { return g_content_loaded; }
const char *ar_rompath_base(void) { return rom_base; }

/* ======================================================================== */
/* Public API: ROM reload                                                    */
/* ======================================================================== */

bool ar_reload_rom(void) {
    core.retro_unload_game();

    struct retro_game_info game_info = {};
    game_info.path = rom_path_saved;

    if (!core.retro_load_game(&game_info)) {
        fprintf(stderr, "Failed to reload ROM: %s\n", rom_path_saved);
        return false;
    }

    core.retro_get_system_av_info(&av_info);
    frame_width  = av_info.geometry.base_width;
    frame_height = av_info.geometry.base_height;
    return true;
}

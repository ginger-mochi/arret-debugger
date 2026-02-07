/*
 * backend.h: Arrêt Debugger backend API
 *
 * C API for the shared backend used by all frontends (SDL, Qt, etc.).
 * The backend handles core loading, libretro callbacks, command processing,
 * save/load, debug, and the TCP socket.  Frontends implement the
 * ar_frontend_cb callbacks and provide their own main loop / UI.
 */

#ifndef AR_BACKEND_H
#define AR_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "libretro.h"
#include "retrodebug.h"
#include "breakpoint.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* Frontend callback struct                                                  */
/* ======================================================================== */

typedef struct ar_frontend_cb {
    /* Called after each frame is rendered into frame_buf. */
    void (*on_video_refresh)(void *user);

    /* Called when core changes geometry (SET_GEOMETRY). */
    void (*on_geometry_change)(void *user, unsigned w, unsigned h);

    /* Return millisecond tick counter (for frame pacing in "run N"). */
    uint32_t (*get_ticks_ms)(void *user);

    /* Sleep for ms milliseconds (for frame pacing in "run N"). */
    void (*delay_ms)(void *user, uint32_t ms);

    /* Pump UI events during "run N" loops. */
    void (*poll_events)(void *user);

    /*
     * Handle a frontend-specific command.
     * Return true if the command was handled (response written to out).
     * Return false to let the backend return "unknown command".
     */
    bool (*handle_command)(void *user, const char *cmd,
                           const char *line, FILE *out);

    /* Opaque pointer passed to all callbacks. */
    void *user;
} ar_frontend_cb;

/* ======================================================================== */
/* Lifecycle                                                                 */
/* ======================================================================== */

/*
 * Set up backend globals, redirect stdout, start TCP socket.
 * Does NOT load a core or content.  Use ar_load_core / ar_load_content
 * afterwards, or call ar_init() to do everything in one shot.
 */
void ar_setup(bool mute, int port, const ar_frontend_cb *cb);

/*
 * Load (or replace) a libretro core.
 * Requires get_proc_address and rd_set_debugger; errors if missing.
 */
bool ar_load_core(const char *core_path);

/* Load (or replace) content into the current core. */
bool ar_load_content(const char *rom_path);

/*
 * Convenience: ar_setup + ar_load_core + ar_load_content in one call.
 * Returns true on success.
 */
bool ar_init(const char *core_path, const char *rom_path,
             bool mute, int port, const ar_frontend_cb *cb);

/* Shut down the core and close the TCP socket. */
void ar_shutdown(void);

/* Query whether core / content are currently loaded. */
bool ar_core_loaded(void);
bool ar_content_loaded(void);
const char *ar_rompath_base(void);

/*
 * TCP command server: init, poll, shutdown.
 * ar_setup() calls ar_cmd_server_init() automatically.
 */
int  ar_cmd_server_init(int port);
void ar_cmd_server_shutdown(void);

/*
 * Client mode: connect to a running instance, send cmd_str, print response.
 * Returns process exit code (0 on success).
 */
int ar_cmd_client(const char *cmd_str, int port);

/* ======================================================================== */
/* Per-frame                                                                 */
/* ======================================================================== */

/* Run one emulated frame (synchronous; works in both threaded and non-threaded modes). */
void ar_run_frame(void);

/* ---- Core thread (for Qt / async frontends) ---- */

void ar_core_thread_start(void);      /* Spawn the core thread */
void ar_core_thread_stop(void);       /* Stop and join the core thread */
bool ar_run_frame_async(void);        /* Signal core thread to run one frame */
int  ar_core_state(void);             /* 0=idle, 1=running, 2=blocked, 3=done */
void ar_core_ack_done(void);          /* Transition DONE -> IDLE */
void ar_core_resume_blocked(void);    /* Resume from BLOCKED (unblock handler) */
bool ar_core_blocked(void);           /* Convenience: state == BLOCKED */

/* Poll TCP socket and process any pending commands. */
void ar_check_socket_commands(void);

/* ======================================================================== */
/* State access                                                              */
/* ======================================================================== */

const uint32_t         *ar_frame_buf(void);
unsigned                ar_frame_width(void);
unsigned                ar_frame_height(void);
const struct retro_system_av_info *ar_av_info(void);
const struct retro_system_info    *ar_sys_info(void);

/* ======================================================================== */
/* Input                                                                     */
/* ======================================================================== */

void    ar_set_input(unsigned id, int16_t value);
void    ar_set_manual_input(bool on);
bool    ar_manual_input(void);

/* ======================================================================== */
/* Audio (pull model)                                                        */
/* ======================================================================== */

/*
 * Read up to max_frames stereo frames from the audio ring buffer.
 * Each frame is 2 x int16_t (left, right).
 * Returns the number of frames actually read.
 */
unsigned ar_audio_read(int16_t *out, unsigned max_frames);

void ar_set_mute(bool muted);
bool ar_is_mute(void);

/* ======================================================================== */
/* Commands                                                                  */
/* ======================================================================== */

/*
 * Process a single command line, writing the JSON response to resp_out.
 * The line is modified in-place (trailing whitespace stripped, strtok).
 */
void ar_process_command(char *line, FILE *resp_out);

/* ======================================================================== */
/* Save / Load                                                               */
/* ======================================================================== */

bool ar_save_state(int slot);
bool ar_load_state(int slot);

/* ======================================================================== */
/* Debug                                                                     */
/* ======================================================================== */

bool               ar_has_debug(void);
rd_DebuggerIf     *ar_get_debugger_if(void);
rd_Cpu const      *ar_debug_cpu(void);
rd_Memory const   *ar_debug_mem(void);
rd_Memory const   *ar_find_memory_by_id(const char *id);
rd_System const   *ar_debug_system(void);

/* Stepping */
#define AR_STEP_IN   0
#define AR_STEP_OVER 1
#define AR_STEP_OUT  2

bool     ar_debug_step_begin(int type);   /* Subscribe for step event */
bool     ar_debug_step_complete(void);    /* Poll: did handle_event fire? */
void     ar_debug_step_end(void);         /* Unsubscribe, clean up */
void     ar_debug_step_reset(void);      /* Reset complete flag (reuse subscription) */
uint64_t ar_debug_pc(void);              /* Get PC for current CPU type */
void     ar_debug_set_skip(void);        /* Set skip address for all CPUs (call before resuming) */

/* ======================================================================== */
/* Run control                                                               */
/* ======================================================================== */

bool ar_running(void);
void ar_set_running(bool r);
void ar_reset(void);
const ar_frontend_cb *ar_get_frontend_cb(void);

/* ======================================================================== */
/* ROM reload                                                                */
/* ======================================================================== */

/* Reload the current ROM (unload + load). Returns true on success. */
bool ar_reload_rom(void);

/* ======================================================================== */
/* Memory search (cheat finder)                                              */
/* ======================================================================== */

typedef enum {
    AR_SEARCH_EQ,        /* == */
    AR_SEARCH_NE,        /* != */
    AR_SEARCH_LT,        /* <  */
    AR_SEARCH_GT,        /* >  */
    AR_SEARCH_LE,        /* <= */
    AR_SEARCH_GE,        /* >= */
} ar_search_op;

/* Pass as value to ar_search_filter to compare against previous snapshot. */
#define AR_SEARCH_VS_PREV ((uint64_t)-1)

typedef struct {
    uint64_t addr;
    uint64_t value;
    uint64_t prev;
} ar_search_result;

/* Reset search on a memory region. data_size: 1/2/4. alignment: 1/2/4. */
bool     ar_search_reset(const char *region_id, int data_size, int alignment);

/* Filter candidates. If value == AR_SEARCH_VS_PREV, compares against previous. */
uint64_t ar_search_filter(ar_search_op op, uint64_t value);

/* Fill out[] with up to max results. Returns count written. */
unsigned ar_search_results(ar_search_result *out, unsigned max);

/* Return current candidate count. */
uint64_t ar_search_count(void);

/* Return true if a search session is active. */
bool     ar_search_active(void);

/* Free all search state. */
void     ar_search_free(void);

#ifdef __cplusplus
}
#endif

#endif /* AR_BACKEND_H */

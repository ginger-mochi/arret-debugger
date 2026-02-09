/*
 * cmd.cpp: TCP command server, client, and command processing
 *
 * Server: listens on a TCP port, accepts one-shot connections,
 *         reads a command line, passes it to ar_process_command,
 *         writes the JSON response, and closes the connection.
 *
 * Client: connects to a running instance, sends one command,
 *         prints the JSON response, and exits.
 *
 * Command processing: parses command lines and dispatches to
 * backend API functions, writing JSON responses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "backend.hpp"
#include "arch.hpp"
#include "registers.hpp"
#include "symbols.hpp"
#include "trace.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#define CMD_BUF_SIZE 4096

/* ========================================================================
 * TCP command server
 * ======================================================================== */

static int listen_fd = -1;

int ar_cmd_server_init(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[arret] socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[arret] bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        perror("[arret] listen");
        close(fd);
        return -1;
    }

    fprintf(stderr, "[arret] Listening on port %d\n", port);
    listen_fd = fd;
    return fd;
}

void ar_check_socket_commands(void) {
    if (listen_fd < 0) return;

    struct pollfd pfd = {};
    pfd.fd = listen_fd;
    pfd.events = POLLIN;
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) return;

        /* Set read timeout so a stuck client can't block the main loop */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char cmd_buf[CMD_BUF_SIZE];
        size_t pos = 0;
        while (pos < CMD_BUF_SIZE - 1) {
            ssize_t n = read(client_fd, &cmd_buf[pos], 1);
            if (n <= 0) break;
            if (cmd_buf[pos] == '\n') break;
            pos++;
        }
        cmd_buf[pos] = '\0';

        FILE *client_file = fdopen(dup(client_fd), "w");
        if (client_file) {
            ar_process_command(cmd_buf, client_file);
            fflush(client_file);
            fclose(client_file);
        }

        close(client_fd);
    }
}

void ar_cmd_server_shutdown(void) {
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}

/* ========================================================================
 * TCP command client
 * ======================================================================== */

int ar_cmd_client(const char *cmd_str, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    dprintf(fd, "%s\n", cmd_str);

    char buf[CMD_BUF_SIZE];
    size_t pos = 0;
    while (pos < sizeof(buf) - 1) {
        ssize_t n = read(fd, &buf[pos], sizeof(buf) - 1 - pos);
        if (n <= 0) break;
        pos += (size_t)n;
    }
    buf[pos] = '\0';
    close(fd);

    /* Strip trailing newline(s) for clean output */
    while (pos > 0 && buf[pos - 1] == '\n') buf[--pos] = '\0';
    printf("%s\n", buf);
    return 0;
}

/* ========================================================================
 * Utility: JSON output (to a FILE*)
 * ======================================================================== */

static void json_ok_f(FILE *out, const char *fmt, ...) {
    va_list ap;
    fprintf(out, "{\"ok\":true");
    if (fmt) {
        fprintf(out, ",");
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
    }
    fprintf(out, "}\n");
    fflush(out);
}

static void json_error_f(FILE *out, const char *fmt, ...) {
    va_list ap;
    fprintf(out, "{\"ok\":false,\"error\":\"");
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fprintf(out, "\"}\n");
    fflush(out);
}

/* ========================================================================
 * Button name mapping
 * ======================================================================== */

static int button_id_from_name(const char *name) {
    static const struct { const char *name; int id; } map[] = {
        {"b",      RETRO_DEVICE_ID_JOYPAD_B},
        {"y",      RETRO_DEVICE_ID_JOYPAD_Y},
        {"select", RETRO_DEVICE_ID_JOYPAD_SELECT},
        {"start",  RETRO_DEVICE_ID_JOYPAD_START},
        {"up",     RETRO_DEVICE_ID_JOYPAD_UP},
        {"down",   RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"left",   RETRO_DEVICE_ID_JOYPAD_LEFT},
        {"right",  RETRO_DEVICE_ID_JOYPAD_RIGHT},
        {"a",      RETRO_DEVICE_ID_JOYPAD_A},
        {"x",      RETRO_DEVICE_ID_JOYPAD_X},
        {"l",      RETRO_DEVICE_ID_JOYPAD_L},
        {"r",      RETRO_DEVICE_ID_JOYPAD_R},
        {"l2",     RETRO_DEVICE_ID_JOYPAD_L2},
        {"r2",     RETRO_DEVICE_ID_JOYPAD_R2},
        {"l3",     RETRO_DEVICE_ID_JOYPAD_L3},
        {"r3",     RETRO_DEVICE_ID_JOYPAD_R3},
        {NULL, -1}
    };
    for (int i = 0; map[i].name; i++) {
        if (strcasecmp(name, map[i].name) == 0)
            return map[i].id;
    }
    return -1;
}


/* ========================================================================
 * Hex dump
 * ======================================================================== */

static void do_dump(rd_Memory const *mem, uint64_t start, uint64_t size,
                    FILE *out) {
    uint64_t end = start + size;

    bool has_mmap = mem->v1.get_memory_map_count && mem->v1.get_memory_map;
    unsigned mmap_count = 0;
    rd_MemoryMap *maps = NULL;
    if (has_mmap) {
        mmap_count = mem->v1.get_memory_map_count(mem);
        if (mmap_count > 0) {
            maps = (rd_MemoryMap *)malloc(mmap_count * sizeof(rd_MemoryMap));
            if (maps) mem->v1.get_memory_map(mem, maps);
            else has_mmap = false;
        } else {
            has_mmap = false;
        }
    }

    int bank_width = 0;
    if (has_mmap) {
        int64_t max_bank = 0;
        for (unsigned i = 0; i < mmap_count; i++)
            if (maps[i].bank > max_bank) max_bank = maps[i].bank;
        bank_width = 1;
        for (int64_t v = max_bank; v >= 10; v /= 10) bank_width++;
    }

    uint64_t max_addr = end > 0 ? end - 1 : 0;
    int addr_width = 1;
    for (uint64_t v = max_addr; v >= 16; v /= 16) addr_width++;

    bool first_line = true;

    for (uint64_t addr = start; addr < end; addr++) {
        bool new_line = false;

        if (addr == start) {
            new_line = true;
        } else if (addr % 16 == 0) {
            new_line = true;
        } else if (has_mmap) {
            for (unsigned i = 0; i < mmap_count; i++) {
                if (maps[i].base_addr == addr) { new_line = true; break; }
            }
        }

        if (new_line) {
            if (!first_line) fputc('\n', out);
            first_line = false;

            int64_t bank = -1;
            if (has_mmap) {
                for (unsigned i = 0; i < mmap_count; i++) {
                    if (addr >= maps[i].base_addr &&
                        addr < maps[i].base_addr + maps[i].size) {
                        bank = maps[i].bank;
                        break;
                    }
                }
            }

            if (has_mmap) {
                if (bank >= 0)
                    fprintf(out, "%*ld:", bank_width, (long)bank);
                else
                    fprintf(out, "%*s:", bank_width, "");
            }
            fprintf(out, "%0*lX:", addr_width, (unsigned long)addr);

            int pad = 1 + (int)(addr % 16) * 3;
            for (int i = 0; i < pad; i++) fputc(' ', out);
        }

        fprintf(out, "%02X", mem->v1.peek(mem, addr, false));

        uint64_t next = addr + 1;
        if (next < end) {
            bool next_nl = (next % 16 == 0);
            if (!next_nl && has_mmap) {
                for (unsigned i = 0; i < mmap_count; i++) {
                    if (maps[i].base_addr == next) { next_nl = true; break; }
                }
            }
            if (!next_nl) fputc(' ', out);
        }
    }

    if (!first_line) fputc('\n', out);
    fflush(out);
    free(maps);
}

/* ========================================================================
 * Address marker resolution for disassembly output
 *
 * The disassembler encodes '@' after '$' to mark address operands:
 *   "$@0150" means the hex digits after @ are a memory address.
 * Consumers strip '@' and optionally resolve the address to a symbol.
 * ======================================================================== */

static std::string resolve_addr_markers(const std::string &text,
                                         const char *mem_id) {
    std::string result;
    const char *p = text.c_str();
    while (*p) {
        if (*p == '@') {
            const char *h = p + 1;
            while ((*h >= '0' && *h <= '9') ||
                   (*h >= 'A' && *h <= 'F') ||
                   (*h >= 'a' && *h <= 'f'))
                h++;
            if (h > p + 1) {
                std::string hexStr(p + 1, h);
                result += hexStr;
                uint64_t addr = strtoull(hexStr.c_str(), nullptr, 16);
                if (mem_id) {
                    auto resolved = ar_sym_resolve(mem_id, addr);
                    if (resolved) {
                        const char *label = ar_sym_get_label(resolved->region_id.c_str(), resolved->addr);
                        if (label) {
                            result += '[';
                            result += label;
                            result += ']';
                        }
                    }
                }
                p = h;
            } else {
                result += *p++;
            }
        } else {
            result += *p++;
        }
    }
    return result;
}

/* ========================================================================
 * Command processing
 * ======================================================================== */

void ar_process_command(char *line, FILE *out) {
    /* Strip trailing newline/whitespace */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' '))
        line[--len] = '\0';

    if (len == 0) return;

    char cmd[64] = {0};
    char arg1[256] = {0};
    char arg2[256] = {0};
    char rest[CMD_BUF_SIZE] = {0};
    int nargs = sscanf(line, "%63s %255s %255s %[^\n]", cmd, arg1, arg2, rest);

    /* --- quit --- */
    if (strcmp(cmd, "quit") == 0) {
        json_ok_f(out, NULL);
        ar_set_running(false);
        return;
    }

    /* --- info --- */
    if (strcmp(cmd, "info") == 0) {
        const struct retro_system_info *si = ar_sys_info();
        const struct retro_system_av_info *ai = ar_av_info();
        fprintf(out, "{\"ok\":true,\"core\":\"%s\",\"version\":\"%s\","
               "\"width\":%u,\"height\":%u,\"fps\":%.2f,"
               "\"sample_rate\":%.0f,\"debug\":%s}\n",
               si->library_name, si->library_version,
               ai->geometry.base_width, ai->geometry.base_height,
               ai->timing.fps, ai->timing.sample_rate,
               ar_has_debug() ? "true" : "false");
        fflush(out);
        return;
    }

    /* --- content --- */
    if (strcmp(cmd, "content") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }
        if (!ar_content_loaded()) { json_error_f(out, "no content loaded"); return; }
        rd_System const *sys = ar_debug_system();
        if (!sys || !sys->v1.get_content_info) {
            json_error_f(out, "core does not support content info");
            return;
        }
        int len = sys->v1.get_content_info(NULL, 0);
        if (len <= 0) {
            json_error_f(out, "no content info available");
            return;
        }
        char *buf = (char *)malloc(len + 1);
        sys->v1.get_content_info(buf, len + 1);
        /* Escape the string for JSON (newlines → \n, quotes, backslashes) */
        fprintf(out, "{\"ok\":true,\"info\":\"");
        for (int i = 0; buf[i]; i++) {
            if (buf[i] == '\n') fprintf(out, "\\n");
            else if (buf[i] == '"') fprintf(out, "\\\"");
            else if (buf[i] == '\\') fprintf(out, "\\\\");
            else fputc(buf[i], out);
        }
        fprintf(out, "\"}\n");
        fflush(out);
        free(buf);
        return;
    }

    /* --- run [N] --- */
    if (strcmp(cmd, "run") == 0) {
        int n = 1;
        if (nargs >= 2) n = atoi(arg1);
        if (n < 1) n = 1;
        if (n > 10000) n = 10000;

        /* Ensure core thread is running so that thread-blocking debug
           events don't deadlock this (TCP handler) thread. */
        ar_core_thread_start();

        /* Auto-resume if the core thread is blocked from a previous hit */
        if (ar_core_blocked()) {
            ar_debug_set_skip();
            ar_bp_ack_hit();
            ar_core_resume_blocked();
            /* Wait for the interrupted frame to finish */
            while (ar_core_state() != 0 /* IDLE */ &&
                   ar_core_state() != 3 /* DONE */)
                usleep(100);
            if (ar_core_state() == 3)
                ar_core_ack_done();
        }

        const ar_frontend_cb *fcb = ar_get_frontend_cb();
        uint32_t frame_ms = 0;
        if (fcb->get_ticks_ms)
            frame_ms = (uint32_t)(1000.0 / ar_av_info()->timing.fps);
        int actual = 0;
        bool was_blocked = false;
        for (int i = 0; i < n; i++) {
            uint32_t t0 = 0;
            if (fcb->get_ticks_ms)
                t0 = fcb->get_ticks_ms(fcb->user);

            if (!ar_run_frame_async()) {
                /* Core busy or not loaded */
                usleep(1000);
                i--;
                continue;
            }

            /* Poll for frame completion or thread block */
            while (true) {
                int state = ar_core_state();
                if (state == 3 /* DONE */) {
                    ar_core_ack_done();
                    break;
                }
                if (state == 2 /* BLOCKED */) {
                    was_blocked = true;
                    break;
                }
                usleep(100);
            }
            actual++;

            if (was_blocked || ar_bp_hit() >= 0)
                break;

            if (fcb->on_video_refresh)
                fcb->on_video_refresh(fcb->user);
            if (fcb->poll_events)
                fcb->poll_events(fcb->user);
            if (fcb->get_ticks_ms && fcb->delay_ms) {
                uint32_t elapsed = fcb->get_ticks_ms(fcb->user) - t0;
                if (elapsed < frame_ms)
                    fcb->delay_ms(fcb->user, frame_ms - elapsed);
            }
        }
        ar_bp_flush_deferred();
        int bp = ar_bp_hit();
        if (bp >= 0) {
            ar_bp_ack_hit();
            json_ok_f(out, "\"frames\":%d,\"breakpoint\":%d%s",
                     actual, bp, was_blocked ? ",\"blocked\":true" : "");
        } else {
            json_ok_f(out, "\"frames\":%d", actual);
        }
        return;
    }

    /* --- s / so / sout (step in / step over / step out) --- */
    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "so") == 0 ||
        strcmp(cmd, "sout") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }
        if (!ar_content_loaded()) { json_error_f(out, "no content loaded"); return; }

        int type;
        if (strcmp(cmd, "so") == 0)        type = AR_STEP_OVER;
        else if (strcmp(cmd, "sout") == 0)  type = AR_STEP_OUT;
        else                                type = AR_STEP_IN;

        ar_core_thread_start();

        /* Resume from blocked state if needed */
        bool was_blocked = ar_core_blocked();
        if (was_blocked) {
            ar_debug_set_skip();
            ar_bp_ack_hit();
            ar_core_resume_blocked();
            while (ar_core_state() != 0 && ar_core_state() != 3)
                usleep(100);
            if (ar_core_state() == 3)
                ar_core_ack_done();
        }

        if (!ar_debug_step_begin(type)) {
            json_error_f(out, "step subscribe failed");
            return;
        }

        /* Run frames until step completes or breakpoint hits */
        int frames = 0;
        for (int i = 0; i < 10000; i++) {
            if (!ar_run_frame_async()) {
                usleep(1000);
                i--;
                continue;
            }

            while (true) {
                int state = ar_core_state();
                if (state == 3 /* DONE */) {
                    ar_core_ack_done();
                    break;
                }
                if (state == 2 /* BLOCKED */) break;
                usleep(100);
            }
            frames++;

            if (ar_debug_step_complete()) break;
            if (ar_bp_hit() >= 0) break;
            if (ar_core_blocked()) break;
        }

        ar_debug_step_end();
        ar_bp_flush_deferred();

        int bp = ar_bp_hit();
        if (bp >= 0) {
            ar_bp_ack_hit();
            json_ok_f(out, "\"frames\":%d,\"breakpoint\":%d", frames, bp);
        } else {
            json_ok_f(out, "\"frames\":%d", frames);
        }
        return;
    }

    /* --- input <button> <0|1> --- */
    if (strcmp(cmd, "input") == 0) {
        if (nargs < 3) { json_error_f(out, "usage: input <button> <0|1>"); return; }
        int id = button_id_from_name(arg1);
        if (id < 0) { json_error_f(out, "unknown button: %s", arg1); return; }
        ar_input_unfix((unsigned)id);
        ar_set_input((unsigned)id, (int16_t)atoi(arg2));
        json_ok_f(out, NULL);
        return;
    }

    /* --- peek <addr> [len] --- */
    if (strcmp(cmd, "peek") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }
        if (nargs < 2) { json_error_f(out, "usage: peek <addr> [len]"); return; }
        uint64_t addr = (uint64_t)strtoul(arg1, NULL, 0);
        unsigned plen = 1;
        if (nargs >= 3) plen = (unsigned)strtoul(arg2, NULL, 0);
        if (plen < 1) plen = 1;
        if (plen > 256) plen = 256;

        fprintf(out, "{\"ok\":true,\"addr\":\"0x%04lx\",\"data\":[",
               (unsigned long)addr);
        for (unsigned i = 0; i < plen; i++) {
            if (i > 0) fprintf(out, ",");
            fprintf(out, "%u", ar_debug_mem()->v1.peek(ar_debug_mem(), addr + i, false));
        }
        fprintf(out, "]}\n");
        fflush(out);
        return;
    }

    /* --- poke <addr> <byte>... --- */
    if (strcmp(cmd, "poke") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }
        if (nargs < 3) { json_error_f(out, "usage: poke <addr> <byte>..."); return; }
        uint64_t addr = (uint64_t)strtoul(arg1, NULL, 0);

        char all_bytes[CMD_BUF_SIZE];
        if (nargs >= 4)
            snprintf(all_bytes, sizeof(all_bytes), "%s %s", arg2, rest);
        else
            snprintf(all_bytes, sizeof(all_bytes), "%s", arg2);

        unsigned count = 0;
        char *tok = strtok(all_bytes, " \t");
        while (tok) {
            uint8_t val = (uint8_t)strtoul(tok, NULL, 0);
            ar_debug_mem()->v1.poke(ar_debug_mem(), addr + count, val);
            count++;
            tok = strtok(NULL, " \t");
        }
        json_ok_f(out, "\"written\":%u", count);
        return;
    }

    /* --- reg [name] [value] --- */
    if (strcmp(cmd, "reg") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }

        if (nargs < 2) {
            fprintf(out, "{\"ok\":true,\"registers\":{");
            unsigned cpu_type = ar_debug_cpu()->v1.type;
            unsigned nregs = ar_reg_count(cpu_type);
            bool first = true;
            for (unsigned i = 0; i < nregs; i++) {
                int idx = ar_reg_by_order(cpu_type, i);
                if (idx < 0) continue;
                const char *name = ar_reg_name(cpu_type, (unsigned)idx);
                uint64_t val = ar_debug_cpu()->v1.get_register(ar_debug_cpu(), (unsigned)idx);
                if (!first) fprintf(out, ",");
                fprintf(out, "\"%s\":%lu", name, (unsigned long)val);
                first = false;
            }
            fprintf(out, "}}\n");
            fflush(out);
            return;
        }

        int rid = ar_reg_from_name(ar_debug_cpu()->v1.type, arg1);
        if (rid < 0) { json_error_f(out, "unknown register: %s", arg1); return; }

        if (nargs >= 3) {
            uint64_t val = (uint64_t)strtoul(arg2, NULL, 0);
            if (ar_debug_cpu()->v1.set_register(ar_debug_cpu(), (unsigned)rid, val))
                json_ok_f(out, NULL);
            else
                json_error_f(out, "failed to set register %s", arg1);
            return;
        }

        uint64_t val = ar_debug_cpu()->v1.get_register(ar_debug_cpu(), (unsigned)rid);
        json_ok_f(out, "\"%s\":%lu", arg1, (unsigned long)val);
        return;
    }

    /* --- regions --- */
    if (strcmp(cmd, "regions") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }
        rd_System const *sys = ar_debug_system();

        /* Collect unique rd_Memory pointers */
        #define MAX_REGIONS 64
        rd_Memory const *seen[MAX_REGIONS];
        unsigned nseen = 0;

        /* Helper: add to seen if not already there */
        #define ADD_UNIQUE(mp) do { \
            bool _dup = false; \
            for (unsigned _k = 0; _k < nseen; _k++) \
                if (seen[_k] == (mp)) { _dup = true; break; } \
            if (!_dup && nseen < MAX_REGIONS) seen[nseen++] = (mp); \
        } while(0)

        for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
            rd_Memory const *m = sys->v1.cpus[i]->v1.memory_region;
            if (m) ADD_UNIQUE(m);
        }
        for (unsigned i = 0; i < sys->v1.num_memory_regions; i++)
            ADD_UNIQUE(sys->v1.memory_regions[i]);
        /* Memory map sources */
        for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
            rd_Memory const *cm = sys->v1.cpus[i]->v1.memory_region;
            if (!cm || !cm->v1.get_memory_map_count || !cm->v1.get_memory_map)
                continue;
            unsigned mc = cm->v1.get_memory_map_count(cm);
            auto *maps = (rd_MemoryMap *)malloc(mc * sizeof(rd_MemoryMap));
            if (!maps) continue;
            cm->v1.get_memory_map(cm, maps);
            for (unsigned j = 0; j < mc; j++)
                if (maps[j].source) ADD_UNIQUE(maps[j].source);
            free(maps);
        }
        #undef ADD_UNIQUE

        fprintf(out, "{\"ok\":true,\"regions\":[");
        for (unsigned i = 0; i < nseen; i++) {
            rd_Memory const *m = seen[i];
            if (i > 0) fputc(',', out);
            fprintf(out, "{\"id\":\"%s\",\"description\":\"%s\""
                         ",\"base_address\":\"0x%lx\",\"size\":%lu"
                         ",\"has_mmap\":%s}",
                    m->v1.id, m->v1.description,
                    (unsigned long)m->v1.base_address,
                    (unsigned long)m->v1.size,
                    (m->v1.get_memory_map_count && m->v1.get_memory_map)
                        ? "true" : "false");
        }
        fprintf(out, "]}\n");
        fflush(out);
        #undef MAX_REGIONS
        return;
    }

    /* --- save <slot> --- */
    if (strcmp(cmd, "save") == 0) {
        if (nargs < 2) { json_error_f(out, "usage: save <slot>"); return; }
        if (ar_core_blocked()) {
            json_error_f(out, "cannot save state while core thread is blocked");
            return;
        }
        int slot = atoi(arg1);
        if (ar_save_state(slot))
            json_ok_f(out, "\"slot\":%d", slot);
        else
            json_error_f(out, "save failed for slot %d", slot);
        return;
    }

    /* --- load <slot> --- */
    if (strcmp(cmd, "load") == 0) {
        if (nargs < 2) { json_error_f(out, "usage: load <slot>"); return; }
        if (ar_core_blocked()) {
            json_error_f(out, "cannot load state while core thread is blocked");
            return;
        }
        int slot = atoi(arg1);
        if (ar_load_state(slot))
            json_ok_f(out, "\"slot\":%d", slot);
        else
            json_error_f(out, "load failed for slot %d", slot);
        return;
    }

    /* --- screen [path] --- */
    if (strcmp(cmd, "screen") == 0) {
        const char *path = (nargs >= 2) ? arg1 : "screenshot.png";
        unsigned w = ar_frame_width();
        unsigned h = ar_frame_height();
        const uint32_t *fb = ar_frame_buf();

        unsigned npixels = w * h;
        auto *rgb = (uint8_t *)malloc(npixels * 3);
        if (!rgb) { json_error_f(out, "out of memory"); return; }
        for (unsigned i = 0; i < npixels; i++) {
            uint32_t px = fb[i];
            rgb[i * 3 + 0] = (px >> 16) & 0xFF;
            rgb[i * 3 + 1] = (px >>  8) & 0xFF;
            rgb[i * 3 + 2] =  px        & 0xFF;
        }

        int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
        free(rgb);

        if (ok)
            json_ok_f(out, "\"width\":%u,\"height\":%u,\"path\":\"%s\"",
                     w, h, path);
        else
            json_error_f(out, "failed to write PNG: %s", path);
        return;
    }

    /* --- dump <id> [start size [path]] --- */
    if (strcmp(cmd, "dump") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }

        char did[64] = {0}, ds[64] = {0}, dn_s[64] = {0}, dpath[4096] = {0};
        int dargs = sscanf(line, "%*s %63s %63s %63s %4095[^\n]",
                           did, ds, dn_s, dpath);

        if (dargs < 1) {
            json_error_f(out, "usage: dump <id> [start size [path]]");
            return;
        }

        rd_Memory const *mem = ar_find_memory_by_id(did);
        if (!mem) { json_error_f(out, "unknown memory region: %s", did); return; }

        uint64_t dstart = mem->v1.base_address;
        uint64_t dsize  = mem->v1.size;

        if (dargs >= 3) {
            dstart = strtoull(ds, NULL, 0);
            dsize  = strtoull(dn_s, NULL, 0);
        } else if (dargs == 2) {
            json_error_f(out, "usage: dump <id> [start size [path]]");
            return;
        }

        if (dsize == 0) {
            json_error_f(out, "memory region has unknown size; specify start and size");
            return;
        }

        if (dargs >= 4) {
            char *e = dpath + strlen(dpath) - 1;
            while (e > dpath && isspace((unsigned char)*e)) *e-- = '\0';
        }

        FILE *dump_out = out;
        bool close_dump = false;
        if (dargs >= 4 && dpath[0]) {
            dump_out = fopen(dpath, "w");
            if (!dump_out) {
                json_error_f(out, "cannot open file: %s", dpath);
                return;
            }
            close_dump = true;
        }

        do_dump(mem, dstart, dsize, dump_out);

        if (close_dump) {
            fclose(dump_out);
            json_ok_f(out, "\"path\":\"%s\"", dpath);
        }
        return;
    }

    /* --- dis [cpu] [region.]<start>-<end> --- */
    if (strcmp(cmd, "dis") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }

        rd_System const *sys = ar_debug_system();
        rd_Cpu const *cpu = nullptr;
        const char *range_arg = nullptr;

        /* Determine CPU and range argument.
         * If >1 CPU: first arg must be CPU ID, second is range.
         * If 1 CPU: first arg is optionally a CPU ID (try lookup), else range. */
        if (nargs < 2) {
            json_error_f(out, "usage: dis [cpu] [region.]<start>-<end>");
            return;
        }

        auto find_cpu = [&](const char *id) -> rd_Cpu const * {
            for (unsigned i = 0; i < sys->v1.num_cpus; i++)
                if (strcasecmp(sys->v1.cpus[i]->v1.id, id) == 0)
                    return sys->v1.cpus[i];
            return nullptr;
        };

        if (sys->v1.num_cpus > 1) {
            /* Multi-CPU: first arg must be CPU ID */
            cpu = find_cpu(arg1);
            if (!cpu) {
                json_error_f(out, "unknown cpu: %s (multi-CPU system requires cpu argument)", arg1);
                return;
            }
            if (nargs < 3) {
                json_error_f(out, "usage: dis <cpu> [region.]<start>-<end>");
                return;
            }
            range_arg = arg2;
        } else {
            /* Single CPU: try arg1 as CPU ID first, fall back to range */
            cpu = find_cpu(arg1);
            if (cpu) {
                if (nargs < 3) {
                    json_error_f(out, "usage: dis [cpu] [region.]<start>-<end>");
                    return;
                }
                range_arg = arg2;
            } else {
                cpu = ar_debug_cpu();
                range_arg = arg1;
            }
        }

        if (!cpu) {
            json_error_f(out, "no cpu available");
            return;
        }

        /* Parse range_arg: optional "region." prefix, then "start-end" */
        char region_id[64] = {0};
        char range_str[256];
        strncpy(range_str, range_arg, sizeof(range_str) - 1);

        char *dot = strchr(range_str, '.');
        if (dot) {
            size_t rlen = (size_t)(dot - range_str);
            if (rlen == 0 || rlen >= sizeof(region_id)) {
                json_error_f(out, "bad range: %s", range_arg);
                return;
            }
            memcpy(region_id, range_str, rlen);
            region_id[rlen] = '\0';
            memmove(range_str, dot + 1, strlen(dot + 1) + 1);
        }

        char *dash = strchr(range_str, '-');
        if (!dash) {
            json_error_f(out, "bad range (expected start-end): %s", range_arg);
            return;
        }
        *dash = '\0';
        uint64_t start = strtoull(range_str, nullptr, 16);
        uint64_t end   = strtoull(dash + 1, nullptr, 16);
        if (end < start) {
            json_error_f(out, "end < start");
            return;
        }

        /* Resolve memory region */
        rd_Memory const *mem;
        if (region_id[0])
            mem = ar_find_memory_by_id(region_id);
        else
            mem = cpu->v1.memory_region;

        if (!mem) {
            json_error_f(out, "unknown memory region: %s",
                         region_id[0] ? region_id : "(cpu default)");
            return;
        }

        /* Get CPU's PC */
        int pc_idx = ar_reg_pc(cpu->v1.type);
        uint64_t pc = (pc_idx >= 0)
            ? cpu->v1.get_register(cpu, (unsigned)pc_idx)
            : UINT64_MAX;

        /* Address width based on memory size */
        int addr_width = (mem->v1.size <= 0x10000) ? 4 : 8;

        /* Fetch bytes from memory */
        uint64_t byte_count = end - start + 1;
        std::vector<uint8_t> buf(byte_count);
        for (uint64_t i = 0; i < byte_count; i++)
            buf[i] = mem->v1.peek(mem, start + i, false);

        /* Disassemble */
        auto insns = arch::disassemble(
            std::span<const uint8_t>(buf.data(), buf.size()),
            start, cpu->v1.type);

        /* Fetch memory map for bank display */
        std::vector<rd_MemoryMap> memMap;
        bool hasMemMap = false;
        if (mem->v1.get_memory_map_count && mem->v1.get_memory_map) {
            unsigned mc = mem->v1.get_memory_map_count(mem);
            if (mc > 0) {
                memMap.resize(mc);
                mem->v1.get_memory_map(mem, memMap.data());
                hasMemMap = true;
            }
        }

        auto bankForAddr = [&](uint64_t a) -> int64_t {
            for (auto &m : memMap)
                if (a >= m.base_addr && a < m.base_addr + m.size)
                    return m.bank;
            return -1;
        };

        /* Compute bank column width from max bank in range */
        int bankColW = 0;
        if (hasMemMap) {
            int64_t maxBank = -1;
            for (auto &insn : insns) {
                if (insn.address > end) break;
                int64_t b = bankForAddr(insn.address);
                if (b > maxBank) maxBank = b;
            }
            if (maxBank >= 0) {
                bankColW = 1;
                for (int64_t v = maxBank; v >= 10; v /= 10) bankColW++;
            }
        }

        /* Print */
        const char *mem_id = mem->v1.id;
        for (size_t i = 0; i < insns.size(); i++) {
            auto &insn = insns[i];
            if (insn.address > end) break;

            /* Label */
            auto resolved = ar_sym_resolve(mem_id, insn.address);
            if (resolved) {
                const char *label = ar_sym_get_label(resolved->region_id.c_str(), resolved->addr);
                if (label)
                    fprintf(out, "%s:\n", label);
            }

            /* Marker */
            char marker = ':';
            if (insn.address == pc)
                marker = '>';
            else if (pc > insn.address && pc < insn.address + insn.length)
                marker = '~';

            /* Bank prefix */
            if (bankColW > 0) {
                int64_t bank = bankForAddr(insn.address);
                if (bank >= 0)
                    fprintf(out, "%*ld:", bankColW, (long)bank);
                else
                    fprintf(out, "%*s ", bankColW, "");
            }

            /* Instruction (resolve @-marked addresses to symbols) */
            std::string resolved_text = resolve_addr_markers(insn.text, mem_id);
            fprintf(out, "%0*lX%c %s",
                    addr_width, (unsigned long)insn.address,
                    marker, resolved_text.c_str());

            /* Comment */
            if (resolved) {
                const char *comment = ar_sym_get_comment(resolved->region_id.c_str(), resolved->addr);
                if (comment) {
                    /* First line only, crop to 24 chars */
                    char crop[32];
                    const char *nl = strchr(comment, '\n');
                    size_t clen = nl ? (size_t)(nl - comment) : strlen(comment);
                    if (clen > 24) {
                        memcpy(crop, comment, 24);
                        crop[24] = '\0';
                        strcat(crop, "...");
                    } else {
                        memcpy(crop, comment, clen);
                        crop[clen] = '\0';
                        if (nl) strcat(crop, "...");
                    }
                    fprintf(out, " ; %s", crop);
                }
            }

            fputc('\n', out);

            /* Blank line after flow-breaking instructions */
            if (insn.breaks_flow)
                fputc('\n', out);
        }
        fflush(out);
        return;
    }

    /* --- search reset|filter|list|count --- */
    if (strcmp(cmd, "search") == 0) {
        if (nargs < 2) {
            json_error_f(out, "usage: search reset|filter|list|count ...");
            return;
        }

        if (strcmp(arg1, "reset") == 0) {
            /* search reset <region_id> [size] [alignment] */
            char rid[64] = {0}, sz_s[16] = {0}, al_s[16] = {0};
            int rargs = sscanf(line, "%*s %*s %63s %15s %15s", rid, sz_s, al_s);
            if (rargs < 1) {
                json_error_f(out, "usage: search reset <region_id> [size] [alignment]");
                return;
            }
            int dsz = (rargs >= 2) ? atoi(sz_s) : 1;
            int aln = (rargs >= 3) ? atoi(al_s) : dsz;
            if (ar_search_reset(rid, dsz, aln))
                json_ok_f(out, "\"candidates\":%lu", (unsigned long)ar_search_count());
            else
                json_error_f(out, "search reset failed (bad region or size)");
            return;
        }

        if (strcmp(arg1, "filter") == 0) {
            /* search filter <op> <value|p> */
            if (nargs < 4) {
                json_error_f(out, "usage: search filter <op> <value|p>");
                return;
            }
            if (!ar_search_active()) {
                json_error_f(out, "no active search (call search reset first)");
                return;
            }

            static const struct { const char *name; ar_search_op op; } op_map[] = {
                {"eq", AR_SEARCH_EQ}, {"ne", AR_SEARCH_NE},
                {"lt", AR_SEARCH_LT}, {"gt", AR_SEARCH_GT},
                {"le", AR_SEARCH_LE}, {"ge", AR_SEARCH_GE},
                {NULL, AR_SEARCH_EQ}
            };

            ar_search_op op = AR_SEARCH_EQ;
            bool found = false;
            for (int i = 0; op_map[i].name; i++) {
                if (strcasecmp(arg2, op_map[i].name) == 0) {
                    op = op_map[i].op;
                    found = true;
                    break;
                }
            }
            if (!found) {
                json_error_f(out, "unknown op: %s", arg2);
                return;
            }

            uint64_t val;
            if (strcasecmp(rest, "p") == 0)
                val = AR_SEARCH_VS_PREV;
            else
                val = strtoull(rest, NULL, 0);

            ar_search_filter(op, val);
            json_ok_f(out, "\"candidates\":%lu", (unsigned long)ar_search_count());
            return;
        }

        if (strcmp(arg1, "list") == 0) {
            /* search list [max] */
            if (!ar_search_active()) {
                json_error_f(out, "no active search");
                return;
            }
            unsigned max = 100;
            if (nargs >= 3) max = (unsigned)strtoul(arg2, NULL, 0);
            if (max > 10000) max = 10000;

            auto *results = new ar_search_result[max];
            unsigned n = ar_search_results(results, max);

            fprintf(out, "{\"ok\":true,\"candidates\":%lu,\"results\":[",
                    (unsigned long)ar_search_count());
            for (unsigned i = 0; i < n; i++) {
                if (i > 0) fputc(',', out);
                fprintf(out, "{\"addr\":\"0x%lx\",\"value\":%lu,\"prev\":%lu}",
                        (unsigned long)results[i].addr,
                        (unsigned long)results[i].value,
                        (unsigned long)results[i].prev);
            }
            fprintf(out, "]}\n");
            fflush(out);

            delete[] results;
            return;
        }

        if (strcmp(arg1, "count") == 0) {
            if (!ar_search_active()) {
                json_error_f(out, "no active search");
                return;
            }
            json_ok_f(out, "\"candidates\":%lu", (unsigned long)ar_search_count());
            return;
        }

        json_error_f(out, "unknown search subcommand: %s", arg1);
        return;
    }

    /* --- cpu --- */
    if (strcmp(cmd, "cpu") == 0) {
        if (!ar_has_debug()) { json_error_f(out, "no debug support"); return; }
        rd_System const *sys = ar_debug_system();
        fprintf(out, "{\"ok\":true,\"cpus\":[");
        for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
            rd_Cpu const *c = sys->v1.cpus[i];
            if (i > 0) fputc(',', out);
            fprintf(out, "{\"id\":\"%s\",\"description\":\"%s\",\"primary\":%s}",
                    c->v1.id, c->v1.description,
                    c->v1.is_main ? "true" : "false");
        }
        fprintf(out, "]}\n");
        fflush(out);
        return;
    }

    /* --- bp add|delete|enable|disable|list|clear --- */
    if (strcmp(cmd, "bp") == 0) {
        if (nargs < 2) {
            json_error_f(out, "usage: bp add|delete|enable|disable|list|clear|save|load ...");
            return;
        }

        if (strcmp(arg1, "add") == 0) {
            /* bp add [cpu.]<addr> [flags] [condition...] */
            char bp_addr_s[64] = {0}, bp_flags_s[64] = {0}, bp_cond[256] = {0};
            int bp_args = sscanf(line, "%*s %*s %63s %63s %255[^\n]",
                                 bp_addr_s, bp_flags_s, bp_cond);
            if (bp_args < 1) {
                json_error_f(out, "usage: bp add [cpu.]<addr> [flags] [condition...]");
                return;
            }
            /* Parse optional cpu_id prefix: "cpu.addr" */
            char bp_cpu_id[64] = {0};
            char *dot = strchr(bp_addr_s, '.');
            if (dot) {
                size_t prefix_len = (size_t)(dot - bp_addr_s);
                if (prefix_len > 0 && prefix_len < sizeof(bp_cpu_id)) {
                    memcpy(bp_cpu_id, bp_addr_s, prefix_len);
                    bp_cpu_id[prefix_len] = '\0';
                }
                memmove(bp_addr_s, dot + 1, strlen(dot + 1) + 1);
            }
            uint64_t addr = strtoull(bp_addr_s, NULL, 16);
            unsigned flags = AR_BP_EXECUTE; /* default */
            bool temporary = false;
            if (bp_args >= 2 && bp_flags_s[0]) {
                /* Check if this is a flags string (X, XR, XRW, RW, XT, etc.) */
                bool is_flags = true;
                for (char *p = bp_flags_s; *p; p++) {
                    char c = *p & ~0x20; /* toupper */
                    if (c != 'X' && c != 'R' && c != 'W' && c != 'T') { is_flags = false; break; }
                }
                if (is_flags) {
                    flags = 0;
                    for (char *p = bp_flags_s; *p; p++) {
                        char c = *p & ~0x20;
                        if (c == 'X') flags |= AR_BP_EXECUTE;
                        else if (c == 'R') flags |= AR_BP_READ;
                        else if (c == 'W') flags |= AR_BP_WRITE;
                        else if (c == 'T') temporary = true;
                    }
                } else {
                    /* Not flags — treat as start of condition */
                    if (bp_args >= 3 && bp_cond[0]) {
                        char tmp[256];
                        snprintf(tmp, sizeof(tmp), "%s %s", bp_flags_s, bp_cond);
                        strncpy(bp_cond, tmp, sizeof(bp_cond) - 1);
                    } else {
                        strncpy(bp_cond, bp_flags_s, sizeof(bp_cond) - 1);
                    }
                    flags = AR_BP_EXECUTE;
                }
            }
            /* Trim trailing whitespace from condition */
            size_t cl = strlen(bp_cond);
            while (cl > 0 && (bp_cond[cl-1] == ' ' || bp_cond[cl-1] == '\t'))
                bp_cond[--cl] = '\0';

            int id = ar_bp_add(addr, flags, true, temporary, bp_cond[0] ? bp_cond : NULL,
                              bp_cpu_id[0] ? bp_cpu_id : NULL);
            if (id < 0) {
                json_error_f(out, "subscription failed (core may not support this breakpoint type)");
                return;
            }
            json_ok_f(out, "\"id\":%d", id);
            return;
        }

        if (strcmp(arg1, "delete") == 0) {
            if (nargs < 3) { json_error_f(out, "usage: bp delete <id>"); return; }
            int id = atoi(arg2);
            if (ar_bp_delete(id))
                json_ok_f(out, NULL);
            else
                json_error_f(out, "breakpoint %d not found", id);
            return;
        }

        if (strcmp(arg1, "enable") == 0) {
            if (nargs < 3) { json_error_f(out, "usage: bp enable <id>"); return; }
            int id = atoi(arg2);
            if (ar_bp_enable(id, true))
                json_ok_f(out, NULL);
            else
                json_error_f(out, "breakpoint %d not found or subscription failed", id);
            return;
        }

        if (strcmp(arg1, "disable") == 0) {
            if (nargs < 3) { json_error_f(out, "usage: bp disable <id>"); return; }
            int id = atoi(arg2);
            if (ar_bp_enable(id, false))
                json_ok_f(out, NULL);
            else
                json_error_f(out, "breakpoint %d not found or subscription failed", id);
            return;
        }

        if (strcmp(arg1, "list") == 0) {
            unsigned count = ar_bp_count();
            ar_breakpoint *bps = NULL;
            if (count > 0) {
                bps = new ar_breakpoint[count];
                count = ar_bp_list(bps, count);
            }
            fprintf(out, "{\"ok\":true,\"breakpoints\":[");
            for (unsigned i = 0; i < count; i++) {
                if (i > 0) fputc(',', out);
                char flags_str[4] = "---";
                if (bps[i].flags & AR_BP_EXECUTE) flags_str[0] = 'X';
                if (bps[i].flags & AR_BP_READ)    flags_str[1] = 'R';
                if (bps[i].flags & AR_BP_WRITE)   flags_str[2] = 'W';
                fprintf(out, "{\"id\":%d,\"address\":\"0x%04lx\","
                        "\"enabled\":%s,\"temporary\":%s,"
                        "\"flags\":\"%s\",\"condition\":\"%s\","
                        "\"cpu\":\"%s\"}",
                        bps[i].id,
                        (unsigned long)bps[i].address,
                        bps[i].enabled ? "true" : "false",
                        bps[i].temporary ? "true" : "false",
                        flags_str,
                        bps[i].condition,
                        bps[i].cpu_id);
            }
            fprintf(out, "]}\n");
            fflush(out);
            delete[] bps;
            return;
        }

        if (strcmp(arg1, "clear") == 0) {
            ar_bp_clear();
            json_ok_f(out, NULL);
            return;
        }

        if (strcmp(arg1, "save") == 0) {
            char bp_path[4096] = {0};
            if (nargs >= 3) {
                /* bp save <path> — use rest of line as path */
                sscanf(line, "%*s %*s %4095[^\n]", bp_path);
                /* Trim trailing whitespace */
                size_t pl = strlen(bp_path);
                while (pl > 0 && (bp_path[pl-1] == ' ' || bp_path[pl-1] == '\t'))
                    bp_path[--pl] = '\0';
            } else {
                const char *base = ar_rompath_base();
                if (!base || !base[0]) {
                    json_error_f(out, "no content loaded and no path given");
                    return;
                }
                snprintf(bp_path, sizeof(bp_path), "%s.bp", base);
            }
            if (ar_bp_save(bp_path))
                json_ok_f(out, "\"path\":\"%s\"", bp_path);
            else
                json_error_f(out, "failed to save breakpoints to %s", bp_path);
            return;
        }

        if (strcmp(arg1, "load") == 0) {
            char bp_path[4096] = {0};
            if (nargs >= 3) {
                sscanf(line, "%*s %*s %4095[^\n]", bp_path);
                size_t pl = strlen(bp_path);
                while (pl > 0 && (bp_path[pl-1] == ' ' || bp_path[pl-1] == '\t'))
                    bp_path[--pl] = '\0';
            } else {
                const char *base = ar_rompath_base();
                if (!base || !base[0]) {
                    json_error_f(out, "no content loaded and no path given");
                    return;
                }
                snprintf(bp_path, sizeof(bp_path), "%s.bp", base);
            }
            if (ar_bp_load(bp_path))
                json_ok_f(out, "\"path\":\"%s\",\"count\":%u", bp_path, ar_bp_count());
            else
                json_error_f(out, "failed to load breakpoints from %s", bp_path);
            return;
        }

        json_error_f(out, "unknown bp subcommand: %s", arg1);
        return;
    }

    /* --- sym label|comment get|set|delete / sym list --- */
    if (strcmp(cmd, "sym") == 0) {
        if (nargs < 2) {
            json_error_f(out, "usage: sym label|comment get|set|delete ... | sym list");
            return;
        }

        if (strcmp(arg1, "list") == 0) {
            unsigned count = ar_sym_count();
            ar_symbol *syms = nullptr;
            if (count > 0) {
                syms = new ar_symbol[count];
                count = ar_sym_list(syms, count);
            }
            fprintf(out, "{\"ok\":true,\"symbols\":[");
            for (unsigned i = 0; i < count; i++) {
                if (i > 0) fputc(',', out);
                fprintf(out, "{\"region\":\"%s\",\"addr\":%lu",
                        syms[i].region_id, (unsigned long)syms[i].address);
                if (syms[i].label[0]) {
                    fprintf(out, ",\"label\":\"%s\"", syms[i].label);
                }
                if (syms[i].comment[0]) {
                    /* Escape comment for JSON */
                    fprintf(out, ",\"comment\":\"");
                    for (const char *p = syms[i].comment; *p; p++) {
                        switch (*p) {
                        case '"':  fputs("\\\"", out); break;
                        case '\\': fputs("\\\\", out); break;
                        case '\n': fputs("\\n", out);  break;
                        case '\r': fputs("\\r", out);  break;
                        case '\t': fputs("\\t", out);  break;
                        default:   fputc(*p, out);     break;
                        }
                    }
                    fputc('"', out);
                }
                fputc('}', out);
            }
            fprintf(out, "]}\n");
            fflush(out);
            delete[] syms;
            return;
        }

        /* sym label|comment get|set|delete <addrspec> [value...]
         * addrspec: <region>.<bank>:<hex_addr> | <region>.<hex_addr> | <hex_addr>
         */
        if (strcmp(arg1, "label") != 0 && strcmp(arg1, "comment") != 0) {
            json_error_f(out, "unknown sym subcommand: %s", arg1);
            return;
        }
        bool is_label = (strcmp(arg1, "label") == 0);

        char sub_cmd[64] = {0}, s_addrspec[256] = {0};
        char s_value[CMD_BUF_SIZE] = {0};
        int sargs = sscanf(line, "%*s %*s %63s %255s %[^\n]",
                           sub_cmd, s_addrspec, s_value);

        if (sargs < 2) {
            json_error_f(out, "usage: sym %s get|set|delete <addrspec> [value]",
                         arg1);
            return;
        }

        /* Parse addrspec */
        char s_region[64] = {0};
        uint64_t addr = 0;
        int64_t bank = -1; /* -1 = no bank specified */
        bool have_bank = false;

        char *dot = strchr(s_addrspec, '.');
        if (!dot) {
            /* No region prefix — default to primary CPU memory */
            if (!ar_has_debug() || !ar_debug_cpu()) {
                json_error_f(out, "no debug support for default region");
                return;
            }
            strncpy(s_region, ar_debug_cpu()->v1.memory_region->v1.id,
                    sizeof(s_region) - 1);

            char *colon = strchr(s_addrspec, ':');
            if (colon) {
                /* bank:hex_addr */
                *colon = '\0';
                bank = (int64_t)strtoll(s_addrspec, NULL, 16);
                have_bank = true;
                addr = strtoull(colon + 1, NULL, 16);
            } else {
                /* bare hex_addr */
                addr = strtoull(s_addrspec, NULL, 16);
            }
        } else {
            /* region.remainder */
            size_t rlen = (size_t)(dot - s_addrspec);
            if (rlen == 0 || rlen >= sizeof(s_region)) {
                json_error_f(out, "bad addrspec: %s", s_addrspec);
                return;
            }
            memcpy(s_region, s_addrspec, rlen);
            s_region[rlen] = '\0';

            char *remainder = dot + 1;
            char *colon = strchr(remainder, ':');
            if (colon) {
                /* region.bank:hex_addr */
                *colon = '\0';
                bank = (int64_t)strtoll(remainder, NULL, 16);
                have_bank = true;
                addr = strtoull(colon + 1, NULL, 16);
            } else {
                /* region.hex_addr */
                addr = strtoull(remainder, NULL, 16);
            }
        }

        /* Resolve through memory maps */
        std::optional<ar_resolved_addr> rslv;
        if (have_bank)
            rslv = ar_sym_resolve_bank(s_region, addr, bank);
        else
            rslv = ar_sym_resolve(s_region, addr);
        if (!rslv) {
            if (have_bank)
                json_error_f(out, "cannot resolve %s bank 0x%lx at 0x%lx",
                             s_region, (unsigned long)bank, (unsigned long)addr);
            else if (!ar_find_memory_by_id(s_region))
                json_error_f(out, "unknown memory region: %s", s_region);
            else
                json_error_f(out, "cycle detected resolving %s:0x%lx",
                             s_region, (unsigned long)addr);
            return;
        }
        const char *resolved_region = rslv->region_id.c_str();
        uint64_t resolved_addr = rslv->addr;

        if (strcmp(sub_cmd, "get") == 0) {
            if (is_label) {
                const char *label = ar_sym_get_label(resolved_region, resolved_addr);
                if (label)
                    json_ok_f(out, "\"label\":\"%s\"", label);
                else
                    json_ok_f(out, "\"label\":null");
            } else {
                const char *comment = ar_sym_get_comment(resolved_region, resolved_addr);
                if (comment) {
                    fprintf(out, "{\"ok\":true,\"comment\":\"");
                    for (const char *p = comment; *p; p++) {
                        switch (*p) {
                        case '"':  fputs("\\\"", out); break;
                        case '\\': fputs("\\\\", out); break;
                        case '\n': fputs("\\n", out);  break;
                        case '\r': fputs("\\r", out);  break;
                        case '\t': fputs("\\t", out);  break;
                        default:   fputc(*p, out);     break;
                        }
                    }
                    fprintf(out, "\"}\n");
                    fflush(out);
                } else {
                    json_ok_f(out, "\"comment\":null");
                }
            }
            return;
        }

        if (strcmp(sub_cmd, "delete") == 0) {
            if (is_label)
                ar_sym_delete_label(resolved_region, resolved_addr);
            else
                ar_sym_delete_comment(resolved_region, resolved_addr);
            json_ok_f(out, NULL);
            return;
        }

        if (strcmp(sub_cmd, "set") == 0) {
            /* Value is everything after the addrspec token */
            char value_buf[CMD_BUF_SIZE] = {0};
            if (sargs >= 3) {
                snprintf(value_buf, sizeof(value_buf), "%s", s_value);
            } else {
                json_error_f(out, "usage: sym %s set <addrspec> <value>", arg1);
                return;
            }
            /* Trim trailing whitespace */
            size_t vl = strlen(value_buf);
            while (vl > 0 && (value_buf[vl-1] == ' ' || value_buf[vl-1] == '\t'
                              || value_buf[vl-1] == '\n' || value_buf[vl-1] == '\r'))
                value_buf[--vl] = '\0';

            if (is_label) {
                if (!ar_sym_set_label(resolved_region, resolved_addr, value_buf)) {
                    json_error_f(out, "invalid label: must match [a-zA-Z_][a-zA-Z0-9_]*");
                    return;
                }
            } else {
                if (!ar_sym_set_comment(resolved_region, resolved_addr, value_buf)) {
                    json_error_f(out, "failed to set comment");
                    return;
                }
            }
            json_ok_f(out, NULL);
            return;
        }

        json_error_f(out, "unknown sym %s subcommand: %s", arg1, sub_cmd);
        return;
    }

    /* --- trace on|off|status|cpu|registers|indent --- */
    if (strcmp(cmd, "trace") == 0) {
        if (nargs < 2) {
            json_error_f(out, "usage: trace on|off|status|cpu|registers|indent ...");
            return;
        }

        if (strcmp(arg1, "on") == 0) {
            /* trace on [path] */
            char tpath[4096] = {0};
            if (nargs >= 3)
                sscanf(line, "%*s %*s %4095[^\n]", tpath);
            /* Trim trailing whitespace */
            size_t tl = strlen(tpath);
            while (tl > 0 && (tpath[tl-1] == ' ' || tpath[tl-1] == '\t'))
                tpath[--tl] = '\0';

            if (ar_trace_start(tpath[0] ? tpath : NULL))
                json_ok_f(out, "\"tracing\":true%s%s%s",
                          tpath[0] ? ",\"file\":\"" : "",
                          tpath[0] ? tpath : "",
                          tpath[0] ? "\"" : "");
            else
                json_error_f(out, "failed to start trace");
            return;
        }

        if (strcmp(arg1, "off") == 0) {
            uint64_t lines = ar_trace_total_lines();
            ar_trace_stop();
            json_ok_f(out, "\"tracing\":false,\"lines\":%lu",
                      (unsigned long)lines);
            return;
        }

        if (strcmp(arg1, "status") == 0) {
            json_ok_f(out, "\"tracing\":%s,\"lines\":%lu,\"registers\":%s"
                          ",\"indent\":%s,\"file\":\"%s\"",
                      ar_trace_active() ? "true" : "false",
                      (unsigned long)ar_trace_total_lines(),
                      ar_trace_get_registers() ? "true" : "false",
                      ar_trace_get_indent() ? "true" : "false",
                      ar_trace_file_path());
            return;
        }

        if (strcmp(arg1, "cpu") == 0) {
            /* trace cpu <name> on|off */
            if (nargs < 4) {
                json_error_f(out, "usage: trace cpu <name> on|off");
                return;
            }
            bool enable;
            if (strcmp(rest, "on") == 0) enable = true;
            else if (strcmp(rest, "off") == 0) enable = false;
            else { json_error_f(out, "usage: trace cpu <name> on|off"); return; }

            if (ar_trace_cpu_enable(arg2, enable))
                json_ok_f(out, "\"cpu\":\"%s\",\"enabled\":%s",
                          arg2, enable ? "true" : "false");
            else
                json_error_f(out, "unknown cpu: %s", arg2);
            return;
        }

        if (strcmp(arg1, "registers") == 0) {
            if (nargs < 3) {
                json_error_f(out, "usage: trace registers on|off");
                return;
            }
            if (strcmp(arg2, "on") == 0) ar_trace_set_registers(true);
            else if (strcmp(arg2, "off") == 0) ar_trace_set_registers(false);
            else { json_error_f(out, "usage: trace registers on|off"); return; }
            json_ok_f(out, "\"registers\":%s",
                      ar_trace_get_registers() ? "true" : "false");
            return;
        }

        if (strcmp(arg1, "indent") == 0) {
            if (nargs < 3) {
                json_error_f(out, "usage: trace indent on|off");
                return;
            }
            if (strcmp(arg2, "on") == 0) ar_trace_set_indent(true);
            else if (strcmp(arg2, "off") == 0) ar_trace_set_indent(false);
            else { json_error_f(out, "usage: trace indent on|off"); return; }
            json_ok_f(out, "\"indent\":%s",
                      ar_trace_get_indent() ? "true" : "false");
            return;
        }

        json_error_f(out, "unknown trace subcommand: %s", arg1);
        return;
    }

    /* --- reset --- */
    if (strcmp(cmd, "reset") == 0) {
        ar_reset();
        json_ok_f(out, NULL);
        return;
    }

    /* --- manual on|off --- */
    if (strcmp(cmd, "manual") == 0) {
        if (nargs < 2) { json_error_f(out, "usage: manual on|off"); return; }
        if (strcmp(arg1, "on") == 0) {
            ar_set_manual_input(true);
            json_ok_f(out, "\"manual\":true");
        } else if (strcmp(arg1, "off") == 0) {
            ar_set_manual_input(false);
            json_ok_f(out, "\"manual\":false");
        } else {
            json_error_f(out, "usage: manual on|off");
        }
        return;
    }

    /* Delegate to frontend for frontend-specific commands */
    const ar_frontend_cb *fcb = ar_get_frontend_cb();
    if (fcb->handle_command &&
        fcb->handle_command(fcb->user, cmd, line, out))
        return;

    json_error_f(out, "unknown command: %s", cmd);
}

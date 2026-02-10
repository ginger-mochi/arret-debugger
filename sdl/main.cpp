/*
 * SDL frontend for Arrêt Debugger
 *
 * Headless mode: TCP-only command control for AI agents
 * Headed mode:   SDL2 display/audio/input + TCP command socket
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include <SDL.h>

#include "stb_image.h"

#include "backend.hpp"
#include "breakpoint.hpp"
#include "symbols.hpp"
#include "assets.hpp"

/* ========================================================================
 * SDL globals
 * ======================================================================== */

static SDL_Window   *sdl_window   = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture  *sdl_texture  = NULL;
static SDL_AudioDeviceID sdl_audio_dev = 0;
static int scale = 3;
static bool headless = false;

/* ========================================================================
 * SDL video
 * ======================================================================== */

static bool sdl_video_init(void) {
    if (sdl_window) return true;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            fprintf(stderr, "SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
            return false;
        }
    }

    sdl_window = SDL_CreateWindow("Arrêt",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ar_frame_width() * scale, ar_frame_height() * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!sdl_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    /* Set window icon from embedded PNG */
    {
        int iw, ih;
        unsigned char *px = stbi_load_from_memory(ar_asset_icon_png, ar_asset_icon_png_size,
                                                   &iw, &ih, NULL, 4);
        if (px) {
            SDL_Surface *icon = SDL_CreateRGBSurfaceFrom(px, iw, ih, 32, iw * 4,
                0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
            if (icon) {
                SDL_SetWindowIcon(sdl_window, icon);
                SDL_FreeSurface(icon);
            }
            stbi_image_free(px);
        }
    }

    sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    sdl_texture = SDL_CreateTexture(sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        ar_frame_width(), ar_frame_height());
    if (!sdl_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    return true;
}

static void sdl_video_cleanup(void) {
    if (sdl_texture)  { SDL_DestroyTexture(sdl_texture);   sdl_texture = NULL; }
    if (sdl_renderer) { SDL_DestroyRenderer(sdl_renderer); sdl_renderer = NULL; }
    if (sdl_window)   { SDL_DestroyWindow(sdl_window);     sdl_window = NULL; }
}

/* ========================================================================
 * SDL audio
 * ======================================================================== */

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int total_samples = len / (int)sizeof(int16_t);
    int frames_wanted = total_samples / 2;

    unsigned got = ar_audio_read(out, (unsigned)frames_wanted);

    /* Zero-fill remainder */
    for (unsigned i = got * 2; i < (unsigned)total_samples; i++)
        out[i] = 0;
}

static bool sdl_audio_init(void) {
    if (sdl_audio_dev) return true;

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
    }

    SDL_AudioSpec want = {}, have;
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = sdl_audio_callback;

    sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sdl_audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_PauseAudioDevice(sdl_audio_dev, 0);
    return true;
}

static void sdl_audio_cleanup(void) {
    if (sdl_audio_dev) { SDL_CloseAudioDevice(sdl_audio_dev); sdl_audio_dev = 0; }
}

/* ========================================================================
 * SDL init / cleanup
 * ======================================================================== */

static bool sdl_init(void) {
    if (!sdl_video_init()) return false;
    if (!ar_is_mute()) sdl_audio_init();
    return true;
}

static void sdl_cleanup(void) {
    sdl_audio_cleanup();
    sdl_video_cleanup();
    SDL_Quit();
}

/* ========================================================================
 * SDL rendering
 * ======================================================================== */

static void sdl_render(void) {
    if (!sdl_texture) return;

    int tw, th;
    SDL_QueryTexture(sdl_texture, NULL, NULL, &tw, &th);
    if ((unsigned)tw != ar_frame_width() || (unsigned)th != ar_frame_height()) {
        SDL_DestroyTexture(sdl_texture);
        sdl_texture = SDL_CreateTexture(sdl_renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            ar_frame_width(), ar_frame_height());
    }

    SDL_UpdateTexture(sdl_texture, NULL, ar_frame_buf(),
                      ar_frame_width() * sizeof(uint32_t));
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}

/* ========================================================================
 * SDL input handling
 * ======================================================================== */

static void sdl_handle_key(SDL_Scancode sc, bool pressed) {
    if (!ar_manual_input()) return;
    int16_t val = pressed ? 1 : 0;
    switch (sc) {
    case SDL_SCANCODE_UP:     ar_set_input(RETRO_DEVICE_ID_JOYPAD_UP,     val); break;
    case SDL_SCANCODE_DOWN:   ar_set_input(RETRO_DEVICE_ID_JOYPAD_DOWN,   val); break;
    case SDL_SCANCODE_LEFT:   ar_set_input(RETRO_DEVICE_ID_JOYPAD_LEFT,   val); break;
    case SDL_SCANCODE_RIGHT:  ar_set_input(RETRO_DEVICE_ID_JOYPAD_RIGHT,  val); break;
    case SDL_SCANCODE_Z:      ar_set_input(RETRO_DEVICE_ID_JOYPAD_B,      val); break;
    case SDL_SCANCODE_X:      ar_set_input(RETRO_DEVICE_ID_JOYPAD_A,      val); break;
    case SDL_SCANCODE_RETURN: ar_set_input(RETRO_DEVICE_ID_JOYPAD_START,  val); break;
    case SDL_SCANCODE_RSHIFT: ar_set_input(RETRO_DEVICE_ID_JOYPAD_SELECT, val); break;
    default: break;
    }
}

static void sdl_handle_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            ar_set_running(false);
            break;
        case SDL_KEYDOWN:
            if (!ar_manual_input() || ev.key.repeat) break;
            if (ev.key.keysym.scancode >= SDL_SCANCODE_1 &&
                ev.key.keysym.scancode <= SDL_SCANCODE_0 &&
                (ev.key.keysym.mod & (KMOD_SHIFT | KMOD_CTRL))) {
                int slot = (ev.key.keysym.scancode == SDL_SCANCODE_0)
                         ? 0 : (ev.key.keysym.scancode - SDL_SCANCODE_1 + 1);
                if (ar_core_blocked()) {
                    fprintf(stderr, "[arret] Cannot save/load state while core thread is blocked\n");
                } else if (ev.key.keysym.mod & KMOD_SHIFT) {
                    if (ar_save_state(slot))
                        fprintf(stderr, "[arret] Saved state slot %d\n", slot);
                    else
                        fprintf(stderr, "[arret] Save slot %d failed\n", slot);
                } else {
                    if (ar_load_state(slot))
                        fprintf(stderr, "[arret] Loaded state slot %d\n", slot);
                    else
                        fprintf(stderr, "[arret] Load slot %d failed\n", slot);
                }
                break;
            }
            sdl_handle_key(ev.key.keysym.scancode, true);
            break;
        case SDL_KEYUP:
            sdl_handle_key(ev.key.keysym.scancode, false);
            break;
        }
    }
}

/* ========================================================================
 * Frontend callback implementations
 * ======================================================================== */

static void cb_on_video_refresh(void *user) {
    (void)user;
    if (sdl_window) sdl_render();
}

static void cb_on_geometry_change(void *user, unsigned w, unsigned h) {
    (void)user;
    if (sdl_texture) {
        SDL_DestroyTexture(sdl_texture);
        sdl_texture = SDL_CreateTexture(sdl_renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetWindowSize(sdl_window, w * scale, h * scale);
    }
}

static uint32_t cb_get_ticks_ms(void *user) {
    (void)user;
    return sdl_window ? SDL_GetTicks() : 0;
}

static void cb_delay_ms(void *user, uint32_t ms) {
    (void)user;
    if (sdl_window) SDL_Delay(ms);
}

static void cb_poll_events(void *user) {
    (void)user;
    if (sdl_window) sdl_handle_events();
}

static bool cb_handle_command(void *user, const char *cmd,
                               const char *line, FILE *out) {
    (void)user; (void)line;

    char arg1[256] = {0};
    sscanf(line, "%*s %255s", arg1);

    /* --- display on|off --- */
    if (strcmp(cmd, "display") == 0) {
        if (arg1[0] == '\0') {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: display on|off\"}\n");
            fflush(out);
        } else if (strcmp(arg1, "on") == 0) {
            if (sdl_video_init()) {
                SDL_ShowWindow(sdl_window);
                fprintf(out, "{\"ok\":true,\"display\":true}\n");
                fflush(out);
            } else {
                fprintf(out, "{\"ok\":false,\"error\":\"failed to initialize display\"}\n");
                fflush(out);
            }
        } else if (strcmp(arg1, "off") == 0) {
            sdl_video_cleanup();
            fprintf(out, "{\"ok\":true,\"display\":false}\n");
            fflush(out);
        } else {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: display on|off\"}\n");
            fflush(out);
        }
        return true;
    }

    /* --- sound on|off --- */
    if (strcmp(cmd, "sound") == 0) {
        if (arg1[0] == '\0') {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: sound on|off\"}\n");
            fflush(out);
        } else if (strcmp(arg1, "on") == 0) {
            ar_set_mute(false);
            if (sdl_audio_init()) {
                SDL_PauseAudioDevice(sdl_audio_dev, 0);
                fprintf(out, "{\"ok\":true,\"sound\":true}\n");
                fflush(out);
            } else {
                fprintf(out, "{\"ok\":false,\"error\":\"failed to initialize audio\"}\n");
                fflush(out);
            }
        } else if (strcmp(arg1, "off") == 0) {
            ar_set_mute(true);
            if (sdl_audio_dev)
                SDL_PauseAudioDevice(sdl_audio_dev, 1);
            fprintf(out, "{\"ok\":true,\"sound\":false}\n");
            fflush(out);
        } else {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: sound on|off\"}\n");
            fflush(out);
        }
        return true;
    }

    return false;
}

/* ========================================================================
 * Main loops
 * ======================================================================== */

static void run_headless(void) {
    while (ar_running()) {
        /* Block on TCP socket with 100ms timeout */
        ar_check_socket_commands();

        if (sdl_window) {
            sdl_handle_events();
            sdl_render();
        }

        /* Small sleep to avoid busy loop when no display */
        if (!sdl_window)
            SDL_Delay(10);
    }

    if (sdl_window) sdl_cleanup();
}

static void run_headed(void) {
    if (!sdl_init()) {
        fprintf(stderr, "SDL initialization failed, falling back to headless\n");
        headless = true;
        run_headless();
        return;
    }

    while (ar_running()) {
        sdl_handle_events();
        ar_check_socket_commands();
        ar_run_frame();
        ar_bp_flush_deferred();
        sdl_render();
    }

    sdl_cleanup();
}

/* ========================================================================
 * Usage
 * ======================================================================== */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <core.so> <rom>\n"
        "       %s --cmd \"command\" [--port N]\n"
        "\n"
        "Options:\n"
        "  --headless          Run without display (AI agent mode)\n"
        "  --mute              Start with audio disabled\n"
        "  --system-dir DIR    System/BIOS directory (default: .)\n"
        "  --scale N           Window scale factor (default: 3)\n"
        "  --port N            TCP command port (default: 2784)\n"
        "  --cmd \"command\"     Send command to running instance and exit\n"
        "\n", prog, prog);
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(int argc, char **argv) {
    const char *core_path = NULL;
    const char *rom_path  = NULL;
    const char *cmd_str   = NULL;
    bool mute_flag = false;
    int port = 2784;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--system-dir") == 0 && i + 1 < argc) {
            /* system-dir is passed but currently unused by SDL frontend */
            i++;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1) scale = 1;
            if (scale > 10) scale = 10;
        } else if (strcmp(argv[i], "--mute") == 0) {
            mute_flag = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cmd") == 0 && i + 1 < argc) {
            cmd_str = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (!core_path) {
            core_path = argv[i];
        } else if (!rom_path) {
            rom_path = argv[i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* Client mode */
    if (cmd_str)
        return ar_cmd_client(cmd_str, port);

    if (!core_path || !rom_path) {
        usage(argv[0]);
        return 1;
    }

    /* SDL needs to be initialised for timing even in headless mode */
    if (SDL_Init(0) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    ar_frontend_cb cb = {
        .on_video_refresh   = cb_on_video_refresh,
        .on_geometry_change = cb_on_geometry_change,
        .get_ticks_ms       = cb_get_ticks_ms,
        .delay_ms           = cb_delay_ms,
        .poll_events        = cb_poll_events,
        .handle_command     = cb_handle_command,
        .user               = NULL,
    };

    if (!ar_init(core_path, rom_path, mute_flag, port, &cb))
        return 1;

    ar_sym_auto_load();

    /* Default: manual keyboard input only in headed mode */
    ar_set_manual_input(!headless);

    if (headless)
        run_headless();
    else
        run_headed();

    ar_shutdown();
    SDL_Quit();
    return 0;
}

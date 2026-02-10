/*
 * Qt frontend for Arrêt Debugger
 */

#include <QApplication>
#include <QMessageBox>
#include <QIcon>
#include <QPixmap>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "MainWindow.h"

#include "backend.hpp"
#include "symbols.hpp"
#include "assets.hpp"

/* Frontend callbacks for Qt */

static MainWindow *g_mainWindow = nullptr;

static void cb_on_video_refresh(void *) {
    if (g_mainWindow)
        g_mainWindow->centralWidget()->update();
}

static void cb_on_geometry_change(void *, unsigned w, unsigned h) {
    if (g_mainWindow)
        g_mainWindow->resize(w * 3, h * 3);
}

static uint32_t cb_get_ticks_ms(void *) {
    return 0;
}

static void cb_delay_ms(void *, uint32_t) {
}

static void cb_poll_events(void *) {
    QApplication::processEvents();
}

static bool cb_handle_command(void *, const char *cmd,
                               const char *line, FILE *out) {
    char arg1[256] = {0};
    sscanf(line, "%*s %255s", arg1);

    if (strcmp(cmd, "display") == 0) {
        if (arg1[0] == '\0') {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: display on|off\"}\n");
        } else if (strcmp(arg1, "on") == 0) {
            if (g_mainWindow) {
                g_mainWindow->show();
                fprintf(out, "{\"ok\":true,\"display\":true}\n");
            } else {
                fprintf(out, "{\"ok\":false,\"error\":\"no window\"}\n");
            }
        } else if (strcmp(arg1, "off") == 0) {
            if (g_mainWindow) g_mainWindow->hide();
            fprintf(out, "{\"ok\":true,\"display\":false}\n");
        } else {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: display on|off\"}\n");
        }
        fflush(out);
        return true;
    }

    if (strcmp(cmd, "sound") == 0) {
        if (arg1[0] == '\0') {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: sound on|off\"}\n");
        } else if (strcmp(arg1, "on") == 0) {
            ar_set_mute(false);
            fprintf(out, "{\"ok\":true,\"sound\":true}\n");
        } else if (strcmp(arg1, "off") == 0) {
            ar_set_mute(true);
            fprintf(out, "{\"ok\":true,\"sound\":false}\n");
        } else {
            fprintf(out, "{\"ok\":false,\"error\":\"usage: sound on|off\"}\n");
        }
        fflush(out);
        return true;
    }

    if (strcmp(cmd, "pause") == 0) {
        fprintf(out, "{\"ok\":true,\"paused\":true}\n");
        fflush(out);
        return true;
    }

    if (strcmp(cmd, "resume") == 0) {
        fprintf(out, "{\"ok\":true,\"paused\":false}\n");
        fflush(out);
        return true;
    }

    return false;
}

/* Usage */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] [core.so] [rom]\n"
        "       %s --cmd \"command\" [--port N]\n"
        "\n"
        "Options:\n"
        "  --headless          Run without display (AI agent mode)\n"
        "  --mute              Start with audio disabled\n"
        "  --port N            TCP command port (default: 2783)\n"
        "  --cmd \"command\"     Send command to running instance and exit\n"
        "\n"
        "Core and ROM are optional; they can be loaded via the File menu.\n"
        "\n", prog, prog);
}

int main(int argc, char **argv) {
    const char *core_path = nullptr;
    const char *rom_path  = nullptr;
    const char *cmd_str   = nullptr;
    bool headless = false;
    bool mute_flag = false;
    int port = 2783;

    /* Pre-parse for --cmd (before QApplication, which modifies argv) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cmd") == 0 && i + 1 < argc) {
            cmd_str = argv[i + 1];
            for (int j = 1; j < argc; j++) {
                if (strcmp(argv[j], "--port") == 0 && j + 1 < argc)
                    port = atoi(argv[j + 1]);
            }
            return ar_cmd_client(cmd_str, port);
        }
    }

    QApplication app(argc, argv);

    /* Set application icon from embedded PNG */
    QPixmap iconPix;
    iconPix.loadFromData(ar_asset_icon_png, ar_asset_icon_png_size, "PNG");
    app.setWindowIcon(QIcon(iconPix));

    /* Parse remaining args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--mute") == 0) {
            mute_flag = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (!core_path) {
            core_path = argv[i];
        } else if (!rom_path) {
            rom_path = argv[i];
        }
    }

    /* Headless implies muted by default */
    if (headless)
        mute_flag = true;

    ar_frontend_cb cb = {};
    cb.on_video_refresh   = cb_on_video_refresh;
    cb.on_geometry_change = cb_on_geometry_change;
    cb.get_ticks_ms       = cb_get_ticks_ms;
    cb.delay_ms           = cb_delay_ms;
    cb.poll_events        = cb_poll_events;
    cb.handle_command     = cb_handle_command;
    cb.user               = nullptr;

    /* Always set up backend (TCP socket, stdout redirect) */
    ar_setup(mute_flag, port, &cb);

    /* Load core and content if provided on command line */
    if (core_path) {
        if (!ar_load_core(core_path)) {
            QMessageBox::critical(nullptr, "Error",
                QString("Failed to load core: %1").arg(core_path));
            return 1;
        }
        if (!ar_has_debug())
            QMessageBox::warning(nullptr, "No Debug Support",
                "This core does not support retrodebug.\n"
                "Debug features will be unavailable.");
        if (rom_path) {
            if (!ar_load_content(rom_path)) {
                QMessageBox::critical(nullptr, "Error",
                    QString("Failed to load content: %1").arg(rom_path));
                return 1;
            }
            if (!headless) {
                ar_bp_set_auto(true);
                ar_bp_auto_load();
                ar_sym_auto_load();
            }
        }
    }

    ar_set_manual_input(!headless);

    auto *win = new MainWindow;
    g_mainWindow = win;

    if (!headless)
        win->show();

    win->startEmulation(headless);

    int ret = app.exec();

    ar_set_running(false);
    ar_shutdown();
    return ret;
}

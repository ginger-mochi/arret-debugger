#include "MainWindow.h"
#include "VideoWidget.h"
#include "AudioOutput.h"
#include "MemoryViewer.h"
#include "MemorySearch.h"
#include "Debugger.h"
#include "Breakpoints.h"
#include "TraceLog.h"
#include "InputTool.h"

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QFileDialog>
#include <QMessageBox>
#include <QScreen>
#include <QRandomGenerator>

#include "backend.hpp"
#include "symbols.hpp"

/* Position a newly-created floating widget so it doesn't overlap existing
   visible floating widgets, staying on the same monitor as the main window. */
static void placeFloatingWidget(QMainWindow *mainWin, QWidget *widget,
                                std::initializer_list<QWidget *> others)
{
    /* Ensure widget has its final size before positioning */
    widget->show();
    widget->raise();

    QScreen *screen = mainWin->screen();
    if (!screen) return;
    QRect avail = screen->availableGeometry();
    QSize sz = widget->frameGeometry().size();

    /* Collect rects of existing visible windows */
    QVector<QRect> occupied;
    occupied.append(mainWin->frameGeometry());
    for (auto *w : others) {
        if (w && w != widget && w->isVisible())
            occupied.append(w->frameGeometry());
    }

    /* Try grid positions to find one with no overlap */
    int step = 40;
    int maxX = avail.right() - sz.width();
    int maxY = avail.bottom() - sz.height();

    auto overlaps = [&](QRect r) {
        for (auto &o : occupied)
            if (r.intersects(o)) return true;
        return false;
    };

    for (int y = avail.top(); y <= maxY; y += step) {
        for (int x = avail.left(); x <= maxX; x += step) {
            QRect candidate(x, y, sz.width(), sz.height());
            if (!overlaps(candidate)) {
                widget->move(x, y);
                return;
            }
        }
    }

    /* No clear spot found — random position on the same monitor */
    int rx = avail.left() + QRandomGenerator::global()->bounded(
                 std::max(1, maxX - avail.left()));
    int ry = avail.top() + QRandomGenerator::global()->bounded(
                 std::max(1, maxY - avail.top()));
    widget->move(rx, ry);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_video(new VideoWidget(this))
    , m_audio(new AudioOutput(this))
    , m_memViewer(nullptr)
    , m_memSearch(nullptr)
    , m_debugger(nullptr)
    , m_breakpoints(nullptr)
    , m_traceLog(nullptr)
    , m_inputTool(nullptr)
    , m_timer(new QTimer(this))
{
    setWindowTitle("Arrêt");
    setCentralWidget(m_video);
    resize(160 * 3, 144 * 3);

    buildMenus();

    connect(m_timer, &QTimer::timeout, this, &MainWindow::tick);
}

MainWindow::~MainWindow() {
    m_timer->stop();
    m_audio->stop();
}

void MainWindow::startEmulation() {
    /* If content is loaded, use its fps; otherwise use a default tick rate */
    int interval = 16;
    if (ar_content_loaded()) {
        const auto *av = ar_av_info();
        interval = (int)(1000.0 / av->timing.fps);
        if (interval < 1) interval = 1;

        resize(ar_frame_width() * 3, ar_frame_height() * 3);

        if (!ar_is_mute())
            m_audio->start();

        ar_core_thread_start();
    }

    m_timer->start(interval);
}

void MainWindow::tick() {
    int state = ar_core_state();

    if (state == 2 /* BLOCKED */) {
        if (m_debugger) m_debugger->setThreadBlocked(true);
        if (ar_bp_hit() >= 0) {
            ar_bp_ack_hit();
            m_bpPaused = true;
            if (!m_debugger || !m_debugger->isVisible())
                openDebugger();
        }
    }

    if (state == 3 /* DONE */) {
        ar_core_ack_done();
        state = 0;
        if (m_stepping && ar_debug_step_complete()) {
            ar_debug_step_end();
            m_stepping = false;
        }
        if (ar_bp_hit() >= 0) {
            ar_bp_ack_hit();
            m_paused = true;
            m_bpPaused = true;
            m_pauseAction->setText("Resume");
            if (!m_debugger || !m_debugger->isVisible())
                openDebugger();
        }
        if (m_debugger) m_debugger->setThreadBlocked(false);
    }

    if (state == 0 /* IDLE */) {
        if (m_stepping || m_frameAdvancing || (!m_paused && ar_content_loaded())) {
            if (m_bpPaused) {
                ar_debug_set_skip();
                m_bpPaused = false;
            }
            ar_run_frame_async();
            m_frameAdvancing = false;
        }
    }

    /* UI refresh always */
    m_video->update();
    if (m_memViewer) m_memViewer->refresh();
    if (m_memSearch) m_memSearch->refresh();
    if (m_debugger) {
        bool paused = (state == 0 && m_paused) || state == 2 || state == 3;
        m_debugger->refresh(paused);
    }
    if (m_breakpoints) m_breakpoints->refresh();
    if (m_traceLog) m_traceLog->refresh();
    if (m_inputTool) m_inputTool->refresh();
    ar_check_socket_commands();

    if (!ar_running())
        close();
}

void MainWindow::loadCore() {
    QString path = QFileDialog::getOpenFileName(this,
        "Load Core", QString(), "Shared Libraries (*.so *.dylib *.dll);;All Files (*)");
    if (path.isEmpty()) return;

    if (!ar_load_core(path.toUtf8().constData())) {
        QMessageBox::critical(this, "Error",
            QString("Failed to load core:\n%1").arg(path));
        return;
    }

    updateMenuState();
    setWindowTitle(QString("Arrêt - %1").arg(ar_sys_info()->library_name));
}

void MainWindow::loadContent() {
    QString path = QFileDialog::getOpenFileName(this,
        "Load Content", QString(), "All Files (*)");
    if (path.isEmpty()) return;

    /* Stop audio while reloading */
    m_audio->stop();

    if (!ar_load_content(path.toUtf8().constData())) {
        QMessageBox::critical(this, "Error",
            QString("Failed to load content:\n%1").arg(path));
        return;
    }

    ar_bp_set_auto(true);
    ar_bp_auto_load();
    ar_sym_auto_load();

    updateMenuState();

    /* Restart with correct timing */
    m_timer->stop();
    const auto *av = ar_av_info();
    int interval = (int)(1000.0 / av->timing.fps);
    if (interval < 1) interval = 1;
    m_timer->start(interval);

    resize(ar_frame_width() * 3, ar_frame_height() * 3);

    if (!ar_is_mute())
        m_audio->start();

    ar_core_thread_start();
}

void MainWindow::togglePause() {
    if (m_paused && ar_core_blocked()) {
        /* Resuming from pause while thread is blocked — need to unblock.
           Unsubscribe BEFORE resuming so the core doesn't re-fire. */
        if (m_stepping) {
            ar_debug_step_end();
            m_stepping = false;
        }
        if (m_bpPaused) {
            ar_debug_set_skip();
            m_bpPaused = false;
        }
        ar_core_resume_blocked();
    }
    m_paused = !m_paused;
    m_pauseAction->setText(m_paused ? "Resume" : "Pause");
}

void MainWindow::frameAdvance() {
    if (!ar_content_loaded()) return;

    /* Ensure we're paused */
    if (!m_paused) {
        m_paused = true;
        m_pauseAction->setText("Resume");
    }

    /* If thread is blocked (breakpoint/watchpoint), resume it first */
    if (ar_core_blocked()) {
        if (m_stepping) {
            ar_debug_step_end();
            m_stepping = false;
        }
        if (m_bpPaused) {
            ar_debug_set_skip();
            m_bpPaused = false;
        }
        ar_core_resume_blocked();
    }

    m_frameAdvancing = true;
}

void MainWindow::toggleSound() {
    bool muted = ar_is_mute();
    ar_set_mute(!muted);
    m_soundAction->setChecked(muted); /* was muted, now unmuted = checked */
    if (muted)
        m_audio->start();
    else
        m_audio->stop();
}

void MainWindow::reloadRom() {
    if (!ar_content_loaded()) return;
    m_audio->stop();
    ar_reload_rom();
    m_video->update();
    if (!ar_is_mute())
        m_audio->start();
}

void MainWindow::openMemoryViewer() {
    openMemoryViewerAt(nullptr, 0);
}

void MainWindow::openMemoryViewerAt(rd_Memory const *mem, uint64_t addr) {
    bool firstOpen = !m_memViewer;
    if (firstOpen) {
        m_memViewer = new MemoryViewer(this);
        m_memViewer->setFloating(true);
        m_memViewer->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    if (firstOpen)
        placeFloatingWidget(this, m_memViewer, {m_memSearch, m_debugger, m_breakpoints, m_traceLog, m_inputTool});
    else {
        m_memViewer->show();
        m_memViewer->raise();
    }
    if (mem || addr)
        m_memViewer->goTo(mem, addr);
}

void MainWindow::openMemorySearch() {
    bool firstOpen = !m_memSearch;
    if (firstOpen) {
        m_memSearch = new MemorySearch(this);
        m_memSearch->setFloating(true);
        m_memSearch->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    if (firstOpen)
        placeFloatingWidget(this, m_memSearch, {m_memViewer, m_debugger, m_breakpoints, m_traceLog, m_inputTool});
    else {
        m_memSearch->show();
        m_memSearch->raise();
    }
}

void MainWindow::openDebugger() {
    bool firstOpen = !m_debugger;
    if (firstOpen) {
        m_debugger = new Debugger(this);
        m_debugger->setFloating(true);
        m_debugger->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    if (firstOpen)
        placeFloatingWidget(this, m_debugger, {m_memViewer, m_memSearch, m_breakpoints, m_traceLog, m_inputTool});
    else {
        m_debugger->show();
        m_debugger->raise();
    }
}

void MainWindow::openBreakpoints() {
    bool firstOpen = !m_breakpoints;
    if (firstOpen) {
        m_breakpoints = new Breakpoints(this);
        m_breakpoints->setFloating(true);
        m_breakpoints->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    if (firstOpen)
        placeFloatingWidget(this, m_breakpoints, {m_memViewer, m_memSearch, m_debugger, m_traceLog, m_inputTool});
    else {
        m_breakpoints->show();
        m_breakpoints->raise();
    }
}

void MainWindow::openTraceLog() {
    bool firstOpen = !m_traceLog;
    if (firstOpen) {
        m_traceLog = new TraceLog(this);
        m_traceLog->setFloating(true);
        m_traceLog->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    if (firstOpen)
        placeFloatingWidget(this, m_traceLog, {m_memViewer, m_memSearch, m_debugger, m_breakpoints, m_inputTool});
    else {
        m_traceLog->show();
        m_traceLog->raise();
    }
}

void MainWindow::openInputTool() {
    bool firstOpen = !m_inputTool;
    if (firstOpen) {
        m_inputTool = new InputTool(this);
        m_inputTool->setFloating(true);
        m_inputTool->setAllowedAreas(Qt::NoDockWidgetArea);
    }
    if (firstOpen)
        placeFloatingWidget(this, m_inputTool, {m_memViewer, m_memSearch, m_debugger, m_breakpoints, m_traceLog});
    else {
        m_inputTool->show();
        m_inputTool->raise();
    }
}

void MainWindow::debugStep(int type) {
    if (!ar_has_debug() || !ar_content_loaded()) return;
    if (!m_paused) {
        m_paused = true;
        m_pauseAction->setText("Resume");
    }
    bool wasSuspended = ar_core_blocked();
    if (wasSuspended && m_stepping) {
        /* Reuse existing subscription to avoid skip_first double-step.
           Just reset the complete flag and resume the core thread. */
        ar_debug_step_reset();
        if (m_bpPaused) {
            ar_debug_set_skip();
            m_bpPaused = false;
        }
        ar_core_resume_blocked();
        return;
    }
    if (m_stepping) {
        ar_debug_step_end();
        m_stepping = false;
    }
    if (ar_debug_step_begin(type)) {
        m_stepping = true;
        if (wasSuspended) {
            if (m_bpPaused) {
                ar_debug_set_skip();
                m_bpPaused = false;
            }
            ar_core_resume_blocked();
        }
    }
}

void MainWindow::debugStepIn()   { debugStep(AR_STEP_IN); }
void MainWindow::debugStepOver() { debugStep(AR_STEP_OVER); }
void MainWindow::debugStepOut()  { debugStep(AR_STEP_OUT); }

void MainWindow::debugResume() {
    if (m_stepping) {
        ar_debug_step_end();
        m_stepping = false;
    }
    if (ar_core_blocked()) {
        if (m_bpPaused) {
            ar_debug_set_skip();
            m_bpPaused = false;
        }
        ar_core_resume_blocked();
    }
    if (m_bpPaused) {
        ar_debug_set_skip();
        m_bpPaused = false;
    }
    m_paused = false;
    m_pauseAction->setText("Pause");
}

void MainWindow::buildMenus() {
    /* File menu */
    auto *fileMenu = menuBar()->addMenu("&File");

    fileMenu->addAction("Load Core...", this, &MainWindow::loadCore);

    m_loadContentAction = fileMenu->addAction("Load Content...",
                                               this, &MainWindow::loadContent);

    fileMenu->addSeparator();

    m_saveMenu = fileMenu->addMenu("Save State");
    m_loadMenu = fileMenu->addMenu("Load State");
    for (int i = 0; i <= 9; i++) {
        auto *saveAct = m_saveMenu->addAction(QString("Slot %1").arg(i));
        saveAct->setShortcut(QKeySequence(Qt::SHIFT | (Qt::Key_0 + i)));
        int slot = i;
        connect(saveAct, &QAction::triggered, this, [this, slot]() {
            /* Only when the main window / video widget has focus */
            QWidget *fw = focusWidget();
            if (fw && fw != m_video && fw != this)
                return;
            if (ar_core_blocked()) {
                QMessageBox::warning(this, "Cannot Save",
                    "Cannot save state while the core thread is blocked mid-frame.");
                return;
            }
            ar_save_state(slot);
        });

        auto *loadAct = m_loadMenu->addAction(QString("Slot %1").arg(i));
        loadAct->setShortcut(QKeySequence(Qt::CTRL | (Qt::Key_0 + i)));
        connect(loadAct, &QAction::triggered, this, [this, slot]() {
            /* Only when the main window / video widget has focus */
            QWidget *fw = focusWidget();
            if (fw && fw != m_video && fw != this)
                return;
            if (ar_core_blocked()) {
                QMessageBox::warning(this, "Load State",
                    "Cannot load state while the core thread is blocked mid-frame.\n"
                    "Resume execution first (advance a frame with no breakpoints).");
                return;
            }
            ar_load_state(slot);
        });
    }

    fileMenu->addSeparator();
    fileMenu->addAction("Quit", this, &QWidget::close, QKeySequence("Ctrl+Q"));

    /* Emulation menu */
    auto *emuMenu = menuBar()->addMenu("&Emulation");

    m_reloadAction = emuMenu->addAction("Reload ROM", this, &MainWindow::reloadRom,
                                         QKeySequence("Ctrl+Shift+R"));

    m_pauseAction = emuMenu->addAction("Pause", this, &MainWindow::togglePause,
                                        QKeySequence("P"));

    m_frameAdvanceAction = emuMenu->addAction("Frame Advance", this, &MainWindow::frameAdvance,
                                               QKeySequence("\\"));

    m_soundAction = emuMenu->addAction("Sound");
    m_soundAction->setCheckable(true);
    m_soundAction->setChecked(!ar_is_mute());
    m_soundAction->setShortcut(QKeySequence("M"));
    connect(m_soundAction, &QAction::triggered, this, &MainWindow::toggleSound);

    /* Tools menu */
    auto *toolsMenu = menuBar()->addMenu("&Tools");
    toolsMenu->addAction("Memory Viewer", this, &MainWindow::openMemoryViewer,
                         QKeySequence("Ctrl+M"));
    toolsMenu->addAction("Memory Search", this, &MainWindow::openMemorySearch,
                         QKeySequence("Ctrl+Shift+M"));
    toolsMenu->addAction("Debugger", this, &MainWindow::openDebugger,
                         QKeySequence("Ctrl+D"));
    toolsMenu->addAction("Breakpoints", this, &MainWindow::openBreakpoints,
                         QKeySequence("Ctrl+B"));
    toolsMenu->addAction("Trace Log", this, &MainWindow::openTraceLog,
                         QKeySequence("Ctrl+L"));
    toolsMenu->addAction("Input", this, &MainWindow::openInputTool,
                         QKeySequence("Ctrl+I"));

    updateMenuState();
}

void MainWindow::updateMenuState() {
    bool coreOk = ar_core_loaded();
    bool contentOk = ar_content_loaded();

    m_loadContentAction->setEnabled(coreOk);
    m_saveMenu->setEnabled(contentOk);
    m_loadMenu->setEnabled(contentOk);
    m_reloadAction->setEnabled(contentOk);
    m_pauseAction->setEnabled(contentOk);
    m_frameAdvanceAction->setEnabled(contentOk);
}

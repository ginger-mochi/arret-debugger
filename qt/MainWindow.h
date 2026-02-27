#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>

class VideoWidget;
class AudioOutput;
class MemoryViewer;
class MemorySearch;
class Debugger;
class Breakpoints;
class TraceLog;
class InputTool;
class TileViewer;
class TilemapViewer;
class VramViewer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void startEmulation(bool headless = false);

    /* Open the memory viewer (singleton) and navigate to addr in the given region.
       If mem is null, keeps the current region. */
    void openMemoryViewerAt(struct rd_Memory const *mem, uint64_t addr);

public slots:
    void debugStepIn();
    void debugStepOver();
    void debugStepOut();
    void debugResume();

private slots:
    void tick();
    void loadCore();
    void loadContent();
    void togglePause();
    void frameAdvance();
    void toggleSound();
    void reloadRom();
    void openMemoryViewer();
    void openMemorySearch();
    void openDebugger();
    void openBreakpoints();
    void openTraceLog();
    void openInputTool();
    void openContentInfo();
    void openTileViewer();
    void openTilemapViewer();
    void openVramViewer();

private:
    void buildMenus();
    void updateMenuState();
    void debugStep(int type);

    VideoWidget  *m_video;
    AudioOutput  *m_audio;
    MemoryViewer *m_memViewer;
    MemorySearch *m_memSearch;
    Debugger     *m_debugger;
    Breakpoints  *m_breakpoints;
    TraceLog     *m_traceLog;
    InputTool    *m_inputTool;
    TileViewer   *m_tileViewer;
    TilemapViewer *m_tilemapViewer;
    VramViewer   *m_vramViewer;
    QTimer       *m_timer;
    bool          m_paused = false;
    bool          m_stepping = false;
    bool          m_bpPaused = false;      /* paused at a breakpoint */
    bool          m_frameAdvancing = false; /* one-shot: run single frame */
    QAction      *m_loadContentAction;
    QAction      *m_pauseAction;
    QAction      *m_frameAdvanceAction;
    QAction      *m_soundAction;
    QAction      *m_reloadAction;
    QMenu        *m_saveMenu;
    QMenu        *m_loadMenu;
    QMenu        *m_systemMenu;
};

#endif // MAINWINDOW_H

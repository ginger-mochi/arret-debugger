#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <QDockWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QMenuBar;
QT_END_NAMESPACE

struct rd_Cpu;
struct rd_System;

class Debugger : public QDockWidget {
    Q_OBJECT
public:
    explicit Debugger(QWidget *parent = nullptr);
    void refresh(bool paused);
    void setThreadBlocked(bool blocked);

protected:
    bool event(QEvent *e) override;

private:
    void buildMenuBar(QMenuBar *menuBar);
    void populateCpus();
    void onCpuChanged(int index);
    void goToAddress();

    class DisasmView;
    class RegistersPane;
    class StackTracePane;

    DisasmView      *m_disasm;
    RegistersPane   *m_regs;
    StackTracePane  *m_stackTrace;
    QComboBox       *m_cpuCombo;
    QAction         *m_goToAction = nullptr;

    rd_Cpu const    *m_cpu = nullptr;
    rd_System const *m_lastSystem = nullptr;
    bool             m_lastPaused = false;
};

#endif // DEBUGGER_H

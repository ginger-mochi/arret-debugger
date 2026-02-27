#include "TraceLog.h"
#include "backend.hpp"
#include "trace.hpp"

#include <QPlainTextEdit>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QScrollBar>
#include <QFont>

TraceLog::TraceLog(QWidget *parent)
    : QDockWidget("Trace Log", parent)
{
    auto *container = new QWidget;
    auto *hbox = new QHBoxLayout(container);
    hbox->setContentsMargins(4, 4, 4, 4);

    /* Left: log view */
    m_logView = new QPlainTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1000000);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("monospace", 9);
    mono.setStyleHint(QFont::Monospace);
    m_logView->setFont(mono);
    hbox->addWidget(m_logView, 1);

    /* Right: controls */
    auto *rhs = new QWidget;
    auto *rvbox = new QVBoxLayout(rhs);
    rvbox->setContentsMargins(0, 0, 0, 0);

    /* File path */
    rvbox->addWidget(new QLabel("File:"));
    auto *fileRow = new QHBoxLayout;
    m_filePath = new QLineEdit;
    m_filePath->setPlaceholderText("(window only)");
    fileRow->addWidget(m_filePath);
    auto *browseBtn = new QPushButton("...");
    browseBtn->setFixedWidth(30);
    connect(browseBtn, &QPushButton::clicked, this, &TraceLog::browseFile);
    fileRow->addWidget(browseBtn);
    rvbox->addLayout(fileRow);

    rvbox->addSpacing(8);

    /* Options */
    m_instrCheck = new QCheckBox("Trace instructions");
    m_instrCheck->setChecked(ar_trace_get_instructions());
    connect(m_instrCheck, &QCheckBox::toggled, this, [](bool on) {
        ar_trace_set_instructions(on);
    });
    rvbox->addWidget(m_instrCheck);

    m_intCheck = new QCheckBox("Trace interrupts");
    m_intCheck->setChecked(ar_trace_get_interrupts());
    connect(m_intCheck, &QCheckBox::toggled, this, [](bool on) {
        ar_trace_set_interrupts(on);
    });
    rvbox->addWidget(m_intCheck);

    m_regCheck = new QCheckBox("Registers");
    m_regCheck->setChecked(ar_trace_get_registers());
    connect(m_regCheck, &QCheckBox::toggled, this, [](bool on) {
        ar_trace_set_registers(on);
    });
    rvbox->addWidget(m_regCheck);

    m_indentCheck = new QCheckBox("Indent (SP)");
    m_indentCheck->setChecked(ar_trace_get_indent());
    connect(m_indentCheck, &QCheckBox::toggled, this, [](bool on) {
        ar_trace_set_indent(on);
    });
    rvbox->addWidget(m_indentCheck);

    rvbox->addSpacing(8);

    /* CPU checkboxes (populated lazily) */
    rvbox->addWidget(new QLabel("CPUs:"));
    m_cpuLayout = new QVBoxLayout;
    rvbox->addLayout(m_cpuLayout);

    rvbox->addSpacing(8);

    /* Start/Stop button */
    m_startBtn = new QPushButton("Start");
    connect(m_startBtn, &QPushButton::clicked, this, &TraceLog::toggleTrace);
    rvbox->addWidget(m_startBtn);

    rvbox->addSpacing(4);

    /* Line count */
    m_lineCount = new QLabel("Lines: 0");
    rvbox->addWidget(m_lineCount);

    rvbox->addStretch();

    rhs->setFixedWidth(180);
    hbox->addWidget(rhs);

    setWidget(container);
    resize(700, 400);
}

void TraceLog::browseFile() {
    QString path = QFileDialog::getSaveFileName(this, "Trace Log File",
        QString(), "Log Files (*.log *.txt);;All Files (*)");
    if (!path.isEmpty())
        m_filePath->setText(path);
}

void TraceLog::toggleTrace() {
    if (ar_trace_active()) {
        ar_trace_stop();
    } else {
        /* Apply current checkbox state before starting */
        ar_trace_set_instructions(m_instrCheck->isChecked());
        ar_trace_set_interrupts(m_intCheck->isChecked());
        ar_trace_set_registers(m_regCheck->isChecked());
        ar_trace_set_indent(m_indentCheck->isChecked());

        /* Apply CPU settings */
        for (auto &cc : m_cpuChecks)
            ar_trace_cpu_enable(cc.id.toUtf8().constData(), cc.check->isChecked());

        QString path = m_filePath->text().trimmed();
        ar_trace_start(path.isEmpty() ? nullptr : path.toUtf8().constData());
    }
    updateUI();
}

void TraceLog::populateCpus() {
    if (m_cpusPopulated) return;
    if (!ar_has_debug()) return;

    rd_System const *sys = ar_debug_system();
    if (!sys || sys->v1.num_cpus == 0) return;

    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        rd_Cpu const *cpu = sys->v1.cpus[i];
        QString id = QString::fromUtf8(cpu->v1.id);

        auto *cb = new QCheckBox(id);
        cb->setChecked(ar_trace_cpu_enabled(cpu->v1.id));

        connect(cb, &QCheckBox::toggled, this, [id](bool on) {
            ar_trace_cpu_enable(id.toUtf8().constData(), on);
        });

        m_cpuLayout->addWidget(cb);
        m_cpuChecks.append({id, cb});
    }

    m_cpusPopulated = true;
}

void TraceLog::updateUI() {
    bool active = ar_trace_active();
    m_startBtn->setText(active ? "Stop" : "Start");
    m_filePath->setEnabled(!active);
}

void TraceLog::refresh() {
    /* Populate CPUs on first refresh with debug support */
    if (!m_cpusPopulated && ar_has_debug())
        populateCpus();

    /* Sync button state (trace may have started/stopped via TCP) */
    bool active = ar_trace_active();
    if ((m_startBtn->text() == "Start") != !active)
        updateUI();

    /* Sync checkbox state */
    if (m_instrCheck->isChecked() != ar_trace_get_instructions())
        m_instrCheck->setChecked(ar_trace_get_instructions());
    if (m_intCheck->isChecked() != ar_trace_get_interrupts())
        m_intCheck->setChecked(ar_trace_get_interrupts());
    if (m_regCheck->isChecked() != ar_trace_get_registers())
        m_regCheck->setChecked(ar_trace_get_registers());
    if (m_indentCheck->isChecked() != ar_trace_get_indent())
        m_indentCheck->setChecked(ar_trace_get_indent());

    if (!active) return;

    /* Read new lines from ring buffer */
    static constexpr unsigned BATCH = 10000;
    static char *readBuf = nullptr;
    if (!readBuf)
        readBuf = (char *)malloc(BATCH * TRACE_LINE_SIZE);
    if (!readBuf) return;

    unsigned n = ar_trace_read_new(readBuf, BATCH);
    if (n > 0) {
        /* Build a single string for batch append */
        QString batch;
        batch.reserve((int)n * 80);
        for (unsigned i = 0; i < n; i++) {
            if (i > 0) batch += '\n';
            batch += QString::fromLatin1(readBuf + (size_t)i * TRACE_LINE_SIZE);
        }

        /* Append to log view */
        bool atBottom = m_logView->verticalScrollBar()->value() >=
                        m_logView->verticalScrollBar()->maximum() - 4;
        m_logView->appendPlainText(batch);
        if (atBottom) {
            m_logView->verticalScrollBar()->setValue(
                m_logView->verticalScrollBar()->maximum());
        }
    }

    /* Update line count */
    uint64_t total = ar_trace_total_lines();
    m_lineCount->setText(QString("Lines: %1").arg(total));
}

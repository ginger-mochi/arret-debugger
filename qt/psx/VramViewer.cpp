#include "VramViewer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QRadioButton>
#include <QButtonGroup>
#include <QPainter>
#include <QMouseEvent>
#include <QGroupBox>
#include <QListWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QFont>
#include <QShortcut>

#include "backend.hpp"
#include "sys/psx_gpu_decode.hpp"
#include "sys/psx_gpu_capture.hpp"

static constexpr int VRAM_BYTES = 1048576;
static constexpr int VRAM_HW_W = 1024;
static constexpr int VRAM_H = 512;

/* ======================================================================== */
/* VramWidget                                                                */
/* ======================================================================== */

VramWidget::VramWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(false);
}

void VramWidget::setImage(const QImage &img) {
    m_image = img;
    setFixedSize(img.isNull() ? QSize(100, 100) : img.size());
    update();
}

QSize VramWidget::sizeHint() const {
    return m_image.isNull() ? QSize(1024, 512) : m_image.size();
}

void VramWidget::pageGrid(int &cols, int &rows, int &pw, int &ph) const {
    ph = 256;
    rows = 2;

    switch (m_format) {
    case FMT_15BIT: pw = 256; cols = 4; break;
    case FMT_8BIT:  pw = 256; cols = 8; break;
    case FMT_4BIT:  pw = 256; cols = 16; break;
    case FMT_24BIT: pw = 227; cols = 3; break;
    }
}

void VramWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    if (m_image.isNull()) {
        p.fillRect(rect(), Qt::black);
        return;
    }

    p.drawImage(0, 0, m_image);

    if (m_selectedPage >= 0) {
        int cols, rows, pw, ph;
        pageGrid(cols, rows, pw, ph);

        int pageCol = m_selectedPage % cols;
        int pageRow = m_selectedPage / cols;
        if (pageRow < rows) {
            int x = pageCol * pw;
            int y = pageRow * ph;

            p.setPen(QPen(Qt::red, 2));
            p.setBrush(QColor(255, 0, 0, 40));
            p.drawRect(x, y, pw, ph);
        }
    }

    if (m_hasHighlight && m_hlW > 0 && m_hlH > 0) {
        int px = 0, py = 0, pw = 0, ph = 0;
        switch (m_format) {
        case FMT_15BIT:
            px = m_hlX; py = m_hlY; pw = m_hlW; ph = m_hlH;
            break;
        case FMT_8BIT:
            px = m_hlX * 2; py = m_hlY; pw = m_hlW * 2; ph = m_hlH;
            break;
        case FMT_4BIT:
            px = m_hlX * 4; py = m_hlY; pw = m_hlW * 4; ph = m_hlH;
            break;
        case FMT_24BIT:
            px = m_hlX * 3 / 2; py = m_hlY; pw = m_hlW * 3 / 2; ph = m_hlH;
            break;
        }

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 1));
        p.drawRect(px - 2, py - 2, pw + 3, ph + 3);
        p.setPen(QPen(Qt::red, 1));
        p.drawRect(px - 1, py - 1, pw + 1, ph + 1);
    }
}

void VramWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    if (m_image.isNull()) return;
    emit clicked(event->pos().x(), event->pos().y());
}

/* ======================================================================== */
/* VramViewer                                                                */
/* ======================================================================== */

VramViewer::VramViewer(QWidget *parent)
    : QDockWidget("VRAM Viewer", parent)
{
    setObjectName("VramViewer");

    auto *main = new QWidget;
    auto *vbox = new QVBoxLayout(main);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    /* Top bar: format radio buttons */
    auto *topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel("Format:"));

    m_formatGroup = new QButtonGroup(this);
    m_radio15 = new QRadioButton("15-bit");
    m_radio24 = new QRadioButton("24-bit");
    m_radio8  = new QRadioButton("8-bit");
    m_radio4  = new QRadioButton("4-bit");

    m_formatGroup->addButton(m_radio15, VramWidget::FMT_15BIT);
    m_formatGroup->addButton(m_radio24, VramWidget::FMT_24BIT);
    m_formatGroup->addButton(m_radio8,  VramWidget::FMT_8BIT);
    m_formatGroup->addButton(m_radio4,  VramWidget::FMT_4BIT);

    m_radio15->setChecked(true);

    topBar->addWidget(m_radio15);
    topBar->addWidget(m_radio24);
    topBar->addWidget(m_radio8);
    topBar->addWidget(m_radio4);
    topBar->addStretch();
    vbox->addLayout(topBar);

    /* Center: scroll area with VRAM widget */
    m_vramWidget = new VramWidget;
    m_scrollArea = new QScrollArea;
    m_scrollArea->setWidget(m_vramWidget);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setMinimumSize(700, 530);
    vbox->addWidget(m_scrollArea, 1);

    /* Bottom: page info label */
    m_pageLabel = new QLabel("Click VRAM to select texture page");
    vbox->addWidget(m_pageLabel);

    /* GPU Event Log group box (collapsible) */
    m_gpuLogGroup = new QGroupBox("GPU Event Log");
    m_gpuLogGroup->setCheckable(true);
    m_gpuLogGroup->setChecked(false);
    vbox->addWidget(m_gpuLogGroup);

    auto *gpuLogVbox = new QVBoxLayout(m_gpuLogGroup);

    /* Unavailable label (shown when core doesn't support GPU Post) */
    m_gpuLogUnavail = new QLabel("Core does not support GPU event logging");
    m_gpuLogUnavail->setStyleSheet("color: grey;");
    m_gpuLogUnavail->setAlignment(Qt::AlignCenter);
    m_gpuLogUnavail->hide();
    gpuLogVbox->addWidget(m_gpuLogUnavail);

    /* Content widget (hidden when collapsed or unavailable) */
    m_gpuLogContent = new QWidget;
    gpuLogVbox->addWidget(m_gpuLogContent);

    auto *contentVbox = new QVBoxLayout(m_gpuLogContent);
    contentVbox->setContentsMargins(0, 0, 0, 0);

    /* Top row: Capture button + memory usage label */
    auto *captureRow = new QHBoxLayout;
    m_captureBtn = new QPushButton("Capture");
    captureRow->addWidget(m_captureBtn);
    captureRow->addStretch();
    m_memUsageLabel = new QLabel;
    captureRow->addWidget(m_memUsageLabel);
    contentVbox->addLayout(captureRow);

    /* Splitter: event list (left) + event detail (right) */
    auto *splitter = new QSplitter(Qt::Horizontal);

    m_eventList = new QListWidget;
    m_eventList->setMinimumWidth(200);
    splitter->addWidget(m_eventList);

    m_eventDetail = new QTextEdit;
    m_eventDetail->setReadOnly(true);
    QFont mono("monospace");
    mono.setStyleHint(QFont::Monospace);
    m_eventDetail->setFont(mono);
    m_eventDetail->setMinimumWidth(200);
    splitter->addWidget(m_eventDetail);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setMinimumHeight(200);
    contentVbox->addWidget(splitter, 1);

    /* Bottom row: prev/next frame buttons */
    auto *navRow = new QHBoxLayout;
    m_prevFrameBtn = new QPushButton("<- Prev Frame");
    m_prevFrameBtn->setToolTip("Previous frame boundary (Left arrow)");
    m_nextFrameBtn = new QPushButton("Next Frame ->");
    m_nextFrameBtn->setToolTip("Next frame boundary (Right arrow)");
    navRow->addWidget(m_prevFrameBtn);
    navRow->addStretch();
    navRow->addWidget(m_nextFrameBtn);
    contentVbox->addLayout(navRow);

    /* Start collapsed */
    m_gpuLogContent->hide();

    setWidget(main);
    resize(720, 620);

    /* Connections */
    connect(m_formatGroup, &QButtonGroup::idClicked,
            this, &VramViewer::formatChanged);
    connect(m_vramWidget, &VramWidget::clicked,
            this, &VramViewer::onVramClicked);
    connect(m_gpuLogGroup, &QGroupBox::toggled,
            this, &VramViewer::gpuLogToggled);
    connect(m_captureBtn, &QPushButton::clicked, this, [this]() {
        if (m_capturing) stopCapture(); else startCapture();
    });
    connect(m_eventList, &QListWidget::currentRowChanged,
            this, &VramViewer::onEventSelected);
    connect(m_prevFrameBtn, &QPushButton::clicked,
            this, &VramViewer::prevFrame);
    connect(m_nextFrameBtn, &QPushButton::clicked,
            this, &VramViewer::nextFrame);

    auto *scLeft = new QShortcut(Qt::Key_Left, this);
    connect(scLeft, &QShortcut::activated, this, &VramViewer::prevFrame);
    auto *scRight = new QShortcut(Qt::Key_Right, this);
    connect(scRight, &QShortcut::activated, this, &VramViewer::nextFrame);

    rebuildImage();
}

VramViewer::~VramViewer() {
    if (m_capturing)
        stopCapture();
}

/* ======================================================================== */
/* refresh                                                                   */
/* ======================================================================== */

void VramViewer::refresh() {
    /* Lazy check for GPU Post availability */
    if (!m_gpuLogChecked && ar_has_debug() && ar_content_loaded())
        checkGpuLogAvailability();

    /* During capture: update memory label, keep VRAM live.
     * Frame boundaries are inserted by the post-frame hook on the core thread. */
    if (m_capturing) {
        m_memUsageLabel->setText(QString("Capture: %1").arg(
            formatBytes(sys::gpu_capture_compressed_bytes())));
        rebuildImage();
        return;
    }

    /* If group box is checked and we have captured events, preserve captured view */
    if (m_gpuLogGroup->isChecked() && !m_capturing &&
        !sys::gpu_capture_events().empty()) {
        return;
    }

    rebuildImage();
}

/* ======================================================================== */
/* Format / click handlers                                                   */
/* ======================================================================== */

void VramViewer::formatChanged(int id) {
    m_format = (VramWidget::Format)id;
    m_vramWidget->setFormat(m_format);
    m_vramWidget->setSelectedPage(-1);
    m_pageLabel->setText("Click VRAM to select texture page");

    /* If viewing captured data, re-render from capture buffer */
    if (m_gpuLogGroup->isChecked() && !sys::gpu_capture_events().empty() &&
        !m_captureVram.empty()) {
        rebuildImageFromBuffer(m_captureVram.data());
        return;
    }

    rebuildImage();
}

void VramViewer::onVramClicked(int x, int y) {
    int cols, rows, pw, ph;
    m_vramWidget->pageGrid(cols, rows, pw, ph);

    int pageCol = x / pw;
    int pageRow = y / ph;
    if (pageCol >= cols) pageCol = cols - 1;
    if (pageRow >= rows) pageRow = rows - 1;
    if (pageCol < 0) pageCol = 0;
    if (pageRow < 0) pageRow = 0;

    int page = pageRow * cols + pageCol;
    m_vramWidget->setSelectedPage(page);

    unsigned page_x_hw = 0;
    switch (m_format) {
    case VramWidget::FMT_15BIT: page_x_hw = pageCol * 256; break;
    case VramWidget::FMT_8BIT:  page_x_hw = pageCol * 128; break;
    case VramWidget::FMT_4BIT:  page_x_hw = pageCol * 64;  break;
    case VramWidget::FMT_24BIT: page_x_hw = pageCol * 384; break;
    }
    unsigned page_y = pageRow * 256;
    unsigned hw_addr = page_x_hw + page_y * 1024;

    m_pageLabel->setText(QString("Page (%1,%2) \u2014 VRAM: %3h")
        .arg(pageCol).arg(pageRow)
        .arg(QString::number(hw_addr, 16).toUpper()));
}

/* ======================================================================== */
/* GPU Event Log group box toggle                                            */
/* ======================================================================== */

void VramViewer::gpuLogToggled(bool checked) {
    if (checked) {
        if (!m_gpuLogChecked && ar_has_debug() && ar_content_loaded())
            checkGpuLogAvailability();

        if (m_gpuLogAvailable) {
            m_gpuLogContent->show();
            m_gpuLogUnavail->hide();
        } else {
            m_gpuLogContent->hide();
            m_gpuLogUnavail->show();
        }
    } else {
        m_gpuLogContent->hide();
        m_gpuLogUnavail->hide();
        rebuildImage();
    }
}

/* ======================================================================== */
/* Check GPU Post breakpoint availability                                    */
/* ======================================================================== */

void VramViewer::checkGpuLogAvailability() {
    m_gpuLogChecked = true;
    m_gpuLogAvailable = false;

    rd_System const *sys = ar_debug_system();
    if (!sys) return;

    for (unsigned i = 0; i < sys->v1.num_break_points; i++) {
        const char *desc = sys->v1.break_points[i]->v1.description;
        if (strcmp(desc, "GPU Post") == 0) {
            m_gpuLogAvailable = true;
            break;
        }
    }

    if (m_gpuLogGroup->isChecked()) {
        m_gpuLogContent->setVisible(m_gpuLogAvailable);
        m_gpuLogUnavail->setVisible(!m_gpuLogAvailable);
    }
}

/* ======================================================================== */
/* VRAM read helpers                                                         */
/* ======================================================================== */

void VramViewer::readLiveVram(std::vector<uint8_t> &buf) {
    if (!m_vramMem) {
        m_vramMem = ar_find_memory_by_id("vram");
        if (!m_vramMem) return;
    }
    buf.resize(VRAM_BYTES);
    if (m_vramMem->v1.peek_range &&
        m_vramMem->v1.peek_range(m_vramMem, 0, VRAM_BYTES, buf.data()))
        return;
    for (int i = 0; i < VRAM_BYTES; i++)
        buf[i] = m_vramMem->v1.peek(m_vramMem, i, false);
}

void VramViewer::rebuildImageFromBuffer(const uint8_t *vram) {
    auto hw = [&](int hw_x, int line) -> uint16_t {
        int byte_addr = (line * VRAM_HW_W + hw_x) * 2;
        if (byte_addr + 1 >= VRAM_BYTES) return 0;
        return (uint16_t)vram[byte_addr] | ((uint16_t)vram[byte_addr + 1] << 8);
    };

    QImage img;

    switch (m_format) {
    case VramWidget::FMT_15BIT: {
        img = QImage(VRAM_HW_W, VRAM_H, QImage::Format_RGB32);
        for (int y = 0; y < VRAM_H; y++) {
            auto *scanline = reinterpret_cast<uint32_t *>(img.scanLine(y));
            for (int x = 0; x < VRAM_HW_W; x++) {
                uint16_t c = hw(x, y);
                int r = (c & 0x1F) << 3;
                int g = ((c >> 5) & 0x1F) << 3;
                int b = ((c >> 10) & 0x1F) << 3;
                scanline[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
        break;
    }
    case VramWidget::FMT_24BIT: {
        int imgW = (VRAM_HW_W * 2) / 3;
        img = QImage(imgW, VRAM_H, QImage::Format_RGB32);
        for (int y = 0; y < VRAM_H; y++) {
            auto *scanline = reinterpret_cast<uint32_t *>(img.scanLine(y));
            int byteRow = y * VRAM_HW_W * 2;
            for (int px = 0; px < imgW; px++) {
                int byteOff = byteRow + px * 3;
                if (byteOff + 2 >= VRAM_BYTES) { scanline[px] = 0xFF000000; continue; }
                uint8_t r = vram[byteOff];
                uint8_t g = vram[byteOff + 1];
                uint8_t b = vram[byteOff + 2];
                scanline[px] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
        break;
    }
    case VramWidget::FMT_8BIT: {
        int imgW = VRAM_HW_W * 2;
        img = QImage(imgW, VRAM_H, QImage::Format_RGB32);
        for (int y = 0; y < VRAM_H; y++) {
            auto *scanline = reinterpret_cast<uint32_t *>(img.scanLine(y));
            int byteRow = y * VRAM_HW_W * 2;
            for (int px = 0; px < imgW; px++) {
                int byteOff = byteRow + px;
                if (byteOff >= VRAM_BYTES) { scanline[px] = 0xFF000000; continue; }
                uint8_t v = vram[byteOff];
                scanline[px] = 0xFF000000 | (v << 16) | (v << 8) | v;
            }
        }
        break;
    }
    case VramWidget::FMT_4BIT: {
        int imgW = VRAM_HW_W * 4;
        img = QImage(imgW, VRAM_H, QImage::Format_RGB32);
        for (int y = 0; y < VRAM_H; y++) {
            auto *scanline = reinterpret_cast<uint32_t *>(img.scanLine(y));
            int byteRow = y * VRAM_HW_W * 2;
            for (int px = 0; px < imgW; px++) {
                int byteOff = byteRow + px / 2;
                if (byteOff >= VRAM_BYTES) { scanline[px] = 0xFF000000; continue; }
                uint8_t nibble;
                if (px & 1)
                    nibble = (vram[byteOff] >> 4) & 0x0F;
                else
                    nibble = vram[byteOff] & 0x0F;
                uint8_t v = nibble * 17;
                scanline[px] = 0xFF000000 | (v << 16) | (v << 8) | v;
            }
        }
        break;
    }
    }

    m_vramWidget->setFormat(m_format);
    m_vramWidget->setImage(img);
}

void VramViewer::rebuildImage() {
    m_vramWidget->clearHighlightRect();
    if (!ar_has_debug() || !ar_content_loaded()) return;

    m_vramMem = ar_find_memory_by_id("vram");
    if (!m_vramMem) return;

    std::vector<uint8_t> buf(VRAM_BYTES);
    if (!m_vramMem->v1.peek_range ||
        !m_vramMem->v1.peek_range(m_vramMem, 0, VRAM_BYTES, buf.data())) {
        for (int i = 0; i < VRAM_BYTES; i++)
            buf[i] = m_vramMem->v1.peek(m_vramMem, i, false);
    }

    rebuildImageFromBuffer(buf.data());
}

/* ======================================================================== */
/* Capture start / stop                                                      */
/* ======================================================================== */

void VramViewer::startCapture() {
    if (!ar_has_debug() || !ar_content_loaded()) return;

    rd_DebuggerIf *dif = ar_get_debugger_if();
    if (!dif) return;

    if (!sys::gpu_capture_start(dif)) return;

    m_capturing = true;
    m_captureBtn->setText("End Capture");
    m_eventList->setEnabled(false);
    m_eventDetail->setEnabled(false);
    m_prevFrameBtn->setEnabled(false);
    m_nextFrameBtn->setEnabled(false);
    m_memUsageLabel->setText("Capture: 0 bytes");
}

void VramViewer::stopCapture() {
    m_capturing = false;

    rd_DebuggerIf *dif = ar_get_debugger_if();
    sys::gpu_capture_stop(dif);

    m_captureBtn->setText("Capture");
    m_eventList->setEnabled(true);
    m_eventDetail->setEnabled(true);
    m_prevFrameBtn->setEnabled(true);
    m_nextFrameBtn->setEnabled(true);

    m_memUsageLabel->setText(QString("Capture: %1").arg(
        formatBytes(sys::gpu_capture_compressed_bytes())));

    populateEventList();
}

/* ======================================================================== */
/* Populate event list after capture                                         */
/* ======================================================================== */

void VramViewer::populateEventList() {
    m_eventList->clear();

    const auto &events = sys::gpu_capture_events();
    for (unsigned i = 0; i < events.size(); i++) {
        const auto &ev = events[i];

        if (ev.type == sys::GpuCapEvent::FRAME_BOUNDARY) {
            m_eventList->addItem(QString("Frame %1").arg(ev.frame_number));
            continue;
        }

        if (ev.word_count == 0 && i == 0) {
            m_eventList->addItem("(initial VRAM)");
            continue;
        }

        char line[256];
        if (ev.port == 0)
            sys::decode_gp0(line, sizeof(line), ev.words, ev.word_count);
        else
            sys::decode_gp1(line, sizeof(line), ev.words);

        auto *item = new QListWidgetItem(QString::fromUtf8(line));
        if (ev.diff.empty())
            item->setForeground(Qt::gray);
        m_eventList->addItem(item);
    }

    if (!events.empty())
        m_eventList->setCurrentRow(0);
}

/* ======================================================================== */
/* Event selection                                                           */
/* ======================================================================== */

void VramViewer::onEventSelected(int row) {
    const auto &events = sys::gpu_capture_events();

    if (row < 0 || (unsigned)row >= events.size()) {
        m_eventDetail->clear();
        return;
    }

    const auto &ev = events[row];

    QString detail;
    if (ev.type == sys::GpuCapEvent::FRAME_BOUNDARY) {
        detail = QString("Frame boundary %1").arg(ev.frame_number);
    } else {
        char line[256];
        if (ev.word_count > 0) {
            if (ev.port == 0)
                sys::decode_gp0(line, sizeof(line), ev.words, ev.word_count);
            else
                sys::decode_gp1(line, sizeof(line), ev.words);
            detail = QString::fromUtf8(line) + "\n";
        }

        const char *src_name = (ev.source == 0) ? "CPU" :
                               (ev.source == 2) ? "DMA2" : "?";
        detail += QString("Source: %1  PC: %2\n")
            .arg(src_name)
            .arg(ev.pc, 8, 16, QChar('0')).toUpper();

        /* Decoded parameters (vertices, texcoords, colors) */
        if (ev.word_count > 0 && ev.port == 0) {
            char det[1024];
            sys::decode_gp0_detail(det, sizeof(det), ev.words, ev.word_count);
            if (det[0])
                detail += "\n" + QString::fromUtf8(det);
        }

        if (ev.word_count > 0) {
            detail += "\nRaw:";
            for (unsigned i = 0; i < ev.word_count; i++) {
                detail += QString(" %1").arg(ev.words[i], 8, 16, QChar('0')).toUpper();
                if (i < ev.word_count - 1 && ((i + 1) % 4) == 0)
                    detail += "\n    ";
            }
            detail += "\n";
        }

        if (!ev.diff.empty())
            detail += QString("\nDiff: %1").arg(formatBytes(ev.diff.size()));
        if (ev.is_keyframe)
            detail += " (keyframe)";
    }

    m_eventDetail->setPlainText(detail);

    if (ev.diff_w > 0 && ev.diff_h > 0)
        m_vramWidget->setHighlightRect(ev.diff_x, ev.diff_y, ev.diff_w, ev.diff_h);
    else
        m_vramWidget->clearHighlightRect();

    seekToEvent((unsigned)row);
}

/* ======================================================================== */
/* Seek to event — reconstruct VRAM via capture module                       */
/* ======================================================================== */

void VramViewer::seekToEvent(unsigned idx) {
    m_captureVram.resize(VRAM_BYTES);
    if (!sys::gpu_capture_reconstruct(idx, m_captureVram.data()))
        return;
    rebuildImageFromBuffer(m_captureVram.data());
}

/* ======================================================================== */
/* Prev / Next Frame navigation                                              */
/* ======================================================================== */

void VramViewer::prevFrame() {
    const auto &events = sys::gpu_capture_events();
    int cur = m_eventList->currentRow();
    if (cur < 0) cur = (int)events.size();

    for (int i = cur - 1; i >= 0; i--) {
        if (events[i].type == sys::GpuCapEvent::FRAME_BOUNDARY) {
            m_eventList->setCurrentRow(i);
            return;
        }
    }
    if (!events.empty())
        m_eventList->setCurrentRow(0);
}

void VramViewer::nextFrame() {
    const auto &events = sys::gpu_capture_events();
    int cur = m_eventList->currentRow();

    for (unsigned i = (unsigned)(cur + 1); i < events.size(); i++) {
        if (events[i].type == sys::GpuCapEvent::FRAME_BOUNDARY) {
            m_eventList->setCurrentRow((int)i);
            return;
        }
    }
    if (!events.empty())
        m_eventList->setCurrentRow((int)events.size() - 1);
}

/* ======================================================================== */
/* Utility                                                                   */
/* ======================================================================== */

QString VramViewer::formatBytes(size_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024)
        return QString("%1 GiB").arg((double)bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    if (bytes >= 1024 * 1024)
        return QString("%1 MiB").arg((double)bytes / (1024.0 * 1024.0), 0, 'f', 1);
    if (bytes >= 10000)
        return QString("%1 KiB").arg((double)bytes / 1024.0, 0, 'f', 1);
    return QString("%1 bytes").arg(bytes);
}

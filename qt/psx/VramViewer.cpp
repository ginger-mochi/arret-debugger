#include "VramViewer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QRadioButton>
#include <QButtonGroup>
#include <QPainter>
#include <QMouseEvent>

#include "backend.hpp"

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
    /* Texture page grid depends on format.
     * Texture pages are 256 texels wide.
     * In VRAM halfwords: 15-bit = 256hw, 24-bit = 384hw (not evenly
     * divisible), 8-bit = 128hw, 4-bit = 64hw.
     * Page height is always 256 lines. */
    ph = 256;
    rows = 2; // 512 / 256

    switch (m_format) {
    case FMT_15BIT:
        /* 256 pixels = 256 halfwords → 1024/256 = 4 columns */
        pw = 256; cols = 4; break;
    case FMT_8BIT:
        /* 256 texels = 128 halfwords → 2048 pixels / 256 = 8 columns */
        pw = 256; cols = 8; break;
    case FMT_4BIT:
        /* 256 texels = 64 halfwords → 4096 pixels / 256 = 16 columns */
        pw = 256; cols = 16; break;
    case FMT_24BIT:
        /* 256 pixels × 1.5 bytes = 384 halfwords; doesn't tile evenly.
         * Approximate: use ~227-pixel pages (682/3 ≈ 227) */
        pw = 227; cols = 3; break;
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

    /* Draw texture page highlight */
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

    setWidget(main);
    resize(720, 620);

    connect(m_formatGroup, &QButtonGroup::idClicked,
            this, &VramViewer::formatChanged);
    connect(m_vramWidget, &VramWidget::clicked,
            this, &VramViewer::onVramClicked);

    rebuildImage();
}

void VramViewer::refresh() {
    rebuildImage();
}

void VramViewer::formatChanged(int id) {
    m_format = (VramWidget::Format)id;
    m_vramWidget->setFormat(m_format);
    m_vramWidget->setSelectedPage(-1);
    m_pageLabel->setText("Click VRAM to select texture page");
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

    /* Compute VRAM halfword address of the page origin.
     * Each page starts at (page_x_hw, page_y_line).
     * page_x_hw depends on format. */
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

void VramViewer::rebuildImage() {
    if (!ar_has_debug() || !ar_content_loaded()) return;

    /* Find VRAM memory region */
    m_vramMem = ar_find_memory_by_id("vram");
    if (!m_vramMem) return;

    /* VRAM is 1024 halfwords wide × 512 lines = 1MB byte-addressed.
     * Read into a local buffer for fast access. */
    constexpr int VRAM_BYTES = 1048576;
    constexpr int VRAM_HW_W = 1024;
    constexpr int VRAM_H = 512;

    /* Read all VRAM bytes into a local buffer (faster than per-pixel peek) */
    QByteArray vramBuf(VRAM_BYTES, 0);
    uint8_t *vram = reinterpret_cast<uint8_t *>(vramBuf.data());
    for (int i = 0; i < VRAM_BYTES; i++)
        vram[i] = m_vramMem->v1.peek(m_vramMem, i, false);

    /* Helper: read halfword at (hw_x, line) */
    auto hw = [&](int hw_x, int line) -> uint16_t {
        int byte_addr = (line * VRAM_HW_W + hw_x) * 2;
        if (byte_addr + 1 >= VRAM_BYTES) return 0;
        return (uint16_t)vram[byte_addr] | ((uint16_t)vram[byte_addr + 1] << 8);
    };

    QImage img;

    switch (m_format) {
    case VramWidget::FMT_15BIT: {
        /* 1024 x 512, RGB555 — 1 pixel per halfword */
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
        /* 682 x 512, RGB888 packed — 3 bytes per pixel, 2 pixels per 3 halfwords */
        int imgW = (VRAM_HW_W * 2) / 3; // 682
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
        /* 2048 x 512, 8-bit indexed — shown as grayscale */
        int imgW = VRAM_HW_W * 2; // 2048
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
        /* 4096 x 512, 4-bit indexed — shown as grayscale (0-15 scaled to 0-255) */
        int imgW = VRAM_HW_W * 4; // 4096
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
                uint8_t v = nibble * 17; // 0→0, 15→255
                scanline[px] = 0xFF000000 | (v << 16) | (v << 8) | v;
            }
        }
        break;
    }
    }

    m_vramWidget->setFormat(m_format);
    m_vramWidget->setImage(img);
}

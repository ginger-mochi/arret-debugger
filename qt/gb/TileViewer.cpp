#include "TileViewer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QPainter>
#include <QMouseEvent>
#include <QFontDatabase>

#include "backend.hpp"

/* ======================================================================== */
/* TileGridWidget                                                            */
/* ======================================================================== */

TileGridWidget::TileGridWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(false);
}

void TileGridWidget::setTileSet(const gb::TileSet &ts) {
    m_tileSet = ts;
    rebuildImage();
    setFixedSize(sizeHint());
    update();
}

QSize TileGridWidget::sizeHint() const {
    int w = COLS * tileSize();
    int sections = BLOCKS_PER_BANK * m_tileSet.banks;
    int h = sections * sectionHeight();
    if (h == 0) h = 100;
    return QSize(w, h);
}

int TileGridWidget::tileAtPos(const QPoint &pos) const {
    int sections = BLOCKS_PER_BANK * m_tileSet.banks;
    if (sections == 0) return -1;

    int ts = tileSize();
    int col = pos.x() / ts;
    if (col < 0 || col >= COLS) return -1;

    int y = pos.y();
    int secH = sectionHeight();
    int section = y / secH;
    if (section < 0 || section >= sections) return -1;

    int yInSection = y - section * secH;
    int yInGrid = yInSection - SECTION_LABEL_H - SECTION_GAP;
    if (yInGrid < 0) return -1;

    int row = yInGrid / ts;
    if (row < 0 || row >= rowsPerBlock()) return -1;

    int tileInBlock = row * COLS + col;
    int tileIndex = section * TILES_PER_BLOCK + tileInBlock;
    if (tileIndex < 0 || tileIndex >= (int)m_tileSet.tiles.size())
        return -1;

    return tileIndex;
}

void TileGridWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    int idx = tileAtPos(event->pos());
    if (idx >= 0) {
        m_selected = idx;
        update();
        emit tileClicked(idx);
    }
}

void TileGridWidget::rebuildImage() {
    int sections = BLOCKS_PER_BANK * m_tileSet.banks;
    if (sections == 0 || m_tileSet.tiles.empty()) {
        m_image = QImage();
        return;
    }

    int w = COLS * TILE_PX;
    int totalTileRows = sections * rowsPerBlock();
    int h = totalTileRows * TILE_PX;

    m_image = QImage(w, h, QImage::Format_ARGB32);
    m_image.fill(0xFF808080);

    for (int section = 0; section < sections; section++) {
        int baseIdx = section * TILES_PER_BLOCK;
        for (int t = 0; t < TILES_PER_BLOCK && baseIdx + t < (int)m_tileSet.tiles.size(); t++) {
            const auto &tile = m_tileSet.tiles[baseIdx + t];
            int col = t % COLS;
            int row = t / COLS;
            int px0 = col * TILE_PX;
            int py0 = (section * rowsPerBlock() + row) * TILE_PX;

            for (int py = 0; py < TILE_PX; py++) {
                auto *scanline = reinterpret_cast<uint32_t *>(m_image.scanLine(py0 + py));
                for (int px = 0; px < TILE_PX; px++) {
                    scanline[px0 + px] = GB_PALETTE[tile.pixels[py * 8 + px] & 3];
                }
            }
        }
    }
}

void TileGridWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    if (m_image.isNull()) {
        p.fillRect(rect(), Qt::gray);
        return;
    }

    int sections = BLOCKS_PER_BANK * m_tileSet.banks;
    int ts = tileSize();

    static const char *blockLabels[] = {
        "Block 0 (8000-87FF)",
        "Block 1 (8800-8FFF)",
        "Block 2 (9000-97FF)",
    };

    QFont labelFont = font();
    labelFont.setPointSize(8);
    p.setFont(labelFont);

    for (int section = 0; section < sections; section++) {
        int bank = section / BLOCKS_PER_BANK;
        int block = section % BLOCKS_PER_BANK;

        int y0 = section * sectionHeight();

        // Section label
        QString label;
        if (m_tileSet.banks > 1)
            label = QString("Bank %1 - %2").arg(bank).arg(blockLabels[block]);
        else
            label = QString(blockLabels[block]);

        p.setPen(Qt::white);
        p.fillRect(0, y0, COLS * ts, SECTION_LABEL_H, QColor(0x40, 0x40, 0x40));
        p.drawText(4, y0, COLS * ts - 4, SECTION_LABEL_H, Qt::AlignVCenter, label);

        // Draw tile image for this section
        int imgRow0 = section * rowsPerBlock() * TILE_PX;
        int imgH = rowsPerBlock() * TILE_PX;
        QRect src(0, imgRow0, COLS * TILE_PX, imgH);
        QRect dst(0, y0 + SECTION_LABEL_H + SECTION_GAP, COLS * ts, rowsPerBlock() * ts);
        p.drawImage(dst, m_image, src);
    }

    // Selection highlight
    if (m_selected >= 0 && m_selected < (int)m_tileSet.tiles.size()) {
        int section = m_selected / TILES_PER_BLOCK;
        int inBlock = m_selected % TILES_PER_BLOCK;
        int col = inBlock % COLS;
        int row = inBlock / COLS;

        int x = col * ts;
        int y = section * sectionHeight() + SECTION_LABEL_H + SECTION_GAP + row * ts;

        p.setPen(QPen(Qt::red, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(x, y, ts, ts);
    }
}

/* ======================================================================== */
/* TileViewer                                                                */
/* ======================================================================== */

TileViewer::TileViewer(QWidget *parent)
    : QDockWidget("Tile Viewer", parent)
{
    setObjectName("TileViewer");

    auto *main = new QWidget;
    auto *hbox = new QHBoxLayout(main);
    hbox->setContentsMargins(4, 4, 4, 4);

    // Left: scrollable tile grid
    m_grid = new TileGridWidget;
    m_scrollArea = new QScrollArea;
    m_scrollArea->setWidget(m_grid);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setMinimumWidth(16 * 8 * 2 + 20);
    hbox->addWidget(m_scrollArea, 1);

    // Right: sidebar
    auto *sidebar = new QWidget;
    sidebar->setFixedWidth(180);
    auto *svbox = new QVBoxLayout(sidebar);
    svbox->setContentsMargins(4, 4, 4, 4);
    svbox->setSpacing(4);

    svbox->addWidget(new QLabel("<b>Tile Info</b>"));

    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(9);

    m_infoIndex = new QLabel("Index: -");
    m_infoIndex->setFont(monoFont);
    svbox->addWidget(m_infoIndex);

    m_infoAddr = new QLabel("Address: -");
    m_infoAddr->setFont(monoFont);
    svbox->addWidget(m_infoAddr);

    m_infoBank = new QLabel("Bank: -");
    m_infoBank->setFont(monoFont);
    svbox->addWidget(m_infoBank);

    m_infoBlock = new QLabel("Block: -");
    m_infoBlock->setFont(monoFont);
    svbox->addWidget(m_infoBlock);

    svbox->addWidget(new QLabel("<b>Preview</b>"));
    m_infoPreview = new QLabel;
    m_infoPreview->setFixedSize(64, 64);
    m_infoPreview->setFrameShape(QFrame::Box);
    m_infoPreview->setAlignment(Qt::AlignCenter);
    svbox->addWidget(m_infoPreview);

    svbox->addWidget(new QLabel("<b>Bytes</b>"));
    m_infoHex = new QLabel("-");
    m_infoHex->setFont(monoFont);
    m_infoHex->setWordWrap(true);
    svbox->addWidget(m_infoHex);

    svbox->addStretch();
    hbox->addWidget(sidebar);

    setWidget(main);
    resize(480, 500);

    connect(m_grid, &TileGridWidget::tileClicked, this, &TileViewer::onTileClicked);

    readTiles();
}

void TileViewer::refresh() {
    readTiles();
}

void TileViewer::readTiles() {
    if (!ar_has_debug() || !ar_content_loaded())
        return;

    rd_System const *sys = ar_debug_system();
    if (!sys || !sys->v1.description) return;

    rd_Memory const *mem = ar_debug_mem();
    if (!mem) return;

    gb::TileSet ts = gb::read_tiles(mem, sys->v1.description);
    m_grid->setTileSet(ts);
    updateSidebar();
}

void TileViewer::onTileClicked(int) {
    updateSidebar();
}

void TileViewer::updateSidebar() {
    int sel = m_grid->selectedTile();
    if (sel < 0) return;

    const gb::TileSet &ts = m_grid->tileSet();
    if (sel >= (int)ts.tiles.size()) return;

    int tilesPerBank = 384;
    int bank = sel / tilesPerBank;
    int inBank = sel % tilesPerBank;
    int block = inBank / 128;
    int inBlock = inBank % 128;

    static constexpr uint64_t blockBase[] = { 0x8000, 0x8800, 0x9000 };
    uint64_t addr = blockBase[block] + (uint64_t)inBlock * 16;

    m_infoIndex->setText(QString("Index: %1").arg(inBank, 3, 16, QChar('0')).toUpper());
    m_infoAddr->setText(QString("Address: %1").arg(addr, 4, 16, QChar('0')).toUpper());
    m_infoBank->setText(QString("Bank: %1").arg(bank));
    m_infoBlock->setText(QString("Block: %1").arg(block));

    // Read raw tile bytes for hex display
    QString hexStr;
    rd_Memory const *mem = ar_debug_mem();
    if (mem && mem->v1.peek) {
        if (bank == 0) {
            for (int i = 0; i < 16; i++) {
                if (i > 0 && i % 8 == 0) hexStr += "\n";
                else if (i > 0) hexStr += " ";
                uint8_t b = mem->v1.peek(mem, addr + i, false);
                hexStr += QString("%1").arg(b, 2, 16, QChar('0')).toUpper();
            }
        } else if (mem->v1.get_bank_address) {
            rd_MemoryMap mapping;
            if (mem->v1.get_bank_address(mem, addr, bank, &mapping) && mapping.source) {
                uint64_t offset = addr - 0x8000;
                for (int i = 0; i < 16; i++) {
                    if (i > 0 && i % 8 == 0) hexStr += "\n";
                    else if (i > 0) hexStr += " ";
                    uint8_t b = mapping.source->v1.peek(mapping.source,
                                    mapping.source_base_addr + offset + i, false);
                    hexStr += QString("%1").arg(b, 2, 16, QChar('0')).toUpper();
                }
            }
        }
    }
    m_infoHex->setText(hexStr.isEmpty() ? "-" : hexStr);

    // Zoomed preview: render the selected tile at 8x
    const auto &tile = ts.tiles[sel];
    QImage preview(8, 8, QImage::Format_ARGB32);
    for (int y = 0; y < 8; y++) {
        auto *scanline = reinterpret_cast<uint32_t *>(preview.scanLine(y));
        for (int x = 0; x < 8; x++)
            scanline[x] = GB_PALETTE[tile.pixels[y * 8 + x] & 3];
    }
    QPixmap pm = QPixmap::fromImage(preview.scaled(64, 64, Qt::KeepAspectRatio,
                                                    Qt::FastTransformation));
    m_infoPreview->setPixmap(pm);
}

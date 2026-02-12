#include "TilemapViewer.h"
#include "TileViewer.h" // for GB_PALETTE[]

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QRadioButton>
#include <QCheckBox>
#include <QButtonGroup>
#include <QPainter>
#include <QMouseEvent>
#include <QFontDatabase>

#include "backend.hpp"

/* ======================================================================== */
/* TilemapGridWidget                                                         */
/* ======================================================================== */

TilemapGridWidget::TilemapGridWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setFixedSize(sizeHint());
}

void TilemapGridWidget::setImage(const QImage &img) {
    m_image = img;
    update();
}

QSize TilemapGridWidget::sizeHint() const {
    return QSize(MAP_DIM * TILE_PX * SCALE, MAP_DIM * TILE_PX * SCALE);
}

void TilemapGridWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    int ts = TILE_PX * SCALE;
    int col = event->pos().x() / ts;
    int row = event->pos().y() / ts;
    if (col >= 0 && col < MAP_DIM && row >= 0 && row < MAP_DIM) {
        m_selRow = row;
        m_selCol = col;
        update();
        emit tileClicked(row, col);
    }
}

void TilemapGridWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    if (m_image.isNull()) {
        p.fillRect(rect(), Qt::gray);
        return;
    }

    // Draw the tilemap image scaled 2x
    p.drawImage(QRect(0, 0, MAP_DIM * TILE_PX * SCALE, MAP_DIM * TILE_PX * SCALE),
                m_image);

    // Selection highlight
    if (m_selRow >= 0 && m_selCol >= 0) {
        int ts = TILE_PX * SCALE;
        int x = m_selCol * ts;
        int y = m_selRow * ts;
        p.setPen(QPen(Qt::red, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(x, y, ts, ts);
    }
}

/* ======================================================================== */
/* TilemapViewer                                                             */
/* ======================================================================== */

static constexpr int TILES_PER_BANK = 384;

// Resolve a tilemap entry to a TileSet index
static int resolve_tile_index(uint8_t tile_index, uint8_t lcdc,
                              uint8_t vram_bank, bool is_gbc) {
    int idx;
    if (lcdc & 0x10) {
        // Unsigned addressing: 0x8000 base, tiles 0-255 = blocks 0+1
        idx = tile_index;
    } else {
        // Signed addressing: 0x9000 base, (int8_t)tile_index
        idx = 256 + (int8_t)tile_index;
    }
    if (is_gbc && vram_bank == 1)
        idx += TILES_PER_BANK;
    return idx;
}

// Compute the VRAM address of tile data
static uint16_t tile_data_address(uint8_t tile_index, uint8_t lcdc) {
    if (lcdc & 0x10)
        return 0x8000 + (uint16_t)tile_index * 16;
    else
        return (uint16_t)(0x9000 + (int8_t)tile_index * 16);
}

TilemapViewer::TilemapViewer(QWidget *parent)
    : QDockWidget("Tilemap Viewer", parent)
{
    setObjectName("TilemapViewer");
    memset(&m_mapData, 0, sizeof(m_mapData));
    memset(m_gbPal, 0, sizeof(m_gbPal));
    memset(m_gbcPal, 0, sizeof(m_gbcPal));

    auto *main = new QWidget;
    auto *hbox = new QHBoxLayout(main);
    hbox->setContentsMargins(4, 4, 4, 4);

    // Left: scrollable tilemap grid
    m_grid = new TilemapGridWidget;
    m_scrollArea = new QScrollArea;
    m_scrollArea->setWidget(m_grid);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setMinimumWidth(32 * 8 * 2 + 20);
    hbox->addWidget(m_scrollArea, 1);

    // Right: sidebar
    auto *sidebar = new QWidget;
    sidebar->setFixedWidth(200);
    auto *svbox = new QVBoxLayout(sidebar);
    svbox->setContentsMargins(4, 4, 4, 4);
    svbox->setSpacing(4);

    // Map selection
    svbox->addWidget(new QLabel("<b>Tilemap</b>"));
    m_map0Radio = new QRadioButton("Tilemap 0 (9800)");
    m_map1Radio = new QRadioButton("Tilemap 1 (9C00)");
    m_map0Radio->setChecked(true);
    auto *mapGroup = new QButtonGroup(this);
    mapGroup->addButton(m_map0Radio, 0);
    mapGroup->addButton(m_map1Radio, 1);
    svbox->addWidget(m_map0Radio);
    svbox->addWidget(m_map1Radio);

    connect(m_map0Radio, &QRadioButton::toggled, this, &TilemapViewer::onMapChanged);

    // Tile addressing mode
    svbox->addSpacing(4);
    svbox->addWidget(new QLabel("<b>Tile Data</b>"));
    m_addrAutoRadio = new QRadioButton("Auto (LCDC)");
    m_addr8000Radio = new QRadioButton("8000 mode");
    m_addr9000Radio = new QRadioButton("9000 mode");
    m_addrAutoRadio->setChecked(true);
    auto *addrGroup = new QButtonGroup(this);
    addrGroup->addButton(m_addrAutoRadio, 0);
    addrGroup->addButton(m_addr8000Radio, 1);
    addrGroup->addButton(m_addr9000Radio, 2);
    svbox->addWidget(m_addrAutoRadio);
    svbox->addWidget(m_addr8000Radio);
    svbox->addWidget(m_addr9000Radio);

    connect(m_addrAutoRadio, &QRadioButton::toggled, this, &TilemapViewer::onAddrModeChanged);
    connect(m_addr8000Radio, &QRadioButton::toggled, this, &TilemapViewer::onAddrModeChanged);

    // Palette checkbox
    m_paletteCheck = new QCheckBox("Use BG palette");
    svbox->addWidget(m_paletteCheck);

    connect(m_paletteCheck, &QCheckBox::toggled, this, &TilemapViewer::onPaletteToggled);

    svbox->addSpacing(8);

    // Tile info section
    svbox->addWidget(new QLabel("<b>Tile Info</b>"));

    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(9);

    m_infoNametable = new QLabel("Nametable: -");
    m_infoNametable->setFont(monoFont);
    svbox->addWidget(m_infoNametable);

    m_infoTileIndex = new QLabel("Tile index: -");
    m_infoTileIndex->setFont(monoFont);
    svbox->addWidget(m_infoTileIndex);

    m_infoTileAddr = new QLabel("Tile data: -");
    m_infoTileAddr->setFont(monoFont);
    svbox->addWidget(m_infoTileAddr);

    m_infoGbcPalette = new QLabel("Palette: -");
    m_infoGbcPalette->setFont(monoFont);
    svbox->addWidget(m_infoGbcPalette);

    m_infoGbcBank = new QLabel("VRAM bank: -");
    m_infoGbcBank->setFont(monoFont);
    svbox->addWidget(m_infoGbcBank);

    m_infoGbcFlip = new QLabel("Flip: -");
    m_infoGbcFlip->setFont(monoFont);
    svbox->addWidget(m_infoGbcFlip);

    svbox->addSpacing(8);

    // Zoomed preview
    svbox->addWidget(new QLabel("<b>Preview</b>"));
    m_infoPreview = new QLabel;
    m_infoPreview->setFixedSize(64, 64);
    m_infoPreview->setFrameShape(QFrame::Box);
    m_infoPreview->setAlignment(Qt::AlignCenter);
    svbox->addWidget(m_infoPreview);

    svbox->addStretch();
    hbox->addWidget(sidebar);

    setWidget(main);
    resize(560, 540);

    connect(m_grid, &TilemapGridWidget::tileClicked, this, &TilemapViewer::onTileClicked);

    readData();
}

void TilemapViewer::refresh() {
    readData();
}

void TilemapViewer::readData() {
    if (!ar_has_debug() || !ar_content_loaded())
        return;

    rd_System const *sys = ar_debug_system();
    if (!sys || !sys->v1.description) return;

    rd_Memory const *mem = ar_debug_mem();
    if (!mem) return;

    int mapIdx = m_map1Radio->isChecked() ? 1 : 0;
    m_mapData = gb::read_tilemap(mem, sys->v1.description, mapIdx);
    m_tileSet = gb::read_tiles(mem, sys->v1.description);

    // Read palettes
    gb::read_gb_palette(mem, m_gbPal);
    rd_Memory const *bgpal = ar_find_memory_by_id("bgpal");
    m_gbcPalValid = gb::read_gbc_palette(bgpal, m_gbcPal);

    // On first load, set palette checkbox default
    if (m_firstLoad) {
        m_paletteCheck->setChecked(m_mapData.is_gbc);
        m_firstLoad = false;
    }

    rebuildImage();
    updateSidebar();
}

void TilemapViewer::rebuildImage() {
    if (m_tileSet.tiles.empty()) {
        m_grid->setImage(QImage());
        return;
    }

    bool usePal = m_paletteCheck->isChecked();
    QImage img(256, 256, QImage::Format_ARGB32);
    img.fill(0xFF808080);

    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 32; col++) {
            const gb::TilemapEntry &e = m_mapData.entries[row][col];

            uint8_t lcdc = effectiveLcdc();
            int tileIdx = resolve_tile_index(e.tile_index, lcdc,
                                             e.has_attrs ? e.vram_bank : 0,
                                             m_mapData.is_gbc);
            if (tileIdx < 0 || tileIdx >= (int)m_tileSet.tiles.size())
                continue;

            const gb::TileImage &tile = m_tileSet.tiles[tileIdx];
            int px0 = col * 8;
            int py0 = row * 8;

            for (int py = 0; py < 8; py++) {
                int srcY = (e.has_attrs && e.v_flip) ? (7 - py) : py;
                auto *scanline = reinterpret_cast<uint32_t *>(img.scanLine(py0 + py));
                for (int px = 0; px < 8; px++) {
                    int srcX = (e.has_attrs && e.h_flip) ? (7 - px) : px;
                    uint8_t cidx = tile.pixels[srcY * 8 + srcX] & 3;

                    uint32_t color;
                    if (usePal && m_mapData.is_gbc && m_gbcPalValid && e.has_attrs) {
                        color = m_gbcPal[e.palette][cidx];
                    } else if (usePal) {
                        color = m_gbPal[cidx];
                    } else {
                        color = GB_PALETTE[cidx];
                    }
                    scanline[px0 + px] = color;
                }
            }
        }
    }

    m_grid->setImage(img);
}

void TilemapViewer::onTileClicked(int, int) {
    updateSidebar();
}

void TilemapViewer::onMapChanged() {
    readData();
}

void TilemapViewer::onAddrModeChanged() {
    rebuildImage();
    updateSidebar();
}

void TilemapViewer::onPaletteToggled() {
    rebuildImage();
    updateSidebar();
}

uint8_t TilemapViewer::effectiveLcdc() const {
    if (m_addr8000Radio->isChecked())
        return m_mapData.lcdc | 0x10;   // force unsigned (8000)
    if (m_addr9000Radio->isChecked())
        return m_mapData.lcdc & ~0x10;  // force signed (9000)
    return m_mapData.lcdc;              // auto
}

void TilemapViewer::updateSidebar() {
    int row = m_grid->selectedRow();
    int col = m_grid->selectedCol();
    if (row < 0 || col < 0) return;

    const gb::TilemapEntry &e = m_mapData.entries[row][col];

    uint16_t mapBase = m_map1Radio->isChecked() ? 0x9C00 : 0x9800;
    uint16_t nametableAddr = mapBase + row * 32 + col;
    uint8_t lcdc = effectiveLcdc();
    uint16_t tileAddr = tile_data_address(e.tile_index, lcdc);

    m_infoNametable->setText(
        QString("Nametable: %1").arg(nametableAddr, 4, 16, QChar('0')).toUpper());
    m_infoTileIndex->setText(
        QString("Tile index: %1").arg(e.tile_index, 2, 16, QChar('0')).toUpper());
    m_infoTileAddr->setText(
        QString("Tile data: %1").arg(tileAddr, 4, 16, QChar('0')).toUpper());

    if (e.has_attrs) {
        m_infoGbcPalette->setText(QString("Palette: %1").arg(e.palette));
        m_infoGbcPalette->show();
        m_infoGbcBank->setText(QString("VRAM bank: %1").arg(e.vram_bank));
        m_infoGbcBank->show();
        QString flipStr;
        if (e.h_flip && e.v_flip) flipStr = "H+V";
        else if (e.h_flip) flipStr = "H";
        else if (e.v_flip) flipStr = "V";
        else flipStr = "none";
        m_infoGbcFlip->setText(QString("Flip: %1").arg(flipStr));
        m_infoGbcFlip->show();
    } else {
        m_infoGbcPalette->hide();
        m_infoGbcBank->hide();
        m_infoGbcFlip->hide();
    }

    // Zoomed preview: render the selected tile at 8x
    int tileIdx = resolve_tile_index(e.tile_index, lcdc,
                                     e.has_attrs ? e.vram_bank : 0,
                                     m_mapData.is_gbc);
    if (tileIdx >= 0 && tileIdx < (int)m_tileSet.tiles.size()) {
        const gb::TileImage &tile = m_tileSet.tiles[tileIdx];
        bool usePal = m_paletteCheck->isChecked();
        QImage preview(8, 8, QImage::Format_ARGB32);
        for (int py = 0; py < 8; py++) {
            int srcY = (e.has_attrs && e.v_flip) ? (7 - py) : py;
            auto *scanline = reinterpret_cast<uint32_t *>(preview.scanLine(py));
            for (int px = 0; px < 8; px++) {
                int srcX = (e.has_attrs && e.h_flip) ? (7 - px) : px;
                uint8_t cidx = tile.pixels[srcY * 8 + srcX] & 3;
                uint32_t color;
                if (usePal && m_mapData.is_gbc && m_gbcPalValid && e.has_attrs)
                    color = m_gbcPal[e.palette][cidx];
                else if (usePal)
                    color = m_gbPal[cidx];
                else
                    color = GB_PALETTE[cidx];
                scanline[px] = color;
            }
        }
        QPixmap pm = QPixmap::fromImage(preview.scaled(64, 64, Qt::KeepAspectRatio,
                                                       Qt::FastTransformation));
        m_infoPreview->setPixmap(pm);
    }
}

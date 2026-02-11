#ifndef GB_TILEVIEWER_H
#define GB_TILEVIEWER_H

#include <QDockWidget>
#include <QWidget>
#include <QImage>
#include <vector>
#include <cstdint>

#include "gb/tiles.hpp"

QT_BEGIN_NAMESPACE
class QLabel;
class QScrollArea;
QT_END_NAMESPACE

// Palette: 2bpp greyscale (ARGB32)
inline constexpr uint32_t GB_PALETTE[4] = {
    0xFFFFFFFF, // 0 = white
    0xFFAAAAAA, // 1 = light grey
    0xFF555555, // 2 = dark grey
    0xFF000000, // 3 = black
};

/* ======================================================================== */
/* TileGridWidget — custom widget that renders the tile grid                 */
/* ======================================================================== */

class TileGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit TileGridWidget(QWidget *parent = nullptr);

    void setTileSet(const gb::TileSet &ts);
    const gb::TileSet &tileSet() const { return m_tileSet; }
    int selectedTile() const { return m_selected; }

    QSize sizeHint() const override;

signals:
    void tileClicked(int index); // index into flat tiles array

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    static constexpr int TILE_PX = 8;
    static constexpr int SCALE = 2;
    static constexpr int COLS = 16;
    static constexpr int TILES_PER_BLOCK = 128;
    static constexpr int BLOCKS_PER_BANK = 3;
    static constexpr int SECTION_LABEL_H = 18;
    static constexpr int SECTION_GAP = 4;

    int tileSize() const { return TILE_PX * SCALE; }
    int rowsPerBlock() const { return TILES_PER_BLOCK / COLS; } // 8
    int blockPixelHeight() const { return rowsPerBlock() * tileSize(); }
    int sectionHeight() const { return SECTION_LABEL_H + SECTION_GAP + blockPixelHeight(); }

    int tileAtPos(const QPoint &pos) const;

    gb::TileSet m_tileSet;
    QImage m_image;
    int m_selected = -1;

    void rebuildImage();
};

/* ======================================================================== */
/* TileViewer — dock widget                                                  */
/* ======================================================================== */

class TileViewer : public QDockWidget {
    Q_OBJECT
public:
    explicit TileViewer(QWidget *parent = nullptr);

    void refresh(); // called each tick

private slots:
    void onTileClicked(int index);

private:
    void readTiles();
    void updateSidebar();

    TileGridWidget *m_grid;
    QScrollArea    *m_scrollArea;

    // Sidebar labels
    QLabel *m_infoIndex;
    QLabel *m_infoAddr;
    QLabel *m_infoBank;
    QLabel *m_infoBlock;
    QLabel *m_infoPreview;
    QLabel *m_infoHex;
};

#endif // GB_TILEVIEWER_H

#ifndef GB_TILEMAPVIEWER_H
#define GB_TILEMAPVIEWER_H

#include <QDockWidget>
#include <QWidget>
#include <QImage>
#include <cstdint>

#include "gb/tiles.hpp"
#include "gb/tilemaps.hpp"

QT_BEGIN_NAMESPACE
class QLabel;
class QScrollArea;
class QRadioButton;
class QCheckBox;
QT_END_NAMESPACE

/* ======================================================================== */
/* TilemapGridWidget — renders the 32x32 tilemap as a 256x256 image (2x)    */
/* ======================================================================== */

class TilemapGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit TilemapGridWidget(QWidget *parent = nullptr);

    void setImage(const QImage &img);
    int selectedRow() const { return m_selRow; }
    int selectedCol() const { return m_selCol; }

    QSize sizeHint() const override;

signals:
    void tileClicked(int row, int col);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    static constexpr int TILE_PX = 8;
    static constexpr int SCALE = 2;
    static constexpr int MAP_DIM = 32;

    QImage m_image;
    int m_selRow = -1;
    int m_selCol = -1;
};

/* ======================================================================== */
/* TilemapViewer — dock widget                                               */
/* ======================================================================== */

class TilemapViewer : public QDockWidget {
    Q_OBJECT
public:
    explicit TilemapViewer(QWidget *parent = nullptr);

    void refresh(); // called each tick

private slots:
    void onTileClicked(int row, int col);
    void onMapChanged();
    void onAddrModeChanged();
    void onPaletteToggled();

private:
    void readData();
    void rebuildImage();
    void updateSidebar();
    uint8_t effectiveLcdc() const;

    TilemapGridWidget *m_grid;
    QScrollArea       *m_scrollArea;
    QRadioButton      *m_map0Radio;
    QRadioButton      *m_map1Radio;
    QRadioButton      *m_addrAutoRadio;
    QRadioButton      *m_addr8000Radio;
    QRadioButton      *m_addr9000Radio;
    QCheckBox         *m_paletteCheck;

    // Sidebar labels
    QLabel *m_infoNametable;
    QLabel *m_infoTileIndex;
    QLabel *m_infoTileAddr;
    QLabel *m_infoGbcPalette;
    QLabel *m_infoGbcBank;
    QLabel *m_infoGbcFlip;
    QLabel *m_infoPreview;

    // Cached data
    gb::TilemapData m_mapData;
    gb::TileSet     m_tileSet;
    uint32_t        m_gbPal[4];
    uint32_t        m_gbcPal[8][4];
    bool            m_gbcPalValid = false;
    bool            m_firstLoad = true;
};

#endif // GB_TILEMAPVIEWER_H

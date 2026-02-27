#ifndef PSX_VRAMVIEWER_H
#define PSX_VRAMVIEWER_H

#include <QDockWidget>
#include <QWidget>
#include <QImage>
#include <cstdint>

QT_BEGIN_NAMESPACE
class QLabel;
class QScrollArea;
class QButtonGroup;
class QRadioButton;
QT_END_NAMESPACE

struct rd_Memory;

/* ======================================================================== */
/* VramWidget — custom widget that paints the VRAM image                     */
/* ======================================================================== */

class VramWidget : public QWidget {
    Q_OBJECT
public:
    explicit VramWidget(QWidget *parent = nullptr);

    void setImage(const QImage &img);
    void setSelectedPage(int page) { m_selectedPage = page; update(); }
    int selectedPage() const { return m_selectedPage; }

    /* Format affects texture page grid */
    enum Format { FMT_15BIT, FMT_24BIT, FMT_8BIT, FMT_4BIT };
    void setFormat(Format fmt) { m_format = fmt; }

    QSize sizeHint() const override;

    /* Returns page grid dimensions (columns, rows, page_w_px, page_h_px) */
    void pageGrid(int &cols, int &rows, int &pw, int &ph) const;

signals:
    void clicked(int x, int y); // pixel coordinates in the image

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QImage m_image;
    int m_selectedPage = -1;
    Format m_format = FMT_15BIT;
};

/* ======================================================================== */
/* VramViewer — dock widget                                                  */
/* ======================================================================== */

class VramViewer : public QDockWidget {
    Q_OBJECT
public:
    explicit VramViewer(QWidget *parent = nullptr);

    void refresh();

private slots:
    void formatChanged(int id);
    void onVramClicked(int x, int y);

private:
    void rebuildImage();

    VramWidget   *m_vramWidget;
    QScrollArea  *m_scrollArea;
    QLabel       *m_pageLabel;

    QRadioButton *m_radio15;
    QRadioButton *m_radio24;
    QRadioButton *m_radio8;
    QRadioButton *m_radio4;
    QButtonGroup *m_formatGroup;

    rd_Memory const *m_vramMem = nullptr;
    VramWidget::Format m_format = VramWidget::FMT_15BIT;
};

#endif // PSX_VRAMVIEWER_H

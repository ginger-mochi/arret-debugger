#ifndef PSX_VRAMVIEWER_H
#define PSX_VRAMVIEWER_H

#include <QDockWidget>
#include <QWidget>
#include <QImage>
#include <cstdint>
#include <vector>

QT_BEGIN_NAMESPACE
class QLabel;
class QScrollArea;
class QButtonGroup;
class QRadioButton;
class QGroupBox;
class QListWidget;
class QTextEdit;
class QPushButton;
class QSplitter;
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

    void setHighlightRect(int x, int y, int w, int h) {
        m_hlX = x; m_hlY = y; m_hlW = w; m_hlH = h;
        m_hasHighlight = true; update();
    }
    void clearHighlightRect() {
        if (!m_hasHighlight) return;
        m_hasHighlight = false; update();
    }

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
    int m_hlX = 0, m_hlY = 0, m_hlW = 0, m_hlH = 0;
    bool m_hasHighlight = false;
};

/* ======================================================================== */
/* VramViewer — dock widget                                                  */
/* ======================================================================== */

class VramViewer : public QDockWidget {
    Q_OBJECT
public:
    explicit VramViewer(QWidget *parent = nullptr);
    ~VramViewer();

    void refresh();

private slots:
    void formatChanged(int id);
    void onVramClicked(int x, int y);
    void gpuLogToggled(bool checked);
    void onEventSelected(int row);
    void startCapture();
    void stopCapture();
    void prevFrame();
    void nextFrame();

private:
    void rebuildImage();
    void readLiveVram(std::vector<uint8_t> &buf);
    void rebuildImageFromBuffer(const uint8_t *vram);
    void checkGpuLogAvailability();
    void populateEventList();
    void seekToEvent(unsigned idx);
    static QString formatBytes(size_t bytes);

    /* Existing widgets */
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

    /* GPU Event Log UI */
    QGroupBox    *m_gpuLogGroup;
    QWidget      *m_gpuLogContent;
    QLabel       *m_gpuLogUnavail;
    QListWidget  *m_eventList;
    QTextEdit    *m_eventDetail;
    QPushButton  *m_captureBtn;
    QPushButton  *m_prevFrameBtn;
    QPushButton  *m_nextFrameBtn;
    QLabel       *m_memUsageLabel;

    /* UI-side state */
    std::vector<uint8_t> m_captureVram;   // 1MB, working buffer for seek/display
    bool m_capturing = false;
    bool m_gpuLogAvailable = false;
    bool m_gpuLogChecked = false;
};

#endif // PSX_VRAMVIEWER_H

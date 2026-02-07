#ifndef MEMORYVIEWER_H
#define MEMORYVIEWER_H

#include <QDockWidget>
#include <QWidget>
#include <QDialog>
#include <QScrollBar>
#include <stdint.h>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLineEdit;
class QMenuBar;
class QVBoxLayout;
QT_END_NAMESPACE

struct rd_Memory;

/* ======================================================================== */
/* HexViewState                                                              */
/* ======================================================================== */

struct HexViewState {
    rd_Memory const *mem = nullptr;
    uint64_t baseAddr = 0;
    uint64_t size = 0;

    int64_t selAnchor = -1;
    int64_t selCursor = -1;
    int editNibble = -1;     /* partial hex digit buffer, -1 = none */
    int scrollRow = 0;

    static constexpr int bytesPerRow() { return 16; }

    int64_t selMin() const {
        if (selAnchor < 0 || selCursor < 0) return -1;
        return selAnchor < selCursor ? selAnchor : selCursor;
    }

    int64_t selMax() const {
        if (selAnchor < 0 || selCursor < 0) return -1;
        return selAnchor > selCursor ? selAnchor : selCursor;
    }

    bool hasRange() const {
        return selAnchor >= 0 && selCursor >= 0 && selAnchor != selCursor;
    }
};

/* ======================================================================== */
/* GoToDialog                                                                */
/* ======================================================================== */

class GoToDialog : public QDialog {
    Q_OBJECT
public:
    explicit GoToDialog(QWidget *parent = nullptr);
    uint64_t address() const;

private:
    QLineEdit *m_input;
};

/* ======================================================================== */
/* AddressColumn                                                             */
/* ======================================================================== */

class AddressColumn : public QWidget {
    Q_OBJECT
public:
    explicit AddressColumn(HexViewState *state, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    HexViewState *m_state;
};

/* ======================================================================== */
/* HexArea                                                                   */
/* ======================================================================== */

class HexArea : public QWidget {
    Q_OBJECT
public:
    explicit HexArea(HexViewState *state, QWidget *parent = nullptr);
    QSize sizeHint() const override;

    void ensureVisible(int64_t byteIdx);
    int visibleRows() const;

signals:
    void selectionChanged();
    void scrollRequested(int row);
    void visibleRowsChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    int64_t byteAtPos(const QPoint &pos) const;
    int charW() const;
    int rowHeight() const;
    int totalRows() const;

    HexViewState *m_state;
    bool m_dragging = false;
};

/* ======================================================================== */
/* AsciiColumn                                                               */
/* ======================================================================== */

class AsciiColumn : public QWidget {
    Q_OBJECT
public:
    explicit AsciiColumn(HexViewState *state, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    HexViewState *m_state;
};

/* ======================================================================== */
/* MemoryViewer                                                              */
/* ======================================================================== */

class MemoryViewer : public QDockWidget {
    Q_OBJECT
public:
    explicit MemoryViewer(QWidget *parent = nullptr);

    void refresh();

    /* Navigate to an address. If mem is non-null, switch to that region first.
       If mem is null, use the currently-selected region. */
    void goTo(rd_Memory const *mem, uint64_t addr);

private slots:
    void regionChanged(int index);
    void onGoTo();
    void onCopy();
    void onPaste();
    void onScrollChanged(int value);

private:
    void populateRegions();

    HexViewState   m_state;
    QComboBox     *m_regionCombo;
    AddressColumn *m_addrCol;
    HexArea       *m_hexArea;
    AsciiColumn   *m_asciiCol;
    QScrollBar    *m_scrollBar;
};

#endif // MEMORYVIEWER_H

#include "MemoryViewer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QMenuBar>
#include <QLabel>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QFontDatabase>
#include <QApplication>
#include <QClipboard>
#include <QPushButton>
#include <QVector>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMessageBox>
#include <QInputDialog>
#include <unordered_map>
#include <vector>

#include "backend.hpp"
#include "symbols.hpp"

/* ======================================================================== */
/* GoToDialog                                                                */
/* ======================================================================== */

GoToDialog::GoToDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Go To Address");
    auto *layout = new QVBoxLayout(this);

    m_input = new QLineEdit;
    m_input->setPlaceholderText("0000");
    layout->addWidget(m_input);

    auto *btn = new QPushButton("Jump To");
    layout->addWidget(btn);

    connect(btn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_input, &QLineEdit::returnPressed, this, &QDialog::accept);
}

uint64_t GoToDialog::address() const {
    bool ok;
    uint64_t addr = m_input->text().toULongLong(&ok, 16);
    return ok ? addr : 0;
}

/* ======================================================================== */
/* AddressColumn                                                             */
/* ======================================================================== */

AddressColumn::AddressColumn(HexViewState *state, QWidget *parent)
    : QWidget(parent), m_state(state)
{
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(10);
    setFont(font);
}

QSize AddressColumn::sizeHint() const {
    int cw = fontMetrics().horizontalAdvance('0');
    return QSize(cw * 10 + 4, 100);
}

void AddressColumn::paintEvent(QPaintEvent *) {
    if (!m_state->mem || m_state->size == 0) return;

    QPainter p(this);
    int rh = fontMetrics().height() + 2;
    int ascent = fontMetrics().ascent();
    int rows = height() / rh + 1;
    int totalRows = (int)((m_state->size + 15) / 16);

    for (int i = 0; i < rows; i++) {
        int row = m_state->scrollRow + i;
        if (row >= totalRows) break;

        uint64_t addr = m_state->baseAddr + (uint64_t)row * 16;
        int y = i * rh;

        p.setPen(Qt::darkGray);
        p.drawText(2, y + ascent,
                   QString("%1:").arg(addr, 8, 16, QChar('0')).toUpper());
    }
}

/* ======================================================================== */
/* HexArea                                                                   */
/* ======================================================================== */

HexArea::HexArea(HexViewState *state, QWidget *parent)
    : QWidget(parent), m_state(state)
{
    setFocusPolicy(Qt::StrongFocus);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(10);
    setFont(font);
}

int HexArea::charW() const {
    return fontMetrics().horizontalAdvance('0');
}

int HexArea::rowHeight() const {
    return fontMetrics().height() + 2;
}

int HexArea::totalRows() const {
    if (m_state->size == 0) return 0;
    return (int)((m_state->size + 15) / 16);
}

int HexArea::visibleRows() const {
    int rh = rowHeight();
    if (rh <= 0) return 1;
    return height() / rh;
}

QSize HexArea::sizeHint() const {
    int cw = fontMetrics().horizontalAdvance('0');
    return QSize(cw * 48 + 4, 100);
}

void HexArea::ensureVisible(int64_t byteIdx) {
    if (byteIdx < 0) return;
    int row = (int)(byteIdx / 16);
    int vis = visibleRows();
    if (row < m_state->scrollRow)
        emit scrollRequested(row);
    else if (row >= m_state->scrollRow + vis)
        emit scrollRequested(row - vis + 1);
}

int64_t HexArea::byteAtPos(const QPoint &pos) const {
    int rh = rowHeight();
    int cw = charW();
    if (rh <= 0 || cw <= 0) return -1;

    int visRow = pos.y() / rh;
    int row = m_state->scrollRow + visRow;
    int col = (pos.x() - 2) / (cw * 3);

    if (col < 0) col = 0;
    if (col >= 16) col = 15;
    if (row < 0) return -1;
    if (row >= totalRows()) return -1;

    int64_t idx = (int64_t)row * 16 + col;
    if (idx >= (int64_t)m_state->size) return -1;
    return idx;
}

void HexArea::paintEvent(QPaintEvent *) {
    if (!m_state->mem || m_state->size == 0) return;

    QPainter p(this);
    int rh = rowHeight();
    int cw = charW();
    int ascent = fontMetrics().ascent();
    int vis = visibleRows() + 1;
    int total = totalRows();

    int64_t sMin = m_state->selMin();
    int64_t sMax = m_state->selMax();

    /* Build watchpoint map: address → {exists, anyEnabled}
     * Only if current region is a CPU's memory_region */
    struct WpInfo { bool anyEnabled; };
    std::unordered_map<uint64_t, WpInfo> wpMap;
    {
        rd_System const *sys = ar_debug_system();
        bool regionIsCpuMem = false;
        if (sys) {
            for (unsigned ci = 0; ci < sys->v1.num_cpus; ci++) {
                if (sys->v1.cpus[ci]->v1.memory_region == m_state->mem) {
                    regionIsCpuMem = true;
                    break;
                }
            }
        }
        if (regionIsCpuMem) {
            unsigned bpCount = ar_bp_count();
            if (bpCount > 0) {
                auto *bps = new ar_breakpoint[bpCount];
                unsigned n = ar_bp_list(bps, bpCount);
                for (unsigned bi = 0; bi < n; bi++) {
                    if (!(bps[bi].flags & (AR_BP_READ | AR_BP_WRITE)))
                        continue;
                    auto it = wpMap.find(bps[bi].address);
                    if (it == wpMap.end())
                        wpMap[bps[bi].address] = { bps[bi].enabled };
                    else if (bps[bi].enabled)
                        it->second.anyEnabled = true;
                }
                delete[] bps;
            }
        }
    }

    QColor wpFill(210, 180, 140);
    QPen wpOutline(QColor(210, 180, 140));

    for (int i = 0; i < vis; i++) {
        int row = m_state->scrollRow + i;
        if (row >= total) break;

        int y = i * rh;

        for (int col = 0; col < 16; col++) {
            int64_t idx = (int64_t)row * 16 + col;
            if (idx >= (int64_t)m_state->size) break;

            uint64_t addr = m_state->baseAddr + (uint64_t)idx;
            uint8_t val = m_state->mem->v1.peek(m_state->mem, addr, false);

            int hx = 2 + col * cw * 3;
            bool selected = (sMin >= 0 && idx >= sMin && idx <= sMax);

            if (selected) {
                p.fillRect(hx, y, cw * 2, rh, QColor(60, 120, 200));
                p.setPen(Qt::white);
            } else {
                /* Watchpoint highlighting (selection supersedes) */
                auto wpIt = wpMap.find(addr);
                if (wpIt != wpMap.end()) {
                    if (wpIt->second.anyEnabled) {
                        p.fillRect(hx, y, cw * 2, rh, wpFill);
                    } else {
                        p.setPen(wpOutline);
                        p.drawRect(hx, y, cw * 2 - 1, rh - 1);
                    }
                } else if (ar_sym_has_annotation(m_state->mem->v1.id, addr)) {
                    p.fillRect(hx, y, cw * 2, rh, QColor(200, 230, 200));
                }
                p.setPen(Qt::black);
            }

            /* Show partial edit in red */
            if (idx == m_state->selCursor && m_state->editNibble >= 0) {
                uint8_t display = (uint8_t)(m_state->editNibble << 4) | (val & 0x0F);
                p.setPen(Qt::red);
                p.drawText(hx, y + ascent,
                           QString("%1").arg(display, 2, 16, QChar('0')).toUpper());
            } else {
                p.drawText(hx, y + ascent,
                           QString("%1").arg(val, 2, 16, QChar('0')).toUpper());
            }
        }
    }
}

void HexArea::mousePressEvent(QMouseEvent *event) {
    if (!m_state->mem || event->button() != Qt::LeftButton) return;

    int64_t idx = byteAtPos(event->pos());
    if (idx >= 0) {
        m_state->selAnchor = idx;
        m_state->selCursor = idx;
        m_state->editNibble = -1;
        m_dragging = true;
        setFocus();
        emit selectionChanged();
        update();
    }
}

void HexArea::mouseMoveEvent(QMouseEvent *event) {
    if (!m_dragging || !m_state->mem) return;

    int64_t idx = byteAtPos(event->pos());
    if (idx >= 0 && idx != m_state->selCursor) {
        m_state->selCursor = idx;
        m_state->editNibble = -1;
        emit selectionChanged();
        update();
    }
}

void HexArea::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        m_dragging = false;
}

void HexArea::keyPressEvent(QKeyEvent *event) {
    if (!m_state->mem) {
        QWidget::keyPressEvent(event);
        return;
    }

    int64_t cur = m_state->selCursor;

    switch (event->key()) {
    case Qt::Key_Escape:
        m_state->selAnchor = -1;
        m_state->selCursor = -1;
        m_state->editNibble = -1;
        emit selectionChanged();
        update();
        return;

    case Qt::Key_Right:
        if (m_state->hasRange()) {
            cur = m_state->selMax();
        } else if (cur >= 0) {
            cur++;
        } else {
            cur = 0;
        }
        if (cur >= (int64_t)m_state->size) cur = (int64_t)m_state->size - 1;
        m_state->selAnchor = cur;
        m_state->selCursor = cur;
        m_state->editNibble = -1;
        ensureVisible(cur);
        emit selectionChanged();
        update();
        return;

    case Qt::Key_Left:
        if (m_state->hasRange()) {
            cur = m_state->selMin();
        } else if (cur > 0) {
            cur--;
        } else {
            cur = 0;
        }
        m_state->selAnchor = cur;
        m_state->selCursor = cur;
        m_state->editNibble = -1;
        ensureVisible(cur);
        emit selectionChanged();
        update();
        return;

    case Qt::Key_Down:
        if (m_state->hasRange()) {
            cur = m_state->selMax();
        } else if (cur >= 0) {
            cur += 16;
        } else {
            cur = 0;
        }
        if (cur >= (int64_t)m_state->size) cur = (int64_t)m_state->size - 1;
        m_state->selAnchor = cur;
        m_state->selCursor = cur;
        m_state->editNibble = -1;
        ensureVisible(cur);
        emit selectionChanged();
        update();
        return;

    case Qt::Key_Up:
        if (m_state->hasRange()) {
            cur = m_state->selMin();
        } else if (cur >= 16) {
            cur -= 16;
        } else {
            cur = 0;
        }
        m_state->selAnchor = cur;
        m_state->selCursor = cur;
        m_state->editNibble = -1;
        ensureVisible(cur);
        emit selectionChanged();
        update();
        return;

    default:
        break;
    }

    /* Hex digit input */
    if (cur < 0 || !m_state->mem->v1.poke) {
        QWidget::keyPressEvent(event);
        return;
    }

    QString text = event->text().toLower();
    if (text.isEmpty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    QChar ch = text[0];
    int digit = -1;
    if (ch >= '0' && ch <= '9') digit = ch.unicode() - '0';
    else if (ch >= 'a' && ch <= 'f') digit = ch.unicode() - 'a' + 10;
    if (digit < 0) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (m_state->editNibble < 0) {
        /* First nibble — store, don't poke yet */
        m_state->editNibble = digit;
        update();
    } else {
        /* Second nibble — combine and poke */
        uint8_t val = (uint8_t)((m_state->editNibble << 4) | digit);
        uint64_t addr = m_state->baseAddr + (uint64_t)cur;
        m_state->mem->v1.poke(m_state->mem, addr, val);
        m_state->editNibble = -1;

        /* Advance cursor */
        cur++;
        if (cur >= (int64_t)m_state->size) cur = (int64_t)m_state->size - 1;
        m_state->selAnchor = cur;
        m_state->selCursor = cur;
        ensureVisible(cur);
        emit selectionChanged();
        update();
    }
}

void HexArea::wheelEvent(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    int rows = -(delta / 40); /* ~3 rows per notch (120 units) */
    int newRow = m_state->scrollRow + rows;
    int maxRow = totalRows() - visibleRows();
    if (maxRow < 0) maxRow = 0;
    if (newRow < 0) newRow = 0;
    if (newRow > maxRow) newRow = maxRow;
    emit scrollRequested(newRow);
}

void HexArea::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    emit visibleRowsChanged();
}

void HexArea::contextMenuEvent(QContextMenuEvent *event) {
    if (!m_state->mem) return;

    int64_t idx = byteAtPos(event->pos());
    if (idx < 0) return;

    uint64_t addr = m_state->baseAddr + (uint64_t)idx;

    /* Collect R/W breakpoints at this address */
    std::vector<int> rwBpIds;
    unsigned bpCount = ar_bp_count();
    if (bpCount > 0) {
        auto *bps = new ar_breakpoint[bpCount];
        unsigned n = ar_bp_list(bps, bpCount);
        for (unsigned i = 0; i < n; i++) {
            if (bps[i].address == addr &&
                (bps[i].flags & (AR_BP_READ | AR_BP_WRITE)))
                rwBpIds.push_back(bps[i].id);
        }
        delete[] bps;
    }

    QMenu menu(this);
    QAction *addWp = nullptr;
    QAction *removeAll = nullptr;

    QString addrStr = QString("%1").arg(addr, 4, 16, QChar('0')).toUpper();

    if (rwBpIds.empty()) {
        addWp = menu.addAction(QString("Add watchpoint at %1").arg(addrStr));
    } else {
        removeAll = menu.addAction(
            rwBpIds.size() > 1 ? "Remove watchpoints" : "Remove watchpoint");
    }

    /* Label / comment actions */
    menu.addSeparator();
    auto *editLabel = menu.addAction("Edit label...");
    auto *editComment = menu.addAction("Edit comment...");

    auto *chosen = menu.exec(event->globalPos());
    if (!chosen) return;

    if (chosen == addWp) {
        int id = ar_bp_add(addr, AR_BP_WRITE, true, false, nullptr, nullptr);
        if (id < 0)
            QMessageBox::warning(this, "Watchpoint Error",
                "Failed to add watchpoint.\n"
                "Watchpoints may not be supported for this memory region.");
    } else if (chosen == removeAll) {
        for (int id : rwBpIds)
            ar_bp_delete(id);
    } else if (chosen == editLabel) {
        auto rslv = ar_sym_resolve(m_state->mem->v1.id, addr);
        if (!rslv) {
            QMessageBox::warning(this, "Symbol Error",
                "Cycle detected resolving address through memory maps.");
        } else {
            const char *rr = rslv->region_id.c_str();
            uint64_t ra = rslv->addr;
            const char *cur = ar_sym_get_label(rr, ra);
            bool ok;
            QString text = QInputDialog::getText(this, "Edit Label",
                QString("Label for %1:0x%2:")
                    .arg(rr)
                    .arg(ra, 0, 16),
                QLineEdit::Normal, cur ? cur : "", &ok);
            if (ok) {
                if (text.isEmpty())
                    ar_sym_delete_label(rr, ra);
                else if (!ar_sym_set_label(rr, ra,
                                           text.toUtf8().constData()))
                    QMessageBox::warning(this, "Invalid Label",
                        "Label must match [a-zA-Z_][a-zA-Z0-9_]*");
            }
        }
    } else if (chosen == editComment) {
        auto rslv = ar_sym_resolve(m_state->mem->v1.id, addr);
        if (!rslv) {
            QMessageBox::warning(this, "Symbol Error",
                "Cycle detected resolving address through memory maps.");
        } else {
            const char *rr = rslv->region_id.c_str();
            uint64_t ra = rslv->addr;
            const char *cur = ar_sym_get_comment(rr, ra);
            bool ok;
            QString text = QInputDialog::getText(this, "Edit Comment",
                QString("Comment for %1:0x%2:")
                    .arg(rr)
                    .arg(ra, 0, 16),
                QLineEdit::Normal, cur ? cur : "", &ok);
            if (ok) {
                if (text.isEmpty())
                    ar_sym_delete_comment(rr, ra);
                else
                    ar_sym_set_comment(rr, ra,
                                       text.toUtf8().constData());
            }
        }
    }

    update();
}

/* ======================================================================== */
/* AsciiColumn                                                               */
/* ======================================================================== */

AsciiColumn::AsciiColumn(HexViewState *state, QWidget *parent)
    : QWidget(parent), m_state(state)
{
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(10);
    setFont(font);
}

QSize AsciiColumn::sizeHint() const {
    int cw = fontMetrics().horizontalAdvance('0');
    return QSize(cw * 16 + 4, 100);
}

void AsciiColumn::paintEvent(QPaintEvent *) {
    if (!m_state->mem || m_state->size == 0) return;

    QPainter p(this);
    int rh = fontMetrics().height() + 2;
    int cw = fontMetrics().horizontalAdvance('0');
    int ascent = fontMetrics().ascent();
    int vis = height() / rh + 1;
    int total = (int)((m_state->size + 15) / 16);

    int64_t sMin = m_state->selMin();
    int64_t sMax = m_state->selMax();

    for (int i = 0; i < vis; i++) {
        int row = m_state->scrollRow + i;
        if (row >= total) break;

        int y = i * rh;

        for (int col = 0; col < 16; col++) {
            int64_t idx = (int64_t)row * 16 + col;
            if (idx >= (int64_t)m_state->size) break;

            uint64_t addr = m_state->baseAddr + (uint64_t)idx;
            uint8_t val = m_state->mem->v1.peek(m_state->mem, addr, false);

            int ax = 2 + col * cw;
            bool selected = (sMin >= 0 && idx >= sMin && idx <= sMax);

            if (selected) {
                p.fillRect(ax, y, cw, rh, QColor(60, 120, 200));
                p.setPen(Qt::white);
            } else {
                p.setPen(val >= 0x20 && val < 0x7F ? Qt::black : Qt::lightGray);
            }

            char ch = (val >= 0x20 && val < 0x7F) ? (char)val : '.';
            p.drawText(ax, y + ascent, QString(QChar(ch)));
        }
    }
}

/* ======================================================================== */
/* MemoryViewer                                                              */
/* ======================================================================== */

MemoryViewer::MemoryViewer(QWidget *parent)
    : QDockWidget("Memory Viewer", parent)
{
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    /* Menu bar */
    auto *menuBar = new QMenuBar;
    menuBar->setNativeMenuBar(false);
    layout->setMenuBar(menuBar);

    auto *editMenu = menuBar->addMenu("Edit");
    editMenu->addAction("Go To...", this, &MemoryViewer::onGoTo,
                         QKeySequence("Ctrl+G"));
    editMenu->addSeparator();
    editMenu->addAction("Copy", this, &MemoryViewer::onCopy,
                         QKeySequence::Copy);
    editMenu->addAction("Paste", this, &MemoryViewer::onPaste,
                         QKeySequence::Paste);

    /* Region selector */
    auto *topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel("Region:"));
    m_regionCombo = new QComboBox;
    topRow->addWidget(m_regionCombo, 1);
    layout->addLayout(topRow);

    /* Hex panel: address | hex | ascii | scrollbar */
    auto *hexPanel = new QHBoxLayout;
    hexPanel->setSpacing(0);

    m_addrCol = new AddressColumn(&m_state);
    m_addrCol->setFixedWidth(m_addrCol->sizeHint().width());
    hexPanel->addWidget(m_addrCol);

    m_hexArea = new HexArea(&m_state);
    m_hexArea->setFixedWidth(m_hexArea->sizeHint().width());
    hexPanel->addWidget(m_hexArea);

    m_asciiCol = new AsciiColumn(&m_state);
    m_asciiCol->setFixedWidth(m_asciiCol->sizeHint().width());
    hexPanel->addWidget(m_asciiCol);

    m_scrollBar = new QScrollBar(Qt::Vertical);
    hexPanel->addWidget(m_scrollBar);

    layout->addLayout(hexPanel, 1);

    setWidget(container);

    /* Connections */
    connect(m_regionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MemoryViewer::regionChanged);
    connect(m_scrollBar, &QScrollBar::valueChanged,
            this, &MemoryViewer::onScrollChanged);

    connect(m_hexArea, &HexArea::scrollRequested, this, [this](int row) {
        m_scrollBar->setValue(row);
    });

    connect(m_hexArea, &HexArea::visibleRowsChanged, this, [this]() {
        int total = (m_state.size > 0) ? (int)((m_state.size + 15) / 16) : 0;
        int vis = m_hexArea->visibleRows();
        int maxRow = total - vis;
        if (maxRow < 0) maxRow = 0;
        m_scrollBar->setRange(0, maxRow);
        m_scrollBar->setPageStep(vis);
    });

    connect(m_hexArea, &HexArea::selectionChanged, this, [this]() {
        m_addrCol->update();
        m_asciiCol->update();
    });

    populateRegions();

    if (m_regionCombo->count() > 0)
        regionChanged(0);

    resize(sizeHint().width(), sizeHint().height() * 2);
}

void MemoryViewer::refresh() {
    m_addrCol->update();
    m_hexArea->update();
    m_asciiCol->update();
}

void MemoryViewer::populateRegions() {
    m_regionCombo->clear();

    if (!ar_has_debug()) return;

    rd_System const *sys = ar_debug_system();
    if (!sys) return;

    QVector<rd_Memory const *> seen;
    auto addUnique = [&](rd_Memory const *m) {
        if (!m) return;
        for (auto *s : seen)
            if (s == m) return;
        seen.append(m);
    };

    for (unsigned i = 0; i < sys->v1.num_cpus; i++)
        addUnique(sys->v1.cpus[i]->v1.memory_region);

    for (unsigned i = 0; i < sys->v1.num_memory_regions; i++)
        addUnique(sys->v1.memory_regions[i]);

    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        rd_Memory const *cm = sys->v1.cpus[i]->v1.memory_region;
        if (!cm || !cm->v1.get_memory_map_count || !cm->v1.get_memory_map)
            continue;
        unsigned count = cm->v1.get_memory_map_count(cm);
        auto *maps = new rd_MemoryMap[count];
        cm->v1.get_memory_map(cm, maps);
        for (unsigned j = 0; j < count; j++)
            addUnique(maps[j].source);
        delete[] maps;
    }

    for (auto *m : seen) {
        m_regionCombo->addItem(
            QString("%1 (0x%2, %3 bytes)")
                .arg(m->v1.id)
                .arg(m->v1.base_address, 0, 16)
                .arg(m->v1.size),
            QVariant::fromValue(reinterpret_cast<quintptr>(m)));
    }
}

void MemoryViewer::regionChanged(int index) {
    if (index < 0) return;
    auto *mem = reinterpret_cast<rd_Memory const *>(
        m_regionCombo->itemData(index).value<quintptr>());

    m_state.mem = mem;
    m_state.baseAddr = mem ? mem->v1.base_address : 0;
    m_state.size = mem ? mem->v1.size : 0;
    m_state.selAnchor = -1;
    m_state.selCursor = -1;
    m_state.editNibble = -1;
    m_state.scrollRow = 0;

    /* Update scrollbar range */
    int total = (m_state.size > 0) ? (int)((m_state.size + 15) / 16) : 0;
    int vis = m_hexArea->visibleRows();
    int maxRow = total - vis;
    if (maxRow < 0) maxRow = 0;
    m_scrollBar->setRange(0, maxRow);
    m_scrollBar->setPageStep(vis);
    m_scrollBar->setValue(0);

    refresh();
}

void MemoryViewer::goTo(rd_Memory const *mem, uint64_t addr) {
    /* Switch region if a specific one was requested */
    if (mem) {
        for (int i = 0; i < m_regionCombo->count(); i++) {
            auto *r = reinterpret_cast<rd_Memory const *>(
                m_regionCombo->itemData(i).value<quintptr>());
            if (r == mem) {
                m_regionCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    if (addr >= m_state.baseAddr && addr < m_state.baseAddr + m_state.size) {
        int64_t idx = (int64_t)(addr - m_state.baseAddr);
        m_state.selAnchor = idx;
        m_state.selCursor = idx;
        m_state.editNibble = -1;
        m_hexArea->ensureVisible(idx);
        refresh();
    }
}

void MemoryViewer::onGoTo() {
    GoToDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted)
        goTo(nullptr, dlg.address());
}

void MemoryViewer::onCopy() {
    int64_t sMin = m_state.selMin();
    int64_t sMax = m_state.selMax();
    if (sMin < 0 || !m_state.mem) return;

    QString result;
    for (int64_t i = sMin; i <= sMax; i++) {
        if (i > sMin) result += ' ';
        uint64_t addr = m_state.baseAddr + (uint64_t)i;
        uint8_t val = m_state.mem->v1.peek(m_state.mem, addr, false);
        result += QString("%1").arg(val, 2, 16, QChar('0')).toUpper();
    }

    QApplication::clipboard()->setText(result);
}

void MemoryViewer::onPaste() {
    int64_t sMin = m_state.selMin();
    if (sMin < 0 || !m_state.mem || !m_state.mem->v1.poke) return;

    QString text = QApplication::clipboard()->text();

    /* Strip non-hex chars and parse byte pairs */
    QString hex;
    for (auto ch : text) {
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))
            hex += ch;
    }

    int64_t pos = sMin;
    for (int i = 0; i + 1 < hex.length(); i += 2) {
        if (pos >= (int64_t)m_state.size) break;
        bool ok;
        uint8_t val = (uint8_t)hex.mid(i, 2).toUInt(&ok, 16);
        if (ok)
            m_state.mem->v1.poke(m_state.mem, m_state.baseAddr + (uint64_t)pos, val);
        pos++;
    }

    refresh();
}

void MemoryViewer::onScrollChanged(int value) {
    m_state.scrollRow = value;
    refresh();
}

#include "Debugger.h"
#include "MainWindow.h"
#include "backend.hpp"
#include "registers.hpp"
#include "symbols.hpp"
#include "arch.hpp"

#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QEvent>
#include <QRegularExpressionValidator>
#include <QMessageBox>
#include <QInputDialog>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_map>
#include <vector>

/* (Register layout is now data-driven via arch::RegLayoutEntry) */

/* ======================================================================== */
/* Helper: get PC for a given CPU                                            */
/* ======================================================================== */

static uint64_t get_pc(rd_Cpu const *cpu) {
    if (!cpu) return 0;
    int pc_reg = ar_reg_pc(cpu->v1.type);
    if (pc_reg < 0) return 0;
    return cpu->v1.get_register(cpu, (unsigned)pc_reg);
}

/* ======================================================================== */
/* DisasmView                                                                */
/* ======================================================================== */

class Debugger::DisasmView : public QWidget {
public:
    explicit DisasmView(MainWindow *mainWindow, QWidget *parent = nullptr)
        : QWidget(parent), m_mainWindow(mainWindow)
    {
        QFont mono("Monospace", 10);
        mono.setStyleHint(QFont::Monospace);
        setFont(mono);
        setMinimumSize(360, 300);
        setMouseTracking(true);

        m_scrollBar = new QScrollBar(Qt::Vertical, this);
        connect(m_scrollBar, &QScrollBar::valueChanged,
                this, &DisasmView::onScrollChanged);
    }

    void goToAddress(uint64_t addr) {
        m_centerAddr = addr;
        m_scrollBar->blockSignals(true);
        m_scrollBar->setValue((int)(m_centerAddr / m_alignment));
        m_scrollBar->blockSignals(false);
        m_selectedAddr = addr;
        if (m_valid && m_mem) {
            disassembleAround();
            update();
        }
    }

    void refresh(rd_Cpu const *cpu, bool paused) {
        if (!paused || !cpu || !ar_content_loaded()) {
            m_valid = false;
            m_statusMsg = "Running...";
            update();
            return;
        }

        auto *mem = cpu->v1.memory_region;
        if (!mem) { m_valid = false; m_statusMsg = "No memory region"; update(); return; }

        auto *arch = arch::arch_for_cpu(cpu->v1.type);
        if (!arch) { m_valid = false; m_statusMsg = "Unknown CPU architecture"; update(); return; }

        m_pc = get_pc(cpu);
        m_delayPc = 0;
        m_hasDelayPc = false;
        if (cpu->v1.pipeline_get_delay_pc) {
            uint64_t dpc;
            if (cpu->v1.pipeline_get_delay_pc(cpu, 1, &dpc) && dpc != m_pc) {
                m_delayPc = dpc;
                m_hasDelayPc = true;
            }
        }
        m_cpu = cpu;
        m_mem = mem;
        m_cpuType = cpu->v1.type;
        m_addrWidth = (mem->v1.size <= 0x10000) ? 4 : 8;
        m_maxInsnSize = arch->max_insn_size;
        m_alignment = arch->alignment;
        m_branchDelaySlots = arch->branch_delay_slots;

        /* Update scrollbar range (units = alignment-sized steps) */
        int maxVal = (int)((mem->v1.size - 1) / m_alignment);
        m_scrollBar->blockSignals(true);
        m_scrollBar->setRange(0, maxVal);
        m_scrollBar->setSingleStep(1);
        QFontMetrics fm(font());
        int visLines = height() / std::max(fm.lineSpacing(), 1);
        m_scrollBar->setPageStep(std::max(visLines, 1));
        m_scrollBar->blockSignals(false);

        /* Snap centre to PC when PC changes */
        uint64_t alignedPc = m_pc & ~(uint64_t)(m_alignment - 1);
        if (alignedPc != m_lastAlignedPc) {
            m_lastAlignedPc = alignedPc;
            m_centerAddr = alignedPc;
            m_scrollBar->blockSignals(true);
            m_scrollBar->setValue((int)(m_centerAddr / m_alignment));
            m_scrollBar->blockSignals(false);
            m_selectedAddr = UINT64_MAX;
        }

        disassembleAround();
        m_valid = true;
        update();
    }

protected:
    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        int sbw = m_scrollBar->sizeHint().width();
        m_scrollBar->setGeometry(width() - sbw, 0, sbw, height());
    }

    void wheelEvent(QWheelEvent *e) override {
        int delta = e->angleDelta().y();
        int steps = delta / 40;  /* finer granularity than 120 */
        m_scrollBar->setValue(m_scrollBar->value() - steps);
    }

    /* Per-address breakpoint summary for gutter drawing */
    struct BpInfo {
        bool anyEnabled;
        bool allHaveCond;
        bool allTemporary;  /* purple gutter: all enabled exec BPs are temp,
                               or all BPs disabled and all are temp */
    };

    std::unordered_map<uint64_t, BpInfo> buildBpMap() const {
        std::unordered_map<uint64_t, BpInfo> map;
        unsigned count = ar_bp_count();
        if (count == 0) return map;
        auto *bps = new ar_breakpoint[count];
        unsigned n = ar_bp_list(bps, count);
        for (unsigned i = 0; i < n; i++) {
            if (!(bps[i].flags & AR_BP_EXECUTE)) continue;
            auto it = map.find(bps[i].address);
            if (it == map.end()) {
                map[bps[i].address] = {
                    bps[i].enabled,
                    bps[i].condition[0] != '\0',
                    bps[i].temporary
                };
            } else {
                if (bps[i].enabled) it->second.anyEnabled = true;
                if (!bps[i].condition[0]) it->second.allHaveCond = false;
                if (!bps[i].temporary) it->second.allTemporary = false;
            }
        }
        delete[] bps;
        return map;
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        int sbw = m_scrollBar->width();
        QRect drawRect(0, 0, width() - sbw, height());
        p.setClipRect(drawRect);
        p.fillRect(drawRect, QColor(240, 240, 240));
        m_labelRegions.clear();

        QFontMetrics fm(font());
        int charW = fm.horizontalAdvance('0');
        int lineH = fm.lineSpacing();
        int gutterW = lineH;  /* breakpoint gutter width */

        if (!m_valid || m_insns.empty()) {
            p.setPen(QColor(150, 150, 150));
            p.drawText(drawRect, Qt::AlignCenter, m_statusMsg);
            return;
        }

        auto bpMap = buildBpMap();

        int byteColChars = m_maxInsnSize * 2;

        /* Find the instruction that overlaps the central address */
        int centerIdx = findCenterInsn();

        /* Compute vertical offset so centerIdx is centred in the view */
        int yOffset = 0;
        if (centerIdx >= 0) {
            int naturalY = lineH;
            for (int i = 0; i < centerIdx; i++) {
                if (hasLabelLine(i)) naturalY += lineH;
                naturalY += lineH;
                if (hasFlowBreakAfter(i)) naturalY += lineH / 2;
            }
            if (hasLabelLine(centerIdx)) naturalY += lineH;
            int rowMid = naturalY - lineH / 2 + fm.descent();
            yOffset = drawRect.height() / 2 - rowMid;
        }

        /* ---- Jump arrows ---- */
        auto rowYs = computeRowYCenters(lineH, yOffset, fm.descent());
        auto arrows = buildJumpArrows(rowYs, drawRect.height(), lineH);

        int numCols = 0;
        for (auto &a : arrows)
            if (a.column + 1 > numCols) numCols = a.column + 1;

        int arrowPaneW = ARROW_PANE_W;
        int arrowSpacing = ARROW_SPACING_PREF;
        if (numCols > 0 && numCols * ARROW_SPACING_PREF > ARROW_PANE_W) {
            arrowSpacing = ARROW_PANE_W / numCols;
            if (arrowSpacing < 1) arrowSpacing = 1;
        }

        int arrowRight = gutterW + arrowPaneW;
        int textX = arrowRight + charW;

        QColor bgColor(240, 240, 240);
        QColor arrowColor(100, 100, 100);

        /* Pre-draw selection highlights (behind arrows) */
        for (int i = 0; i < (int)m_insns.size(); i++) {
            if (m_insns[i].address != m_selectedAddr) continue;
            int rowTop = rowYs[i] - lineH / 2;
            if (rowTop <= drawRect.height() && rowTop + lineH >= 0)
                p.fillRect(gutterW, rowTop, drawRect.width() - gutterW, lineH,
                           QColor(180, 210, 255));
        }

        /* Gutter shading: soft black gradient on right half */
        {
            QLinearGradient grad(gutterW / 2, 0, gutterW, 0);
            grad.setColorAt(0.0, QColor(0, 0, 0, 0));
            grad.setColorAt(1.0, QColor(0, 0, 0, 90));
            p.fillRect(gutterW / 2, 0, gutterW - gutterW / 2,
                       drawRect.height(), grad);
        }

        int tipX = arrowRight + 4;
        int headLen = 5;
        int headBaseX = tipX - headLen;
        auto stubs = buildStubArrows(rowYs, drawRect.height(), lineH);

        if (!arrows.empty()) {
            /* Pass 1: draw all horizontal bars (bg outline, then arrow) */
            for (auto &a : arrows) {
                int colX = arrowRight - arrowSpacing / 2 - a.column * arrowSpacing;
                int hLeft = colX - 2;  /* left edge of vertical bar's inner rect */

                /* Source horizontal (full extent) */
                p.fillRect(hLeft, a.srcY - 3, tipX - hLeft, 6, bgColor);
                p.fillRect(hLeft, a.srcY - 2, tipX - hLeft, 4, arrowColor);

                /* Dest horizontal (stops at arrowhead base) */
                p.fillRect(hLeft, a.dstY - 3, headBaseX - hLeft, 6, bgColor);
                p.fillRect(hLeft, a.dstY - 2, headBaseX - hLeft, 4, arrowColor);
            }

            /* Pass 2: draw vertical bars, outer columns first (highest col → lowest) */
            std::vector<size_t> order(arrows.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return arrows[a].column > arrows[b].column;
            });
            for (size_t idx : order) {
                auto &a = arrows[idx];
                int colX = arrowRight - arrowSpacing / 2 - a.column * arrowSpacing;
                int yTop = std::min(a.srcY, a.dstY) + 2;
                int yBot = std::max(a.srcY, a.dstY) - 2;
                if (yBot > yTop) {
                    p.fillRect(colX - 3, yTop, 6, yBot - yTop, bgColor);
                    p.fillRect(colX - 2, yTop, 4, yBot - yTop, arrowColor);
                }
            }
        }

        /* Stub arrows: short leftward bar for off-screen targets.
           Extend left far enough to clear any overlapping vertical bars.
           If incoming arrows land on the same row, shorten the right side
           so the stub doesn't overlap their arrowheads. */
        struct StubGeom { int y, sTipX, sRightX; };
        std::vector<StubGeom> stubGeom;
        for (int sy : stubs) {
            /* Find outermost column whose vertical bar spans this y */
            int maxCol = -1;
            bool hasIncoming = false;
            for (auto &a : arrows) {
                int yTop = std::min(a.srcY, a.dstY);
                int yBot = std::max(a.srcY, a.dstY);
                if (sy >= yTop && sy <= yBot && a.column > maxCol)
                    maxCol = a.column;
                if (a.dstY == sy)
                    hasIncoming = true;
            }
            int sTipX;
            if (maxCol >= 0) {
                int colX = arrowRight - arrowSpacing / 2 - maxCol * arrowSpacing;
                sTipX = colX - 3 - 2;  /* clear outer edge + small gap */
            } else {
                sTipX = tipX - 12;
            }
            if (sTipX > tipX - 12) sTipX = tipX - 12;  /* minimum length */
            sTipX -= 4;                                  /* extend 4px further left */
            if (sTipX < gutterW) sTipX = gutterW;       /* don't invade gutter */
            int sRightX = hasIncoming ? headBaseX - 2 : tipX;
            stubGeom.push_back({ sy, sTipX, sRightX });
        }
        /* Stub arrowheads first (with bg outline), then stub bars on top */
        if (!stubGeom.empty()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            for (auto &s : stubGeom) {
                int sBaseX = s.sTipX + headLen;
                QPointF tri[3] = {
                    { (qreal)s.sTipX, (qreal)s.y },
                    { (qreal)sBaseX,  (qreal)s.y - 4 },
                    { (qreal)sBaseX,  (qreal)s.y + 4 },
                };
                p.setBrush(bgColor);
                p.setPen(QPen(bgColor, 2));
                p.drawPolygon(tri, 3);
                p.setBrush(arrowColor);
                p.setPen(Qt::NoPen);
                p.drawPolygon(tri, 3);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }
        for (auto &s : stubGeom) {
            int sBaseX = s.sTipX + headLen;
            p.fillRect(sBaseX, s.y - 3, s.sRightX - sBaseX, 6, bgColor);
            p.fillRect(sBaseX, s.y - 2, s.sRightX - sBaseX, 4, arrowColor);
        }

        /* Normal arrowheads last */
        if (!arrows.empty()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(arrowColor);
            p.setPen(Qt::NoPen);
            for (auto &a : arrows) {
                QPointF tri[3] = {
                    { (qreal)tipX,       (qreal)a.dstY },
                    { (qreal)headBaseX,  (qreal)a.dstY - 4 },
                    { (qreal)headBaseX,  (qreal)a.dstY + 4 },
                };
                p.drawPolygon(tri, 3);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }

        /* First pass: find max bank among visible instructions */
        int64_t maxVisibleBank = -1;
        if (m_hasMemMap) {
            int vy = lineH + yOffset;
            for (int i = 0; i < (int)m_insns.size(); i++) {
                if (hasLabelLine(i)) vy += lineH;
                int rowTop = vy - lineH + fm.descent();
                int rowBot = vy + fm.descent();
                if (rowBot >= 0 && rowTop <= drawRect.height()) {
                    if (i < (int)m_banks.size() && m_banks[i] > maxVisibleBank)
                        maxVisibleBank = m_banks[i];
                }
                vy += lineH;
                if (hasFlowBreakAfter(i)) vy += lineH / 2;
                if (rowTop > drawRect.height()) break;
            }
        }
        int bankColChars = 0;
        if (maxVisibleBank >= 0) {
            bankColChars = 1;
            for (int64_t v = maxVisibleBank; v >= 10; v /= 10) bankColChars++;
        }

        /* Second pass: draw */
        int y = lineH + yOffset;
        for (int i = 0; i < (int)m_insns.size(); i++) {
            auto &insn = m_insns[i];

            /* Label line above instruction */
            const char *label = resolvedLabel(insn.address);
            if (label) {
                int labelTop = y - lineH + fm.descent();
                int labelBot = y + fm.descent();
                if (labelBot >= 0 && labelTop <= drawRect.height()) {
                    QFont italicFont = font();
                    italicFont.setItalic(true);
                    p.setFont(italicFont);
                    p.setPen(Qt::black);
                    p.drawText(textX, y, QString("%1:").arg(label));
                    p.setFont(font());
                }
                y += lineH;
            }

            int rowTop = y - lineH + fm.descent();
            int rowBot = y + fm.descent();

            /* Skip rows entirely off-screen */
            if (rowBot < 0) {
                y += lineH;
                if (hasFlowBreakAfter(i)) y += lineH / 2;
                continue;
            }
            if (rowTop > drawRect.height()) break;

            bool selected = (insn.address == m_selectedAddr);
            p.setPen(Qt::black);

            /* Breakpoint gutter */
            auto bpIt = bpMap.find(insn.address);
            if (bpIt != bpMap.end()) {
                int circleD = lineH * 3 / 4;
                int cx = gutterW / 2;
                int cy = rowTop + lineH / 2;
                QColor brown(165, 82, 42);
                QColor darkPurple(64, 22, 180);
                QColor circleColor = bpIt->second.allTemporary ? darkPurple : brown;

                p.setRenderHint(QPainter::Antialiasing, true);
                if (bpIt->second.anyEnabled) {
                    p.setBrush(circleColor);
                    p.setPen(Qt::NoPen);
                } else {
                    p.setBrush(Qt::NoBrush);
                    p.setPen(QPen(circleColor, 1.5));
                }
                p.drawEllipse(QPointF(cx, cy), circleD / 2.0, circleD / 2.0);

                /* Condition badge: small circle at top-right */
                if (bpIt->second.allHaveCond) {
                    int smallD = circleD / 3;
                    double sx = cx + circleD / 2.0 * 0.6;
                    double sy = cy - circleD / 2.0 * 0.6;
                    QColor green(60, 180, 60);
                    if (bpIt->second.anyEnabled) {
                        p.setBrush(green);
                        p.setPen(Qt::NoPen);
                    } else {
                        p.setBrush(Qt::NoBrush);
                        p.setPen(QPen(green, 1.0));
                    }
                    p.drawEllipse(QPointF(sx, sy), smallD / 2.0, smallD / 2.0);
                }
                p.setRenderHint(QPainter::Antialiasing, false);

                /* Restore text pen */
                p.setPen(Qt::black);
            }

            char marker;
            if (insn.address == m_pc)
                marker = '>';
            else if (m_pc > insn.address && m_pc < insn.address + insn.length)
                marker = '~';
            else
                marker = ':';

            /* Bank prefix */
            QString bankPrefix;
            if (bankColChars > 0) {
                int64_t bank = (i < (int)m_banks.size()) ? m_banks[i] : -1;
                if (bank >= 0)
                    bankPrefix = QString("%1:").arg(bank, bankColChars, 10, QChar(' '));
                else
                    bankPrefix = QString(bankColChars + 1, ' ');
            }

            QString addr = QString("%1").arg(insn.address, m_addrWidth, 16, QChar('0')).toUpper();

            QString bytes;
            if (m_mem) {
                for (uint8_t b = 0; b < insn.length; b++) {
                    uint8_t val = m_mem->v1.peek(m_mem, insn.address + b, false);
                    bytes += QString("%1").arg(val, 2, 16, QChar('0')).toUpper();
                }
            }
            while (bytes.length() < byteColChars)
                bytes += ' ';

            bool isDelayPc = m_hasDelayPc && insn.address == m_delayPc;
            if (marker == '>' && !selected)
                p.setPen(QColor(0, 140, 0));
            else if (isDelayPc && !selected)
                p.setPen(QColor(40, 10, 210));

            /* Draw prefix: bank + address + marker + bytes */
            QString prefix = bankPrefix + addr + marker + ' ' + bytes + "  ";
            p.drawText(textX, y, prefix);
            int curX = textX + fm.horizontalAdvance(prefix);

            /* Draw mnemonic segments (resolving @-marked addresses to labels) */
            auto segments = parseInsnText(insn.text);
            QFont italicFont = font();
            italicFont.setItalic(true);
            QFontMetrics ifm(italicFont);
            QColor addrBlue(40, 60, 180);
            for (auto &seg : segments) {
                if (seg.isLabel) {
                    p.setFont(italicFont);
                    p.drawText(curX, y, seg.text);
                    int segW = ifm.horizontalAdvance(seg.text);
                    m_labelRegions.push_back({ QRect(curX, rowTop, segW, lineH), seg.addr });
                    curX += segW;
                    p.setFont(font());
                } else if (seg.isAddr) {
                    QColor saved = p.pen().color();
                    p.setPen(addrBlue);
                    p.drawText(curX, y, seg.text);
                    int segW = fm.horizontalAdvance(seg.text);
                    m_labelRegions.push_back({ QRect(curX, rowTop, segW, lineH), seg.addr });
                    curX += segW;
                    p.setPen(saved);
                } else {
                    p.drawText(curX, y, seg.text);
                    curX += fm.horizontalAdvance(seg.text);
                }
            }

            /* Append comment */
            const char *comment = resolvedComment(insn.address);
            if (comment) {
                QString commentStr = QString::fromUtf8(comment);
                int nlPos = commentStr.indexOf('\n');
                if (nlPos >= 0)
                    commentStr = commentStr.left(nlPos) + "...";
                QColor savedPen = p.pen().color();
                p.setPen(QColor(0, 140, 0));
                p.drawText(curX, y, " ; " + commentStr);
                p.setPen(savedPen);
            }

            if ((marker == '>' || isDelayPc) && !selected)
                p.setPen(Qt::black);

            y += lineH;
            if (hasFlowBreakAfter(i))
                y += lineH / 2;
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) return;
        int idx = hitTest(event->position().toPoint().y());
        if (idx < 0) return;

        m_selectedAddr = m_insns[idx].address;

        /* Check if click is in the breakpoint gutter */
        QFontMetrics fm(font());
        int gutterW = fm.lineSpacing();
        if (event->position().x() < gutterW) {
            uint64_t addr = m_insns[idx].address;

            /* Collect execution breakpoints at this address */
            unsigned count = ar_bp_count();
            std::vector<int> execIds;
            bool anyEnabled = false;
            bool allEnabledTemp = true;
            if (count > 0) {
                auto *bps = new ar_breakpoint[count];
                unsigned n = ar_bp_list(bps, count);
                for (unsigned i = 0; i < n; i++) {
                    if (bps[i].address == addr &&
                        (bps[i].flags & AR_BP_EXECUTE)) {
                        execIds.push_back(bps[i].id);
                        if (bps[i].enabled) {
                            anyEnabled = true;
                            if (!bps[i].temporary) allEnabledTemp = false;
                        }
                    }
                }
                delete[] bps;
            }

            if (execIds.empty()) {
                /* No execution breakpoints — add one */
                ar_bp_add(addr, AR_BP_EXECUTE, true, false, nullptr, nullptr);
            } else if (anyEnabled && allEnabledTemp) {
                /* All enabled are temporary — make them non-temporary */
                for (int id : execIds)
                    ar_bp_set_temporary(id, false);
            } else if (anyEnabled) {
                /* Some enabled, not all temp — disable all */
                for (int id : execIds)
                    ar_bp_enable(id, false);
            } else {
                /* All disabled — enable all and clear temporary */
                for (int id : execIds) {
                    ar_bp_enable(id, true);
                    ar_bp_set_temporary(id, false);
                }
            }
        }

        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) return;
        QFontMetrics fm(font());
        int gutterW = fm.lineSpacing();
        if (event->position().x() >= gutterW) return;

        int idx = hitTest(event->position().toPoint().y());
        if (idx < 0) return;

        uint64_t addr = m_insns[idx].address;
        unsigned count = ar_bp_count();
        if (count == 0) return;
        auto *bps = new ar_breakpoint[count];
        unsigned n = ar_bp_list(bps, count);
        for (unsigned i = 0; i < n; i++) {
            if (bps[i].address == addr)
                ar_bp_delete(bps[i].id);
        }
        delete[] bps;
        update();
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        int idx = hitTest(event->y());
        if (idx < 0) return;

        m_selectedAddr = m_insns[idx].address;
        update();

        uint64_t clickAddr = m_insns[idx].address;

        QMenu menu(this);
        auto *viewMem = menu.addAction("View in Memory");

        /* If a memory map entry with a backing source covers this address,
           offer to view in that source memory region too. */
        QAction *viewSource = nullptr;
        rd_Memory const *srcMem = nullptr;
        uint64_t srcAddr = 0;
        if (m_hasMemMap) {
            for (auto &m : m_memMap) {
                if (clickAddr >= m.base_addr && clickAddr < m.base_addr + m.size
                    && m.source) {
                    srcMem = m.source;
                    srcAddr = m.source_base_addr + (clickAddr - m.base_addr);
                    viewSource = menu.addAction(
                        QString("View in %1").arg(srcMem->v1.id));
                    break;
                }
            }
        }

        /* Set PC / Run to */
        QString addrLabel = QString("%1").arg(clickAddr, m_addrWidth, 16, QChar('0')).toUpper();
        QAction *jumpPc = nullptr;
        QAction *runTo = nullptr;
        if (m_cpu) {
            jumpPc = menu.addAction(QString("Set PC to %1").arg(addrLabel));
            runTo = menu.addAction(QString("Run to %1").arg(addrLabel));
            uint64_t currentPC = get_pc(m_cpu);
            if (clickAddr == currentPC) runTo->setEnabled(false);
        }

        /* Label / comment actions */
        menu.addSeparator();
        auto *editLabel = menu.addAction("Edit label...");
        auto *editComment = menu.addAction("Edit comment...");

        /* Separator before breakpoint actions */
        menu.addSeparator();

        /* Collect breakpoints at this address */
        unsigned bpCount = ar_bp_count();
        ar_breakpoint *bps = nullptr;
        unsigned nBps = 0;
        std::vector<int> bpIdsAtAddr;
        bool anyEnabled = false;
        bool allDisabled = true;
        bool hasUsableExecBp = false; /* enabled, execute, no condition */
        if (bpCount > 0) {
            bps = new ar_breakpoint[bpCount];
            nBps = ar_bp_list(bps, bpCount);
            for (unsigned i = 0; i < nBps; i++) {
                if (bps[i].address == clickAddr) {
                    bpIdsAtAddr.push_back(bps[i].id);
                    if (bps[i].enabled) { anyEnabled = true; allDisabled = false; }
                    if (bps[i].enabled && (bps[i].flags & AR_BP_EXECUTE) &&
                        !bps[i].condition[0])
                        hasUsableExecBp = true;
                }
            }
            delete[] bps;
        }

        QAction *addBp = nullptr;
        QAction *removeBp = nullptr;
        QAction *disableBp = nullptr;
        QAction *enableBp = nullptr;

        if (bpIdsAtAddr.empty()) {
            addBp = menu.addAction("Add Breakpoint");
        } else {
            QString removeLabel = (bpIdsAtAddr.size() > 1)
                ? "Remove Breakpoints" : "Remove Breakpoint";
            removeBp = menu.addAction(removeLabel);

            if (anyEnabled) {
                QString disableLabel = (bpIdsAtAddr.size() > 1)
                    ? "Disable Breakpoints" : "Disable Breakpoint";
                disableBp = menu.addAction(disableLabel);
            }
            if (allDisabled && !bpIdsAtAddr.empty()) {
                QString enableLabel = (bpIdsAtAddr.size() > 1)
                    ? "Enable Breakpoints" : "Enable Breakpoint";
                enableBp = menu.addAction(enableLabel);
            }
        }

        auto *chosen = menu.exec(event->globalPos());
        if (!chosen) return;

        if (chosen == editLabel && m_mem) {
            auto rslv = ar_sym_resolve(m_mem->v1.id, clickAddr);
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
            update();
        }
        else if (chosen == editComment && m_mem) {
            auto rslv = ar_sym_resolve(m_mem->v1.id, clickAddr);
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
            update();
        }
        else if (chosen == viewMem && m_mem && m_mainWindow)
            m_mainWindow->openMemoryViewerAt(m_mem, clickAddr);
        else if (chosen == viewSource && m_mainWindow)
            m_mainWindow->openMemoryViewerAt(srcMem, srcAddr);
        else if (chosen == jumpPc && m_cpu) {
            int pc_reg = ar_reg_pc(m_cpu->v1.type);
            if (pc_reg < 0) return;
            m_cpu->v1.set_register(m_cpu, (unsigned)pc_reg, clickAddr);
        }
        else if (chosen == addBp) {
            int bpId = ar_bp_add(clickAddr, AR_BP_EXECUTE, true, false, nullptr, nullptr);
            if (bpId < 0)
                QMessageBox::warning(this, "Breakpoint Error",
                    "Failed to add breakpoint.\n"
                    "Breakpoints may not be supported for this memory region.");
        }
        else if (chosen == runTo && m_mainWindow) {
            if (hasUsableExecBp) {
                m_mainWindow->debugResume();
            } else {
                int bpId = ar_bp_add(clickAddr, AR_BP_EXECUTE, true, true, nullptr, nullptr);
                if (bpId < 0)
                    QMessageBox::warning(this, "Breakpoint Error",
                        "Failed to add temporary breakpoint.\n"
                        "Breakpoints may not be supported for this memory region.");
                else
                    m_mainWindow->debugResume();
            }
        }
        else if (chosen == removeBp) {
            for (int id : bpIdsAtAddr) ar_bp_delete(id);
        }
        else if (chosen == disableBp) {
            for (int id : bpIdsAtAddr) ar_bp_enable(id, false);
        }
        else if (chosen == enableBp) {
            for (int id : bpIdsAtAddr) ar_bp_enable(id, true);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        QPoint pos = event->pos();
        for (auto &lr : m_labelRegions) {
            if (lr.rect.contains(pos)) {
                QString tip = QString("%1")
                    .arg(lr.addr, m_addrWidth, 16, QChar('0')).toUpper();
                QToolTip::showText(event->globalPosition().toPoint(), tip, this);
                return;
            }
        }
        QToolTip::hideText();
    }

private:
    /* ---- Jump arrow structures and helpers ---- */

    static constexpr int ARROW_PANE_W = 35;
    static constexpr int ARROW_SPACING_PREF = 10;

    struct JumpArrow {
        int srcY;      /* y-center of source instruction row */
        int dstY;      /* y-center of destination instruction row */
        int column;    /* assigned column (0 = innermost/rightmost) */
    };

    /* For pipelined CPUs (MIPS), breaks_flow and arrow sources shift to the
       delay slot.  Returns the index where the visual gap/arrow should appear
       for a branch at instruction i. */
    int delayedFlowIdx(int i) const {
        int d = i + (int)m_branchDelaySlots;
        return std::min(d, (int)m_insns.size() - 1);
    }

    /* True if instruction i should have a flow-break gap after it (accounting
       for branch delay: the gap appears after the delay slot, not the branch). */
    bool hasFlowBreakAfter(int i) const {
        if (m_branchDelaySlots == 0)
            return m_insns[i].breaks_flow;
        // On delay-slot architectures, the gap goes after the delay slot.
        // Check if any instruction within delay range targets this row.
        int lo = std::max(0, i - (int)m_branchDelaySlots);
        for (int k = lo; k <= i; k++) {
            if (m_insns[k].breaks_flow && delayedFlowIdx(k) == i)
                return true;
        }
        return false;
    }

    /* Compute vertical center of each instruction row */
    std::vector<int> computeRowYCenters(int lineH, int yOffset, int descent) const {
        std::vector<int> ys(m_insns.size());
        int y = lineH + yOffset;
        for (int i = 0; i < (int)m_insns.size(); i++) {
            if (hasLabelLine(i)) y += lineH;
            ys[i] = y - lineH / 2 + descent;
            y += lineH;
            if (hasFlowBreakAfter(i)) y += lineH / 2;
        }
        return ys;
    }

    /* Build arrows and assign columns using interval graph coloring */
    std::vector<JumpArrow> buildJumpArrows(const std::vector<int> &rowYs,
                                           int viewHeight, int lineH) const {
        /* Map address → instruction index */
        std::unordered_map<uint64_t, int> addrIdx;
        for (int i = 0; i < (int)m_insns.size(); i++)
            addrIdx[m_insns[i].address] = i;

        /* Collect arrows where both endpoints are visible (or within 1 row).
           For delay-slot architectures, the arrow originates from the delay
           slot (branch_delay_slots after the branch instruction). */
        int margin = lineH;
        std::vector<JumpArrow> arrows;
        for (int i = 0; i < (int)m_insns.size(); i++) {
            if (!m_insns[i].has_target) continue;
            auto it = addrIdx.find(m_insns[i].target);
            if (it == addrIdx.end()) continue;
            int src = delayedFlowIdx(i);
            int j = it->second;
            if (src == j) continue;
            int sy = rowYs[src], dy = rowYs[j];
            if (sy < -margin || sy > viewHeight + margin) continue;
            if (dy < -margin || dy > viewHeight + margin) continue;
            arrows.push_back({ sy, dy, -1 });
        }

        /* Sort by span length, shortest first → short jumps get inner columns */
        std::sort(arrows.begin(), arrows.end(), [](const JumpArrow &a, const JumpArrow &b) {
            return std::abs(a.srcY - a.dstY) < std::abs(b.srcY - b.dstY);
        });

        /* Greedy column assignment */
        for (size_t i = 0; i < arrows.size(); i++) {
            int yTop = std::min(arrows[i].srcY, arrows[i].dstY);
            int yBot = std::max(arrows[i].srcY, arrows[i].dstY);
            std::set<int> used;
            for (size_t j = 0; j < i; j++) {
                int jTop = std::min(arrows[j].srcY, arrows[j].dstY);
                int jBot = std::max(arrows[j].srcY, arrows[j].dstY);
                if (yTop < jBot && jTop < yBot)
                    used.insert(arrows[j].column);
            }
            int col = 0;
            while (used.count(col)) col++;
            arrows[i].column = col;
        }

        return arrows;
    }

    /* Collect y-centers for visible jump instructions whose target is off-screen */
    std::vector<int> buildStubArrows(const std::vector<int> &rowYs,
                                     int viewHeight, int lineH) const {
        std::unordered_map<uint64_t, int> addrIdx;
        for (int i = 0; i < (int)m_insns.size(); i++)
            addrIdx[m_insns[i].address] = i;

        int margin = lineH;
        std::vector<int> stubs;
        for (int i = 0; i < (int)m_insns.size(); i++) {
            if (!m_insns[i].has_target) continue;
            int src = delayedFlowIdx(i);
            int sy = rowYs[src];
            if (sy < -margin || sy > viewHeight + margin) continue;

            auto it = addrIdx.find(m_insns[i].target);
            if (it != addrIdx.end()) {
                int dy = rowYs[it->second];
                if (dy >= -margin && dy <= viewHeight + margin)
                    continue;  /* both visible — handled by full arrow */
            }
            stubs.push_back(sy);
        }
        return stubs;
    }

    int hitTest(int clickY) const {
        QFontMetrics fm(font());
        int lineH = fm.lineSpacing();

        int centerIdx = findCenterInsn();
        int yOffset = 0;
        if (centerIdx >= 0) {
            int naturalY = lineH;
            for (int i = 0; i < centerIdx; i++) {
                if (hasLabelLine(i)) naturalY += lineH;
                naturalY += lineH;
                if (hasFlowBreakAfter(i)) naturalY += lineH / 2;
            }
            if (hasLabelLine(centerIdx)) naturalY += lineH;
            int rowMid = naturalY - lineH / 2 + fm.descent();
            yOffset = height() / 2 - rowMid;
        }

        int y = lineH + yOffset;
        for (int i = 0; i < (int)m_insns.size(); i++) {
            if (hasLabelLine(i)) y += lineH;
            int rowTop = y - lineH + fm.descent();
            int rowBot = y + fm.descent();
            if (clickY >= rowTop && clickY < rowBot)
                return i;
            y += lineH;
            if (hasFlowBreakAfter(i))
                y += lineH / 2;
        }
        return -1;
    }

    int findCenterInsn() const {
        for (int i = 0; i < (int)m_insns.size(); i++) {
            auto &insn = m_insns[i];
            if (insn.address <= m_centerAddr &&
                insn.address + insn.length > m_centerAddr)
                return i;
        }
        /* No exact overlap — find first instruction at or after centerAddr */
        for (int i = 0; i < (int)m_insns.size(); i++) {
            if (m_insns[i].address >= m_centerAddr) return i;
        }
        return m_insns.empty() ? -1 : (int)m_insns.size() - 1;
    }

    void onScrollChanged(int value) {
        m_centerAddr = (uint64_t)value * m_alignment;
        if (m_valid && m_mem) {
            disassembleAround();
            update();
        }
    }

    void disassembleAround() {
        uint64_t radius = (uint64_t)30 * m_maxInsnSize;
        uint64_t start = m_centerAddr;
        if (start >= radius) start -= radius;
        else start = 0;
        start &= ~(uint64_t)(m_alignment - 1);

        uint64_t end = m_centerAddr + radius;
        if (end > m_mem->v1.size) end = m_mem->v1.size;

        uint64_t fetch_size = end - start;
        std::vector<uint8_t> buf(fetch_size);
        for (uint64_t i = 0; i < fetch_size; i++)
            buf[i] = m_mem->v1.peek(m_mem, start + i, false);

        m_insns = arch::disassemble(buf, start, m_cpuType);

        /* Fetch memory map and compute bank for each instruction */
        m_banks.clear();
        m_memMap.clear();
        m_hasMemMap = false;

        if (m_mem->v1.get_memory_map_count && m_mem->v1.get_memory_map) {
            unsigned count = m_mem->v1.get_memory_map_count(m_mem);
            if (count > 0) {
                m_memMap.resize(count);
                m_mem->v1.get_memory_map(m_mem, m_memMap.data());
                m_hasMemMap = true;
            }
        }

        m_banks.resize(m_insns.size(), -1);
        if (m_hasMemMap) {
            for (size_t i = 0; i < m_insns.size(); i++)
                m_banks[i] = bankForAddr(m_insns[i].address);
        }
    }

    int64_t bankForAddr(uint64_t addr) const {
        for (auto &m : m_memMap) {
            if (addr >= m.base_addr && addr < m.base_addr + m.size)
                return m.bank;
        }
        return -1;
    }

    /* Resolve address through memory maps, then look up label/comment */
    const char *resolvedLabel(uint64_t address) const {
        if (!m_mem) return nullptr;
        auto rslv = ar_sym_resolve(m_mem->v1.id, address);
        if (!rslv) return nullptr;
        return ar_sym_get_label(rslv->region_id.c_str(), rslv->addr);
    }
    const char *resolvedComment(uint64_t address) const {
        if (!m_mem) return nullptr;
        auto rslv = ar_sym_resolve(m_mem->v1.id, address);
        if (!rslv) return nullptr;
        return ar_sym_get_comment(rslv->region_id.c_str(), rslv->addr);
    }

    /* Return true if the instruction at index has a symbol label above it */
    bool hasLabelLine(int idx) const {
        if (idx < 0 || idx >= (int)m_insns.size()) return false;
        return resolvedLabel(m_insns[idx].address) != nullptr;
    }

    /* ---- Address marker (@) parsing for symbol resolution in operands ---- */

    struct TextSegment {
        QString text;
        bool isLabel;       /* draw italic (symbol found) */
        bool isAddr;        /* address operand (blue, tooltip) */
        uint64_t addr;      /* original address (for tooltip; valid if isAddr) */
    };

    std::vector<TextSegment> parseInsnText(const std::string &text) const {
        std::vector<TextSegment> segs;
        std::string cur;
        const char *p = text.c_str();

        while (*p) {
            if (*p == '@') {
                const char *h = p + 1;
                while ((*h >= '0' && *h <= '9') ||
                       (*h >= 'A' && *h <= 'F') ||
                       (*h >= 'a' && *h <= 'f'))
                    h++;
                if (h > p + 1) {
                    bool hadDollar = (!cur.empty() && cur.back() == '$');
                    if (hadDollar) cur.pop_back();
                    if (!cur.empty()) {
                        segs.push_back({ QString::fromStdString(cur), false, false, 0 });
                        cur.clear();
                    }
                    std::string hexStr(p + 1, h);
                    uint64_t addr = strtoull(hexStr.c_str(), nullptr, 16);
                    const char *label = nullptr;
                    if (m_mem) {
                        auto rslv = ar_sym_resolve(m_mem->v1.id, addr);
                        if (rslv)
                            label = ar_sym_get_label(rslv->region_id.c_str(), rslv->addr);
                    }
                    if (label)
                        segs.push_back({ QString::fromUtf8(label), true, true, addr });
                    else {
                        QString display = hadDollar ? QStringLiteral("$") : QString();
                        display += QString::fromStdString(hexStr);
                        segs.push_back({ display, false, true, addr });
                    }
                    p = h;
                } else {
                    cur += *p++;
                }
            } else {
                cur += *p++;
            }
        }
        if (!cur.empty())
            segs.push_back({ QString::fromStdString(cur), false, false, 0 });
        return segs;
    }

    struct LabelRegion {
        QRect rect;
        uint64_t addr;
    };

    std::vector<arch::Instruction> m_insns;
    std::vector<int64_t> m_banks;
    std::vector<rd_MemoryMap> m_memMap;
    bool m_hasMemMap = false;
    MainWindow *m_mainWindow;
    uint64_t m_selectedAddr = UINT64_MAX;
    uint64_t m_pc = 0;
    uint64_t m_delayPc = 0;
    bool m_hasDelayPc = false;
    uint64_t m_centerAddr = 0;
    uint64_t m_lastAlignedPc = UINT64_MAX;   /* force initial snap */
    bool m_valid = false;
    const char *m_statusMsg = "Running...";
    int m_addrWidth = 4;
    unsigned m_maxInsnSize = 3;
    unsigned m_alignment = 1;
    unsigned m_cpuType = 0;
    unsigned m_branchDelaySlots = 0;
    rd_Cpu const *m_cpu = nullptr;
    rd_Memory const *m_mem = nullptr;
    QScrollBar *m_scrollBar;
    std::vector<LabelRegion> m_labelRegions;
};

/* ======================================================================== */
/* RegistersPane                                                             */
/* ======================================================================== */

class Debugger::RegistersPane : public QWidget {
public:
    explicit RegistersPane(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        m_outerLayout = new QVBoxLayout(this);
        m_outerLayout->setContentsMargins(0, 0, 0, 0);
    }

    void setCpu(rd_Cpu const *cpu) {
        if (cpu == m_cpu) return;
        m_cpu = cpu;
        rebuild();
    }

    void refresh() {
        if (!m_cpu) return;

        for (auto &rf : m_regFields) {
            if (rf.edit->hasFocus()) continue;
            uint64_t val = m_cpu->v1.get_register(m_cpu, rf.regIndex);
            rf.edit->blockSignals(true);
            rf.edit->setText(QString("%1").arg(val, rf.hexDigits, 16, QChar('0')).toUpper());
            rf.edit->blockSignals(false);
        }

        for (auto &fg : m_flagGroups) {
            uint64_t val = m_cpu->v1.get_register(m_cpu, fg.regIndex);
            for (auto &[bit, cb] : fg.bits) {
                cb->blockSignals(true);
                cb->setChecked((val >> bit) & 1);
                cb->blockSignals(false);
            }
        }
    }

private:
    void rebuild() {
        delete m_content;
        m_content = nullptr;
        m_regFields.clear();
        m_flagGroups.clear();

        if (!m_cpu) return;

        m_content = new QWidget(this);
        auto *layout = new QVBoxLayout(m_content);
        layout->setContentsMargins(4, 4, 4, 4);

        const arch::Arch *a = arch::arch_for_cpu(m_cpu->v1.type);
        if (a && a->reg_layout)
            buildFromLayout(layout, a->reg_layout, a->num_reg_layout);
        else
            buildGeneric(layout);

        layout->addStretch();
        m_outerLayout->addWidget(m_content);
    }

    QLineEdit *makeRegEdit(int hexDigits) {
        auto *edit = new QLineEdit(m_content);
        QFont mono("Monospace", 10);
        mono.setStyleHint(QFont::Monospace);
        edit->setFont(mono);
        edit->setMaxLength(hexDigits);
        QFontMetrics fm(mono);
        int charW = fm.horizontalAdvance('0');
        edit->setFixedWidth(charW * hexDigits + 16);
        edit->setValidator(new QRegularExpressionValidator(
            QRegularExpression("[0-9A-Fa-f]*"), edit));
        return edit;
    }

    void connectRegEdit(QLineEdit *edit, unsigned regIdx, int hexDigits) {
        connect(edit, &QLineEdit::editingFinished, this,
                [this, regIdx, edit, hexDigits]() {
            if (!m_cpu) return;
            bool ok;
            uint64_t val = edit->text().toULongLong(&ok, 16);
            if (ok) m_cpu->v1.set_register(m_cpu, regIdx, val);
            val = m_cpu->v1.get_register(m_cpu, regIdx);
            edit->setText(QString("%1").arg(val, hexDigits, 16, QChar('0')).toUpper());
        });
    }

    void buildFromLayout(QVBoxLayout *layout,
                         const arch::RegLayoutEntry *entries, unsigned count) {
        QFormLayout *form = nullptr;

        for (unsigned i = 0; i < count; i++) {
            const auto &e = entries[i];

            if (e.type == arch::REG_HEX) {
                if (!form) {
                    form = new QFormLayout();
                    form->setContentsMargins(0, 0, 0, 0);
                    layout->addLayout(form);
                }
                int digits = (int)e.bits / 4;
                auto *edit = makeRegEdit(digits);
                form->addRow(e.label, edit);
                m_regFields.push_back({e.reg_index, digits, edit});
                connectRegEdit(edit, e.reg_index, digits);
            } else {
                /* REG_FLAGS — start a new form group after this */
                form = nullptr;

                auto *row = new QHBoxLayout();
                FlagGroup fg;
                fg.regIndex = e.reg_index;

                for (unsigned f = 0; f < e.num_flags; f++) {
                    const auto &flag = e.flags[f];
                    QString label = flag.name
                        ? QString::fromUtf8(flag.name)
                        : QString::number(flag.bit);
                    auto *cb = new QCheckBox(label, m_content);
                    row->addWidget(cb);
                    fg.bits.push_back({flag.bit, cb});

                    unsigned regIdx = e.reg_index;
                    unsigned bit = flag.bit;
                    connect(cb, &QCheckBox::toggled, this,
                            [this, regIdx, bit](bool checked) {
                        if (!m_cpu) return;
                        uint64_t v = m_cpu->v1.get_register(m_cpu, regIdx);
                        if (checked) v |= (1u << bit);
                        else v &= ~(1u << bit);
                        m_cpu->v1.set_register(m_cpu, regIdx, v);
                    });
                }

                row->addStretch();
                layout->addLayout(row);
                m_flagGroups.push_back(std::move(fg));
            }
        }
    }

    void buildGeneric(QVBoxLayout *layout) {
        unsigned nregs = ar_reg_count(m_cpu->v1.type);
        if (nregs == 0) return;

        auto *form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);

        for (unsigned i = 0; i < nregs; i++) {
            int idx = ar_reg_by_order(m_cpu->v1.type, i);
            if (idx < 0) continue;
            const char *name = ar_reg_name(m_cpu->v1.type, (unsigned)idx);
            if (!name) continue;
            int digits = ar_reg_digits(m_cpu->v1.type, (unsigned)idx);

            /* Uppercase label */
            QString label;
            for (const char *p = name; *p; p++)
                label += QChar::fromLatin1(toupper((unsigned char)*p));

            auto *edit = makeRegEdit(digits);
            form->addRow(label, edit);
            m_regFields.push_back({(unsigned)idx, digits, edit});
            connectRegEdit(edit, (unsigned)idx, digits);
        }

        layout->addLayout(form);
    }

    rd_Cpu const *m_cpu = nullptr;
    QVBoxLayout *m_outerLayout;
    QWidget *m_content = nullptr;

    struct RegField {
        unsigned regIndex;
        int hexDigits;
        QLineEdit *edit;
    };
    std::vector<RegField> m_regFields;

    struct FlagGroup {
        unsigned regIndex;
        std::vector<std::pair<unsigned, QCheckBox*>> bits;
    };
    std::vector<FlagGroup> m_flagGroups;
};

/* ======================================================================== */
/* Debugger                                                                  */
/* ======================================================================== */

Debugger::Debugger(QWidget *parent)
    : QDockWidget("Debugger", parent)
{
    auto *container = new QWidget(this);
    auto *vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);

    /* Menu bar with Step menu */
    auto *menuBar = new QMenuBar(container);
    vbox->setMenuBar(menuBar);
    buildMenuBar(menuBar);

    /* Splitter: disasm left, registers right */
    auto *splitter = new QSplitter(Qt::Horizontal, container);

    auto *mw = qobject_cast<MainWindow *>(parentWidget());
    m_disasm = new DisasmView(mw, splitter);

    auto *rightPanel = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(4, 4, 4, 4);

    m_cpuCombo = new QComboBox(rightPanel);
    rightLayout->addWidget(m_cpuCombo);
    connect(m_cpuCombo, &QComboBox::currentIndexChanged,
            this, &Debugger::onCpuChanged);

    auto *scroll = new QScrollArea(rightPanel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_regs = new RegistersPane(scroll);
    scroll->setWidget(m_regs);
    rightLayout->addWidget(scroll);

    splitter->addWidget(m_disasm);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);

    m_disasm->setMinimumWidth(300);
    rightPanel->setFixedWidth(200);

    vbox->addWidget(splitter);
    setWidget(container);
    resize(800, 500);
}

void Debugger::buildMenuBar(QMenuBar *menuBar) {
    auto *stepMenu = menuBar->addMenu("&Step");

    auto *mw = qobject_cast<MainWindow *>(parentWidget());

    auto addAction = [&](const char *name, void (MainWindow::*slot)(),
                         const QKeySequence &key) {
        auto *action = stepMenu->addAction(name);
        action->setShortcut(key);
        if (mw) {
            connect(action, &QAction::triggered, mw, slot);
            mw->addAction(action);
        }
    };

    addAction("Resume",    &MainWindow::debugResume,   Qt::Key_F5);
    addAction("Step Over", &MainWindow::debugStepOver, Qt::Key_F10);
    addAction("Step In",   &MainWindow::debugStepIn,   Qt::Key_F11);
    addAction("Step Out",  &MainWindow::debugStepOut,
              QKeySequence(Qt::SHIFT | Qt::Key_F11));

    stepMenu->addSeparator();
    m_goToAction = stepMenu->addAction("Go to Address...");
    m_goToAction->setShortcut(QKeySequence("Ctrl+G"));
    m_goToAction->setEnabled(false);
    connect(m_goToAction, &QAction::triggered, this, &Debugger::goToAddress);
}

void Debugger::populateCpus() {
    m_cpuCombo->blockSignals(true);
    m_cpuCombo->clear();

    auto *sys = ar_debug_system();
    if (!sys) { m_cpuCombo->blockSignals(false); return; }

    int mainIdx = 0;
    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        auto *cpu = sys->v1.cpus[i];
        m_cpuCombo->addItem(QString("%1 — %2").arg(cpu->v1.id).arg(cpu->v1.description));
        if (cpu->v1.is_main) mainIdx = (int)i;
    }

    m_cpuCombo->blockSignals(false);

    if (sys->v1.num_cpus > 0) {
        m_cpuCombo->setCurrentIndex(mainIdx);
        onCpuChanged(mainIdx);
    }
}

void Debugger::onCpuChanged(int index) {
    auto *sys = ar_debug_system();
    if (!sys || index < 0 || (unsigned)index >= sys->v1.num_cpus) {
        m_cpu = nullptr;
        m_regs->setCpu(nullptr);
        return;
    }
    m_cpu = sys->v1.cpus[index];
    m_regs->setCpu(m_cpu);
}

void Debugger::goToAddress() {
    if (!m_lastPaused) return;

    bool ok;
    QString text = QInputDialog::getText(this, "Go to Address",
        "Address (hex):", QLineEdit::Normal, "", &ok);
    if (!ok || text.isEmpty()) return;

    uint64_t addr = text.toULongLong(&ok, 16);
    if (!ok) return;

    m_disasm->goToAddress(addr);
}

void Debugger::setThreadBlocked(bool blocked) {
    setWindowTitle(blocked ? "Debugger [thread blocked]" : "Debugger");
}

void Debugger::refresh(bool paused) {
    m_lastPaused = paused;
    if (m_goToAction) m_goToAction->setEnabled(paused);
    if (!isVisible()) return;

    auto *sys = ar_debug_system();
    if (sys != m_lastSystem) {
        m_cpuCombo->clear();
        m_cpu = nullptr;
        m_regs->setCpu(nullptr);
        m_lastSystem = sys;
        if (sys) populateCpus();
    }

    m_disasm->refresh(m_cpu, paused);
    if (paused)
        m_regs->refresh();
}

bool Debugger::event(QEvent *e) {
    if (e->type() == QEvent::WindowActivate && m_lastPaused && m_cpu) {
        m_disasm->refresh(m_cpu, true);
        m_regs->refresh();
    }
    return QDockWidget::event(e);
}

#include "Breakpoints.h"

#include <QListWidget>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QSplitter>
#include <QKeyEvent>
#include <QMessageBox>

#include "backend.hpp"

Breakpoints::Breakpoints(QWidget *parent)
    : QDockWidget("Breakpoints", parent)
{
    auto *splitter = new QSplitter(Qt::Horizontal);

    /* Left: list */
    m_list = new QListWidget;
    m_list->setFont(QFont("Monospace", 10));
    m_list->setMinimumWidth(160);
    m_list->installEventFilter(this);
    splitter->addWidget(m_list);

    /* Right: form */
    auto *formWidget = new QWidget;
    auto *form = new QFormLayout(formWidget);

    m_cpuCombo = new QComboBox;
    form->addRow("CPU", m_cpuCombo);

    m_addrEdit = new QLineEdit;
    m_addrEdit->setPlaceholderText("0000");
    form->addRow("Address", m_addrEdit);

    m_enabledCheck = new QCheckBox("Enabled");
    m_enabledCheck->setChecked(true);
    form->addRow(m_enabledCheck);

    m_tempCheck = new QCheckBox("Temporary");
    form->addRow(m_tempCheck);

    m_execCheck = new QCheckBox("Execute");
    m_execCheck->setChecked(true);
    form->addRow(m_execCheck);

    m_readCheck = new QCheckBox("Read");
    form->addRow(m_readCheck);

    m_writeCheck = new QCheckBox("Write");
    form->addRow(m_writeCheck);

    m_condEdit = new QLineEdit;
    m_condEdit->setEnabled(false);
    form->addRow("Condition", m_condEdit);

    m_addBtn = new QPushButton("Add");
    form->addRow(m_addBtn);

    m_replaceBtn = new QPushButton("Replace");
    m_replaceBtn->setEnabled(false);
    form->addRow(m_replaceBtn);

    m_deleteBtn = new QPushButton("Delete");
    m_deleteBtn->setEnabled(false);
    form->addRow(m_deleteBtn);

    splitter->addWidget(formWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    setWidget(splitter);
    resize(420, 300);

    connect(m_list, &QListWidget::currentItemChanged,
            this, &Breakpoints::onSelectionChanged);
    connect(m_addBtn, &QPushButton::clicked,
            this, &Breakpoints::onAdd);
    connect(m_replaceBtn, &QPushButton::clicked,
            this, &Breakpoints::onReplace);
    connect(m_deleteBtn, &QPushButton::clicked,
            this, &Breakpoints::onDelete);
}

bool Breakpoints::eventFilter(QObject *obj, QEvent *event) {
    if (obj == m_list && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
            if (m_list->currentItem()) {
                onDelete();
                return true;
            }
        }
    }
    return QDockWidget::eventFilter(obj, event);
}

void Breakpoints::refresh() {
    if (m_cpuCombo->count() == 0)
        refreshCpuCombo();
    unsigned count = ar_bp_count();
    if (count != m_lastCount) {
        rebuildList();
        return;
    }
    /* Check if content differs */
    if (count == 0) return;
    auto *bps = new ar_breakpoint[count];
    unsigned n = ar_bp_list(bps, count);
    bool differs = false;
    if ((int)n != m_list->count()) {
        differs = true;
    } else {
        for (unsigned i = 0; i < n; i++) {
            QListWidgetItem *item = m_list->item(i);
            int stored_id = item->data(Qt::UserRole).toInt();
            if (stored_id != bps[i].id) { differs = true; break; }
            /* Rebuild on any change */
            char flags_str[4] = "---";
            if (bps[i].flags & AR_BP_EXECUTE) flags_str[0] = 'X';
            if (bps[i].flags & AR_BP_READ)    flags_str[1] = 'R';
            if (bps[i].flags & AR_BP_WRITE)   flags_str[2] = 'W';
            QString addrStr;
            if (bps[i].cpu_id[0])
                addrStr = QString("%1.%2")
                    .arg(bps[i].cpu_id)
                    .arg(bps[i].address, 4, 16, QChar('0'));
            else
                addrStr = QString("%1").arg(bps[i].address, 4, 16, QChar('0'));
            QString core = QString("%1 %2").arg(addrStr).arg(flags_str);
            if (bps[i].temporary)
                core += " t";
            if (!bps[i].enabled)
                core = QString("(%1)").arg(core);
            QString label = core;
            if (bps[i].condition[0])
                label += QString(" %1").arg(bps[i].condition);
            if (item->text() != label) { differs = true; break; }
        }
    }
    delete[] bps;
    if (differs) rebuildList();
}

void Breakpoints::refreshCpuCombo() {
    QString prev = m_cpuCombo->currentData().toString();
    m_cpuCombo->clear();
    if (!ar_has_debug()) return;
    rd_System const *sys = ar_debug_system();
    if (!sys) return;
    for (unsigned i = 0; i < sys->v1.num_cpus; i++) {
        rd_Cpu const *c = sys->v1.cpus[i];
        QString label = QString::fromUtf8(c->v1.id);
        if (c->v1.is_main) label += " [primary]";
        m_cpuCombo->addItem(label, QString::fromUtf8(c->v1.id));
    }
    /* Restore previous selection */
    int idx = m_cpuCombo->findData(prev);
    if (idx >= 0) m_cpuCombo->setCurrentIndex(idx);
}

void Breakpoints::rebuildList() {
    int selectedId = -1;
    auto *cur = m_list->currentItem();
    if (cur) selectedId = cur->data(Qt::UserRole).toInt();

    m_list->clear();
    unsigned count = ar_bp_count();
    m_lastCount = count;
    if (count == 0) return;

    auto *bps = new ar_breakpoint[count];
    unsigned n = ar_bp_list(bps, count);

    int selectRow = -1;
    for (unsigned i = 0; i < n; i++) {
        char flags_str[4] = "---";
        if (bps[i].flags & AR_BP_EXECUTE) flags_str[0] = 'X';
        if (bps[i].flags & AR_BP_READ)    flags_str[1] = 'R';
        if (bps[i].flags & AR_BP_WRITE)   flags_str[2] = 'W';
        QString addrStr;
        if (bps[i].cpu_id[0])
            addrStr = QString("%1.%2")
                .arg(bps[i].cpu_id)
                .arg(bps[i].address, 4, 16, QChar('0'));
        else
            addrStr = QString("%1").arg(bps[i].address, 4, 16, QChar('0'));
        QString core = QString("%1 %2").arg(addrStr).arg(flags_str);
        if (bps[i].temporary)
            core += " t";
        if (!bps[i].enabled)
            core = QString("(%1)").arg(core);
        QString label = core;
        if (bps[i].condition[0])
            label += QString(" %1").arg(bps[i].condition);

        auto *item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, bps[i].id);
        m_list->addItem(item);

        if (bps[i].id == selectedId)
            selectRow = (int)i;
    }
    delete[] bps;

    if (selectRow >= 0)
        m_list->setCurrentRow(selectRow);
}

void Breakpoints::onSelectionChanged() {
    auto *item = m_list->currentItem();
    if (!item) {
        resetForm();
        return;
    }

    int id = item->data(Qt::UserRole).toInt();
    const ar_breakpoint *bp = ar_bp_get(id);
    if (!bp) { resetForm(); return; }

    m_addrEdit->setText(QString("%1").arg(bp->address, 4, 16, QChar('0')));
    m_enabledCheck->setChecked(bp->enabled);
    m_tempCheck->setChecked(bp->temporary);
    m_execCheck->setChecked(bp->flags & AR_BP_EXECUTE);
    m_readCheck->setChecked(bp->flags & AR_BP_READ);
    m_writeCheck->setChecked(bp->flags & AR_BP_WRITE);
    m_condEdit->setText(QString::fromUtf8(bp->condition));
    /* Select matching CPU in combo */
    if (bp->cpu_id[0]) {
        int idx = m_cpuCombo->findData(QString::fromUtf8(bp->cpu_id));
        if (idx >= 0) m_cpuCombo->setCurrentIndex(idx);
    } else {
        /* Primary CPU = first item */
        m_cpuCombo->setCurrentIndex(0);
    }
    m_replaceBtn->setEnabled(true);
    m_deleteBtn->setEnabled(true);
}

void Breakpoints::onAdd() {
    QString addrStr = m_addrEdit->text().trimmed();
    if (addrStr.isEmpty()) return;
    uint64_t addr = addrStr.toULongLong(nullptr, 16);

    unsigned flags = 0;
    if (m_execCheck->isChecked())  flags |= AR_BP_EXECUTE;
    if (m_readCheck->isChecked())  flags |= AR_BP_READ;
    if (m_writeCheck->isChecked()) flags |= AR_BP_WRITE;
    if (flags == 0) flags = AR_BP_EXECUTE;

    bool enabled = m_enabledCheck->isChecked();
    bool temporary = m_tempCheck->isChecked();
    QString cond = m_condEdit->text().trimmed();

    /* Pass CPU ID: NULL for primary (first/is_main), otherwise the selected ID */
    const char *cpu_id = nullptr;
    QByteArray cpuBuf = m_cpuCombo->currentData().toString().toUtf8();
    if (m_cpuCombo->currentIndex() > 0)
        cpu_id = cpuBuf.constData();

    QByteArray condBuf = cond.toUtf8();
    int id = ar_bp_add(addr, flags, enabled, temporary, cond.isEmpty() ? nullptr : condBuf.constData(), cpu_id);
    if (id < 0)
        QMessageBox::warning(this, "Breakpoint Error",
            "Failed to add breakpoint.\n"
            "One or more subscriptions may not be supported for this memory region.");
    rebuildList();
}

void Breakpoints::onReplace() {
    auto *item = m_list->currentItem();
    if (!item) return;
    int id = item->data(Qt::UserRole).toInt();

    QString addrStr = m_addrEdit->text().trimmed();
    if (addrStr.isEmpty()) return;
    uint64_t addr = addrStr.toULongLong(nullptr, 16);

    unsigned flags = 0;
    if (m_execCheck->isChecked())  flags |= AR_BP_EXECUTE;
    if (m_readCheck->isChecked())  flags |= AR_BP_READ;
    if (m_writeCheck->isChecked()) flags |= AR_BP_WRITE;
    if (flags == 0) flags = AR_BP_EXECUTE;

    bool enabled = m_enabledCheck->isChecked();
    bool temporary = m_tempCheck->isChecked();
    QString cond = m_condEdit->text().trimmed();

    const char *cpu_id = nullptr;
    QByteArray cpuBuf = m_cpuCombo->currentData().toString().toUtf8();
    if (m_cpuCombo->currentIndex() > 0)
        cpu_id = cpuBuf.constData();

    QByteArray condBuf = cond.toUtf8();
    bool ok = ar_bp_replace(id, addr, flags, enabled, temporary,
                            cond.isEmpty() ? nullptr : condBuf.constData(), cpu_id);
    if (!ok)
        QMessageBox::warning(this, "Breakpoint Error",
            "Failed to update breakpoint.\n"
            "One or more subscriptions may not be supported for this memory region.");
    rebuildList();
}

void Breakpoints::onDelete() {
    auto *item = m_list->currentItem();
    if (!item) return;
    int id = item->data(Qt::UserRole).toInt();
    ar_bp_delete(id);
    m_list->setCurrentItem(nullptr);
    resetForm();
    rebuildList();
}

void Breakpoints::resetForm() {
    m_cpuCombo->setCurrentIndex(0);
    m_addrEdit->setText("0");
    m_enabledCheck->setChecked(true);
    m_tempCheck->setChecked(false);
    m_execCheck->setChecked(true);
    m_readCheck->setChecked(false);
    m_writeCheck->setChecked(false);
    m_condEdit->clear();
    m_replaceBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);
}

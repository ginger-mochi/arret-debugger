#include "MemorySearch.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QRadioButton>
#include <QMenu>
#include <QVector>

#include "backend.hpp"
#include "MainWindow.h"

MemorySearch::MemorySearch(QWidget *parent)
    : QDockWidget("Memory Search", parent)
{
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);

    /* Row 1: Region, Size, Alignment, Reset */
    auto *row1 = new QHBoxLayout;
    row1->addWidget(new QLabel("Region:"));
    m_regionCombo = new QComboBox;
    row1->addWidget(m_regionCombo, 1);

    row1->addWidget(new QLabel("Size:"));
    m_sizeCombo = new QComboBox;
    m_sizeCombo->addItem("1", 1);
    m_sizeCombo->addItem("2", 2);
    m_sizeCombo->addItem("4", 4);
    row1->addWidget(m_sizeCombo);

    row1->addWidget(new QLabel("Align:"));
    m_alignCombo = new QComboBox;
    m_alignCombo->addItem("1", 1);
    m_alignCombo->addItem("2", 2);
    m_alignCombo->addItem("4", 4);
    row1->addWidget(m_alignCombo);

    m_resetBtn = new QPushButton("New Search");
    row1->addWidget(m_resetBtn);
    layout->addLayout(row1);

    /* Row 2: Op, compare-against radio, value input, Filter */
    auto *row2 = new QHBoxLayout;
    row2->addWidget(new QLabel("Op:"));
    m_opCombo = new QComboBox;
    m_opCombo->addItem("==", 0);
    m_opCombo->addItem("!=", 1);
    m_opCombo->addItem("<",  2);
    m_opCombo->addItem(">",  3);
    m_opCombo->addItem("<=", 4);
    m_opCombo->addItem(">=", 5);
    row2->addWidget(m_opCombo);

    m_prevRadio = new QRadioButton("Previous");
    m_prevRadio->setChecked(true);
    m_valueRadio = new QRadioButton("Value:");
    row2->addWidget(m_prevRadio);
    row2->addWidget(m_valueRadio);

    m_valueEdit = new QLineEdit;
    m_valueEdit->setPlaceholderText("0");
    row2->addWidget(m_valueEdit);

    m_filterBtn = new QPushButton("Filter");
    row2->addWidget(m_filterBtn);
    layout->addLayout(row2);

    /* Candidate count */
    m_countLabel = new QLabel("Candidates: —");
    layout->addWidget(m_countLabel);

    /* Results table */
    m_table = new QTableWidget(0, 3);
    m_table->setHorizontalHeaderLabels({"Address", "Value", "Previous"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_table, 1);

    setWidget(container);

    /* Connections */
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &MemorySearch::onTableContextMenu);
    connect(m_resetBtn, &QPushButton::clicked, this, &MemorySearch::onReset);
    connect(m_filterBtn, &QPushButton::clicked, this, &MemorySearch::onFilter);
    connect(m_prevRadio, &QRadioButton::toggled, this, &MemorySearch::onCompareChanged);
    connect(m_valueRadio, &QRadioButton::toggled, this, &MemorySearch::onCompareChanged);

    populateRegions();
    onCompareChanged();
    m_filterBtn->setEnabled(ar_search_active());
}

void MemorySearch::populateRegions() {
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
            QString("%1 (%2 bytes)")
                .arg(m->v1.id)
                .arg(m->v1.size),
            QVariant::fromValue(QString(m->v1.id)));
    }
}

void MemorySearch::onReset() {
    if (m_regionCombo->currentIndex() < 0) return;

    QString regionId = m_regionCombo->currentData().toString();
    int size = m_sizeCombo->currentData().toInt();
    int align = m_alignCombo->currentData().toInt();

    if (ar_search_reset(regionId.toUtf8().constData(), size, align)) {
        m_filterBtn->setEnabled(true);
        updateResults();
    } else {
        m_countLabel->setText("Candidates: reset failed");
    }
}

void MemorySearch::onFilter() {
    if (!ar_search_active()) return;

    static const ar_search_op ops[] = {
        AR_SEARCH_EQ, AR_SEARCH_NE, AR_SEARCH_LT,
        AR_SEARCH_GT, AR_SEARCH_LE, AR_SEARCH_GE,
    };
    ar_search_op op = ops[m_opCombo->currentData().toInt()];

    uint64_t value = AR_SEARCH_VS_PREV;
    if (m_valueRadio->isChecked()) {
        bool ok;
        value = m_valueEdit->text().toULongLong(&ok, 0);
    }

    ar_search_filter(op, value);
    updateResults();
}

void MemorySearch::onCompareChanged() {
    bool useValue = m_valueRadio->isChecked();
    m_valueEdit->setEnabled(useValue);
    if (!useValue) m_valueEdit->clear();
}

static constexpr unsigned MAX_DISPLAY = 500;

void MemorySearch::updateResults() {
    uint64_t count = ar_search_count();
    m_countLabel->setText(QString("Candidates: %1").arg(count));

    if (count > MAX_DISPLAY) {
        m_table->setRowCount(0);
        return;
    }

    auto *results = new ar_search_result[MAX_DISPLAY];
    unsigned n = ar_search_results(results, MAX_DISPLAY);

    m_table->setRowCount((int)n);
    for (unsigned i = 0; i < n; i++) {
        m_table->setItem((int)i, 0,
            new QTableWidgetItem(QString("%1").arg(results[i].addr, 4, 16, QChar('0')).toUpper()));
        m_table->setItem((int)i, 1,
            new QTableWidgetItem(QString::number(results[i].value)));
        m_table->setItem((int)i, 2,
            new QTableWidgetItem(QString::number(results[i].prev)));
    }

    delete[] results;
}

void MemorySearch::refresh() {
    if (!ar_search_active()) return;

    uint64_t count = ar_search_count();
    m_countLabel->setText(QString("Candidates: %1").arg(count));

    if (count > MAX_DISPLAY) {
        if (m_table->rowCount() > 0)
            m_table->setRowCount(0);
        return;
    }

    /* Re-read values for visible rows */
    int rows = m_table->rowCount();
    if (rows == 0) return;

    auto *results = new ar_search_result[(unsigned)rows];
    unsigned n = ar_search_results(results, (unsigned)rows);

    for (unsigned i = 0; i < n && (int)i < rows; i++) {
        auto *valItem = m_table->item((int)i, 1);
        if (valItem)
            valItem->setText(QString::number(results[i].value));
    }

    delete[] results;
}

void MemorySearch::onTableContextMenu(const QPoint &pos) {
    auto *item = m_table->itemAt(pos);
    if (!item) return;

    int row = item->row();
    auto *addrItem = m_table->item(row, 0);
    if (!addrItem) return;

    bool ok;
    uint64_t addr = addrItem->text().toULongLong(&ok, 16);
    if (!ok) return;

    /* Find the memory region used by the current search */
    QString regionId = m_regionCombo->currentData().toString();
    rd_Memory const *mem = ar_find_memory_by_id(regionId.toUtf8().constData());

    QMenu menu(this);
    auto *viewAction = menu.addAction("View in memory");
    if (menu.exec(m_table->viewport()->mapToGlobal(pos)) == viewAction) {
        auto *mw = qobject_cast<MainWindow *>(parent());
        if (mw)
            mw->openMemoryViewerAt(mem, addr);
    }
}

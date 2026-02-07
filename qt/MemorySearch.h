#ifndef MEMORYSEARCH_H
#define MEMORYSEARCH_H

#include <QDockWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QTableWidget;
class QRadioButton;
QT_END_NAMESPACE

class MemorySearch : public QDockWidget {
    Q_OBJECT
public:
    explicit MemorySearch(QWidget *parent = nullptr);
    void refresh();

private slots:
    void onReset();
    void onFilter();
    void onCompareChanged();
    void onTableContextMenu(const QPoint &pos);

private:
    void populateRegions();
    void updateResults();

    QComboBox    *m_regionCombo;
    QComboBox    *m_sizeCombo;
    QComboBox    *m_alignCombo;
    QComboBox    *m_opCombo;
    QRadioButton *m_valueRadio;
    QRadioButton *m_prevRadio;
    QLineEdit    *m_valueEdit;
    QPushButton  *m_resetBtn;
    QPushButton  *m_filterBtn;
    QLabel       *m_countLabel;
    QTableWidget *m_table;
};

#endif // MEMORYSEARCH_H

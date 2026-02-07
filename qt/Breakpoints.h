#ifndef BREAKPOINTS_H
#define BREAKPOINTS_H

#include <QDockWidget>

QT_BEGIN_NAMESPACE
class QListWidget;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QPushButton;
QT_END_NAMESPACE

class Breakpoints : public QDockWidget {
    Q_OBJECT
public:
    explicit Breakpoints(QWidget *parent = nullptr);
    void refresh();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onSelectionChanged();
    void onAdd();
    void onReplace();
    void onDelete();

private:
    void rebuildList();
    void resetForm();
    void refreshCpuCombo();

    QListWidget  *m_list;
    QComboBox    *m_cpuCombo;
    QLineEdit    *m_addrEdit;
    QCheckBox    *m_enabledCheck;
    QCheckBox    *m_tempCheck;
    QCheckBox    *m_execCheck;
    QCheckBox    *m_readCheck;
    QCheckBox    *m_writeCheck;
    QLineEdit    *m_condEdit;
    QPushButton  *m_addBtn;
    QPushButton  *m_replaceBtn;
    QPushButton  *m_deleteBtn;

    unsigned      m_lastCount = 0;
};

#endif // BREAKPOINTS_H

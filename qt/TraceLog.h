#ifndef TRACELOG_H
#define TRACELOG_H

#include <QDockWidget>
#include <QVector>

QT_BEGIN_NAMESPACE
class QPlainTextEdit;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QLabel;
class QVBoxLayout;
QT_END_NAMESPACE

class TraceLog : public QDockWidget {
    Q_OBJECT
public:
    explicit TraceLog(QWidget *parent = nullptr);

    /* Called from MainWindow::tick() */
    void refresh();

private slots:
    void browseFile();
    void toggleTrace();

private:
    void populateCpus();
    void updateUI();

    QPlainTextEdit *m_logView;
    QLineEdit      *m_filePath;
    QCheckBox      *m_instrCheck;
    QCheckBox      *m_intCheck;
    QCheckBox      *m_regCheck;
    QCheckBox      *m_indentCheck;
    QPushButton    *m_startBtn;
    QLabel         *m_lineCount;
    QVBoxLayout    *m_cpuLayout;

    struct CpuCheck {
        QString id;
        QCheckBox *check;
    };
    QVector<CpuCheck> m_cpuChecks;
    bool m_cpusPopulated = false;
};

#endif // TRACELOG_H

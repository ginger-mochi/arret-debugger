#ifndef INPUTTOOL_H
#define INPUTTOOL_H

#include <QDockWidget>
#include <QVector>

QT_BEGIN_NAMESPACE
class QComboBox;
class QCheckBox;
class QSlider;
class QLabel;
class QGroupBox;
QT_END_NAMESPACE

class InputTool : public QDockWidget {
    Q_OBJECT
public:
    explicit InputTool(QWidget *parent = nullptr);

    /* Called from MainWindow::tick() */
    void refresh();

private slots:
    void clearAll();

private:
    struct ButtonRow { unsigned retroId; QComboBox *combo; };
    QVector<ButtonRow> m_buttons;

    struct AnalogAxis { unsigned index, axis; QCheckBox *fixCheck; QSlider *slider; QLabel *valueLabel; };
    QVector<AnalogAxis> m_axes;

    QGroupBox *m_analogGroup;
    bool m_suppressSync = false;
};

#endif // INPUTTOOL_H

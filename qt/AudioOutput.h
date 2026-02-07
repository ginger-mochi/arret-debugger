#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H

#include <QObject>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QAudioSink;
class QIODevice;
QT_END_NAMESPACE

class AudioOutput : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();

    void start();
    void stop();

private slots:
    void pullAudio();

private:
    QAudioSink *m_sink = nullptr;
    QIODevice  *m_io   = nullptr;
    QTimer     *m_timer = nullptr;
};

#endif // AUDIOOUTPUT_H

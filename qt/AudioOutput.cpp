#include "AudioOutput.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QIODevice>

#include "backend.hpp"

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, &AudioOutput::pullAudio);
}

AudioOutput::~AudioOutput() {
    stop();
}

void AudioOutput::start() {
    if (m_sink) return;

    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);

    m_sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), fmt, this);
    m_sink->setBufferSize(4096);
    m_io = m_sink->start();

    /* Pull timer at ~5ms intervals */
    m_timer->start(5);
}

void AudioOutput::stop() {
    m_timer->stop();
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
        m_io = nullptr;
    }
}

void AudioOutput::pullAudio() {
    if (!m_io || !m_sink) return;

    int avail = m_sink->bytesFree();
    if (avail <= 0) return;

    /* Each frame = 2 * int16_t = 4 bytes */
    int max_frames = avail / 4;
    if (max_frames <= 0) return;
    if (max_frames > 2048) max_frames = 2048;

    int16_t buf[2048 * 2];
    unsigned got = ar_audio_read(buf, (unsigned)max_frames);
    if (got > 0)
        m_io->write(reinterpret_cast<const char *>(buf), got * 4);
}

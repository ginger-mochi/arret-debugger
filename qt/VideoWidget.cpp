#include "VideoWidget.h"

#include <QPainter>
#include <QKeyEvent>

#include "backend.hpp"

VideoWidget::VideoWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(160, 144);
}

void VideoWidget::paintEvent(QPaintEvent *) {
    const uint32_t *buf = ar_frame_buf();
    unsigned w = ar_frame_width();
    unsigned h = ar_frame_height();

    if (!buf || w == 0 || h == 0) return;

    /* XRGB8888 maps to QImage::Format_RGB32 (0xffRRGGBB) */
    QImage img(reinterpret_cast<const uchar *>(buf),
               w, h, w * 4, QImage::Format_RGB32);

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(rect(), img);
}

void VideoWidget::keyPressEvent(QKeyEvent *event) {
    if (event->isAutoRepeat()) return;
    handleKey(event->key(), true);
}

void VideoWidget::keyReleaseEvent(QKeyEvent *event) {
    if (event->isAutoRepeat()) return;
    handleKey(event->key(), false);
}

void VideoWidget::handleKey(int key, bool pressed) {
    if (!ar_manual_input()) return;

    int16_t val = pressed ? 1 : 0;
    switch (key) {
    case Qt::Key_Up:     ar_set_input(RETRO_DEVICE_ID_JOYPAD_UP, val);     break;
    case Qt::Key_Down:   ar_set_input(RETRO_DEVICE_ID_JOYPAD_DOWN, val);   break;
    case Qt::Key_Left:   ar_set_input(RETRO_DEVICE_ID_JOYPAD_LEFT, val);   break;
    case Qt::Key_Right:  ar_set_input(RETRO_DEVICE_ID_JOYPAD_RIGHT, val);  break;
    case Qt::Key_Z:      ar_set_input(RETRO_DEVICE_ID_JOYPAD_B, val);      break;
    case Qt::Key_X:      ar_set_input(RETRO_DEVICE_ID_JOYPAD_A, val);      break;
    case Qt::Key_Return: ar_set_input(RETRO_DEVICE_ID_JOYPAD_START, val);  break;
    case Qt::Key_Shift:  ar_set_input(RETRO_DEVICE_ID_JOYPAD_SELECT, val); break;
    default: break;
    }
}

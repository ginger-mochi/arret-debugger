#include "InputTool.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QGroupBox>
#include <QPushButton>
#include <QWidget>

#include "backend.hpp"
#include "libretro.h"

static const struct { const char *label; unsigned id; } kButtons[] = {
    {"Up",     RETRO_DEVICE_ID_JOYPAD_UP},
    {"Down",   RETRO_DEVICE_ID_JOYPAD_DOWN},
    {"Left",   RETRO_DEVICE_ID_JOYPAD_LEFT},
    {"Right",  RETRO_DEVICE_ID_JOYPAD_RIGHT},
    {"B",      RETRO_DEVICE_ID_JOYPAD_B},
    {"A",      RETRO_DEVICE_ID_JOYPAD_A},
    {"Y",      RETRO_DEVICE_ID_JOYPAD_Y},
    {"X",      RETRO_DEVICE_ID_JOYPAD_X},
    {"Select", RETRO_DEVICE_ID_JOYPAD_SELECT},
    {"Start",  RETRO_DEVICE_ID_JOYPAD_START},
    {"L",      RETRO_DEVICE_ID_JOYPAD_L},
    {"R",      RETRO_DEVICE_ID_JOYPAD_R},
    {"L2",     RETRO_DEVICE_ID_JOYPAD_L2},
    {"R2",     RETRO_DEVICE_ID_JOYPAD_R2},
    {"L3",     RETRO_DEVICE_ID_JOYPAD_L3},
    {"R3",     RETRO_DEVICE_ID_JOYPAD_R3},
};
static constexpr int kNumButtons = sizeof(kButtons) / sizeof(kButtons[0]);

static const struct { const char *label; unsigned index, axis; } kAxes[] = {
    {"Left X",  0, 0},
    {"Left Y",  0, 1},
    {"Right X", 1, 0},
    {"Right Y", 1, 1},
};
static constexpr int kNumAxes = sizeof(kAxes) / sizeof(kAxes[0]);

InputTool::InputTool(QWidget *parent)
    : QDockWidget("Input", parent)
{
    auto *container = new QWidget;
    auto *mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(6, 6, 6, 6);

    /* Buttons section */
    auto *btnForm = new QFormLayout;
    btnForm->setSpacing(4);
    for (int i = 0; i < kNumButtons; i++) {
        auto *combo = new QComboBox;
        combo->addItem("---");
        combo->addItem("Released");
        combo->addItem("Pressed");

        unsigned retroId = kButtons[i].id;
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, retroId](int idx) {
            if (m_suppressSync) return;
            if (idx == 0)
                ar_input_unfix(retroId);
            else
                ar_input_fix(retroId, idx == 2 ? 1 : 0);
        });

        btnForm->addRow(kButtons[i].label, combo);
        m_buttons.append({retroId, combo});
    }
    mainLayout->addLayout(btnForm);

    /* Analog section */
    m_analogGroup = new QGroupBox("Analog");
    auto *analogLayout = new QFormLayout(m_analogGroup);
    analogLayout->setSpacing(4);

    for (int i = 0; i < kNumAxes; i++) {
        auto *row = new QHBoxLayout;
        auto *fixCheck = new QCheckBox("Fix");
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(-32768, 32767);
        slider->setValue(0);
        slider->setEnabled(false);
        auto *valueLabel = new QLabel("0");
        valueLabel->setFixedWidth(50);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        row->addWidget(fixCheck);
        row->addWidget(slider, 1);
        row->addWidget(valueLabel);

        unsigned idx = kAxes[i].index;
        unsigned axis = kAxes[i].axis;

        connect(fixCheck, &QCheckBox::toggled, this, [this, idx, axis, slider](bool checked) {
            if (m_suppressSync) return;
            slider->setEnabled(checked);
            if (checked)
                ar_analog_fix(idx, axis, (int16_t)slider->value());
            else
                ar_analog_unfix(idx, axis);
        });

        connect(slider, &QSlider::valueChanged, this, [this, idx, axis, fixCheck, valueLabel](int value) {
            valueLabel->setText(QString::number(value));
            if (m_suppressSync) return;
            if (fixCheck->isChecked())
                ar_analog_fix(idx, axis, (int16_t)value);
        });

        analogLayout->addRow(kAxes[i].label, row);
        m_axes.append({idx, axis, fixCheck, slider, valueLabel});
    }

    mainLayout->addWidget(m_analogGroup);

    /* Clear All button */
    auto *clearBtn = new QPushButton("Clear All");
    connect(clearBtn, &QPushButton::clicked, this, &InputTool::clearAll);
    mainLayout->addWidget(clearBtn);

    mainLayout->addStretch();
    setWidget(container);
    resize(280, 520);
}

void InputTool::clearAll() {
    ar_input_unfix_all();

    m_suppressSync = true;
    for (auto &b : m_buttons)
        b.combo->setCurrentIndex(0);
    for (auto &a : m_axes) {
        a.fixCheck->setChecked(false);
        a.slider->setValue(0);
        a.slider->setEnabled(false);
        a.valueLabel->setText("0");
    }
    m_suppressSync = false;
}

void InputTool::refresh() {
    m_suppressSync = true;

    for (auto &b : m_buttons) {
        int expected;
        if (!ar_input_is_fixed(b.retroId))
            expected = 0;
        else
            expected = ar_input_fixed_value(b.retroId) ? 2 : 1;
        if (b.combo->currentIndex() != expected)
            b.combo->setCurrentIndex(expected);
    }

    for (auto &a : m_axes) {
        bool fixed = ar_analog_is_fixed(a.index, a.axis);
        if (a.fixCheck->isChecked() != fixed)
            a.fixCheck->setChecked(fixed);
        a.slider->setEnabled(fixed);
        if (fixed) {
            int16_t val = ar_analog_fixed_value(a.index, a.axis);
            if (a.slider->value() != val)
                a.slider->setValue(val);
            a.valueLabel->setText(QString::number(val));
        }
    }

    bool showAnalog = ar_controller_has_analog();
    if (m_analogGroup->isVisible() != showAnalog)
        m_analogGroup->setVisible(showAnalog);

    m_suppressSync = false;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/toolwidgets/toolsettings.h"
#include "desktop/widgets/kis_slider_spin_box.h"
#include "libclient/tools/freehand.h"
#include "libclient/tools/toolcontroller.h"
#include "libclient/tools/toolproperties.h"
#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <functional>

namespace tools {

QWidget *ToolSettings::createUi(QWidget *parent)
{
	Q_ASSERT(!m_widget);
	m_widget = createUiWidget(parent);

	if(m_widget && m_widget->layout() && toolType() == QStringLiteral("brush")) {
		Freehand *freehand =
			static_cast<Freehand *>(m_ctrl->getTool(Tool::FREEHAND));
		if(freehand) {
			QGroupBox *group = new QGroupBox(tr("Symmetry"), m_widget);
			QVBoxLayout *groupLayout = new QVBoxLayout(group);
			QHBoxLayout *modeLayout = new QHBoxLayout;
			QLabel *modeLabel = new QLabel(tr("Mode:"), group);
			QComboBox *mode = new QComboBox(group);
			mode->addItem(
				tr("Off"), int(Freehand::SymmetryMode::Off));
			mode->addItem(
				tr("Vertical"), int(Freehand::SymmetryMode::Vertical));
			mode->addItem(
				tr("Horizontal"), int(Freehand::SymmetryMode::Horizontal));
			mode->addItem(
				tr("Both (4-way)"), int(Freehand::SymmetryMode::Both));
			mode->setCurrentIndex(mode->findData(int(freehand->symmetryMode())));
			modeLayout->addWidget(modeLabel);
			modeLayout->addWidget(mode, 1);
			groupLayout->addLayout(modeLayout);

			QHBoxLayout *optionsLayout = new QHBoxLayout;
			QPushButton *center = new QPushButton(tr("Set center here"), group);
			center->setToolTip(tr(
				"Move the cursor over the desired canvas position, then click this button."));
			QCheckBox *guides = new QCheckBox(tr("Show guides"), group);
			guides->setChecked(freehand->symmetryGuidesVisible());
			optionsLayout->addWidget(center);
			optionsLayout->addWidget(guides);
			optionsLayout->addStretch(1);
			groupLayout->addLayout(optionsLayout);

			connect(
				mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[freehand, mode](int index) {
					freehand->setSymmetryMode(
						static_cast<Freehand::SymmetryMode>(
							mode->itemData(index).toInt()));
				});
			connect(center, &QPushButton::clicked, this, [freehand] {
				freehand->setSymmetryCenterToCursor();
			});
			connect(
				guides, &QCheckBox::toggled, this,
				[freehand](bool checked) {
					freehand->setSymmetryGuidesVisible(checked);
				});

			m_widget->layout()->addWidget(group);
		}
	}

	return m_widget;
}

void ToolSettings::pushSettings() {}

ToolProperties ToolSettings::saveToolSettings()
{
	return ToolProperties();
}

void ToolSettings::restoreToolSettings(const ToolProperties &) {}

static int
step(int min, int max, int current, bool increase, std::function<int(int)> inc)
{
	int size = min;
	while(size <= current) {
		int next = inc(size);
		if(increase) {
			if(next > current) {
				return qMin(next, max);
			}
		} else if(size < current && next >= current) {
			return qMax(size, min);
		}
		size = next;
	}
	return increase ? max : min;
}

int ToolSettings::stepLogarithmic(
	int min, int max, int current, bool increase, double stepSize)
{
	return step(min, max, current, increase, [&](int size) {
		return size + qMax(1, qCeil(size / stepSize));
	});
}

int ToolSettings::stepLinear(
	int min, int max, int current, bool increase, int stepSize)
{
	return step(min, max, current, increase, [&](int size) {
		return size + stepSize;
	});
}

void ToolSettings::checkGroupButton(QButtonGroup *group, int id)
{
	QAbstractButton *button = group->button(id);
	if(button) {
		button->setChecked(true);
	}
}

void ToolSettings::quickAdjustOn(
	KisSliderSpinBox *slider, qreal adjustment, bool wheel, qreal &quickAdjustN)
{
	if(slider && slider->isEnabled()) {
		if(wheel) {
			int i;
			if(adjustment < 0.0) {
				i = qMin(-1, qRound(adjustment));
			} else if(adjustment > 0.0) {
				i = qMax(1, qRound(adjustment));
			} else {
				return;
			}
			quickAdjustN = 0.0;
			adjustSlider(slider, slider->value() + i);
		} else {
			quickAdjustN += adjustment;
			qreal i;
			qreal f = modf(quickAdjustN, &i);
			int delta = int(i);
			if(delta != 0) {
				quickAdjustN = f;
				adjustSlider(slider, slider->value() + delta);
			}
		}
	}
}

void ToolSettings::adjustSlider(KisSliderSpinBox *slider, int value)
{
	if(slider->isSoftRangeActive()) {
		slider->setValue(
			qBound(slider->softMinimum(), value, slider->softMaximum()));
	} else {
		slider->setValue(value);
	}
}

}

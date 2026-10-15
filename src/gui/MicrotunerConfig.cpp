/*
 * MicrotunerConfig.cpp - configuration widget for scales and keymaps
 *
 * Copyright (c) 2020 Martin Pavelek <he29.HS/at/gmail.com>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "MicrotunerConfig.h"

#include <limits>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>

#include "ComboBox.h"
#include "embed.h"
#include "Engine.h"
#include "FileDialog.h"
#include "GuiApplication.h"
#include "Keymap.h"
#include "LcdSpinBox.h"
#include "lmms_constants.h"
#include "lmmsversion.h"
#include "MainWindow.h"
#include "Note.h"
#include "Scale.h"
#include "Song.h"
#include "SubWindow.h"
#include "TuningFile.h"

namespace lmms::gui
{


MicrotunerConfig::MicrotunerConfig() :
	QWidget(),
	m_scaleComboModel(nullptr, tr("Selected scale slot")),
	m_keymapComboModel(nullptr, tr("Selected keymap slot")),
	m_firstKeyModel(0, 0, NumKeys - 1, nullptr, tr("First key")),
	m_lastKeyModel(NumKeys - 1, 0, NumKeys - 1, nullptr, tr("Last key")),
	m_middleKeyModel(DefaultMiddleKey, 0, NumKeys - 1, nullptr, tr("Middle key")),
	m_baseKeyModel(DefaultBaseKey, 0, NumKeys - 1, nullptr, tr("Base key"))
{
	setWindowIcon(embed::getIconPixmap("microtuner"));
	setWindowTitle(tr("Microtuner Configuration"));

	// Organize into 2 main columns: scales and keymaps
	auto microtunerLayout = new QGridLayout();
	microtunerLayout->setSpacing(2);

	// ----------------------------------
	// Scale sub-column
	//
	auto scaleLabel = new QLabel(tr("Scale slot to edit:"));
	microtunerLayout->addWidget(scaleLabel, 0, 0, 1, 2, Qt::AlignBottom);

	for (unsigned int i = 0; i < MaxScaleCount; i++)
	{
		m_scaleComboModel.addItem(QString::number(i) + ": " + Engine::getSong()->getScale(i)->getDescription());
	}
	auto scaleCombo = new ComboBox();
	scaleCombo->setModel(&m_scaleComboModel);
	microtunerLayout->addWidget(scaleCombo, 1, 0, 1, 2);
	connect(&m_scaleComboModel, &ComboBoxModel::dataChanged, this, &MicrotunerConfig::updateScaleForm);

	m_scaleNameEdit = new QLineEdit("12-TET");
	m_scaleNameEdit->setToolTip(tr("Scale description. Cannot start with \"!\" and cannot contain a newline character."));
	microtunerLayout->addWidget(m_scaleNameEdit, 2, 0, 1, 2);

	auto loadScaleButton = new QPushButton(tr("Load"));
	auto saveScaleButton = new QPushButton(tr("Save"));
	loadScaleButton->setToolTip(tr("Load scale definition from a file."));
	saveScaleButton->setToolTip(tr("Save scale definition to a file."));
	microtunerLayout->addWidget(loadScaleButton, 3, 0, 1, 1);
	microtunerLayout->addWidget(saveScaleButton, 3, 1, 1, 1);
	connect(loadScaleButton, &QPushButton::clicked, this, &MicrotunerConfig::loadScaleFromFile);
	connect(saveScaleButton, &QPushButton::clicked, this, &MicrotunerConfig::saveScaleToFile);

	m_scaleTextEdit = new QPlainTextEdit();
	m_scaleTextEdit->setPlainText("100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n700.0\n800.0\n900.0\n1000.0\n1100.0\n1200.0");
	m_scaleTextEdit->setToolTip(tr("Enter intervals on separate lines. Numbers containing a decimal point are treated as cents.\nOther inputs are treated as integer ratios and must be in the form of \'a/b\' or \'a\'.\nUnity (0.0 cents or ratio 1/1) is always present as a hidden first value; do not enter it manually."));
	microtunerLayout->addWidget(m_scaleTextEdit, 4, 0, 2, 2);

	auto applyScaleButton = new QPushButton(tr("Apply scale changes"));
	applyScaleButton->setToolTip(tr("Verify and apply changes made to the selected scale. To use the scale, select it in the settings of a supported instrument."));
	microtunerLayout->addWidget(applyScaleButton, 6, 0, 1, 2);
	connect(applyScaleButton, &QPushButton::clicked, this, &MicrotunerConfig::applyScale);

	// ----------------------------------
	// Mapping sub-column
	//
	auto keymapLabel = new QLabel(tr("Keymap slot to edit:"));
	microtunerLayout->addWidget(keymapLabel, 0, 2, 1, 2, Qt::AlignBottom);

	for (unsigned int i = 0; i < MaxKeymapCount; i++)
	{
		m_keymapComboModel.addItem(QString::number(i) + ": " + Engine::getSong()->getKeymap(i)->getDescription());
	}
	auto keymapCombo = new ComboBox();
	keymapCombo->setModel(&m_keymapComboModel);
	microtunerLayout->addWidget(keymapCombo, 1, 2, 1, 2);
	connect(&m_keymapComboModel, &ComboBoxModel::dataChanged, this, &MicrotunerConfig::updateKeymapForm);

	m_keymapNameEdit = new QLineEdit("default");
	m_keymapNameEdit->setToolTip(tr("Keymap description. Cannot start with \"!\" and cannot contain a newline character."));
	microtunerLayout->addWidget(m_keymapNameEdit, 2, 2, 1, 2);

	auto loadKeymapButton = new QPushButton(tr("Load"));
	auto saveKeymapButton = new QPushButton(tr("Save"));
	loadKeymapButton->setToolTip(tr("Load key mapping definition from a file."));
	saveKeymapButton->setToolTip(tr("Save key mapping definition to a file."));
	microtunerLayout->addWidget(loadKeymapButton, 3, 2, 1, 1);
	microtunerLayout->addWidget(saveKeymapButton, 3, 3, 1, 1);
	connect(loadKeymapButton, &QPushButton::clicked, this, &MicrotunerConfig::loadKeymapFromFile);
	connect(saveKeymapButton, &QPushButton::clicked, this, &MicrotunerConfig::saveKeymapToFile);

	m_keymapTextEdit = new QPlainTextEdit();
	m_keymapTextEdit->setPlainText("0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11");
	m_keymapTextEdit->setToolTip(tr("Enter key mappings on separate lines. Each line assigns a scale degree to a MIDI key,\nstarting with the middle key and continuing in sequence.\nThe pattern repeats for keys outside of the explicit keymap range.\nMultiple keys can be mapped to the same scale degree.\nEnter \'x\' if you wish to leave the key disabled / not mapped."));
	microtunerLayout->addWidget(m_keymapTextEdit, 4, 2, 1, 2);

	// Mapping ranges
	auto keymapRangeLayout = new QGridLayout();
	microtunerLayout->addLayout(keymapRangeLayout, 5, 2, 1, 2, Qt::AlignCenter | Qt::AlignTop);

	auto firstKeySpin = new LcdSpinBox(3, nullptr, tr("First key"));
	firstKeySpin->setLabel(tr("FIRST"));
	firstKeySpin->setToolTip(tr("First MIDI key that will be mapped"));
	firstKeySpin->setModel(&m_firstKeyModel);
	keymapRangeLayout->addWidget(firstKeySpin, 0, 0);

	auto lastKeySpin = new LcdSpinBox(3, nullptr, tr("Last key"));
	lastKeySpin->setLabel(tr("LAST"));
	lastKeySpin->setToolTip(tr("Last MIDI key that will be mapped"));
	lastKeySpin->setModel(&m_lastKeyModel);
	keymapRangeLayout->addWidget(lastKeySpin, 0, 1);

	auto middleKeySpin = new LcdSpinBox(3, nullptr, tr("Middle key"));
	middleKeySpin->setLabel(tr("MIDDLE"));
	middleKeySpin->setToolTip(tr("First line in the keymap refers to this MIDI key"));
	middleKeySpin->setModel(&m_middleKeyModel);
	keymapRangeLayout->addWidget(middleKeySpin, 0, 2);

	auto baseKeySpin = new LcdSpinBox(3, nullptr, tr("Base key"));
	baseKeySpin->setLabel(tr("BASE N."));
	baseKeySpin->setToolTip(tr("Base note frequency will be assigned to this MIDI key"));
	baseKeySpin->setModel(&m_baseKeyModel);
	keymapRangeLayout->addWidget(baseKeySpin, 1, 0);

	keymapRangeLayout->addWidget(new QLabel(tr("Base note frequency (Hz):")), 1, 1);
	m_baseFreqEdit = new QLineEdit();
	m_baseFreqEdit->setToolTip(tr("Finite positive reference frequency. Decimal and scientific notation are accepted."));
	keymapRangeLayout->addWidget(m_baseFreqEdit, 1, 2);
	keymapRangeLayout->addWidget(new QLabel(tr("Formal octave degree:")), 2, 0, 1, 2);
	m_octaveDegreeEdit = new QLineEdit();
	m_octaveDegreeEdit->setToolTip(tr("Signed scale degree advanced for each keyboard-map period. Zero uses the final scale degree."));
	keymapRangeLayout->addWidget(m_octaveDegreeEdit, 2, 2);

	auto applyKeymapButton = new QPushButton(tr("Apply keymap changes"));
	applyKeymapButton->setToolTip(tr("Verify and apply changes made to the selected key mapping. To use the mapping, select it in the settings of a supported instrument."));
	microtunerLayout->addWidget(applyKeymapButton, 6, 2, 1, 2);
	connect(applyKeymapButton, &QPushButton::clicked, this, &MicrotunerConfig::applyKeymap);

	updateScaleForm();
	updateKeymapForm();
	connect(Engine::getSong(), SIGNAL(scaleListChanged(int)), this, SLOT(updateScaleList(int)));
	connect(Engine::getSong(), SIGNAL(scaleListChanged(int)), this, SLOT(updateScaleForm()));
	connect(Engine::getSong(), SIGNAL(keymapListChanged(int)), this, SLOT(updateKeymapList(int)));
	connect(Engine::getSong(), SIGNAL(keymapListChanged(int)), this, SLOT(updateKeymapForm()));

	microtunerLayout->setRowStretch(4, 10);
	this->setLayout(microtunerLayout);

	// Add to the main window and setup fixed size etc.
	SubWindow* subWin = getGUI()->mainWindow()->addWindowedWidget(this);
	subWin->setAttribute(Qt::WA_DeleteOnClose, false);
	setMinimumSize(300, 300);
	setMaximumSize(500, 700);
	subWin->hide();

	// No maximize button
	Qt::WindowFlags flags = subWin->windowFlags();
	flags &= ~Qt::WindowMaximizeButtonHint;
	subWin->setWindowFlags(flags);
}


void MicrotunerConfig::updateScaleList(int index)
{
	if (index >= 0 && static_cast<std::size_t>(index) < MaxScaleCount)
	{
		m_scaleComboModel.replaceItem(index,
			QString::number(index) + ": " + Engine::getSong()->getScale(index)->getDescription());
	}
	else
	{
		for (auto i = std::size_t{0}; i < MaxScaleCount; i++)
		{
			m_scaleComboModel.replaceItem(i,
				QString::number(i) + ": " + Engine::getSong()->getScale(i)->getDescription());
		}
	}
}


void MicrotunerConfig::updateKeymapList(int index)
{
	if (index >= 0 && static_cast<std::size_t>(index) < MaxKeymapCount)
	{
		m_keymapComboModel.replaceItem(index,
			QString::number(index) + ": " + Engine::getSong()->getKeymap(index)->getDescription());
	}
	else
	{
		for (auto i = std::size_t{0}; i < MaxKeymapCount; i++)
		{
			m_keymapComboModel.replaceItem(i,
				QString::number(i) + ": " + Engine::getSong()->getKeymap(i)->getDescription());
		}
	}
}


void MicrotunerConfig::updateScaleForm()
{
	Song *song = Engine::getSong();
	if (song == nullptr) {return;}

	auto newScale = song->getScale(m_scaleComboModel.value());

	m_scaleNameEdit->setText(newScale->getDescription());

	// fill in the intervals
	m_scaleTextEdit->setPlainText("");
	const std::vector<Interval> &intervals = newScale->getIntervals();
	for (std::size_t i = 1; i < intervals.size(); i++)
	{
		m_scaleTextEdit->appendPlainText(intervals[i].getString());
	}
	// scroll back to the top
	QTextCursor tmp = m_scaleTextEdit->textCursor();
	tmp.movePosition(QTextCursor::Start);
	m_scaleTextEdit->setTextCursor(tmp);
}


void MicrotunerConfig::updateKeymapForm()
{
	Song *song = Engine::getSong();
	if (song == nullptr) {return;}

	auto newMap = song->getKeymap(m_keymapComboModel.value());

	m_keymapNameEdit->setText(newMap->getDescription());

	m_keymapTextEdit->setPlainText("");
	const std::vector<int> &map = newMap->getMap();
	for (std::size_t i = 0; i < map.size(); ++i)
	{
		if (newMap->isDegreeMapped(i)) {m_keymapTextEdit->appendPlainText(QString::number(map[i]));}
		else {m_keymapTextEdit->appendPlainText("x");}
	}
	QTextCursor tmp = m_keymapTextEdit->textCursor();
	tmp.movePosition(QTextCursor::Start);
	m_keymapTextEdit->setTextCursor(tmp);

	m_firstKeyModel.setValue(newMap->getFirstKey());
	m_lastKeyModel.setValue(newMap->getLastKey());
	m_middleKeyModel.setValue(newMap->getMiddleKey());
	m_baseKeyModel.setValue(newMap->getBaseKey());
	m_baseFreqEdit->setText(QString::number(newMap->getBaseFreq(), 'g', std::numeric_limits<float>::max_digits10));
	m_octaveDegreeEdit->setText(QString::number(newMap->octaveDegree()));
}


bool MicrotunerConfig::applyScale()
{
	const auto song = Engine::getSong();
	if (!song) { return false; }
	QString error; std::shared_ptr<Scale> scale;
	if (!TuningFile::parseScale(m_scaleNameEdit->text(), m_scaleTextEdit->toPlainText(), scale, error))
	{
		QMessageBox::critical(this, tr("Scale parsing error"), error); return false;
	}
	song->setScale(m_scaleComboModel.value(), std::move(scale));
	return true;
}

bool MicrotunerConfig::applyKeymap()
{
	const auto song = Engine::getSong();
	if (!song) { return false; }
	const auto previous = song->getKeymap(m_keymapComboModel.value());
	QString error; std::shared_ptr<Keymap> keymap;
	bool frequencyOk = false, octaveOk = false;
	auto frequency = m_baseFreqEdit->text().toDouble(&frequencyOk);
	if (!frequencyOk) { frequency = QLocale().toDouble(m_baseFreqEdit->text(), &frequencyOk); }
	const auto octave = m_octaveDegreeEdit->text().toInt(&octaveOk);
	if (!frequencyOk || !octaveOk)
	{
		QMessageBox::critical(this, tr("Keymap parsing error"), tr("Enter a numeric reference frequency and a signed integer formal octave degree."));
		return false;
	}
	if (!TuningFile::parseKeymap(m_keymapNameEdit->text(), m_keymapTextEdit->toPlainText(),
		m_firstKeyModel.value(), m_lastKeyModel.value(), m_middleKeyModel.value(), m_baseKeyModel.value(),
		frequency, octave, keymap, error, previous->linearFromMiddle()))
	{
		QMessageBox::critical(this, tr("Keymap parsing error"), error); return false;
	}
	const bool referenceMapped = keymap->isMapped(keymap->getBaseKey());
	song->setKeymap(m_keymapComboModel.value(), std::move(keymap));
	if (!referenceMapped)
	{
		QMessageBox::warning(this, tr("Invalid keymap"), tr("Base key is unmapped; no reference frequency can be assigned."));
	}
	return true;
}

bool MicrotunerConfig::loadScaleFromFile()
{
	const auto song = Engine::getSong();
	if (!song) { return false; }
	const auto path = FileDialog::getOpenFileName(this, tr("Open scale"), "", tr("Scala scale definition (*.scl)"));
	if (path.isEmpty()) { return false; }
	QString error; std::shared_ptr<Scale> scale;
	if (!TuningFile::loadScale(path, scale, error))
	{
		QMessageBox::critical(this, tr("Scale load failure"), error); return false;
	}
	song->setScale(m_scaleComboModel.value(), std::move(scale));
	updateScaleForm(); return true;
}

bool MicrotunerConfig::loadKeymapFromFile()
{
	const auto song = Engine::getSong();
	if (!song) { return false; }
	const auto path = FileDialog::getOpenFileName(this, tr("Open keymap"), "", tr("Scala keymap definition (*.kbm)"));
	if (path.isEmpty()) { return false; }
	QString error; std::shared_ptr<Keymap> keymap;
	if (!TuningFile::loadKeymap(path, keymap, error))
	{
		QMessageBox::critical(this, tr("Keymap load failure"), error); return false;
	}
	song->setKeymap(m_keymapComboModel.value(), std::move(keymap));
	updateKeymapForm(); return true;
}

bool MicrotunerConfig::saveScaleToFile()
{
	if (!applyScale()) { return false; }
	auto path = FileDialog::getSaveFileName(this, tr("Save scale"), "", tr("Scala scale definition (*.scl)"));
	if (path.isEmpty()) { return false; }
	if (QFileInfo(path).suffix() != "scl") { path += ".scl"; }
	QString error;
	if (!TuningFile::saveScale(path, *Engine::getSong()->getScale(m_scaleComboModel.value()), error))
	{
		QMessageBox::critical(this, tr("Scale save failure"), error); return false;
	}
	return true;
}

bool MicrotunerConfig::saveKeymapToFile()
{
	if (!applyKeymap()) { return false; }
	auto path = FileDialog::getSaveFileName(this, tr("Save keymap"), "", tr("Scala keymap definition (*.kbm)"));
	if (path.isEmpty()) { return false; }
	if (QFileInfo(path).suffix() != "kbm") { path += ".kbm"; }
	QString error;
	if (!TuningFile::saveKeymap(path, *Engine::getSong()->getKeymap(m_keymapComboModel.value()), error))
	{
		QMessageBox::critical(this, tr("Keymap save failure"), error); return false;
	}
	return true;
}

void MicrotunerConfig::saveSettings(QDomDocument &document, QDomElement &element)
{
	MainWindow::saveWidgetState(this, element);
}


void MicrotunerConfig::loadSettings(const QDomElement &element)
{
	MainWindow::restoreWidgetState(this, element);
}


} // namespace lmms::gui

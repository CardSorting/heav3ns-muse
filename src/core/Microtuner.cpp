/*
 * Microtuner.cpp - manage tuning and scale information of an instrument
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

#include "Microtuner.h"

#include <vector>
#include <cmath>
#include <QPointer>
#include <cstdint>

#include "Engine.h"
#include "Keymap.h"
#include "Note.h"
#include "Scale.h"
#include "Song.h"

namespace lmms
{


Microtuner::Microtuner(Model* parent) :
	Model(parent, tr("Microtuner")),
	m_enabledModel(false, this, tr("Microtuner on / off")),
	m_scaleModel(this, tr("Selected scale")),
	m_keymapModel(this, tr("Selected keyboard mapping")),
	m_keyRangeImportModel(true, this, tr("Import key range"))
{
	for (unsigned int i = 0; i < MaxScaleCount; i++)
	{
		m_scaleModel.addItem(QString::number(i) + ": " + Engine::getSong()->getScale(i)->getDescription());
	}

	for (unsigned int i = 0; i < MaxKeymapCount; i++)
	{
		m_keymapModel.addItem(QString::number(i) + ": " + Engine::getSong()->getKeymap(i)->getDescription());
	}
	connect(Engine::getSong(), SIGNAL(scaleListChanged(int)), this, SLOT(updateScaleList(int)));
	connect(Engine::getSong(), SIGNAL(keymapListChanged(int)), this, SLOT(updateKeymapList(int)));
	connect(&m_enabledModel, &AutomatableModel::dataChanged, this, &Microtuner::tuningChanged);
	connect(&m_scaleModel, &AutomatableModel::dataChanged, this, &Microtuner::tuningChanged);
	connect(&m_keymapModel, &AutomatableModel::dataChanged, this, &Microtuner::tuningChanged);
	connect(&m_keyRangeImportModel, &AutomatableModel::dataChanged, this, &Microtuner::tuningChanged);
}


float Microtuner::keyToFreq(int key, int userBaseNote) const
{
	if (key < 0 || key >= NumKeys) { return 0; }
	const auto song = Engine::getSong();
	if (!song) { return 0; }
	const auto keymap = song->getKeymap(m_keymapModel.value());
	const auto scale = song->getScale(m_scaleModel.value());
	const auto& intervals = scale->getIntervals();
	const auto baseNote = m_keyRangeImportModel.value() ? keymap->getBaseKey() : userBaseNote;
	if (intervals.empty() || !keymap->isMapped(key) || !keymap->isMapped(baseNote)) { return 0; }
	const int scalePeriod = static_cast<int>(intervals.size()) - 1;
	if (scalePeriod == 0)
	{
		const auto frequency = keymap->getBaseFreq();
		return std::isfinite(frequency) && frequency > 0 ? frequency : 0;
	}
	// The keyboard pattern's formal octave is a scale degree, not necessarily
	// the scale's last degree. Zero keeps legacy last-degree behavior.
	const int mappingPeriod = keymap->octaveDegree() == 0 ? scalePeriod : keymap->octaveDegree();
	const auto totalDegree = [&](int midiKey) {
		return static_cast<int64_t>(keymap->getDegree(midiKey))
			+ static_cast<int64_t>(keymap->getOctave(midiKey)) * mappingPeriod;
	};
	const auto floorDivide = [scalePeriod](int64_t degree) {
		return degree / scalePeriod - (degree % scalePeriod < 0 ? 1 : 0);
	};
	const auto degree = totalDegree(key);
	const auto referenceDegree = totalDegree(baseNote);
	const auto octave = floorDivide(degree);
	const auto referenceOctave = floorDivide(referenceDegree);
	const auto interval = degree - octave * scalePeriod;
	const auto referenceInterval = referenceDegree - referenceOctave * scalePeriod;
	const double frequency = keymap->getBaseFreq() * static_cast<double>(intervals[interval].getRatio())
		/ intervals[referenceInterval].getRatio()
		* std::pow(static_cast<double>(intervals.back().getRatio()), octave - referenceOctave);
	const auto result = static_cast<float>(frequency);
	return std::isfinite(result) && result > 0 ? result : 0;
}

int Microtuner::octaveSize() const
{
	const int keymapSize = Engine::getSong()->getKeymap(currentKeymap())->getSize();
	if (keymapSize > 0)
	{
		return keymapSize;
	}

	// Determine octave size from the scale if the keymap isn't in use.
	const auto degrees = Engine::getSong()->getScale(currentScale())->getIntervals().size() - 1;
	// A zero-degree Scala scale is constant pitch; editing still needs a nonzero key stride.
	return degrees > 0 ? static_cast<int>(degrees) : 1;
}


void Microtuner::updateScaleList(int index)
{
	if (index >= 0 && static_cast<std::size_t>(index) < MaxScaleCount)
	{
		m_scaleModel.replaceItem(index,
			QString::number(index) + ": " + Engine::getSong()->getScale(index)->getDescription());
	}
	else
	{
		for (auto i = std::size_t{0}; i < MaxScaleCount; i++)
		{
			m_scaleModel.replaceItem(i,
				QString::number(i) + ": " + Engine::getSong()->getScale(i)->getDescription());
		}
	}
	if (index < 0 || index == currentScale()) { emit tuningChanged(); }
}


void Microtuner::updateKeymapList(int index)
{
	if (index >= 0 && static_cast<std::size_t>(index) < MaxKeymapCount)
	{
		m_keymapModel.replaceItem(index,
			QString::number(index) + ": " + Engine::getSong()->getKeymap(index)->getDescription());
	}
	else
	{
		for (auto i = std::size_t{0}; i < MaxKeymapCount; i++)
		{
			m_keymapModel.replaceItem(i,
				QString::number(i) + ": " + Engine::getSong()->getKeymap(i)->getDescription());
		}
	}
	if (index < 0 || index == currentKeymap()) { emit tuningChanged(); }
}


void Microtuner::saveSettings(QDomDocument &document, QDomElement &element)
{
	m_enabledModel.saveSettings(document, element, "enabled");
	m_scaleModel.saveSettings(document, element, "scale");
	m_keymapModel.saveSettings(document, element, "keymap");
	m_keyRangeImportModel.saveSettings(document, element, "range_import");
}


void Microtuner::loadSettings(const QDomElement &element)
{
	const QPointer<Microtuner> owner(this);
	m_enabledModel.loadSettings(element, "enabled");
	if (!owner) { return; }
	m_scaleModel.loadSettings(element, "scale");
	if (!owner) { return; }
	m_keymapModel.loadSettings(element, "keymap");
	if (!owner) { return; }
	m_keyRangeImportModel.loadSettings(element, "range_import");
}


} // namespace lmms

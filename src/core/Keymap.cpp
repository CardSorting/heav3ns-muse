/*
 * Keymap.cpp - implementation of keymap class
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

#include "Keymap.h"

#include <QDomElement>
#include <limits>

#include "Note.h"


namespace lmms
{


Keymap::Keymap() :
	m_description(tr("empty")),
	m_firstKey(0),
	m_lastKey(NumKeys - 1),
	m_middleKey(DefaultMiddleKey),
	m_baseKey(DefaultBaseKey),
	m_baseFreq(DefaultBaseFreq)
{
}


Keymap::Keymap(
	QString description,
	std::vector<int> newMap,
	int newFirst,
	int newLast,
	int newMiddle,
	int newBaseKey,
	float newBaseFreq,
	int octaveDegree,
	std::vector<bool> mapped,
	bool linearFromMiddle
) :
	m_description(description),
	m_map(std::move(newMap)),
	m_firstKey(newFirst),
	m_lastKey(newLast),
	m_middleKey(newMiddle),
	m_baseKey(newBaseKey),
	m_baseFreq(newBaseFreq),
	m_octaveDegree(octaveDegree),
	m_mapped(std::move(mapped)),
	m_linearFromMiddle(linearFromMiddle)
{
}


bool Keymap::isDegreeMapped(std::size_t index) const
{
	if (index >= m_map.size()) { return false; }
	return m_mapped.size() == m_map.size() ? m_mapped[index] : m_map[index] != -1;
}

bool Keymap::isMapped(int key) const
{
	if (key < m_firstKey || key > m_lastKey) { return false; }
	if (m_map.empty()) { return true; }
	const int size = static_cast<int>(m_map.size());
	const int remainder = (key - m_middleKey) % size;
	return isDegreeMapped(remainder < 0 ? remainder + size : remainder);
}

int Keymap::getDegree(int key) const
{
	if (!isMapped(key)) {return -1;}
	if (m_map.empty()) {return m_linearFromMiddle ? key - m_middleKey : key;}

	const int keyOffset = key - m_middleKey;								// -127..127
	const int key_rem = keyOffset % static_cast<int>(m_map.size());			// remainder
	const int key_mod = key_rem >= 0 ? key_rem : key_rem + static_cast<int>(m_map.size());	// true modulo
	return m_map[key_mod];
}


int Keymap::getOctave(int key) const
{
	// The keymap wraparound cannot cause an octave transition if a key isn't mapped or the map is empty → return 0
	if (m_map.empty() || !isMapped(key)) {return 0;}

	const int keyOffset = key - m_middleKey;
	if (keyOffset >= 0)
	{
		return keyOffset / static_cast<int>(m_map.size());
	}
	else
	{
		return (keyOffset + 1) / static_cast<int>(m_map.size()) - 1;
	}
}


QString Keymap::getDescription() const
{
	return m_description;
}


void Keymap::setDescription(QString description)
{
	m_description = description;
}


void Keymap::saveSettings(QDomDocument &document, QDomElement &element)
{
	element.setAttribute("description", m_description);

	element.setAttribute("first_key", m_firstKey);
	element.setAttribute("last_key", m_lastKey);
	element.setAttribute("middle_key", m_middleKey);
	element.setAttribute("base_key", m_baseKey);
	element.setAttribute("base_freq", QString::number(m_baseFreq, 'g', std::numeric_limits<float>::max_digits10));
	if (m_octaveDegree != 0) { element.setAttribute("octave_degree", m_octaveDegree); }
	if (m_linearFromMiddle) { element.setAttribute("linear_from_middle", 1); }

	for (std::size_t index = 0; index < m_map.size(); ++index)
	{
		QDomElement degree = document.createElement("degree");
		element.appendChild(degree);
		degree.setAttribute("value", m_map[index]);
		if (isDegreeMapped(index) != (m_map[index] != -1)) { degree.setAttribute("mapped", isDegreeMapped(index) ? 1 : 0); }
	}
}


void Keymap::loadSettings(const QDomElement &element)
{
	m_description = element.attribute("description");

	m_firstKey	= element.attribute("first_key", "0").toInt();
	m_lastKey	= element.attribute("last_key", QString::number(NumKeys - 1)).toInt();
	m_middleKey	= element.attribute("middle_key", QString::number(DefaultMiddleKey)).toInt();
	m_baseKey	= element.attribute("base_key", QString::number(DefaultBaseKey)).toInt();
	m_baseFreq	= element.attribute("base_freq", QString::number(DefaultBaseFreq)).toDouble();
	m_octaveDegree = element.attribute("octave_degree", "0").toInt();
	m_linearFromMiddle = element.attribute("linear_from_middle", "0").toInt() != 0;

	QDomElement node = element.firstChildElement("degree");
	m_map.clear();
	m_mapped.clear();

	while (!node.isNull())
	{
		const int degree = node.attribute("value").toInt();
		m_map.push_back(degree);
		m_mapped.push_back(node.hasAttribute("mapped") ? node.attribute("mapped").toInt() != 0 : degree != -1);
		node = node.nextSiblingElement("degree");
	}
}


} // namespace lmms

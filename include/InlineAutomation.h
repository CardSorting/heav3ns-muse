/*
 * InlineAutomation.h - class for automating something "inline"
 *
 * Copyright (c) 2008-2010 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_INLINE_AUTOMATION_H
#define LMMS_INLINE_AUTOMATION_H

#include "AutomationNode.h"
#include "AutomationClip.h"

namespace lmms
{

class InlineAutomation : public FloatModel
{
public:
	InlineAutomation() :
		FloatModel()
	{
	}

	InlineAutomation(const InlineAutomation& _copy) :
		FloatModel(_copy.storedValue(), _copy.minValue(), _copy.maxValue(), _copy.step<float>()),
		m_autoClip(_copy.m_autoClip ? _copy.m_autoClip->clone() : nullptr)
	{
		if (m_autoClip)
		{
			m_autoClip->bindInlineObject(this);
		}
	}

	~InlineAutomation() override
	{
	}

	virtual float defaultValue() const = 0;

	bool hasAutomation() const
	{
		if( m_autoClip != nullptr && m_autoClip->getTimeMap().isEmpty() == false )
		{
			// Prevent saving inline automation if there's just one node at the beginning of
			// the clip, which has a InValue equal to the value of model (which is going
			// to be saved anyways) and no offset between the InValue and OutValue
			AutomationClip::timeMap::const_iterator firstNode =
				m_autoClip->getTimeMap().begin();

			if (isAtInitValue()
				&& m_autoClip->getTimeMap().size() == 1
				&& POS(firstNode) == 0
				&& INVAL(firstNode) == value()
				&& OFFSET(firstNode) == 0)
			{
				return false;
			}

			return true;
		}

		return false;
	}

	AutomationClip * automationClip()
	{
		if( m_autoClip == nullptr )
		{
			m_autoClip = std::make_unique<AutomationClip>(nullptr);
			m_autoClip->addObject( this );
		}
		return m_autoClip.get();
	}
	//! Inspect without creating the lazy native curve.
	const AutomationClip* existingAutomationClip() const { return m_autoClip.get(); }

	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	void loadSettings( const QDomElement & _this ) override;
	// Owner-thread project/copy metadata; ordinary Note copies keep their native
	// constructor path and do not allocate XML or discover project models.
	void saveControlSettings(QDomDocument& document, QDomElement& parent);
	void loadControlSettings(const QDomElement& parent);


private:
	void saveControlSettingsImpl(QDomDocument& document, QDomElement& parent, bool curveControls);
	std::unique_ptr<AutomationClip> m_autoClip;

} ;


} // namespace lmms

#endif // LMMS_INLINE_AUTOMATION_H

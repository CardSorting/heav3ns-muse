/*
 * InlineAutomation.cpp - class for automating something "inline"
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

#include <QDomElement>
#include <QPointer>
#include <stdexcept>

#include "InlineAutomation.h"
#include "ControllerConnection.h"
#include "NativeLinkedControls.h"


namespace lmms
{

void InlineAutomation::saveSettings( QDomDocument & _doc,
							QDomElement & _parent )
{
	saveControlSettingsImpl(_doc, _parent, false);
	if( m_autoClip )
	{
		QDomElement ap = _doc.createElement(
					AutomationClip::classNodeName() );
		QDomElement v = _doc.createElement( nodeName() );
		automationClip()->saveSettings( _doc, v );
		ap.appendChild( v );
		_parent.appendChild( ap );
	}
	NativeLinkedControls::recordInlineOwner(this, _doc, _parent);
}




void InlineAutomation::loadSettings( const QDomElement & _this )
{
	const QPointer<InlineAutomation> receiver(this);
	loadControlSettings(_this);
	if (!receiver) { return; }
	QDomNode node = _this.namedItem( AutomationClip::classNodeName() );
	if( node.isElement() )
	{
		node = node.namedItem( nodeName() );
		if( node.isElement() )
		{
			automationClip()->loadSettings(
							node.toElement() );
			if (!receiver) { return; }
			m_autoClip->bindInlineObject(this);
		}
	}
}

void InlineAutomation::saveControlSettings(QDomDocument& document, QDomElement& parent)
{ saveControlSettingsImpl(document, parent, true); NativeLinkedControls::recordInlineOwner(this, document, parent); }

void InlineAutomation::saveControlSettingsImpl(QDomDocument& document, QDomElement& parent, bool curveControls)
{
	auto state = document.createElement("inlinecontrol"); state.setAttribute("metadata", 1); state.setAttribute("version", 1);
	state.setAttribute("hasCurve", m_autoClip ? 1 : 0); FloatModel::saveSettings(document, state, "value");
	if (curveControls && m_autoClip)
	{ auto curve = document.createElement("curve"); m_autoClip->saveControlSettings(document, curve, "mute"); state.appendChild(curve); }
	parent.appendChild(state);
}

void InlineAutomation::loadControlSettings(const QDomElement& parent)
{
	const auto state = parent.firstChildElement("inlinecontrol"); if (state.isNull()) { return; }
	const auto legacyCurve = parent.firstChildElement(AutomationClip::classNodeName()).firstChildElement(nodeName());
	if (state.attribute("version") != "1" || (state.attribute("hasCurve") != "0" && state.attribute("hasCurve") != "1")
		|| !state.nextSiblingElement("inlinecontrol").isNull() || !state.firstChildElement("curve").nextSiblingElement("curve").isNull()
		|| (state.attribute("hasCurve") == "0" && (!state.firstChildElement("curve").isNull() || !legacyCurve.isNull()))
		|| (state.attribute("hasCurve") == "1" && state.firstChildElement("curve").isNull() && legacyCurve.isNull()))
	{ throw std::invalid_argument("Invalid native inline control metadata"); }
	const QPointer<InlineAutomation> receiver(this);
	if (NativeLinkedControls::isRestoring(parent.ownerDocument())) { unlink(); }
	delete controllerConnection(); if (!receiver) { return; }
	FloatModel::loadSettings(state, "value"); if (!receiver) { return; }
	if (state.attribute("hasCurve") == "0") { m_autoClip.reset(); return; }
	const auto curve = state.firstChildElement("curve");
	if (!curve.isNull()) { automationClip()->loadControlSettings(curve, "mute"); }
}


} // namespace lmms

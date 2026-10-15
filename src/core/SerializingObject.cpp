/*
 * SerializingObject.cpp - implementation of SerializingObject
 *
 * Copyright (c) 2008-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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
#include <QObject>
#include <stdexcept>

#include "SerializingObject.h"

namespace lmms
{

SerializingObject::SerializingObject() :
	m_hook( nullptr )
{
}




SerializingObject::~SerializingObject()
{
	if( m_hook )
	{
		m_hook->m_hookedIn = nullptr;
	}
}




QDomElement SerializingObject::saveState( QDomDocument& doc, QDomElement& parent )
{
	const QPointer<QObject> receiver(dynamic_cast<QObject*>(this)); const bool observable = !receiver.isNull();
	const auto ready = [&] { if (observable && !receiver) { throw std::runtime_error("Native serializing object retired during save"); } };
	QDomElement element = doc.createElement( nodeName() );
	ready();
	parent.appendChild( element );

	saveSettings( doc, element );
	ready();

	if( hook() )
	{
		hook()->saveSettings( doc, element );
		ready();
	}

	return element;
}




void SerializingObject::restoreState( const QDomElement& element )
{
	const QPointer<QObject> receiver(dynamic_cast<QObject*>(this)); const bool observable = !receiver.isNull();
	const auto ready = [&] { if (observable && !receiver) { throw std::runtime_error("Native serializing object retired during restore"); } };
	loadSettings( element );
	ready();

	if( hook() )
	{
		hook()->loadSettings( element );
		ready();
	}
}




void SerializingObject::setHook( SerializingObjectHook* hook )
{
	if( m_hook )
	{
		m_hook->m_hookedIn = nullptr;
	}

	m_hook = hook;

	if( m_hook )
	{
		m_hook->m_hookedIn = this;
	}
}




void SerializingObject::saveSettings( QDomDocument& doc, QDomElement& element )
{
	Q_UNUSED(doc)
	Q_UNUSED(element)
}




void SerializingObject::loadSettings( const QDomElement& element )
{
	Q_UNUSED(element)
}

} // namespace lmms

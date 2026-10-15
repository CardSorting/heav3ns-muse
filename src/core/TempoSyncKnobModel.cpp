/*
 * TempoSyncKnobModel.cpp - adds bpm to ms conversion for knob class
 *
 * Copyright (c) 2005-2007 Danny McRae <khjklujn/at/yahoo.com>
 * Copyright (c) 2005-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <QAction>
#include <QDomElement>
#include <QPointer>
#include <QScopeGuard>
#include <QDebug>
#include <exception>
#include <cmath>
#include <stdexcept>

#include "TempoSyncKnobModel.h"
#include "Engine.h"
#include "Song.h"
#include "NativeValueNotification.h"


namespace lmms
{


TempoSyncKnobModel::TempoSyncKnobModel( const float _val, const float _min,
				const float _max, const float _step,
				const float _scale, Model * _parent,
				const QString & _display_name ) :
	FloatModel( _val, _min, _max, _step, _parent, _display_name ),
	m_tempoSyncMode( SyncMode::None ),
	m_tempoLastSyncMode( SyncMode::None ),
	m_scale( _scale ),
	m_custom( _parent )
{
	connect(Engine::getSong(), &Song::tempoChanged, this, [this](bpm_t tempo) {
		try { calculateTempoSyncTime(tempo); }
		catch (...)
		{
			if (!NativeValueNotification::report(std::current_exception()))
			{ qWarning("Tempo synchronization failed outside a native value notification frame"); }
		}
	}, Qt::DirectConnection);
}




void TempoSyncKnobModel::setTempoSync( QAction * _item )
{
	setTempoSync( static_cast<SyncMode>(_item->data().toInt()) );
}




void TempoSyncKnobModel::setTempoSync( SyncMode _note_type )
{
	setSyncMode( _note_type );
	Engine::getSong()->setModified();
}




void TempoSyncKnobModel::calculateTempoSyncTime( bpm_t _bpm )
{
	const QPointer<TempoSyncKnobModel> receiver(this);
	float conversionFactor = 1.0;
	
	if( m_tempoSyncMode != SyncMode::None )
	{
		if (_bpm <= 0 || !std::isfinite(m_scale) || m_scale <= 0.f)
		{ throw std::invalid_argument("Native tempo synchronization requires positive tempo and scale"); }
		switch( m_tempoSyncMode )
		{
			case SyncMode::Custom:
			{
				const auto denominator = m_custom.getDenominator(); if (!receiver) { return; }
				const auto numerator = receiver->m_custom.getNumerator(); if (!receiver) { return; }
				if (denominator <= 0 || numerator <= 0)
				{ throw std::invalid_argument("Native custom tempo synchronization requires a positive meter"); }
				conversionFactor = static_cast<float>(denominator) / static_cast<float>(numerator);
				break;
			}
			case SyncMode::DoubleWholeNote:
				conversionFactor = 0.125;
				break;
			case SyncMode::WholeNote:
				conversionFactor = 0.25;
				break;
			case SyncMode::HalfNote:
				conversionFactor = 0.5;
				break;
			case SyncMode::QuarterNote:
				conversionFactor = 1.0;
				break;
			case SyncMode::EighthNote:
				conversionFactor = 2.0;
				break;
			case SyncMode::SixteenthNote:
				conversionFactor = 4.0;
				break;
			case SyncMode::ThirtysecondNote:
				conversionFactor = 8.0;
				break;
			default: ;
		}
		const auto journalRestore = qScopeGuard([receiver, journalling = testAndSetJournalling(false)] { if (receiver) { receiver->setJournalling(journalling); } });
		float oneUnit = 60000.0 / ( _bpm * conversionFactor * m_scale );
		if (!std::isfinite(oneUnit) || oneUnit <= 0.f || !std::isfinite(oneUnit * maxValue()))
		{ throw std::invalid_argument("Native tempo synchronization exceeds its finite parameter range"); }
		setValue( oneUnit * maxValue() );
		if (!receiver) { return; }
	}

	if( m_tempoSyncMode != m_tempoLastSyncMode )
	{
		NativeValueNotification notification;
		emit syncModeChanged( m_tempoSyncMode );
		notification.rethrow();
		if (!receiver) { return; }
		m_tempoLastSyncMode = m_tempoSyncMode;
	}
	else if (m_tempoSyncMode == SyncMode::Custom)
	{
		NativeValueNotification notification;
		emit syncModeChanged(m_tempoSyncMode);
		notification.rethrow();
	}
}




void TempoSyncKnobModel::saveSettings( QDomDocument & _doc, QDomElement & _this,
							const QString & _name )
{
	_this.setAttribute( _name + "_syncmode", (int) syncMode() );
	m_custom.saveSettings( _doc, _this, _name );
	FloatModel::saveSettings( _doc, _this, _name );
}




void TempoSyncKnobModel::loadSettings( const QDomElement & _this,
							const QString & _name )
{
	const QPointer<TempoSyncKnobModel> receiver(this);
	FloatModel::loadSettings( _this, _name );
	if (!receiver) { return; }
	m_custom.loadSettings( _this, _name );
	if (!receiver) { return; }
	setSyncMode( ( SyncMode ) _this.attribute( _name + "_syncmode" ).toInt() );
}




void TempoSyncKnobModel::setSyncMode( SyncMode _new_mode )
{
	if( m_tempoSyncMode != _new_mode )
	{
		m_tempoSyncMode = _new_mode;
		if( _new_mode == SyncMode::Custom )
		{
			connect(&m_custom, &Model::dataChanged, this, &TempoSyncKnobModel::receiveCustomChange,
				static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));
		}
	}
	calculateTempoSyncTime( Engine::getSong()->getTempo() );
}




void TempoSyncKnobModel::setScale( float _new_scale )
{
	const QPointer<TempoSyncKnobModel> receiver(this);
	m_scale = _new_scale;
	calculateTempoSyncTime( Engine::getSong()->getTempo() );
	if (!receiver) { return; }
	NativeValueNotification notification;
	emit scaleChanged( _new_scale );
	notification.rethrow();
}




void TempoSyncKnobModel::receiveCustomChange()
{
	try { updateCustom(); }
	catch (...)
	{
		if (!NativeValueNotification::report(std::current_exception()))
		{ qWarning("Custom tempo synchronization failed outside a native value notification frame"); }
	}
}

void TempoSyncKnobModel::updateCustom()
{
	setSyncMode( SyncMode::Custom );
}


} // namespace lmms

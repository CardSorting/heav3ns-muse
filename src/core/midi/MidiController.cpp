/*
 * MidiController.cpp - implementation of class midi-controller which handles
 *                      MIDI control change messages
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail.com>
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


#include "AudioEngine.h"
#include "Engine.h"
#include "MidiController.h"
#include "Song.h"
#include "FileResourceIO.h"

namespace lmms
{


MidiController::MidiController( Model * _parent ) :
	Controller( ControllerType::Midi, _parent, tr( "MIDI Controller" ) ),
	MidiEventProcessor(),
	m_midiPort( tr( "unnamed_midi_controller" ), Engine::audioEngine()->midiClient(), this, this, MidiPort::Mode::Input ),
	m_lastValue( 0.0f ),
	m_previousValue( 0.0f )
{
	setSampleExact( true );
	// The base constructor fills its own neutral buffer before derived dispatch.
	updateValueBuffer();
	connect( &m_midiPort, SIGNAL(modeChanged()),
			this, SLOT(updateName()));
}




MidiController::~MidiController()
{
	beginRetirement();
	m_midiPort.detachInputProcessor();
}

void MidiController::updateValueBuffer()
{
	if( m_previousValue != m_lastValue )
	{
		m_valueBuffer.interpolate( m_previousValue, m_lastValue );
		m_previousValue = m_lastValue;
	}
	else
	{
		m_valueBuffer.fill( m_lastValue );
	}
	m_bufferLastUpdated = s_periods;
}


void MidiController::updateName()
{
	setName( QString("MIDI ch%1 ctrl%2").
			arg( m_midiPort.inputChannel() ).
			arg( m_midiPort.inputController() ) );
}




void MidiController::processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t)
{
	if (isRetiring()) { return; }
	if (!event.isValidControlChange()) { return; }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	if (event.source() == MidiEvent::Source::External
		&& (Engine::audioEngine()->hasStoredAudioDevice() || (Engine::getSong() && (Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup())))) { return; }
	if (m_midiPort.inputController() != event.controllerNumber()
		|| (m_midiPort.inputChannel() != 0 && m_midiPort.inputChannel() != event.channel() + 1)) { return; }
	m_previousValue = m_lastValue;
	m_lastValue = static_cast<float>(event.controllerValue()) / MidiMaxControllerValue;
	notifyValueChange();
}




void MidiController::subscribeReadablePorts( const MidiPort::Map & _map )
{
	for( MidiPort::Map::ConstIterator it = _map.constBegin();
						it != _map.constEnd(); ++it )
	{
		m_midiPort.subscribeReadablePort( it.key(), *it );
	}
}




void MidiController::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; SettingsGuard changing(this);
	saveBaseSettings(_doc, _this); changing.check();
	m_midiPort.saveSettings( _doc, _this );
	changing.check();

}




void MidiController::loadSettings( const QDomElement & _this )
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; SettingsGuard changing(this);
	loadBaseSettings(_this); changing.check();

	m_midiPort.loadSettings( _this );
	changing.check();

	updateName();
	changing.check();
}




QString MidiController::nodeName() const
{
	return( "Midicontroller" );
}




gui::ControllerDialog* MidiController::createDialog( QWidget * _parent )
{
	return nullptr;
}



} // namespace lmms

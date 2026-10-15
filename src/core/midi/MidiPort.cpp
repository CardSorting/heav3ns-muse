/*
 * MidiPort.cpp - abstraction of MIDI-ports which are part of LMMS's MIDI-
 *                sequencing system
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <stdexcept>

#include "MidiPort.h"
#include "AudioEngine.h"
#include "MidiClient.h"
#include "MidiDummy.h"
#include "MidiEventProcessor.h"
#include "Note.h"
#include "Song.h"
#include "MidiController.h"
#include "MidiEventToByteSeq.h"
#include "FileResourceIO.h"


namespace lmms
{


// Surviving ports may detach during static or engine teardown. Keep this one
// fallback registry alive for the process; ports unregister their own entries.
static MidiDummy* const s_dummyClient = new MidiDummy;

MidiPort::Transition::Transition(MidiPort* port, bool MidiPort::* flag, bool release)
	: m_owner(port), m_client(port->m_midiClient), m_flag(flag)
{
	if ((!release && (port->thread() != QThread::currentThread() || port->m_retiring || port->m_clientTransfer)) || port->*flag)
	{ throw std::logic_error("Native MIDI port transition cannot reenter, retire or change client concurrently"); }
	port->*flag = true;
}
MidiPort::Transition::~Transition() { if (m_owner) { m_owner.data()->*m_flag = false; } }
void MidiPort::Transition::check() const
{
	if (!m_owner || m_owner->m_midiClient != m_client) { throw std::runtime_error("Native MIDI port retired or changed client during transition"); }
}



MidiPort::MidiPort( const QString& name,
					MidiClient* client,
					MidiEventProcessor* eventProcessor,
					Model* parent,
					Mode mode ) :
	Model( parent ),
	m_readablePortsMenu( nullptr ),
	m_writablePortsMenu( nullptr ),
	m_midiClient( client ),
	m_midiEventProcessor( eventProcessor ),
	m_mode( mode ),
	m_inputChannelModel( 0, 0, MidiChannelCount, this, tr( "Input channel" ) ),
	m_outputChannelModel( 1, 0, MidiChannelCount, this, tr( "Output channel" ) ),
	m_inputControllerModel(MidiController::NONE, MidiController::NONE, MidiControllerCount - 1, this, tr( "Input controller" )),
	m_outputControllerModel(MidiController::NONE, MidiController::NONE, MidiControllerCount - 1, this, tr( "Output controller" )),
	m_fixedInputVelocityModel( -1, -1, MidiMaxVelocity, this, tr( "Fixed input velocity" ) ),
	m_fixedOutputVelocityModel( -1, -1, MidiMaxVelocity, this, tr( "Fixed output velocity" ) ),
	m_fixedOutputNoteModel( -1, -1, MidiMaxKey, this, tr( "Fixed output note" ) ),
	m_outputProgramModel( 1, 1, MidiProgramCount, this, tr( "Output MIDI program" ) ),
	m_baseVelocityModel( MidiMaxVelocity/2, 1, MidiMaxVelocity, this, tr( "Base velocity" ) ),
	m_readableModel( false, this, tr( "Receive MIDI-events" ) ),
	m_writableModel( false, this, tr( "Send MIDI-events" ) )
{
	bool constructed = false;
	const auto retirePartial = qScopeGuard([&] {
		if (constructed) { return; }
		m_midiClient->MidiClient::removePort(this);
		try { m_midiClient->removePort(this); } catch (...) { }
	});
	m_midiClient->addPort( this );

	m_readableModel.setValue( m_mode == Mode::Input || m_mode == Mode::Duplex );
	m_writableModel.setValue( m_mode == Mode::Output || m_mode == Mode::Duplex );

	connect( &m_readableModel, SIGNAL(dataChanged()),
			this, SLOT(updateMidiPortMode()), Qt::DirectConnection );
	connect( &m_writableModel, SIGNAL(dataChanged()),
			this, SLOT(updateMidiPortMode()), Qt::DirectConnection );
	connect( &m_outputProgramModel, SIGNAL(dataChanged()),
			this, SLOT(updateOutputProgram()), Qt::DirectConnection );
	const auto releaseBeforeRetargeting = [this](IntModel* model) {
		const QPointer<IntModel> target(model);
		connect(model, &IntModel::dataChanged, this, [this, target, previous = model->value()]() mutable {
			try
			{
				const auto lock = lockOutput();
				if (m_clientTransfer) { throw std::runtime_error("MIDI client transfer is active"); }
				releaseOutput();
			}
			catch (...)
			{
				if (target)
				{
					const bool blocked = target->blockSignals(true);
					const auto restore = qScopeGuard([target, blocked] { if (target) { target->blockSignals(blocked); } });
					target->setValue(previous);
				}
				throw;
			}
			if (target) { previous = target->value(); }
		}, Qt::DirectConnection);
	};
	releaseBeforeRetargeting(&m_outputChannelModel);
	releaseBeforeRetargeting(&m_fixedOutputNoteModel);


	// when using with non-raw-clients we can provide buttons showing
	// our port-menus when being clicked
	if( m_midiClient->isRaw() == false )
	{
		updateReadablePorts();
		updateWritablePorts();

		// we want to get informed about port-changes!
		m_midiClient->connectRPChanged( this, SLOT(updateReadablePorts()));
		m_midiClient->connectWPChanged( this, SLOT(updateWritablePorts()));
	}

	updateMidiPortMode();
	constructed = true;
}




MidiPort::~MidiPort()
{
	m_retiring = true;
	try { releaseOutput(); }
	catch (...)
	{
		qWarning("MIDI output cleanup failed while retiring a port");
		std::uint16_t channels = 0;
		for (int channel = 0; channel < MidiChannelCount; ++channel)
		{
			bool active = m_outputPedals[channel][0] || m_outputPedals[channel][1];
			for (const auto count : m_outputNotes[channel]) { active = active || count != 0; }
			if (active) { channels |= static_cast<std::uint16_t>(1u << channel); }
		}
		m_midiClient->retainFailedOutputCleanup(this, channels);
		for (auto& notes : m_outputNotes) { notes.fill(0); }
		m_trackedOutputNotes = 0;
		for (auto& pedals : m_outputPedals) { pedals.fill(false); }
	}
	detachInputProcessor();
	// Stop callback enqueueing before any model/member begins destruction.
	m_midiClient->MidiClient::removePort(this);
	// Native removal owns final unsubscription. Do not reenter a failed output
	// cancellation through model signals from a noexcept destructor.
	const QSignalBlocker readableBlocker(&m_readableModel), writableBlocker(&m_writableModel);
	m_readableModel.setValue(false);
	m_writableModel.setValue(false);
	m_mode = Mode::Disabled;
	m_midiClient->removePort(this);
}




void MidiPort::setName( const QString& name )
{
	const QString requested(name); Transition changing(this, &MidiPort::m_changingName);
	setDisplayName(requested); changing.check();
	m_midiClient->applyPortName( this );
	changing.check();
}




void MidiPort::setMode( Mode mode )
{
	if (m_updatingMode) { throw std::logic_error("Native MIDI mode notification cannot replace its active transition"); }
	setModeImpl(mode);
}

void MidiPort::setModeImpl(Mode mode)
{
	const auto outputLock = lockOutput();
	Transition changing(this, &MidiPort::m_changingMode);
	if (mode != Mode::Disabled && mode != Mode::Input && mode != Mode::Output && mode != Mode::Duplex) { throw std::invalid_argument("Unknown MIDI port mode"); }
	if (isOutputEnabled() && mode != Mode::Output && mode != Mode::Duplex) { releaseOutput(); changing.check(); }
	const bool wasInputEnabled = isInputEnabled();
	m_mode = mode;
	if (wasInputEnabled != isInputEnabled()) { m_midiClient->setInputDeliveryEnabled(this, isInputEnabled()); }
	m_midiClient->applyPortMode( this );
	changing.check();
}

void MidiPort::detachInputProcessor()
{
	m_midiClient->setInputDeliveryEnabled(this, false);
	if (const auto engine = Engine::audioEngine())
	{
		const auto guard = engine->requestChangesGuard();
		m_midiEventProcessor = nullptr;
	}
	else { m_midiEventProcessor = nullptr; }
}

bool MidiPort::inputDeliveryFailed()
{
	const auto engine = Engine::audioEngine();
	if (!engine || QThread::currentThread() != thread()) { return false; }
	const auto guard = engine->requestChangesGuard();
	// External MIDI is ignored while exporting or a device remains reserved. Retain the cleanup failure
	// for an explicit retry afterwards instead of interrupting project voices
	// or acknowledging an all-sound-off the instrument would not process.
	if (Engine::audioEngine()->hasStoredAudioDevice() || (Engine::getSong() && (Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup()))) { return false; }
	if (!m_midiEventProcessor) { return false; }
	if (!m_midiEventProcessor->requiresNoteCleanup()) { return true; }
	const QPointer<MidiPort> self(this);
	// Losing a release can leave voices held. Native all-sound-off bypasses input mode/channel filtering.
	for (int channel = 0; channel < MidiChannelCount; ++channel)
	{
		if (!self || !self->m_midiEventProcessor) { return false; }
		self->m_midiEventProcessor->processInEvent(MidiEvent(MidiControlChange, channel, MidiControllerAllSoundOff, 0), TimePos{});
	}
	return true;
}

bool MidiPort::resetInputDelivery(QString& error, bool& changed)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (!m_midiEventProcessor) { error = "Native MIDI input processor is retired"; changed = false; return false; }
	return m_midiClient->resetInputDelivery(this, error, changed);
}




void MidiPort::processInEvent( const MidiEvent& event, const TimePos& time )
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (event.source() == MidiEvent::Source::External
		&& (Engine::audioEngine()->hasStoredAudioDevice() || (Engine::getSong() && (Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup())))) { return; }
	if (!m_midiEventProcessor) { return; }
	// mask event
	if( isInputEnabled() &&
		( inputChannel() == 0 || inputChannel()-1 == event.channel() ) )
	{
		MidiEvent inEvent = event;
		if( event.type() == MidiNoteOn ||
			event.type() == MidiNoteOff ||
			event.type() == MidiKeyPressure )
		{
			if( inEvent.key() < 0 || inEvent.key() >= NumKeys )
			{
				return;
			}

			if( fixedInputVelocity() >= 0 && inEvent.velocity() > 0 )
			{
				inEvent.setVelocity( fixedInputVelocity() );
			}
		}

		m_midiEventProcessor->processInEvent( inEvent, time );
	}
}




MidiPort::OutputSource MidiPort::outputSource() const
{
	const auto lock = lockOutput(); return m_outputSource;
}

unsigned int MidiPort::trackedOutputNotes() const
{
	const auto lock = lockOutput(); return m_trackedOutputNotes;
}

bool MidiPort::acceptsOutput(OutputSource source) const
{
	const auto lock = lockOutput(); return !m_retiring && !m_releasingOutput && !m_clientTransfer && isOutputEnabled() && source == m_outputSource;
}

void MidiPort::setOutputSource(OutputSource source)
{
	if (source != OutputSource::Track && source != OutputSource::Instrument) { throw std::invalid_argument("Unknown MIDI output source"); }
	Transition changing(this, &MidiPort::m_changingSource);
	const QPointer<Song> song(Engine::getSong());
	{
		const auto lock = lockOutput();
		if (source == m_outputSource) { return; }
		releaseOutput(); changing.check(); m_outputSource = source;
	}
	if (song && Engine::getSong() == song.data()) { song->setModified(); changing.check(); }
	emit outputSourceChanged();
	changing.check();
}

void MidiPort::releaseOutput()
{
	const auto lock = lockOutput();
	Transition releasing(this, &MidiPort::m_releasingOutput, true);
	// This is necessary even when the local ledger is empty: a future note-on/off
	// pair, controller, or program change may already be in the backend's queue.
	m_midiClient->cancelOutputEvents(this);
	releasing.check();
	for (int channel = 0; channel < MidiChannelCount; ++channel)
	{
		for (int pedal = 0; pedal < 2; ++pedal)
		{
			if (!m_outputPedals[channel][pedal]) { continue; }
			m_midiClient->processOutEvent(MidiEvent(MidiControlChange, channel,
				pedal == 0 ? MidiControllerSustain : MidiControllerSostenuto, 0, nullptr, MidiEvent::Source::Internal), {}, this);
			releasing.check();
			m_outputPedals[channel][pedal] = false;
		}
		for (int key = 0; key <= MidiMaxKey; ++key)
		{
			while (m_outputNotes[channel][key] > 0)
			{
				m_midiClient->processOutEvent(MidiEvent(MidiNoteOff, channel, key, 0, nullptr, MidiEvent::Source::Internal), {}, this);
				releasing.check();
				--m_outputNotes[channel][key]; --m_trackedOutputNotes;
			}
		}
	}
}

void MidiPort::processOutEvent(const MidiEvent& event, const TimePos& time, OutputSource source)
{
	const auto lock = lockOutput();
	const QPointer<MidiPort> owner(this); MidiClient* const client = m_midiClient;
	if (m_retiring || m_releasingOutput || m_clientTransfer || source != m_outputSource || (Engine::getSong() && Engine::getSong()->isExporting())) { return; }
	uint8_t encoded[3]{};
	if (!writeToByteSeq(event, encoded, sizeof(encoded))) { return; }
	// When output is enabled, route midi events if the selected channel matches
	// the event channel or if there's no selected channel (value 0, represented by "--")
	if( isOutputEnabled() && ( outputChannel() == 0 || realOutputChannel() == event.channel() ) )
	{
		MidiEvent outEvent = event;

		if( fixedOutputVelocity() >= 0 && event.velocity() > 0 &&
			( event.type() == MidiNoteOn || event.type() == MidiKeyPressure ) )
		{
			outEvent.setVelocity( fixedOutputVelocity() );
		}

		if( fixedOutputNote() >= 0 &&
			( event.type() == MidiNoteOn || event.type() == MidiNoteOff || event.type() == MidiKeyPressure ) )
		{
			outEvent.setKey( fixedOutputNote() );
		}

		// Retain possible activation if a send throws after changing the device.
		// Releases clear only after the native API returns successfully.
		const int channel = outEvent.channel();
		const bool noteOn = outEvent.type() == MidiNoteOn && outEvent.velocity() > 0;
		const bool noteOff = outEvent.type() == MidiNoteOff || (outEvent.type() == MidiNoteOn && outEvent.velocity() == 0);
		const int pedal = outEvent.type() == MidiControlChange && outEvent.controllerNumber() == MidiControllerSustain ? 0
			: outEvent.type() == MidiControlChange && outEvent.controllerNumber() == MidiControllerSostenuto ? 1 : -1;
		if (noteOn)
		{
			auto& count = m_outputNotes[channel][outEvent.key()];
			if (m_trackedOutputNotes == OutputNoteLimit) { throw std::runtime_error("MIDI output note tracking limit reached"); }
			++count; ++m_trackedOutputNotes;
		}
		if (pedal >= 0 && outEvent.controllerValue() >= 64) { m_outputPedals[channel][pedal] = true; }
		m_midiClient->processOutEvent(outEvent, time, this);
		if (!owner || owner->m_midiClient != client) { throw std::runtime_error("Native MIDI output port retired or changed client during submission"); }
		if (noteOff && m_outputNotes[channel][outEvent.key()] > 0) { --m_outputNotes[channel][outEvent.key()]; --m_trackedOutputNotes; }
		if (pedal >= 0 && outEvent.controllerValue() < 64) { m_outputPedals[channel][pedal] = false; }
		if (outEvent.type() == MidiControlChange)
		{
			if (outEvent.controllerNumber() == MidiControllerAllNotesOff || outEvent.controllerNumber() == MidiControllerAllSoundOff)
			{
				for (const auto count : m_outputNotes[channel]) { m_trackedOutputNotes -= count; }
				m_outputNotes[channel].fill(0);
			}
			if (outEvent.controllerNumber() == MidiControllerResetAllControllers) { m_outputPedals[channel].fill(false); }
		}
	}
}




void MidiPort::saveSettings( QDomDocument& doc, QDomElement& thisElement )
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	Transition settings(this, &MidiPort::m_serializingSettings);
	const auto check = [&] { settings.check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } settings.check(); };
	check();
	thisElement.setAttribute("outputsource", static_cast<int>(outputSource()));
	m_inputChannelModel.saveSettings( doc, thisElement, "inputchannel" );
	check();
	m_outputChannelModel.saveSettings( doc, thisElement, "outputchannel" );
	check();
	m_inputControllerModel.saveSettings( doc, thisElement, "inputcontroller" );
	check();
	m_outputControllerModel.saveSettings( doc, thisElement, "outputcontroller" );
	check();
	m_fixedInputVelocityModel.saveSettings( doc, thisElement, "fixedinputvelocity" );
	check();
	m_fixedOutputVelocityModel.saveSettings( doc, thisElement, "fixedoutputvelocity" );
	check();
	m_fixedOutputNoteModel.saveSettings( doc, thisElement, "fixedoutputnote" );
	check();
	m_outputProgramModel.saveSettings( doc, thisElement, "outputprogram" );
	check();
	m_baseVelocityModel.saveSettings( doc, thisElement, "basevelocity" );
	check();
	m_readableModel.saveSettings( doc, thisElement, "readable" );
	check();
	m_writableModel.saveSettings( doc, thisElement, "writable" );
	check();
	// Child elements retain exact native endpoint names, including commas and
	// temporarily unavailable ports. Legacy attributes remain for older readers.
	auto previous = thisElement.firstChildElement("midisubscriptions");
	if (!previous.isNull()) { thisElement.removeChild(previous); }
	auto subscriptions = doc.createElement("midisubscriptions");
	for (const auto& name : requestedReadablePorts())
	{
		auto endpoint = doc.createElement("input"); endpoint.setAttribute("name", name); subscriptions.appendChild(endpoint);
	}
	for (const auto& name : requestedWritablePorts())
	{
		auto endpoint = doc.createElement("output"); endpoint.setAttribute("name", name); subscriptions.appendChild(endpoint);
	}
	thisElement.appendChild(subscriptions);

	if( isInputEnabled() )
	{
		QString rp;
		for (const auto& name : requestedReadablePorts()) { rp += name + ","; }
		// cut off comma
		if( rp.length() > 0 )
		{
			rp.truncate( rp.length() - 1 );
		}
		thisElement.setAttribute( "inports", rp );
	}

	if( isOutputEnabled() )
	{
		QString wp;
		for (const auto& name : requestedWritablePorts()) { wp += name + ","; }
		// cut off comma
		if( wp.length() > 0 )
		{
			wp.truncate( wp.length() - 1 );
		}
		thisElement.setAttribute( "outports", wp );
	}
}




void MidiPort::loadSettings( const QDomElement& thisElement )
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	Transition settings(this, &MidiPort::m_serializingSettings);
	const auto check = [&] { settings.check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } settings.check(); };
	check();
	bool sourceValid = false; const int source = thisElement.attribute("outputsource", "0").toInt(&sourceValid);
	if (!sourceValid || source < 0 || source > 1) { throw std::invalid_argument("Invalid saved MIDI output source"); }
	// Retire previous subscriptions before replacing settings on an existing port.
	m_readableModel.setValue(false); check(); m_writableModel.setValue(false); check();
	setOutputSource(static_cast<OutputSource>(source)); check();
	m_requestedReadablePorts.clear(); m_requestedWritablePorts.clear();
	m_inputChannelModel.loadSettings( thisElement, "inputchannel" );
	check();
	m_outputChannelModel.loadSettings( thisElement, "outputchannel" );
	check();
	m_inputControllerModel.loadSettings( thisElement, "inputcontroller" );
	check();
	m_outputControllerModel.loadSettings( thisElement, "outputcontroller" );
	check();
	m_fixedInputVelocityModel.loadSettings( thisElement, "fixedinputvelocity" );
	check();
	m_fixedOutputVelocityModel.loadSettings( thisElement, "fixedoutputvelocity" );
	check();
	m_fixedOutputNoteModel.loadSettings( thisElement, "fixedoutputnote" );
	check();
	m_outputProgramModel.loadSettings( thisElement, "outputprogram" );
	check();
	m_baseVelocityModel.loadSettings( thisElement, "basevelocity" );
	check();
	m_readableModel.loadSettings( thisElement, "readable" );
	check();
	m_writableModel.loadSettings( thisElement, "writable" );
	check();

	// Structured names take precedence; legacy comma lists remain readable.
	QSet<QString> readable, writable;
	const auto subscriptions = thisElement.firstChildElement("midisubscriptions");
	if (!subscriptions.isNull())
	{
		for (auto endpoint = subscriptions.firstChildElement(); !endpoint.isNull(); endpoint = endpoint.nextSiblingElement())
		{
			const auto name = endpoint.attribute("name");
			if (name.isEmpty()) { continue; }
			if (endpoint.tagName() == "input") { readable.insert(name); }
			if (endpoint.tagName() == "output") { writable.insert(name); }
		}
	}
	else
	{
		for (const auto& name : thisElement.attribute("inports").split(',', Qt::SkipEmptyParts)) { readable.insert(name); }
		for (const auto& name : thisElement.attribute("outports").split(',', Qt::SkipEmptyParts)) { writable.insert(name); }
	}
	m_requestedReadablePorts = isInputEnabled() ? readable : QSet<QString>{};
	m_requestedWritablePorts = isOutputEnabled() ? writable : QSet<QString>{};
	updateReadablePorts(); check(); updateWritablePorts(); check();

	if( thisElement.hasAttribute( "basevelocity" ) == false )
	{
		// for projects created by LMMS < 0.9.92 there's no value for the base
		// velocity and for compat reasons we have to stick with maximum velocity
		// which did not allow note volumes > 100%
		m_baseVelocityModel.setValue( MidiMaxVelocity ); check();
	}
}





void MidiPort::subscribeReadablePort( const QString& port, bool subscribe )
{
	const QString requested(port); Transition changing(this, &MidiPort::m_changingReadable);
	const auto lock = lockOutput();
	if (subscribe) { m_requestedReadablePorts.insert(requested); } else { m_requestedReadablePorts.remove(requested); }
	if (m_readablePorts.contains(requested)) { m_readablePorts[requested] = subscribe; }

	// make sure, MIDI-port is configured for input
	if( subscribe == true && !isInputEnabled() )
	{
		m_readableModel.setValue( true );
		changing.check();
	}

	m_midiClient->subscribeReadablePort(this, requested, subscribe); changing.check();
	if (!subscribe) { m_midiClient->cancelInputEvents(this, requested); changing.check(); }
}




void MidiPort::subscribeWritablePort( const QString& port, bool subscribe )
{
	const QString requested(port); Transition changing(this, &MidiPort::m_changingWritable);
	const auto lock = lockOutput();
	if (!subscribe && m_requestedWritablePorts.contains(requested)) { releaseOutput(); changing.check(); }
	if (subscribe) { m_requestedWritablePorts.insert(requested); } else { m_requestedWritablePorts.remove(requested); }
	if (m_writablePorts.contains(requested)) { m_writablePorts[requested] = subscribe; }

	// make sure, MIDI-port is configured for output
	if( subscribe == true && !isOutputEnabled() )
	{
		m_writableModel.setValue( true );
		changing.check();
	}
	m_midiClient->subscribeWritablePort(this, requested, subscribe); changing.check();
}




void MidiPort::updateMidiPortMode()
{
	Transition updating(this, &MidiPort::m_updatingMode);
	const QPointer<MidiPort> owner(this); MidiClient* const client = m_midiClient;
	// this small lookup-table makes everything easier
	static const Mode modeTable[2][2] =
	{
		{ Mode::Disabled, Mode::Output },
		{ Mode::Input, Mode::Duplex }
	} ;
	try { setModeImpl(modeTable[m_readableModel.value()][m_writableModel.value()]); updating.check(); }
	catch (...)
	{
		// A failed release must not leave the checkbox disabled while the native
		// mode still routes output. Restore the models without recursive signals.
		if (owner && owner->m_midiClient == client)
		{
			const QPointer<BoolModel> readable(&owner->m_readableModel), writable(&owner->m_writableModel);
			const bool readBlocked = readable->blockSignals(true), writeBlocked = writable->blockSignals(true);
			const auto restore = qScopeGuard([=] { if (readable) { readable->blockSignals(readBlocked); } if (writable) { writable->blockSignals(writeBlocked); } });
			readable->setValue(owner->isInputEnabled());
			if (owner && writable) { writable->setValue(owner->isOutputEnabled()); }
		}
		throw;
	}

	// check whether we have to dis-check items in connection-menu
	if( !isInputEnabled() )
	{
		for (const auto& name : requestedReadablePorts()) { subscribeReadablePort(name, false); updating.check(); }
		m_requestedReadablePorts.clear();
	}

	if( !isOutputEnabled() )
	{
		for (const auto& name : requestedWritablePorts()) { subscribeWritablePort(name, false); updating.check(); }
		m_requestedWritablePorts.clear();
	}

	emit readablePortsChanged();
	updating.check();
	emit writablePortsChanged();
	updating.check();
	emit modeChanged();
	updating.check();

	if( Engine::getSong() )
	{
		Engine::getSong()->setModified();
		updating.check();
	}
}




QStringList MidiPort::requestedReadablePorts() const
{
	const auto lock = lockOutput();
	auto names = m_requestedReadablePorts.values(); std::sort(names.begin(), names.end()); return names;
}

QStringList MidiPort::requestedWritablePorts() const
{
	const auto lock = lockOutput();
	auto names = m_requestedWritablePorts.values(); std::sort(names.begin(), names.end()); return names;
}

void MidiPort::updateReadablePorts()
{
	if (m_clientTransfer) { return; }
	Transition changing(this, &MidiPort::m_changingReadable);
	const auto lock = lockOutput();
	const auto old = m_readablePorts;
	const auto names = m_midiClient->readablePorts(); changing.check();
	Map next;
	for (const auto& name : names)
	{
		next.insert(name, isInputEnabled() && m_requestedReadablePorts.contains(name));
	}
	m_readablePorts = next;
	// Retain desired names across disappearance and reapply them on reappearance.
	// Native client APIs report no acknowledgment, so this remains routing intent.
	for (auto it = old.begin(); it != old.end(); ++it)
	{
		if (it.value() && !m_requestedReadablePorts.contains(it.key())) { m_midiClient->subscribeReadablePort(this, it.key(), false); changing.check(); }
	}
	for (auto it = next.begin(); it != next.end(); ++it)
	{
		if (it.value()) { m_midiClient->subscribeReadablePort(this, it.key(), true); changing.check(); }
	}
	emit readablePortsChanged();
	changing.check();
}

void MidiPort::updateWritablePorts()
{
	if (m_clientTransfer) { return; }
	Transition changing(this, &MidiPort::m_changingWritable);
	const auto lock = lockOutput();
	const auto old = m_writablePorts;
	const auto names = m_midiClient->writablePorts(); changing.check();
	Map next;
	for (const auto& name : names)
	{
		next.insert(name, isOutputEnabled() && m_requestedWritablePorts.contains(name));
	}
	m_writablePorts = next;
	for (auto it = old.begin(); it != old.end(); ++it)
	{
		if (it.value() && !m_requestedWritablePorts.contains(it.key())) { m_midiClient->subscribeWritablePort(this, it.key(), false); changing.check(); }
	}
	for (auto it = next.begin(); it != next.end(); ++it)
	{
		if (it.value()) { m_midiClient->subscribeWritablePort(this, it.key(), true); changing.check(); }
	}
	emit writablePortsChanged();
	changing.check();
}




void MidiPort::updateOutputProgram()
{
	processOutEvent(MidiEvent(MidiProgramChange, realOutputChannel(), outputProgram()-1), {}, outputSource());
}




void MidiPort::invalidateCilent()
{
	const auto lock = lockOutput();
	m_midiClient = s_dummyClient;
	for (auto& notes : m_outputNotes) { notes.fill(0); }
	m_trackedOutputNotes = 0;
	for (auto& pedals : m_outputPedals) { pedals.fill(false); }
}


} // namespace lmms

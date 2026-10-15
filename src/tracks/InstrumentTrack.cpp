/*
 * InstrumentTrack.cpp - implementation of InstrumentTrack class
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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
#include "InstrumentTrack.h"
#include "NativeLinkedControls.h"
#include <cmath>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <QPointer>
#include <QScopeGuard>

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "FileResourceIO.h"
#include "GuiApplication.h"
#include "Mixer.h"
#include "InstrumentTrackView.h"
#include "Instrument.h"
#include "Keymap.h"
#include "MidiClient.h"
#include "MidiClip.h"
#include "MixHelpers.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PianoRoll.h"
#include "Pitch.h"
#include "Song.h"

namespace lmms
{

namespace
{
bool validTrackMidiEvent(const MidiEvent& event, bool input)
{
	const bool channel = event.channel() >= 0 && event.channel() < MidiChannelCount;
	const bool noteChannel = channel || (input && event.channel() == -1); // Native piano preview sentinel.
	switch (event.type())
	{
	case MidiNoteOn: case MidiNoteOff: case MidiKeyPressure:
		return noteChannel && event.param(0) >= 0 && event.param(0) < NumKeys
			&& event.param(1) >= 0 && event.param(1) <= MidiMaxVelocity;
	case MidiControlChange: return event.isValidControlChange();
	case MidiProgramChange: case MidiChannelPressure:
		return channel && event.param(0) >= 0 && event.param(0) <= 127;
	case MidiPitchBend: return channel && event.param(0) >= 0 && event.param(0) <= MidiMaxPitchBend;
	case MidiMetaEvent:
		return event.metaEvent() != MidiNotePanning || (noteChannel && event.param(0) >= 0 && event.param(0) < NumKeys
			&& event.param(1) >= MidiMinPanning && event.param(1) <= MidiMaxPanning);
	default: return true; // Preserve existing system/plugin-specific dispatch.
	}
}
}

InstrumentTrack::InstrumentTrack(TrackContainer* tc) :
	Track(Track::Type::Instrument, tc),
	MidiEventProcessor(),
	m_midiPort(tr("unnamed_track"), Engine::audioEngine()->midiClient(), this, this),
	m_notes(),
	m_sustainPedalPressed(false),
	m_silentBuffersProcessed(false),
	m_previewMode(false),
	m_baseNoteModel(0, 0, NumKeys - 1, this, tr("Base note")),
	m_firstKeyModel(0, 0, NumKeys - 1, this, tr("First note")),
	m_lastKeyModel(0, 0, NumKeys - 1, this, tr("Last note")),
	m_hasAutoMidiDev(false),
	m_volumeModel(DefaultVolume, MinVolume, MaxVolume, 0.1f, this, tr("Volume")),
	m_panningModel(DefaultPanning, PanningLeft, PanningRight, 0.1f, this, tr("Panning")),
	m_audioBusHandle(tr("unnamed_track"), true, &m_volumeModel, &m_panningModel, &m_mutedModel, this),
	m_pitchModel(0, MinPitchDefault, MaxPitchDefault, 1, this, tr("Pitch")),
	m_pitchRangeModel(1, 1, 60, this, tr("Pitch range")),
	m_mixerChannelModel(0, 0, 0, this, tr("Mixer channel")),
	m_useMasterPitchModel(true, this, tr("Master pitch")),
	m_instrument(nullptr),
	m_soundShaping(this),
	m_arpeggio(this),
	m_noteStacking(this),
	m_piano(this),
	m_microtuner(this)
{
	m_pitchModel.setCenterValue( 0 );
	m_pitchModel.setStrictStepSize(true);
	m_panningModel.setCenterValue( DefaultPanning );
	m_baseNoteModel.setInitValue( DefaultKey );
	m_firstKeyModel.setInitValue(0);
	m_lastKeyModel.setInitValue(NumKeys - 1);

	m_mixerChannelModel.setRange( 0, Engine::mixer()->numChannels()-1, 1);

	for( int i = 0; i < NumKeys; ++i )
	{
		m_notes[i] = nullptr;
		m_runningMidiNotes[i] = 0;
	}


	// Initialize the m_midiCCEnabled variable, but it's actually going to be connected
	// to a LedButton
	m_midiCCEnable = std::make_unique<BoolModel>(false, this, tr("Enable/Disable MIDI CC"));
	connect(m_midiCCEnable.get(), &BoolModel::dataChanged, this, [this]
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		invalidateMidiCC();
		if (!m_loadingMidiCC && m_midiCCEnable->value() && !Engine::getSong()->isPlaying()) { refreshMidiCC(); }
	}, Qt::DirectConnection);

	// Initialize the MIDI CC controller models and connect them to the method that processes
	// the midi cc events
	for (int i = 0; i < MidiControllerCount; ++i)
	{
		m_midiCCModel[i] = std::make_unique<FloatModel>(0.0f, 0.0f, 127.0f, 1.0f,
			this, tr("CC Controller %1").arg(i));

		connect(m_midiCCModel[i].get(), &FloatModel::dataChanged,
			this, [this, i]{ processCCEvent(i); }, Qt::DirectConnection);
	}

	setName( tr( "Default preset" ) );

	connect(&m_baseNoteModel, SIGNAL(dataChanged()), this, SLOT(updateBaseNote()), Qt::DirectConnection);
	connect(&m_pitchModel, SIGNAL(dataChanged()), this, SLOT(updatePitch()), Qt::DirectConnection);
	connect(&m_pitchRangeModel, SIGNAL(dataChanged()), this, SLOT(updatePitchRange()), Qt::DirectConnection);
	connect(&m_mixerChannelModel, SIGNAL(dataChanged()), this, SLOT(updateMixerChannel()), Qt::DirectConnection);
	connect(&m_mixerChannelModel, SIGNAL(propertiesChanged()), this, SLOT(updateMixerChannel()), Qt::DirectConnection);
	connect(&m_microtuner, &Microtuner::tuningChanged, this, &InstrumentTrack::updateBaseNote, Qt::DirectConnection);

	autoAssignMidiDevice(true);
}



bool InstrumentTrack::keyRangeImport() const
{
	return m_microtuner.enabled() && m_microtuner.keyRangeImport();
}


bool InstrumentTrack::isKeyMapped(int key) const
{
	if (key < firstKey() || key > lastKey()) {return false;}
	if (!m_microtuner.enabled()) {return true;}

	Song *song = Engine::getSong();
	if (!song) {return false;}

	return song->getKeymap(m_microtuner.currentKeymap())->isMapped(key);
}


int InstrumentTrack::firstKey() const
{
	if (keyRangeImport())
	{
		return Engine::getSong()->getKeymap(m_microtuner.currentKeymap())->getFirstKey();
	}
	else
	{
		return m_firstKeyModel.value();
	}
}


int InstrumentTrack::lastKey() const
{
	if (keyRangeImport())
	{
		return Engine::getSong()->getKeymap(m_microtuner.currentKeymap())->getLastKey();
	}
	else
	{
		return m_lastKeyModel.value();
	}
}


int InstrumentTrack::baseNote() const
{
	int mp = m_useMasterPitchModel.value() ? Engine::getSong()->masterPitch() : 0;

	if (keyRangeImport())
	{
		return Engine::getSong()->getKeymap(m_microtuner.currentKeymap())->getBaseKey() - mp;
	}
	else
	{
		return m_baseNoteModel.value() - mp;
	}
}


float InstrumentTrack::baseFreq() const
{
	if (m_microtuner.enabled())
	{
		return Engine::getSong()->getKeymap(m_microtuner.currentKeymap())->getBaseFreq();
	}
	else
	{
		return DefaultBaseFreq;
	}
}



float InstrumentTrack::nominalFrequency(int key) const
{
	if (m_microtuner.enabled()) { return m_microtuner.keyToFreq(key, m_baseNoteModel.value()); }
	return DefaultBaseFreq * std::exp2((key - m_baseNoteModel.value()) / 12.0f);
}

InstrumentTrack::~InstrumentTrack()
{
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	m_retiring = true; m_replacingInstrument = true;
	m_midiPort.detachInputProcessor();
	// De-assign midi device
	if (s_autoAssignedTrack == this)
	{
		if (m_hasAutoMidiDev)
		{
			autoAssignMidiDevice(false);
		}
		s_autoAssignedTrack = nullptr;
	}

	// kill all running notes and the iph
	silenceAllNotes( true );

	// now we're save deleting the instrument
	const auto previous = m_instrument; m_instrument = nullptr; delete previous.data();
}

Instrument* InstrumentTrack::instrument() { return m_instrument.data(); }
const Instrument* InstrumentTrack::instrument() const { return m_instrument.data(); }




void InstrumentTrack::processAudioBuffer( SampleFrame* buf, const f_cnt_t frames, NotePlayHandle* n )
{
	// we must not play the sound if this InstrumentTrack is muted...
	if( isMuted() || ( Engine::getSong()->playMode() != Song::PlayMode::MidiClip &&
				n && n->isPatternTrackMuted() ) || ! m_instrument )
	{
		return;
	}

	// Test for silent input data if instrument provides a single stream only (i.e. driven by InstrumentPlayHandle)
	// We could do that in all other cases as well but the overhead for silence test is bigger than
	// what we potentially save. While playing a note, a NotePlayHandle-driven instrument will produce sound in
	// 99 of 100 cases so that test would be a waste of time.
	if (m_instrument->isSingleStreamed() && MixHelpers::isSilent(buf, frames))
	{
		// at least pass one silent buffer to allow
		if( m_silentBuffersProcessed )
		{
			// skip further processing
			return;
		}
		m_silentBuffersProcessed = true;
	}
	else
	{
		m_silentBuffersProcessed = false;
	}

	// get volume knob data
	static const float DefaultVolumeRatio = 1.0f / DefaultVolume;
	/*ValueBuffer * volBuf = m_volumeModel.valueBuffer();
	float v_scale = volBuf
		? 1.0f
		: getVolume() * DefaultVolumeRatio;*/

	// instruments using instrument-play-handles will call this method
	// without any knowledge about notes, so they pass NULL for n, which
	// is no problem for us since we just bypass the envelopes+LFOs
	if (!m_instrument->isSingleStreamed() && n != nullptr)
	{
		const f_cnt_t offset = n->noteOffset();
		m_soundShaping.processAudioBuffer( buf + offset, frames - offset, n );
		const float vol = ( (float) n->getVolume() * DefaultVolumeRatio );
		const panning_t pan = std::clamp(n->getPanning(), PanningLeft, PanningRight);
		StereoVolumeVector vv = panningToVolumeVector( pan, vol );
		for( f_cnt_t f = offset; f < frames; ++f )
		{
			for( int c = 0; c < 2; ++c )
			{
				buf[f][c] *= vv.vol[c];
			}
		}
	}
}




MidiEvent InstrumentTrack::applyMasterKey( const MidiEvent& event )
{
	MidiEvent copy( event );
	switch( event.type() )
	{
		case MidiNoteOn:
		case MidiNoteOff:
		case MidiKeyPressure:
			copy.setKey( masterKey( event.key() ) );
			break;
		default:
			break;
	}
	return copy;
}




void InstrumentTrack::processCCEvent(int controller)
{
	if (m_loadingMidiCC || !activateMidiCC(controller)) { return; }
	// Playback applies rack values once per native tick with its sample offset.
	if (!Engine::getSong()->isPlaying()) { refreshMidiCC(); }
}

bool InstrumentTrack::activateMidiCC(int controller)
{
	if (controller < 0 || controller >= MidiControllerCount) { return false; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	m_activeMidiCC.set(controller);
	return true;
}

bool InstrumentTrack::initializeMidiCC(int controller, float value)
{
	if (controller < 0 || controller >= MidiControllerCount || !std::isfinite(value)
		|| value < 0 || value > MidiMaxControllerValue) { return false; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<InstrumentTrack> receiver(this);
	const bool previous = m_loadingMidiCC; m_loadingMidiCC = true;
	const auto restore = qScopeGuard([receiver, previous] { if (receiver) { receiver->m_loadingMidiCC = previous; } });
	m_midiCCModel[controller]->setInitValue(value); if (!receiver) { return false; } invalidateMidiCC(controller);
	return true;
}

void InstrumentTrack::refreshMidiCC(const TimePos& time, f_cnt_t offset)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_loadingMidiCC || m_refreshingMidiCC || !m_midiCCEnable->value() || !m_instrument) { return; }
	const QPointer<Instrument> receiver(m_instrument);
	const int channel = midiPort()->realOutputChannel();
	if (channel != m_lastMidiCCChannel) { invalidateMidiCC(); m_lastMidiCCChannel = channel; }
	m_refreshingMidiCC = true;
	const auto restore = qScopeGuard([this] { m_refreshingMidiCC = false; });
	// Never send the unused rack's zeroes: channel-mode controllers are commands,
	// even with a zero value. Preserve explicitly used zero-valued controllers.
	const auto active = m_activeMidiCC;
	for (int controller = 0; controller < MidiControllerCount; ++controller)
	{
		if (!m_midiCCEnable->value()) { break; }
		if (!active.test(controller)) { continue; }
		const auto current = m_midiCCModel[controller]->value();
		if (!std::isfinite(current)) { invalidateMidiCC(controller); continue; }
		const auto value = static_cast<int>(std::lround(std::clamp(current, 0.f, float(MidiMaxControllerValue))));
		if (m_sentMidiCC.test(controller) && m_lastMidiCC[controller] == value) { continue; }
		const MidiEvent event(MidiControlChange, channel, controller, value, nullptr, MidiEvent::Source::Internal);
		const auto previous = m_midiCCRackEvent; m_midiCCRackEvent = &event;
		const auto restoreEvent = qScopeGuard([this, previous] { m_midiCCRackEvent = previous; });
		processInEvent(event, time, offset);
		if (!receiver || receiver.data() != m_instrument) { invalidateMidiCC(); return; }
	}
}

void InstrumentTrack::invalidateMidiCC(int controller)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	++m_midiCCDeliveryGeneration;
	if (controller < 0) { m_sentMidiCC.reset(); }
	else { m_sentMidiCC.reset(controller); }
}

bool InstrumentTrack::sendMidiEventToInstrument(const MidiEvent& event, const TimePos& time, f_cnt_t offset)
{
	const QPointer<InstrumentTrack> owner(this);
	if (event.type() != MidiControlChange) { return instrument() && instrument()->handleMidiEvent(event, time, offset); }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const int controller = event.controllerNumber(), value = event.controllerValue(), channel = event.channel();
	if (!event.isValidControlChange()) { return false; }
	// Input and native output can change the instrument behind an unchanged rack
	// model. Retain the last completed attempt, never an assumed plugin state.
	invalidateMidiCC(controller == MidiControllerResetAllControllers ? -1 : controller);
	if (controller == MidiControllerSustain)
	{
		if (value > MidiMaxControllerValue / 2) { m_sustainPedalPressed = true; }
		else { releaseMidiSustain(offset); }
		if (!owner) { return false; }
	}
	else if (controller == MidiControllerResetAllControllers)
	{
		releaseMidiSustain(offset);
		if (!owner) { return false; }
		// Native note-off callbacks may have sent a CC before this reset reaches
		// the instrument. None of those attempts survive the later reset.
		invalidateMidiCC();
	}
	const auto generation = m_midiCCDeliveryGeneration;
	const QPointer<Instrument> receiver(m_instrument);
	const bool handled = receiver && receiver->handleMidiEvent(event, time, offset);
	if (!owner) { return handled; }
	if (generation != m_midiCCDeliveryGeneration || !receiver || receiver.data() != m_instrument)
	{
		// A callback may deliver another CC or invalidate native state while this
		// attempt is in flight. Its completion must not conceal that change.
		invalidateMidiCC(controller); return handled;
	}
	if (channel == m_lastMidiCCChannel)
	{
		m_lastMidiCC[controller] = value; m_sentMidiCC.set(controller);
	}
	return handled;
}

void InstrumentTrack::releaseMidiSustain(f_cnt_t offset)
{
	if (!isSustainPedalPressed()) { return; }
	for (NotePlayHandle* nph : m_sustainedNotes)
	{
		if (nph && nph->isReleased() && nph->origin() == NotePlayHandle::Origin::MidiInput)
		{
			nph->setLength(nph->recordedMidiLength(offset)); midiNoteOff(*nph);
		}
	}
	m_sustainedNotes.clear(); m_sustainPedalPressed = false;
}



void InstrumentTrack::processInEvent( const MidiEvent& event, const TimePos& time, f_cnt_t offset )
{
	if (!validTrackMidiEvent(event, true)) { return; }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	if (event.source() == MidiEvent::Source::External && (Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup() || Engine::audioEngine()->hasStoredAudioDevice()))
	{
		return;
	}

	bool eventHandled = false;

	switch( event.type() )
	{
		// we don't send MidiNoteOn, MidiNoteOff and MidiKeyPressure
		// events to instrument as NotePlayHandle will send them on its
		// own
		case MidiNoteOn:
			if( event.velocity() > 0 )
			{
				// play a note only if it is not already playing and if it is within configured bounds
				if (event.key() >= firstKey() && event.key() <= lastKey() && m_notes[event.key()] == nullptr)
				{
					NotePlayHandle* nph =
						NotePlayHandleManager::acquire(
								this, offset,
								std::numeric_limits<f_cnt_t>::max() / 2,
								Note(TimePos(), Engine::getSong()->getPlayPos(Engine::getSong()->playMode()),
										event.key(), event.volume(midiPort()->baseVelocity())),
								nullptr, event.channel(),
								NotePlayHandle::Origin::MidiInput);
					m_notes[event.key()] = nph;
					if( ! Engine::audioEngine()->addPlayHandle( nph ) )
					{
						m_notes[event.key()] = nullptr;
					}
				}
				eventHandled = true;
				break;
			}

		case MidiNoteOff:
			if( m_notes[event.key()] != nullptr )
			{
				// do actual note off and remove internal reference to NotePlayHandle (which itself will
				// be deleted later automatically)
				m_notes[event.key()]->noteOff( offset );
				if (isSustainPedalPressed() &&
					m_notes[event.key()]->origin() ==
					NotePlayHandle::Origin::MidiInput)
				{
					m_sustainedNotes << m_notes[event.key()];
				}
				m_notes[event.key()] = nullptr;
			}
			eventHandled = true;
			break;

		case MidiKeyPressure:
			if( m_notes[event.key()] != nullptr )
			{
				// setVolume() calls processOutEvent() with MidiKeyPressure so the
				// attached instrument will receive the event as well
				m_notes[event.key()]->setVolume( event.volume( midiPort()->baseVelocity() ) );
			}
			eventHandled = true;
			break;

		case MidiPitchBend:
			// updatePitch() is connected to m_pitchModel::dataChanged() which will send out
			// MidiPitchBend events
			m_pitchModel.setValue( m_pitchModel.minValue() + event.pitchBend() * m_pitchModel.range() / MidiMaxPitchBend );
			recordMidiInputPitch(offset);
			break;

		case MidiControlChange:
			if( event.controllerNumber() == MidiControllerAllSoundOff ||
				event.controllerNumber() == MidiControllerAllNotesOff ||
				event.controllerNumber() == MidiControllerOmniOn ||
				event.controllerNumber() == MidiControllerOmniOff ||
				event.controllerNumber() == MidiControllerMonoOn ||
				event.controllerNumber() == MidiControllerPolyOn )
			{
				// Preserve prior rack attempts only for this exact rack command.
				// Nested input and explicit native silence must invalidate them.
				silenceAllNotesImpl(false, &event == m_midiCCRackEvent);
			}
			break;

		case MidiMetaEvent:
			// handle special cases such as note panning
			switch( event.metaEvent() )
			{
				case MidiNotePanning:
					if( m_notes[event.key()] != nullptr )
					{
						eventHandled = true;
						m_notes[event.key()]->setPanning( event.panning() );
					}
					break;
				default:
					qWarning( "InstrumentTrack: unhandled MIDI meta event: %i", event.metaEvent() );
					break;
			}
			break;

		default:
			break;
	}

	// If the event wasn't handled, check if there's a loaded instrument and if so send the
	// event to it. If it returns false means the instrument didn't handle the event, so we trigger a warning.
	if (eventHandled == false && !sendMidiEventToInstrument(event, time, offset))
	{
		qWarning("InstrumentTrack: unhandled MIDI event %d", event.type());
	}

}




void InstrumentTrack::processOutEvent( const MidiEvent& event, const TimePos& time, f_cnt_t offset )
{
	const QPointer<InstrumentTrack> owner(this);
	if (!validTrackMidiEvent(event, false)) { return; }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	// do nothing if we do not have an instrument instance (e.g. when loading settings)
	if( m_instrument == nullptr )
	{
		return;
	}

	const MidiEvent transposedEvent = applyMasterKey( event );
	const int key = transposedEvent.key();

	// If we have a selected output midi channel between 1-16, we will use that channel to handle the midi event.
	// But if our selected midi output channel is 0 ("--"), we will use the event channel instead.
	const auto handleEventOutputChannel = midiPort()->outputChannel() == 0
		? event.channel()
		: midiPort()->realOutputChannel();

	const auto type = event.type() == MidiNoteOn && event.param(1) == 0 ? MidiNoteOff : event.type();
	switch( type )
	{
		case MidiNoteOn:
		{
			const auto notes = std::lock_guard{m_midiNotesMutex};
			m_piano.setKeyState( event.key(), true );	// event.key() = original key

			if( key >= 0 && key < NumKeys )
			{
				if( m_runningMidiNotes[key] > 0 )
				{
					m_instrument->handleMidiEvent( MidiEvent( MidiNoteOff, handleEventOutputChannel, key, 0 ), time, offset );
				}
				if (m_runningMidiNotes[key] < std::numeric_limits<int>::max()) { ++m_runningMidiNotes[key]; }
				m_instrument->handleMidiEvent( MidiEvent( MidiNoteOn, handleEventOutputChannel, key, event.velocity() ), time, offset );

			}
		}
			emit newNote();
			break;

		case MidiNoteOff:
		{
			const auto notes = std::lock_guard{m_midiNotesMutex};
			m_piano.setKeyState( event.key(), false );	// event.key() = original key

			if (key >= 0 && key < NumKeys)
			{
				if (m_runningMidiNotes[key] > 0) { --m_runningMidiNotes[key]; }
				if (m_runningMidiNotes[key] == 0)
				{ m_instrument->handleMidiEvent(MidiEvent(MidiNoteOff, handleEventOutputChannel, key, 0), time, offset); }
			}
		}
			emit endNote();
			break;

		default:
			sendMidiEventToInstrument(transposedEvent, time, offset);
			break;
	}

	// if appropriate, midi-port does further routing
	if (owner) { owner->m_midiPort.processOutEvent(event, time); }
}




void InstrumentTrack::recordMidiInputPitch(f_cnt_t offset)
{
	if (!isNoteRecording()) { return; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	for (const auto handle : m_processHandles)
	{
		if (handle->origin() != NotePlayHandle::Origin::MidiInput
			|| (handle->isReleased() && !isSustainPedalPressed())) { continue; }
		Note snapshot(*handle); snapshot.setLength(handle->recordedMidiLength(offset));
		emit midiNotePitch(snapshot, m_pitchModel.value());
	}
}

void InstrumentTrack::finishMidiInputRecording()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	for (const auto handle : m_processHandles)
	{
		if (handle->origin() != NotePlayHandle::Origin::MidiInput
			|| (handle->isReleased() && !isSustainPedalPressed())) { continue; }
		Note completed(*handle); completed.setLength(handle->recordedMidiLength());
		emit midiNoteOff(completed);
	}
}

void InstrumentTrack::silenceAllNotes( bool removeIPH )
{
	silenceAllNotesImpl(removeIPH, false);
}

void InstrumentTrack::silenceAllNotesImpl(bool removeIPH, bool preserveMidiCC)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<InstrumentTrack> receiver(this);
	// Native playback cleanup invalidates state, but a rack-generated channel-
	// mode command must not cause every earlier controller to resend each tick.
	if (!preserveMidiCC) { invalidateMidiCC(); }
	else
	{
		// Silence clears native pedal state. Reassert value controllers once,
		// retaining channel-mode attempts so unchanged commands do not loop.
		++m_midiCCDeliveryGeneration;
		for (int controller = 0; controller < MidiControllerAllSoundOff; ++controller) { m_sentMidiCC.reset(controller); }
	}
	m_midiNotesMutex.lock();
	for( int i = 0; i < NumKeys; ++i )
	{
		m_notes[i] = nullptr;
		m_runningMidiNotes[i] = 0;
	}
	m_midiNotesMutex.unlock();

	m_sustainedNotes.clear();
	m_sustainPedalPressed = false;
	// invalidate all NotePlayHandles and PresetPreviewHandles linked to this track
	m_processHandles.clear();

	auto flags = PlayHandle::Type::NotePlayHandle | PlayHandle::Type::PresetPreviewHandle;
	if( removeIPH )
	{
		flags |= PlayHandle::Type::InstrumentPlayHandle;
	}
	Engine::audioEngine()->removePlayHandlesOfTypes( this, flags );
	if (receiver) { receiver->m_midiPort.releaseOutput(); }
}




f_cnt_t InstrumentTrack::beatLen( NotePlayHandle * _n ) const
{
	if( m_instrument != nullptr )
	{
		const f_cnt_t len = m_instrument->beatLen( _n );
		if( len > 0 )
		{
			return len;
		}
	}
	return m_soundShaping.envFrames();
}




void InstrumentTrack::playNote( NotePlayHandle* n, SampleFrame* workingBuffer )
{
	// Note: under certain circumstances the working buffer is a nullptr.
	// These cases are triggered in PlayHandle::doProcessing when the play method is called with a nullptr.
	// TODO: Find out if we can skip processing at a higher level if the buffer is nullptr.

	// arpeggio- and chord-widget has to do its work -> adding sub-notes
	// for chords/arpeggios
	m_noteStacking.processNote( n );
	m_arpeggio.processNote( n );

	if( n->isMasterNote() == false && m_instrument != nullptr )
	{
		// all is done, so now lets play the note!
		m_instrument->playNote( n, workingBuffer );

		// This is effectively the same as checking if workingBuffer is not a nullptr.
		// Calling processAudioBuffer with a nullptr leads to crashes. Hence the check.
		if (n->usesBuffer())
		{
			const f_cnt_t frames = n->framesLeftForCurrentPeriod();
			const f_cnt_t offset = n->noteOffset();
			processAudioBuffer(workingBuffer, frames + offset, n);
		}
	}
}




QString InstrumentTrack::instrumentName() const
{
	if( m_instrument != nullptr )
	{
		return m_instrument->displayName();
	}
	return QString();
}




void InstrumentTrack::deleteNotePluginData( NotePlayHandle* n )
{
	if( m_instrument != nullptr )
	{
		m_instrument->deleteNotePluginData( n );
	}
}




void InstrumentTrack::setName(const QString& new_name)
{
	const QPointer<InstrumentTrack> owner(this);
	Track::setName(new_name);
	if (!owner) { return; } const QString actual = owner->name();
	owner->m_midiPort.setName(actual);
	if (owner) { owner->m_audioBusHandle.setName(actual); }
}






void InstrumentTrack::updateBaseNote()
{
	Engine::audioEngine()->requestChangeInModel();
	for (const auto& processHandle : m_processHandles)
	{
		processHandle->setFrequencyUpdate();
	}
	Engine::audioEngine()->doneChangeInModel();
}




void InstrumentTrack::updatePitch()
{
	const QPointer<InstrumentTrack> owner(this);
	updateBaseNote();
	if (owner) { owner->processOutEvent(MidiEvent(MidiPitchBend, owner->midiPort()->realOutputChannel(), owner->midiPitch())); }
}




void InstrumentTrack::updatePitchRange()
{
	const QPointer<InstrumentTrack> owner(this);
	const int r = m_pitchRangeModel.value();
	m_pitchModel.setRange( MinPitchDefault * r, MaxPitchDefault * r );
	if (!owner) { return; }

	processOutEvent( MidiEvent( MidiControlChange, midiPort()->realOutputChannel(),
								MidiControllerRegisteredParameterNumberLSB, MidiPitchBendSensitivityRPN & 0x7F ) );
	if (!owner) { return; }
	processOutEvent( MidiEvent( MidiControlChange, midiPort()->realOutputChannel(),
								MidiControllerRegisteredParameterNumberMSB, ( MidiPitchBendSensitivityRPN >> 8 ) & 0x7F ) );
	if (!owner) { return; }
	processOutEvent( MidiEvent( MidiControlChange, midiPort()->realOutputChannel(), MidiControllerDataEntry, midiPitchRange() ) );
}




void InstrumentTrack::updateMixerChannel()
{
	m_audioBusHandle.setNextMixerChannel(m_mixerChannelModel.value());
}




int InstrumentTrack::masterKey( int _midi_key ) const
{

	int key = baseNote();
	return std::clamp(_midi_key - (key - DefaultKey), 0, NumKeys);
}




void InstrumentTrack::removeMidiPortNode( DataFile & _dataFile )
{
	// Preserve the destination track's port, without deleting an embedded
	// connection-owned MIDI controller's independently serialized port.
	auto instrument = _dataFile.content().firstChildElement("instrumenttrack");
	auto port = instrument.firstChildElement("midiport");
	if (!port.isNull()) { instrument.removeChild(port); }
}




bool InstrumentTrack::play( const TimePos & _start, const f_cnt_t _frames,
							const f_cnt_t _offset, int _clip_num )
{
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	if( ! m_instrument || ! tryLock() )
	{
		return false;
	}
	const auto unlockTrack = qScopeGuard([this] { unlock(); });
	refreshMidiCC(_start, _offset);
	const float frames_per_tick = Engine::framesPerTick();

	clipVector clips;
	class PatternTrack * pattern_track = nullptr;
	if( _clip_num >= 0 )
	{
		Clip * clip = getClip( _clip_num );
		clips.push_back( clip );
		if (trackContainer() == Engine::patternStore())
		{
			pattern_track = PatternTrack::findPatternTrack(_clip_num);
		}
	}
	else
	{
		getClipsInRange( clips, _start, _start + static_cast<int>(
					_frames / frames_per_tick ) );
	}

	// Handle automation: detuning
	for (const auto& processHandle : m_processHandles)
	{
		processHandle->processTimePos(_start);
	}

	if ( clips.size() == 0 )
	{
		return false;
	}

	bool played_a_note = false;	// will be return variable

	for (const auto& clip : clips)
	{
		auto c = dynamic_cast<MidiClip*>(clip);
		// everything which is not a MIDI clip won't be played
		// A MIDI clip playing in the Piano Roll window will always play
		if (c == nullptr || (Engine::getSong()->playMode() != Song::PlayMode::MidiClip && clip->isMuted()))
		{
			continue;
		}
		TimePos cur_start = _start;
		if( _clip_num < 0 )
		{
			cur_start -= c->startPosition() + c->startTimeOffset();
		}

		// get all notes from the given clip...
		const NoteVector & notes = c->notes();
		// ...and set our index to zero
		auto nit = notes.begin();

		// very effective algorithm for playing notes that are
		// posated within the current sample-frame

		if( cur_start > 0 )
		{
			// skip notes which end before start-bar
			while( nit != notes.end() && ( *nit )->endPos() < cur_start )
			{
				++nit;
			}
		}

		while (nit != notes.end() && (*nit)->pos() < c->length() - c->startTimeOffset())
		{
			const auto currentNote = *nit;
			// Skip any notes note at the current time pos or not overlapping with the start.
			if (!(currentNote->pos() == cur_start
				|| (cur_start == -c->startTimeOffset() && (*nit)->pos() < cur_start && (*nit)->endPos() > cur_start)))
			{
				++nit;
				continue;
			}

			// Calculate the overlap of the note over the clip end.
			const auto noteOverlap = std::max(0, currentNote->endPos() - (c->length() - c->startTimeOffset()));
			// If the note is a Step Note, frames will be 0 so the NotePlayHandle
			// plays for the whole length of the sample
			const auto noteFrames = currentNote->type() == Note::Type::Step
				? 0
				: (currentNote->endPos() - cur_start - noteOverlap) * frames_per_tick;

			NotePlayHandle* notePlayHandle = NotePlayHandleManager::acquire(this, _offset, noteFrames, *currentNote);
			notePlayHandle->setPatternTrack(pattern_track);
			// are we playing global song?
			if( _clip_num < 0 )
			{
				// then set song-global offset of clip in order to
				// properly perform the note detuning
				notePlayHandle->setSongGlobalParentOffset( c->startPosition() + c->startTimeOffset());
			}

			Engine::audioEngine()->addPlayHandle( notePlayHandle );
			played_a_note = true;
			++nit;
		}
	}
	return played_a_note;
}




Clip* InstrumentTrack::createClip(const TimePos & pos)
{
	auto p = new MidiClip(this);
	p->movePosition(pos);
	return p;
}




gui::TrackView* InstrumentTrack::createView( gui::TrackContainerView* tcv )
{
	return new gui::InstrumentTrackView( this, tcv );
}




void InstrumentTrack::saveTrackSpecificSettings(QDomDocument& doc, QDomElement& thisElement, bool presetMode)
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	if (!canSaveTrackState()) { throw std::logic_error("Native instrument serialization cannot reenter an active transition"); }
	const QPointer<InstrumentTrack> owner(this); const QPointer<Song> song(Engine::getSong()); const QPointer<Instrument> native(m_instrument);
	Instrument* const selected = native.data(); const auto active = m_activeMidiCC; m_savingInstrumentSettings = true;
	const auto finish = qScopeGuard([&] { if (owner) { owner->m_savingInstrumentSettings = false; } });
	const auto ready = [&] {
		if (!owner || owner->m_retiring || !song || Engine::getSong() != song.data() || native.data() != selected || owner->m_instrument.data() != selected || owner->m_activeMidiCC != active)
		{ throw std::runtime_error("Native instrument serialization source retired or changed"); }
	};
	const auto check = [&] { ready(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready(); };
	check();

	m_volumeModel.saveSettings( doc, thisElement, "vol" );
	check();
	m_panningModel.saveSettings( doc, thisElement, "pan" );
	check();
	m_pitchModel.saveSettings( doc, thisElement, "pitch" );
	check();
	m_pitchRangeModel.saveSettings( doc, thisElement, "pitchrange" );
	check();

	m_mixerChannelModel.saveSettings( doc, thisElement, "mixch" );
	check();
	m_baseNoteModel.saveSettings( doc, thisElement, "basenote" );
	check();
	m_firstKeyModel.saveSettings(doc, thisElement, "firstkey");
	check();
	m_lastKeyModel.saveSettings(doc, thisElement, "lastkey");
	check();
	m_useMasterPitchModel.saveSettings( doc, thisElement, "usemasterpitch");
	check();
	m_microtuner.saveSettings(doc, thisElement);
	check();

	// Save MIDI CC stuff
	m_midiCCEnable->saveSettings(doc, thisElement, "enablecc");
	check();
	QStringList activeCC;
	for (int i = 0; i < MidiControllerCount; ++i)
	{ if (m_activeMidiCC.test(i)) { activeCC.append(QString::number(i)); } }
	thisElement.setAttribute("activecc", activeCC.join(','));
	QDomElement midiCC = doc.createElement("midicontrollers");
	thisElement.appendChild(midiCC);
	for (int i = 0; i < MidiControllerCount; ++i)
	{
		m_midiCCModel[i]->saveSettings(doc, midiCC, "cc" + QString::number(i));
		check();
	}

	if( m_instrument != nullptr )
	{
		QDomElement i = doc.createElement( "instrument" );
		i.setAttribute( "name", m_instrument->descriptor()->name );
		QDomElement ins = m_instrument->saveState( doc, i );
		check();
		if(native->key().isValid()) {
			ins.appendChild(native->key().saveXML(doc));
		}
		thisElement.appendChild( i );
	}
	m_soundShaping.saveState( doc, thisElement );
	check();
	m_noteStacking.saveState( doc, thisElement );
	check();
	m_arpeggio.saveState( doc, thisElement );
	check();

	// Presets omit the top-level MIDI port, so they must not temporarily
	// unsubscribe the live automatic input merely to serialize other settings.
	if (!presetMode && !song->shouldDiscardMidiConnections())
	{
		const bool raw = m_midiPort.client()->isRaw(); check();
		const QString automaticPort = ConfigManager::inst()->value("midi", "midiautoassign");
		// The assignment flag can survive settings restoration or an explicit
		// input change. Suspend only an assignment that is actually active;
		// otherwise serialization would enable a disabled input or erase a
		// manually enabled raw input when restoring the flag's intended state.
		// Recovery snapshots retain active input instead of applying the
		// portable export exclusion, so a rollback restores the current mode.
		const bool suspend = !song->isSavingCheckpoint() && m_hasAutoMidiDev && m_midiPort.isInputEnabled()
			&& (raw || m_midiPort.requestedReadablePorts().contains(automaticPort));
		bool pending = suspend;
		const auto restore = [&] { if (owner && pending) { pending = false; owner->autoAssignMidiDevice(true); } };
		const auto cleanup = qScopeGuard([&] { try { restore(); } catch (...) { if (song) { song->collectError("Native automatic MIDI assignment could not be restored after serialization"); } } });
		if (suspend) { autoAssignMidiDevice(false); check(); }
		m_midiPort.saveState(doc, thisElement); check();
		restore(); check();
	}

	m_audioBusHandle.effects()->saveState(doc, thisElement);
	check();
}





void InstrumentTrack::loadTrackSpecificSettings(const QDomElement& thisElement)
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	if (m_retiring || m_replacingInstrument || m_loadingMidiCC || m_savingInstrumentSettings || isSavingTrackState()
		|| (isLoadingTrackState() && !isDispatchingTrackSpecificState())
		|| (m_replacingPreset && (!m_permitPresetTrackLoad || !isDispatchingTrackSpecificState())))
	{ throw std::logic_error("Native instrument settings cannot load during instrument replacement or retirement"); }
	const QPointer<InstrumentTrack> owner(this); InstrumentTrack* const identity = this;
	const QPointer<Song> song(Engine::getSong()); const QPointer<TrackContainer> tracks(trackContainer());
	const auto engine = Engine::audioEngine(); QPointer<Instrument> expected(m_instrument); Instrument* expectedIdentity = expected.data();
	m_loadingMidiCC = true;
	const auto restoreMidiCC = qScopeGuard([owner] { if (owner && !owner->m_retiring) { owner->m_loadingMidiCC = false; owner->invalidateMidiCC(); } });
	const auto ready = [&] {
		if (!owner || owner->m_retiring || !song || Engine::getSong() != song.data() || Engine::audioEngine() != engine
			|| !tracks || owner->trackContainer() != tracks.data() || !owner->m_loadingMidiCC
			|| expected.data() != expectedIdentity || owner->m_instrument.data() != expectedIdentity)
		{ throw std::runtime_error("Native instrument settings owner or selected plugin changed"); }
	};
	const auto check = [&] { ready(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready(); };
	const auto load = [&](auto& model, const char* name) { model.loadSettings(thisElement, name); check(); };
	check(); m_activeMidiCC.reset();
	// Preview reuse remains native; ordinary preset/project restores still
	// retire the old plugin before constructing its replacement.
	const bool reuseInstrument = m_previewMode && expected && expected->nodeName() == getSavedInstrumentName(thisElement); ready();
	silenceAllNotes(!reuseInstrument); check();
	// The audio model guard excludes processing. No track mutex is held across
	// model, resource, plugin or observer callbacks.
	load(m_volumeModel, "vol"); load(m_panningModel, "pan"); load(m_pitchRangeModel, "pitchrange"); load(m_pitchModel, "pitch");
	m_mixerChannelModel.setRange(0, Engine::mixer()->numChannels() - 1); check();
	if (!m_previewMode) { load(m_mixerChannelModel, "mixch"); }
	load(m_baseNoteModel, "basenote"); load(m_firstKeyModel, "firstkey"); load(m_lastKeyModel, "lastkey"); load(m_useMasterPitchModel, "usemasterpitch");
	m_microtuner.loadSettings(thisElement); check();
	if (!m_audioBusHandle.effects()->clear()) { throw std::runtime_error("Native preset effect chain is already changing"); } check();
	m_midiCCEnable->setValue(false); check();
	const auto install = [&](const QString& name, const Plugin::Descriptor::SubPluginFeatures::Key* key, bool legacy, const QDomElement& state) {
		const auto previous = owner->m_instrument; owner->m_instrument = nullptr; expected = nullptr; expectedIdentity = nullptr;
		delete previous.data(); check();
		const QPointer<Instrument> native(owner->createInstrument(name, key, legacy));
		const auto cleanup = qScopeGuard([owner, native, identity] {
			if (native && native->isFromTrack(identity) && (!owner || owner->m_instrument != native)) { delete native.data(); }
		});
		check();
		if (!native || !native->isFromTrack(identity)) { throw std::runtime_error("Native preset instrument initialization or ownership failed"); }
		owner->m_instrument = native; expected = native; expectedIdentity = native.data();
		const bool restore = !legacy || native->nodeName() == state.nodeName(); ready();
		if (restore) { native->restoreState(state); check(); }
		emit owner->instrumentChanged(); check();
	};
	for (auto node = thisElement.firstChild(); !node.isNull(); node = node.nextSibling())
	{
		check(); if (!node.isElement()) { continue; }
		if (m_soundShaping.nodeName() == node.nodeName()) { m_soundShaping.restoreState(node.toElement()); check(); }
		else if (m_noteStacking.nodeName() == node.nodeName()) { m_noteStacking.restoreState(node.toElement()); check(); }
		else if (m_arpeggio.nodeName() == node.nodeName()) { m_arpeggio.restoreState(node.toElement()); check(); }
		else if (m_midiPort.nodeName() == node.nodeName()) { m_midiPort.restoreState(node.toElement()); check(); }
		else if (m_audioBusHandle.effects()->nodeName() == node.nodeName()) { m_audioBusHandle.effects()->restoreState(node.toElement()); check(); }
		else if (node.nodeName() == "instrument")
		{
			using PluginKey = Plugin::Descriptor::SubPluginFeatures::Key; PluginKey key(node.toElement().elementsByTagName("key").item(0).toElement());
			if (reuseInstrument) { expected->restoreState(node.firstChildElement()); check(); }
			else { install(node.toElement().attribute("name"), &key, false, node.firstChildElement()); }
		}
		else if (node.nodeName() == "midicontrollers")
		{
			for (int i = 0; i < MidiControllerCount; ++i) { m_midiCCModel[i]->loadSettings(node.toElement(), "cc" + QString::number(i)); check(); }
		}
		else if (AutomationClip::classNodeName() != node.nodeName() && ControllerConnection::classNodeName() != node.nodeName() && !node.toElement().hasAttribute("id"))
		{ install(node.nodeName(), nullptr, true, node.toElement()); }
	}
	m_midiCCEnable->loadSettings(thisElement, "enablecc"); check();
	if (thisElement.hasAttribute("activecc"))
	{
		const auto text = thisElement.attribute("activecc"); bool valid = text.size() <= MidiControllerCount * 4;
		if (valid && !text.isEmpty())
		{
			for (const auto& item : text.split(','))
			{
				bool converted = false; const int controller = item.toInt(&converted);
				if (!converted || controller < 0 || controller >= MidiControllerCount) { valid = false; break; }
				m_activeMidiCC.set(controller);
			}
		}
		if (!valid) { m_activeMidiCC.reset(); m_midiCCEnable->setValue(false); check(); song->collectError(tr("Invalid active MIDI CC controllers in instrument track")); }
	}
	else
	{
		const auto controllers = thisElement.firstChildElement("midicontrollers");
		for (int i = 0; i < MidiControllerCount; ++i)
		{ if (m_midiCCModel[i]->value() != 0 || !controllers.firstChildElement("cc" + QString::number(i)).isNull()) { m_activeMidiCC.set(i); } }
	}
	updatePitchRange(); check();
}


void InstrumentTrack::setPreviewMode( const bool value )
{
	m_previewMode = value;
}




void InstrumentTrack::replaceInstrument(DataFile dataFile)
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	if (m_retiring || isReplacingInstrument()) { throw std::logic_error("Native preset replacement cannot overlap instrument replacement or retirement"); }
	const QPointer<InstrumentTrack> owner(this); const QPointer<Song> song(Engine::getSong()); const QPointer<TrackContainer> tracks(trackContainer());
	m_replacingPreset = true;
	const auto finish = qScopeGuard([owner] { if (owner && !owner->m_retiring) { owner->m_replacingPreset = false; owner->m_permitPresetTrackLoad = false; } });
	std::vector<QPointer<Clip>> clips; for (auto clip : getClips()) { clips.emplace_back(clip); }
	const int mixerChannel = mixerChannelModel()->value(); const bool oldMute = isMutedByModel(), oldSolo = isSolo(), oldBeforeSolo = isMutedBeforeSolo();
	const auto ready = [&] {
		if (!owner || owner->m_retiring || !song || Engine::getSong() != song.data() || !tracks || owner->trackContainer() != tracks.data())
		{ throw std::runtime_error("Native preset destination retired during replacement"); }
		if (owner->getClips().size() != clips.size()) { throw std::runtime_error("Native preset destination clips changed"); }
		for (std::size_t index = 0; index < clips.size(); ++index)
		{ if (!clips[index] || owner->getClips()[index] != clips[index].data()) { throw std::runtime_error("Native preset destination clip identity changed"); } }
	};
	const auto check = [&] { ready(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready(); };
	check(); InstrumentTrack::removeMidiPortNode(dataFile);
	JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope links(dataFile);
	if (!dataFile.content().firstChildElement("nativeLinkedControls").isNull()) { NativeLinkedControls::unlinkInternalGroups(NativeLinkedControls::trackModels(owner.data())); ready(); }
	{
		owner->m_permitPresetTrackLoad = true;
		const auto permit = qScopeGuard([&] { if (owner) { owner->m_permitPresetTrackLoad = false; } });
		owner->Track::loadPreset(dataFile.content().toElement());
	}
	check(); owner->setMuted(oldMute); check(); owner->setSolo(oldSolo); check(); owner->setMutedBeforeSolo(oldBeforeSolo);
	owner->m_mixerChannelModel.setValue(mixerChannel); check();
	QString linkedError; const bool linked = links.finish(dataFile.content(), linkedError); ready();
	if (!linked) { song->collectError("Native preset links did not restore: " + linkedError); }
	const QPointer<Instrument> native(owner->instrument()); Instrument* const selected = native.data();
	song->setModified(); ready();
	if (native.data() != selected || owner->instrument() != selected || owner->isMutedByModel() != oldMute || owner->isSolo() != oldSolo
		|| owner->isMutedBeforeSolo() != oldBeforeSolo || owner->m_mixerChannelModel.value() != mixerChannel)
	{ throw std::runtime_error("Native preset preservation state changed during final notification"); }
}


QString InstrumentTrack::getSavedInstrumentName(const QDomElement &thisElement) const
{
	QDomElement elem = thisElement.firstChildElement("instrument");
	if (!elem.isNull())
	{
		return elem.attribute("name");
	}
	return "";
}




Instrument * InstrumentTrack::loadInstrument(const QString & _plugin_name,
	const Plugin::Descriptor::SubPluginFeatures::Key *key, bool keyFromDnd)
{
	if(keyFromDnd)
		Q_ASSERT(!key);

	const auto changes = Engine::audioEngine()->requestChangesGuard();
	if (m_retiring || isReplacingInstrument()) { return nullptr; }
	const QPointer<InstrumentTrack> owner(this); InstrumentTrack* const identity = this;
	const QString requestedName = _plugin_name;
	const std::optional<Plugin::Descriptor::SubPluginFeatures::Key> requestedKey = key ? std::make_optional(*key) : std::nullopt;
	FileResourceIO::EventSuspension events;
	m_replacingInstrument = true;
	const auto finish = qScopeGuard([owner] { if (owner && !owner->m_retiring) { owner->m_replacingInstrument = false; } });
	QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); }
	if (!owner || owner->m_retiring) { return nullptr; }
	silenceAllNotes(true);
	if (!owner || owner->m_retiring) { return nullptr; }
	// Audio ownership excludes native processing. Do not hold the track's
	// nonrecursive mutex across plugin destructors, factories or observers.
	const auto previous = owner->m_instrument; owner->m_instrument = nullptr;
	delete previous.data();
	if (!owner || owner->m_retiring || owner->m_instrument) { return nullptr; }
	const QPointer<Instrument> native(owner->createInstrument(requestedName, requestedKey ? &*requestedKey : nullptr, keyFromDnd));
	const auto cleanup = qScopeGuard([owner, native, identity] {
		if (native && native->isFromTrack(identity) && (!owner || owner->m_instrument != native)) { delete native.data(); }
	});
	if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); }
	if (!owner || owner->m_retiring || owner->m_instrument || !native || !native->isFromTrack(identity)) { return nullptr; }
	owner->m_instrument = native;
	const auto current = [&] { return owner && !owner->m_retiring && native && owner->m_instrument == native; };
	const auto name = native->displayName(); if (!current()) { return nullptr; }
	owner->setName(name); if (!current()) { return nullptr; }
	emit owner->instrumentChanged();
	return current() ? native.data() : nullptr;
}

Instrument* InstrumentTrack::createInstrument(const QString& name,
	const Plugin::Descriptor::SubPluginFeatures::Key* key, bool keyFromDnd)
{
	return Instrument::instantiate(name, this, key, keyFromDnd);
}



InstrumentTrack *InstrumentTrack::s_autoAssignedTrack = nullptr;

void InstrumentTrack::autoAssignMidiDevice(bool assign)
{
	if (assign)
	{
		if (s_autoAssignedTrack)
		{
			s_autoAssignedTrack->autoAssignMidiDevice(false);
		}
		s_autoAssignedTrack = this;
	}

	const QString &device = ConfigManager::inst()->value("midi", "midiautoassign");
	if ( Engine::audioEngine()->midiClient()->isRaw() && device != "none" )
	{
		m_midiPort.setReadable( assign );
		m_hasAutoMidiDev = assign;
		return;
	}

	// Check if the device exists
	if ( Engine::audioEngine()->midiClient()->readablePorts().indexOf(device) >= 0 )
	{
		m_midiPort.subscribeReadablePort(device, assign);
		m_hasAutoMidiDev = assign;
	}
}


} // namespace lmms

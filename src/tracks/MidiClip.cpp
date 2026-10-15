/*
 * MidiClip.cpp - implementation of class MidiClip, which holds notes
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2005-2007 Danny McRae <khjklujn/at/yahoo.com>
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

#include "MidiClip.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <QDomElement>
#include <QDebug>
#include <QScopeGuard>
#include "AudioEngine.h"

#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MidiClipView.h"
#include "PatternStore.h"
#include "PianoRoll.h"
#include "NativeNoteTransfer.h"
#include "Song.h"



namespace lmms
{

MidiClip::MidiClip( InstrumentTrack * _instrument_track ) :
	Clip( _instrument_track ),
	m_instrumentTrack( _instrument_track ),
	m_clipType( Type::BeatClip ),
	m_steps( TimePos::stepsPerBar() )
{
	if (_instrument_track->trackContainer()	== Engine::patternStore())
	{
		resizeToFirstTrack();
	}
	init();
}




MidiClip::MidiClip(const MidiClip& other) : MidiClip(other, other.m_instrumentTrack) { }

MidiClip::MidiClip(const MidiClip& other, InstrumentTrack* destination) :
	Clip(other, destination),
	m_instrumentTrack(destination),
	m_clipType( other.m_clipType ),
	m_steps( other.m_steps )
{
	std::vector<std::unique_ptr<Note>> prepared;
	prepared.reserve(other.m_notes.size());
	for (const auto& note : other.m_notes) { prepared.emplace_back(note->clone()); }
	m_notes.reserve(prepared.size());
	for (const auto& note : prepared) { m_notes.push_back(note.get()); }

	init();
	// Initializing a song clip may recompute its geometry. A copy keeps the
	// original playback window even when crossing the PatternStore boundary.
	changeLength(other.length());
	setStartTimeOffset(other.startTimeOffset());
	for (auto& note : prepared) { note.release(); }
}


MidiClip::~MidiClip()
{
	emit destroyedMidiClip( this );

	for (const auto& note : m_notes)
	{
		delete note;
	}

	m_notes.clear();
}




void MidiClip::resizeToFirstTrack()
{
	// Resize this track to be the same as existing tracks in the pattern
	const TrackContainer::TrackList & tracks =
		m_instrumentTrack->trackContainer()->tracks();
	for (const auto& track : tracks)
	{
		if (track->type() == Track::Type::Instrument)
		{
			if (track != m_instrumentTrack)
			{
				const auto& instrumentTrackClips = m_instrumentTrack->getClips();
				const auto currentClipIt = std::find(instrumentTrackClips.begin(), instrumentTrackClips.end(), this);
				unsigned int currentClip = currentClipIt != instrumentTrackClips.end() ?
					std::distance(instrumentTrackClips.begin(), currentClipIt) : -1;
				m_steps = static_cast<MidiClip*>(track->getClip(currentClip))->m_steps;
			}
			break;
		}
	}
}




void MidiClip::init()
{
	connect(Engine::getSong(), &Song::timeSignatureChanged, this, &MidiClip::changeTimeSignature);
	if (getTrack()->trackContainer() != Engine::patternStore())
	{
		saveJournallingState(false);
		updateLength();
		restoreJournallingState();
	}
}




void MidiClip::updateLength()
{
	// If the clip hasn't already been manually resized, automatically resize it.
	if (getAutoResize())
	{
		if (m_instrumentTrack->trackContainer()->type() == TrackContainer::Type::Pattern)
		{
			changeLength(beatClipLength());
			updatePatternTrack();
			return;
		}
		tick_t max_length = TimePos::ticksPerBar();

		for (const auto& note : m_notes)
		{
			if (note->length() > 0)
			{
				max_length = std::max<tick_t>(max_length, note->endPos());
			}
		}
		changeLength( TimePos( max_length ).nextFullBar() *
							TimePos::ticksPerBar() );
		setStartTimeOffset(TimePos(0));
		updatePatternTrack();
	}
}




TimePos MidiClip::beatClipLength() const
{
	tick_t max_length = TimePos::ticksPerBar();

	for (const auto& note : m_notes)
	{
		if (note->type() == Note::Type::Step)
		{
			max_length = std::max<tick_t>(max_length, note->pos() + 1);
		}
	}

	if (m_steps != TimePos::stepsPerBar())
	{
		max_length = m_steps * TimePos::ticksPerBar() / TimePos::stepsPerBar();
	}

	return TimePos{max_length}.nextFullBar() * TimePos::ticksPerBar();
}




Note * MidiClip::addNote( const Note & _new_note, const bool _quant_pos )
{
	auto new_note = _new_note.clone();
	if (_quant_pos && gui::getGUI() != nullptr && gui::getGUI()->pianoRoll())
	{
		new_note->quantizePos(gui::getGUI()->pianoRoll()->quantization());
	}

	instrumentTrack()->lock();
	m_notes.insert(std::upper_bound(m_notes.begin(), m_notes.end(), new_note, Note::lessThan), new_note);
	instrumentTrack()->unlock();

	checkType();
	updateLength();

	emit dataChanged();

	return new_note;
}




void MidiClip::appendRecordedNotes(const std::vector<Note>& notes, const std::function<void()>& checkpoint)
{
	if (notes.empty()) { return; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	std::vector<std::unique_ptr<Note>> prepared; prepared.reserve(notes.size());
	for (const auto& note : notes)
	{
		if (checkpoint && prepared.size() % 256 == 0) { checkpoint(); }
		prepared.emplace_back(note.clone());
	}
	if (checkpoint) { checkpoint(); }
	// Prepare the complete pointer vector before replacing native clip contents.
	// An allocation/copy failure cannot publish a prefix of the take.
	{
		instrumentTrack()->lock();
		const auto unlock = qScopeGuard([&] { instrumentTrack()->unlock(); });
		NoteVector combined = m_notes; combined.reserve(m_notes.size() + prepared.size());
		for (const auto& note : prepared) { combined.push_back(note.get()); }
		std::stable_sort(combined.begin(), combined.end(), Note::lessThan);
		if (checkpoint) { checkpoint(); }
		m_notes.swap(combined);
		for (auto& note : prepared) { note.release(); }
	}
	checkType(); updateLength(); emit dataChanged();
}

void MidiClip::appendPreparedNotes(std::vector<std::unique_ptr<Note>>& notes, const std::function<void()>& checkpoint, int stepCount)
{
	if (notes.empty() && stepCount < 0) { return; }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	{
		instrumentTrack()->lock(); const auto unlock = qScopeGuard([&] { instrumentTrack()->unlock(); });
		NoteVector combined = m_notes; combined.reserve(m_notes.size() + notes.size());
		for (const auto& note : notes) { combined.push_back(note.get()); }
		std::stable_sort(combined.begin(), combined.end(), Note::lessThan);
		// Allocate/sort before publishing history; checkpoint callbacks must not
		// dispatch events or mutate this clip while its native track is locked.
		checkpoint();
		if (stepCount >= 0) { m_steps = stepCount; }
		m_notes.swap(combined); for (auto& note : notes) { note.release(); }
	}
	checkType(); updateLength(); emit dataChanged();
}

NoteVector::const_iterator MidiClip::removeNote(NoteVector::const_iterator it)
{
	instrumentTrack()->lock();
	delete *it;
	auto new_it = m_notes.erase(it);
	instrumentTrack()->unlock();

	checkType();
	updateLength();

	emit dataChanged();
	return new_it;
}

NoteVector::const_iterator MidiClip::removeNote(Note* note)
{
	instrumentTrack()->lock();

	auto it = std::find(m_notes.begin(), m_notes.end(), note);
	if (it != m_notes.end())
	{
		delete *it;
		it = m_notes.erase(it);
	}

	instrumentTrack()->unlock();

	checkType();
	updateLength();

	emit dataChanged();
	return it;
}


// Returns a pointer to the note at specified step, or nullptr if note doesn't exist
Note * MidiClip::noteAtStep(int step)
{
	for (const auto& note : m_notes)
	{
		if (note->pos() == TimePos::stepPosition(step)
			&& note->type() == Note::Type::Step)
		{
			return note;
		}
	}
	return nullptr;
}



void MidiClip::rearrangeAllNotes()
{
	// sort notes by start time
	std::sort(m_notes.begin(), m_notes.end(), Note::lessThan);
}



void MidiClip::clearNotes()
{
	instrumentTrack()->lock();
	for (const auto& note : m_notes)
	{
		delete note;
	}
	m_notes.clear();
	instrumentTrack()->unlock();

	checkType();
	updateLength();
	emit dataChanged();
}




Note * MidiClip::addStepNote( int step )
{
	Note stepNote = Note(TimePos(DefaultTicksPerBar / 16), TimePos::stepPosition(step));
	stepNote.setType(Note::Type::Step);

	return addNote(stepNote, false);
}




void MidiClip::setStep( int step, bool enabled )
{
	StepSetResult result; QString error;
	if (!setStep(step, enabled, result, error)) { qWarning("Native step toggle refused: %s", qPrintable(error)); }
}



void MidiClip::reverseNotes(const NoteVector& notes)
{
	bool changed; QString error;
	if (!reverseNotes(notes, changed, error)) { qWarning("Native note reversal refused: %s", qPrintable(error)); }
}



void MidiClip::splitNotes(const NoteVector& notes, TimePos pos)
{
	NoteVector rights; QString error;
	if (!splitNotes(notes, pos, rights, error)) { qWarning("Native note split refused: %s", qPrintable(error)); }
}

bool MidiClip::splitNotes(const NoteVector& notes, TimePos pos, NoteVector& rightFragments, QString& error)
{
	return NativeNoteTransfer::split(this, notes, pos, rightFragments, error);
}

void MidiClip::splitNotesAlongLine(const NoteVector notes, TimePos pos1, int key1, TimePos pos2, int key2, bool deleteShortEnds)
{
	KnifeResult result; QString error;
	if (!splitNotesAlongLine(notes, pos1, key1, pos2, key2, deleteShortEnds, result, error)) { qWarning("Native note knife refused: %s", qPrintable(error)); }
}

bool MidiClip::splitNotesAlongLine(const NoteVector& notes, TimePos pos1, int key1, TimePos pos2, int key2, bool deleteShortEnds, KnifeResult& result, QString& error)
{
	return NativeNoteTransfer::knife(this, notes, pos1, key1, pos2, key2, deleteShortEnds, result.rights, result.positions, error);
}



void MidiClip::setType( Type _new_clip_type )
{
	if( _new_clip_type == Type::BeatClip ||
				_new_clip_type == Type::MelodyClip )
	{
		m_clipType = _new_clip_type;
	}
}




void MidiClip::checkType()
{
	// If all notes are StepNotes, we have a BeatClip
	const auto beatClip = std::all_of(m_notes.begin(), m_notes.end(), [](auto note) { return note->type() == Note::Type::Step; });

	setType(beatClip ? Type::BeatClip : Type::MelodyClip);
}


void MidiClip::exportToXML(QDomDocument& doc, QDomElement& midiClipElement, bool onlySelectedNotes)
{
	midiClipElement.setAttribute("type", static_cast<int>(m_clipType));
	midiClipElement.setAttribute("name", name());
	midiClipElement.setAttribute("autoresize", QString::number(getAutoResize()));
	midiClipElement.setAttribute("off", startTimeOffset());

	if (const auto& c = color())
	{
		midiClipElement.setAttribute("color", c->name());
	}
	// as the target of copied/dragged MIDI clip is always an existing
	// MIDI clip, we must not store actual position, instead we store -1
	// which tells loadSettings() not to mess around with position
	if (midiClipElement.parentNode().nodeName() == "clipboard" ||
			midiClipElement.parentNode().nodeName() == "dnddata")
	{
		midiClipElement.setAttribute("pos", -1);
	}
	else
	{
		midiClipElement.setAttribute("pos", startPosition());
	}
	saveControlSettings(doc, midiClipElement);
	midiClipElement.setAttribute("steps", m_steps);
	midiClipElement.setAttribute("len", length());

	// now save settings of all notes
	for (auto& note : m_notes)
	{
		if (!onlySelectedNotes || note->selected())
		{
			note->saveState(doc, midiClipElement);
		}
	}
}


void MidiClip::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	exportToXML(_doc, _this);
}




void MidiClip::loadSettings( const QDomElement & _this )
{
	m_clipType = static_cast<Type>( _this.attribute( "type"
								).toInt() );
	setName( _this.attribute( "name" ) );

	if (_this.hasAttribute("color"))
	{
		setColor(QColor{_this.attribute("color")});
	}

	if( _this.attribute( "pos" ).toInt() >= 0 )
	{
		movePosition( _this.attribute( "pos" ).toInt() );
	}
	loadControlSettings(_this);

	clearNotes();

	QDomNode node = _this.firstChild();
	while( !node.isNull() )
	{
		if( node.isElement() && node.nodeName() != "muted" && node.nodeName() != "solo"
			&& node.nodeName() != "connection" && !node.toElement().attribute( "metadata" ).toInt() )
		{
			auto n = new Note;
			n->restoreState( node.toElement() );
			m_notes.push_back( n );
		}
		node = node.nextSibling();
        }

	m_steps = _this.attribute( "steps" ).toInt();
	if( m_steps == 0 )
	{
		m_steps = TimePos::stepsPerBar();
	}

	checkType();

	int len = _this.attribute("len").toInt();
	if (len <= 0)
	{
		// TODO: Handle with an upgrade method
		updateLength();
	}
	else
	{
		changeLength(len);
	}

	setAutoResize(_this.attribute("autoresize", "1").toInt());
	setStartTimeOffset(_this.attribute("off").toInt());

	emit dataChanged();
}




MidiClip *  MidiClip::previousMidiClip() const
{
	return adjacentMidiClipByOffset(-1);
}




MidiClip *  MidiClip::nextMidiClip() const
{
	return adjacentMidiClipByOffset(1);
}




MidiClip * MidiClip::adjacentMidiClipByOffset(int offset) const
{
	auto& clips = m_instrumentTrack->getClips();
	int clipNum = m_instrumentTrack->getClipNum(this) + offset;
	if (clipNum < 0 || static_cast<size_t>(clipNum) >= clips.size()) { return nullptr; }
	return dynamic_cast<MidiClip*>(clips[clipNum]);
}




void MidiClip::clear()
{
	int removed = 0; QString error;
	if (!clearNotes(removed, error)) { qWarning("Native note clearing refused: %s", qPrintable(error)); }
}




void MidiClip::addSteps()
{
	const qint64 target = static_cast<qint64>(m_steps) + TimePos::stepsPerBar();
	StepResizeResult result; QString error;
	if (target > std::numeric_limits<int>::max()) { qWarning("Native step addition exceeds count bounds"); return; }
	if (!resizeSteps(static_cast<int>(target), result, error)) { qWarning("Native step addition refused: %s", qPrintable(error)); }
}

void MidiClip::cloneSteps()
{
	StepCloneResult result; QString error;
	if (!cloneSteps(result, error)) { qWarning("Native step clone refused: %s", qPrintable(error)); }
}




void MidiClip::removeSteps()
{
	const int steps = TimePos::stepsPerBar(); if (steps >= m_steps) { return; }
	StepResizeResult result; QString error;
	if (!resizeSteps(m_steps - steps, result, error)) { qWarning("Native step removal refused: %s", qPrintable(error)); }
}




gui::ClipView * MidiClip::createView( gui::TrackView * _tv )
{
	return new gui::MidiClipView( this, _tv );
}




void MidiClip::updatePatternTrack()
{
	if (getTrack()->trackContainer() == Engine::patternStore())
	{
		Engine::patternStore()->updatePatternTrack(this);
	}

	if (gui::getGUI() != nullptr
		&& gui::getGUI()->pianoRoll()
		&& gui::getGUI()->pianoRoll()->currentMidiClip() == this)
	{
		gui::getGUI()->pianoRoll()->update();
	}
}




bool MidiClip::empty()
{
	for (const auto& note : m_notes)
	{
		if (note->length() != 0)
		{
			return false;
		}
	}
	return true;
}




void MidiClip::changeTimeSignature()
{
	TimePos last_pos = TimePos::ticksPerBar() - 1;
	for (const auto& note : m_notes)
	{
		if (note->length() < 0 && note->pos() > last_pos)
		{
			last_pos = note->pos() + TimePos::ticksPerBar() / TimePos::stepsPerBar();
		}
	}
	last_pos = last_pos.nextFullBar() * TimePos::ticksPerBar();
	m_steps = std::max<tick_t>(TimePos::stepsPerBar(),
				last_pos.getBar() * TimePos::stepsPerBar());
	updateLength();
}


} // namespace lmms

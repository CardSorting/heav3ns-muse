/*
 * MidiClip.h - declaration of class MidiClip, which contains all information
 *              about a clip
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

#ifndef LMMS_MIDI_CLIP_H
#define LMMS_MIDI_CLIP_H

#include <functional>
#include <optional>
#include <vector>
#include "Clip.h"
#include "Note.h"


namespace lmms
{


class InstrumentTrack;

namespace gui
{
class MidiClipView;
}


class LMMS_EXPORT MidiClip : public Clip
{
	Q_OBJECT
public:
	enum class Type
	{
		BeatClip,
		MelodyClip
	} ;

	MidiClip( InstrumentTrack* instrumentTrack );
	~MidiClip() override;

	void init();

	void updateLength() override;

	// note management
	Note * addNote( const Note & _new_note, const bool _quant_pos = true );
	struct NoteSpec
	{
		int position = 0, length = 0, key = DefaultKey, volume = DefaultVolume, panning = DefaultPanning;
		Note::Type type = Note::Type::Regular;
	};
	// Construct fresh unselected native notes from scalar data. Copies with
	// existing curves/control links use NativeNoteTransfer instead.
	bool addNotes(const std::vector<NoteSpec>& notes, NoteVector& inserted, QString& error);
	// Prepare the complete batch before publication. Checkpoints must not pump
	// events or mutate native models while this method holds their locks.
	void appendRecordedNotes(const std::vector<Note>& notes, const std::function<void()>& checkpoint = {});

	NoteVector::const_iterator removeNote(NoteVector::const_iterator it);
	NoteVector::const_iterator removeNote(Note* note);

	Note * noteAtStep( int _step );

	void rearrangeAllNotes();
	void clearNotes();
	// Prepare one complete retained roster and enabled clip checkpoint before
	// removing the selection. Remaining notes retain identities/control content.
	bool removeNotes(const NoteVector& notes, int& removed, QString& error);
	bool clearNotes(int& removed, QString& error);

	struct ShiftResult { int appliedTicks = 0; bool changed = false; };
	// Shift current notes in place, preserving their identities and inline graphs.
	// Negative time shifts stop at clip start; keys use native per-note clamping.
	bool shiftNotes(const NoteVector& notes, int semitones, int ticks, ShiftResult& result, QString& error);
	struct NoteEdit { std::optional<int> position, length, key, volume, panning; };
	// Apply supplied absolute fields to each note in place. Omitted fields,
	// identities, native note types and inline control content are retained.
	bool editNotes(const NoteVector& notes, const NoteEdit& edit, bool& changed, QString& error);
	enum class QuantizeMode { Both, Positions, Lengths };
	// Uses native rounding in place; zero-length notes are skipped as in piano roll.
	bool quantizeNotes(const NoteVector& notes, int grid, QuantizeMode mode, bool& changed, QString& error);
	enum class LengthMode { Maximum, Minimum, Trim, Fill };
	bool adjustNoteLengths(const NoteVector& notes, LengthMode mode, int length, bool& changed, QString& error);
	struct GlueResult { NoteVector survivors; int removed = 0; };
	// Each input maps to its surviving note; consumed notes retire normally.
	bool glueNotes(const NoteVector& notes, GlueResult& result, QString& error);
	struct StepCloneResult { int fromCount = 0; NoteVector sources, copies; std::vector<int> steps; };
	bool cloneSteps(StepCloneResult& result, QString& error, int limit = 0);
	struct StepResizeResult { int fromCount = 0; int removed = 0; bool changed = false; };
	// Shrinking clears every Step note on removed grid cells, retaining regular
	// notes and off-grid/outside-prefix steps. A positive limit bounds the target.
	bool resizeSteps(int count, StepResizeResult& result, QString& error, int limit = 0);
	struct StepSetResult { Note* note = nullptr; int removed = 0; bool changed = false; };
	// Enable preserves an existing Step note; disable removes every Step duplicate
	// at this native cell. A positive limit bounds the current row's count.
	bool setStep(int step, bool enabled, StepSetResult& result, QString& error, int limit = 0);

	inline const NoteVector & notes() const
	{
		return m_notes;
	}

	Note * addStepNote( int step );
	void setStep( int step, bool enabled );
	int stepCount() const { return m_steps; }
	void setType(Type type);

	//! Horizontally flip the positions of the given notes.
	void reverseNotes(const NoteVector& notes);
	bool reverseNotes(const NoteVector& notes, bool& changed, QString& error);

	// Split the list of notes on the given position
	void splitNotes(const NoteVector& notes, TimePos pos);
	bool splitNotes(const NoteVector& notes, TimePos pos, NoteVector& rightFragments, QString& error);

	// Split the list of notes along a line
	void splitNotesAlongLine(const NoteVector notes, TimePos pos1, int key1, TimePos pos2, int key2, bool deleteShortEnds);
	struct KnifeResult { NoteVector rights; std::vector<int> positions; };
	bool splitNotesAlongLine(const NoteVector& notes, TimePos pos1, int key1, TimePos pos2, int key2, bool deleteShortEnds, KnifeResult& result, QString& error);

	// clip-type stuff
	inline Type type() const
	{
		return m_clipType;
	}


	// next/previous track based on position in the containing track
	MidiClip * previousMidiClip() const;
	MidiClip * nextMidiClip() const;

	// settings-management
	void exportToXML(QDomDocument& doc, QDomElement& midiClipElement, bool onlySelectedNotes = false);
	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	void loadSettings( const QDomElement & _this ) override;
	inline QString nodeName() const override
	{
		return "midiclip";
	}

	inline InstrumentTrack * instrumentTrack() const
	{
		return m_instrumentTrack;
	}

	bool empty();


	gui::ClipView * createView( gui::TrackView * _tv ) override;

	MidiClip* clone() override
	{
		return static_cast<MidiClip*>(cloneNative());
	}


	using Model::dataChanged;

public slots:
	void addSteps();
	void cloneSteps();
	void removeSteps();
	void clear();

protected:
	MidiClip( const MidiClip& other );
	MidiClip(const MidiClip& other, InstrumentTrack* destination);
	void updatePatternTrack();

protected slots:
	void changeTimeSignature();


private:
	struct SelectionEditOptions
	{
		enum class Action { Shift, Quantize, Reverse, Length, Patch };
		Action action = Action::Shift;
		int semitones = 0, ticks = 0, grid = 1;
		QuantizeMode mode = QuantizeMode::Both;
		LengthMode lengthMode = LengthMode::Maximum;
		int length = 0;
		NoteEdit edit;
	};
	bool editNoteSelection(const NoteVector& notes, const SelectionEditOptions& options, ShiftResult& result, QString& error);
	struct StepEditOptions { bool set = false, enabled = false; int value = 0, limit = 0; };
	bool editSteps(const StepEditOptions& options, StepResizeResult& result, Note*& active, QString& error);
	void appendPreparedNotes(std::vector<std::unique_ptr<Note>>& notes, const std::function<void()>& checkpoint, int stepCount = -1);
	friend class NativeNoteTransfer;
	TimePos beatClipLength() const;

	void checkType();

	void resizeToFirstTrack();

	InstrumentTrack * m_instrumentTrack;

	Type m_clipType;

	// data-stuff
	NoteVector m_notes;
	int m_steps;

	MidiClip * adjacentMidiClipByOffset(int offset) const;

	friend class gui::MidiClipView;
	friend class Clip;


signals:
	void destroyedMidiClip( lmms::MidiClip* );
} ;


} // namespace lmms

#endif // LMMS_MIDI_CLIP_H

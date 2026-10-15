/* Shared native in-place note transforms for GUI and headless callers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "MidiClip.h"
#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NativeNoteTransfer.h"
#include "Song.h"
namespace lmms
{
bool MidiClip::shiftNotes(const NoteVector& selection, int semitones, int ticks, ShiftResult& result, QString& error)
{
	SelectionEditOptions options; options.semitones = semitones; options.ticks = ticks;
	return editNoteSelection(selection, options, result, error);
}
bool MidiClip::editNotes(const NoteVector& selection, const NoteEdit& edit, bool& changed, QString& error)
{
	changed = false; SelectionEditOptions options; options.action = SelectionEditOptions::Action::Patch; options.edit = edit;
	ShiftResult result; if (!editNoteSelection(selection, options, result, error)) { return false; } changed = result.changed; return true;
}
bool MidiClip::quantizeNotes(const NoteVector& selection, int grid, QuantizeMode mode, bool& changed, QString& error)
{
	changed = false; SelectionEditOptions options; options.action = SelectionEditOptions::Action::Quantize; options.grid = grid; options.mode = mode;
	ShiftResult result; if (!editNoteSelection(selection, options, result, error)) { return false; } changed = result.changed; return true;
}
bool MidiClip::reverseNotes(const NoteVector& selection, bool& changed, QString& error)
{
	changed = false; SelectionEditOptions options; options.action = SelectionEditOptions::Action::Reverse;
	ShiftResult result; if (!editNoteSelection(selection, options, result, error)) { return false; } changed = result.changed; return true;
}
bool MidiClip::adjustNoteLengths(const NoteVector& selection, LengthMode mode, int length, bool& changed, QString& error)
{
	changed = false; SelectionEditOptions options; options.action = SelectionEditOptions::Action::Length; options.lengthMode = mode; options.length = length;
	ShiftResult result; if (!editNoteSelection(selection, options, result, error)) { return false; } changed = result.changed; return true;
}
bool MidiClip::editNoteSelection(const NoteVector& selection, const SelectionEditOptions& options, ShiftResult& result, QString& error)
{
	result = {};
	try
	{
		const bool quantize = options.action == SelectionEditOptions::Action::Quantize, reverse = options.action == SelectionEditOptions::Action::Reverse;
		const bool lengths = options.action == SelectionEditOptions::Action::Length;
		const bool patch = options.action == SelectionEditOptions::Action::Patch;
		if (patch && ((options.edit.volume && (*options.edit.volume < MinVolume || *options.edit.volume > MaxVolume))
			|| (options.edit.panning && (*options.edit.panning < PanningLeft || *options.edit.panning > PanningRight)))) { throw std::invalid_argument("Native note edit volume or panning exceeds its bounds"); }
		const bool fill = lengths && options.lengthMode == LengthMode::Fill, trim = lengths && options.lengthMode == LengthMode::Trim;
		if (lengths && ((options.lengthMode != LengthMode::Maximum && options.lengthMode != LengthMode::Minimum && !trim && !fill)
			|| options.length < -MaxSongLength || options.length > MaxSongLength || ((fill || trim) && options.length != 0)))
		{ throw std::invalid_argument("Invalid native note length mode or bound"); }
		const int bar = TimePos::ticksPerBar(); if (fill && bar < 1) { throw std::invalid_argument("Native note fill requires a positive ticks-per-bar value"); }
		if (quantize && (options.grid < 1 || options.grid > MaxSongLength || (options.mode != QuantizeMode::Both && options.mode != QuantizeMode::Positions && options.mode != QuantizeMode::Lengths)))
		{ throw std::invalid_argument("Invalid native note quantization grid or mode"); }
		// Bounded source/grid values keep native float-to-int rounding and its
		// final integer multiplication representable before endpoint admission.
		static_assert(MaxSongLength < std::numeric_limits<int>::max() / 2);
		const int semitones = options.semitones, ticks = options.ticks;
		if (!QCoreApplication::instance() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject()
			|| QThread::currentThread() != QCoreApplication::instance()->thread() || thread() != QThread::currentThread()
			|| !getTrack() || getTrack()->isNoteRecording()) { throw std::invalid_argument("Native note edit requires an idle clip on its model owner thread"); }
		const auto& clips = getTrack()->getClips(); if (std::find(clips.begin(), clips.end(), this) == clips.end()) { throw std::invalid_argument("Native note edit clip is detached"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(this);
		const NoteVector notes = selection, roster = m_notes;
		const int count = m_steps, steps = TimePos::stepsPerBar();
		if (patch)
		{
			if (count < 1 || bar < 1 || steps < 1) { throw std::invalid_argument("Native note property edit requires a positive count and meter"); }
			const qint64 product = static_cast<qint64>(count) * bar, span = product / steps;
			if (product > std::numeric_limits<int>::max() || ((span + bar - 1) / bar) * bar > MaxSongLength) { throw std::invalid_argument("Native note property edit step span exceeds its bounds"); }
		}
		struct RosterEntry { Note* note; std::shared_ptr<const void> identity; int position, key, length; volume_t volume; panning_t pan; Note::Type type; bool selected; };
		std::vector<RosterEntry> original; original.reserve(roster.size());
		for (auto note : roster) { if (!note) { throw std::invalid_argument("Native note edit roster contains a missing note"); } original.push_back({note, note->selectionIdentity(), note->pos().getTicks(), note->key(), note->length().getTicks(), note->volume(), note->panning(), note->type(), note->selected()}); }
		struct Change { Note* note; std::shared_ptr<const void> identity; int oldPosition, oldKey, oldLength, oldVolume, oldPan, position, key, length, volume, pan; };
		std::vector<Change> planned; planned.reserve(notes.size()); QSet<Note*> seen; int first = MaxSongLength; qint64 lastEnd = std::numeric_limits<qint64>::min();
		const auto check = [&] { QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } if (!receiver) { throw std::runtime_error("Native note edit clip retired during admission"); } };
		for (auto note : notes)
		{
			check(); if (!note || seen.contains(note) || std::find(receiver->notes().begin(), receiver->notes().end(), note) == receiver->notes().end()) { throw std::invalid_argument("Native note edit selection has duplicate or foreign notes"); }
			seen.insert(note); const int position = note->pos().getTicks(), length = note->length().getTicks();
			if (!patch && (position < 0 || position > MaxSongLength || length < -MaxSongLength || length > MaxSongLength || (length > 0 && length > MaxSongLength - position) || note->key() < 0 || note->key() >= NumKeys))
			{ throw std::invalid_argument("Native note edit source exceeds native time or key bounds"); }
			first = std::min(first, position); lastEnd = std::max(lastEnd, static_cast<qint64>(position) + length); planned.push_back({note, note->selectionIdentity(), position, note->key(), length, note->volume(), note->panning(), position, note->key(), length, note->volume(), note->panning()});
		}
		check(); if (notes.empty()) { error.clear(); return true; }
		std::vector<int> references;
		if (fill || trim)
		{
			references.reserve(original.size());
			for (const auto& item : original) { if (item.position < 0 || item.position > MaxSongLength) { throw std::invalid_argument("Native length reference note position exceeds the song range"); } references.push_back(item.position); }
			std::sort(references.begin(), references.end());
		}
		const qint64 tail = fill ? ((lastEnd + bar - 1) / bar) * bar : 0;
		const int applied = quantize || reverse || lengths || patch ? 0 : static_cast<int>(std::max<qint64>(ticks, -static_cast<qint64>(first))); bool changed = false; QHash<const Note*, int> positions, keys, durations;
		for (auto& item : planned)
		{
			if (patch)
			{
				item.position = options.edit.position.value_or(item.oldPosition); item.length = options.edit.length.value_or(item.oldLength); item.key = options.edit.key.value_or(item.oldKey);
				item.volume = options.edit.volume.value_or(item.oldVolume); item.pan = options.edit.panning.value_or(item.oldPan);
			}
			else if (lengths)
			{
				qint64 candidate = options.length;
				if (fill || trim)
				{
					const auto next = fill ? std::lower_bound(references.begin(), references.end(), static_cast<qint64>(item.oldPosition) + item.oldLength)
						: std::upper_bound(references.begin(), references.end(), item.oldPosition);
					if (next == references.end() && trim) { candidate = item.oldLength; }
					else { candidate = (next == references.end() ? tail : *next) - item.oldPosition; }
				}
				if ((options.lengthMode == LengthMode::Minimum || fill) ? item.oldLength < candidate : item.oldLength > candidate)
				{
					if (candidate < -MaxSongLength || candidate > MaxSongLength) { throw std::invalid_argument("Edited native note length exceeds the song range"); }
					item.length = static_cast<int>(candidate);
				}
			}
			else if (quantize)
			{
				// Match the piano-roll convention: zero-length notes are skipped.
				// Note::quantized retains native half-up/negative-length behavior.
				if (item.oldLength != 0)
				{
					if (options.mode != QuantizeMode::Lengths) { item.position = Note::quantized(item.oldPosition, options.grid).getTicks(); }
					if (options.mode != QuantizeMode::Positions) { item.length = Note::quantized(item.oldLength, options.grid).getTicks(); if (item.length == 0) { item.length = options.grid; } }
				}
			}
			else if (reverse)
			{
				const qint64 position = lastEnd - (static_cast<qint64>(item.oldPosition) - first) - item.oldLength;
				if (position < 0 || position > MaxSongLength) { throw std::invalid_argument("Reversed native note position exceeds the song range"); }
				item.position = static_cast<int>(position);
			}
			else
			{
				const qint64 position = static_cast<qint64>(item.oldPosition) + applied;
				if (position > MaxSongLength) { throw std::invalid_argument("Edited native note position exceeds the song range"); }
				item.position = static_cast<int>(position); item.key = static_cast<int>(std::clamp<qint64>(static_cast<qint64>(item.oldKey) + semitones, 0, NumKeys - 1));
			}
			if (item.position < 0 || item.position > MaxSongLength || item.length < -MaxSongLength || item.length > MaxSongLength || (item.length > 0 && item.length > MaxSongLength - item.position) || item.key < 0 || item.key >= NumKeys)
			{ throw std::invalid_argument("Edited native note endpoint exceeds the song range"); }
			changed |= item.position != item.oldPosition || item.key != item.oldKey || item.length != item.oldLength || item.volume != item.oldVolume || item.pan != item.oldPan;
			positions.insert(item.note, item.position); keys.insert(item.note, item.key); durations.insert(item.note, item.length);
		}
		if (patch)
		{
			qint64 noteSpan = bar, stepSpan = bar;
			for (const auto& item : original)
			{
				const int position = positions.value(item.note, item.position), length = durations.value(item.note, item.length);
				if (position < 0 || position > MaxSongLength || length < -MaxSongLength || length > MaxSongLength || (length > 0 && length > MaxSongLength - position)) { throw std::invalid_argument("Native property edit retained endpoint exceeds the song range"); }
				if (length > 0) { noteSpan = std::max(noteSpan, static_cast<qint64>(position) + length); } if (item.type == Note::Type::Step) { stepSpan = std::max(stepSpan, static_cast<qint64>(position) + 1); }
			}
			if (((noteSpan + bar - 1) / bar) * bar > MaxSongLength || (count == steps && ((stepSpan + bar - 1) / bar) * bar > MaxSongLength)) { throw std::invalid_argument("Native property edit rounded length exceeds the song range"); }
		}
		const auto validate = [&] {
			if (!receiver || receiver->notes() != roster) { throw std::runtime_error("Native note edit roster changed during admission"); }
			if (fill && TimePos::ticksPerBar() != bar) { throw std::runtime_error("Native note fill meter changed during admission"); }
			if (receiver->getTrack()->isNoteRecording() || (patch && (receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps))) { throw std::runtime_error("Native note edit recording, count or meter changed during admission"); }
			for (const auto& item : original)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->key() != item.key || item.note->length().getTicks() != item.length || item.note->volume() != item.volume || item.note->panning() != item.pan || item.note->type() != item.type || item.note->selected() != item.selected) { throw std::runtime_error("Native note edit roster identity or scalar state changed during admission"); } }
			for (const auto& item : planned)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.oldPosition || item.note->key() != item.oldKey || item.note->length().getTicks() != item.oldLength) { throw std::runtime_error("Native note edit selection changed during admission"); } }
		};
		check(); validate(); if (!changed) { result.appliedTicks = applied; error.clear(); return true; }
		NoteVector sorted = roster;
		std::stable_sort(sorted.begin(), sorted.end(), [&](const Note* a, const Note* b) {
			const int first = positions.value(a, a->pos().getTicks()), second = positions.value(b, b->pos().getTicks());
			return first != second ? first < second : keys.value(a, a->key()) > keys.value(b, b->key());
		});
		check(); validate();
		if (!NativeNoteTransfer::checkpoint(receiver.data(), error)) { throw std::runtime_error(error.toStdString()); }
		validate();
		{
			auto track = receiver->instrumentTrack(); track->lock(); const auto unlock = qScopeGuard([&] { track->unlock(); });
			for (const auto& item : planned)
			{
				item.note->setKey(item.key); item.note->setPos(item.position); item.note->setLength(item.length);
				if (patch && item.volume != item.oldVolume) { item.note->setVolume(static_cast<volume_t>(item.volume)); }
				if (patch && item.pan != item.oldPan) { item.note->setPanning(static_cast<panning_t>(item.pan)); }
			}
			receiver->m_notes.swap(sorted);
		}
		receiver->updateLength(); if (!receiver) { throw std::runtime_error("Native note edit clip retired during length notification"); }
		emit receiver->dataChanged(); Engine::getSong()->setModified();
		if (!receiver) { throw std::runtime_error("Native note edit clip retired during notification"); }
		QSet<const Note*> current; for (auto note : receiver->notes()) { current.insert(note); }
		for (const auto& item : planned)
		{
			if (!current.contains(item.note) || item.note->selectionIdentity() != item.identity) { throw std::runtime_error("Edited native note retired or was replaced during notification"); }
			if (patch && (item.note->pos().getTicks() != item.position || item.note->key() != item.key || item.note->length().getTicks() != item.length || item.note->volume() != item.volume || item.note->panning() != item.pan)) { throw std::runtime_error("Native property edit fields changed during notification"); }
		}
		if (patch)
		{
			if (receiver->notes().size() != roster.size() || receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps) { throw std::runtime_error("Native property edit result changed during notification"); }
			for (const auto& item : original) { if (!current.contains(item.note) || item.note->selectionIdentity() != item.identity) { throw std::runtime_error("Native property edit roster note retired during notification"); } }
		}
		result = {applied, true}; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native note edit failed with a nonstandard exception"; }
	return false;
}
}

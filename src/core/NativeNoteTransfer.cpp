/* Native selected notes and their closed inline control graphs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "NativeNoteTransfer.h"
#include <QCoreApplication>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <QHash>
#include <QMap>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include "AudioEngine.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "NativeLinkedControls.h"
#include "ProjectJournal.h"
#include "Song.h"
namespace lmms
{
namespace
{
void owner(MidiClip* clip)
{
	if (!QCoreApplication::instance() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject()
		|| QThread::currentThread() != QCoreApplication::instance()->thread() || !clip || clip->thread() != QThread::currentThread()
		|| !clip->getTrack() || clip->getTrack()->isNoteRecording()) { throw std::invalid_argument("Native note transfer requires an idle clip on its model owner thread"); }
	const auto& clips = clip->getTrack()->getClips(); if (std::find(clips.begin(), clips.end(), clip) == clips.end()) { throw std::invalid_argument("Native note transfer clip is detached"); }
}
void check() { QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } }
QList<AutomatableModel*> models(const NoteVector& notes)
{
	QList<AutomatableModel*> result; QSet<AutomatableModel*> seen;
	for (auto note : notes) { for (auto model : NativeLinkedControls::noteModels(note)) { if (!seen.contains(model)) { seen.insert(model); result.append(model); } } } return result;
}
void connections(const NoteVector& notes)
{
	for (auto model : models(notes))
	{
		const auto connection = model->controllerConnection(); if (!connection) { continue; } const auto controller = connection->getController(); const auto& rack = Engine::getSong()->controllers();
		if (!controller || controller->type() == Controller::ControllerType::Dummy || (controller->type() != Controller::ControllerType::Midi && std::find(rack.begin(), rack.end(), controller) == rack.end())) { throw std::invalid_argument("Native note transfer has an unresolved controller binding"); }
	}
}
int integer(const QDomElement& element, const QString& key, int minimum, int maximum, const QString& fallback = {})
{
	bool valid = false; const auto value = element.attribute(key, fallback).toInt(&valid);
	if (!valid || value < minimum || value > maximum) { throw std::invalid_argument(("Invalid native note attribute: " + key).toStdString()); } return value;
}
}
bool NativeNoteTransfer::save(MidiClip* source, const NoteVector& notes, TimePos origin, DataFile& document, QString& error) { return capture(source, notes, origin, document, error, false); }
bool NativeNoteTransfer::copy(MidiClip* source, const NoteVector& notes, TimePos origin, MidiClip* target, TimePos position, NoteVector& inserted, QString& error)
{
	const NoteVector selected = notes; const QPointer<MidiClip> sender(source); DataFile document(DataFile::Type::ClipboardData);
	if (!capture(source, selected, origin, document, error, true)) { return false; }
	return restore(document, target, position, inserted, error, sender.data(), &selected);
}
bool NativeNoteTransfer::load(const DataFile& document, MidiClip* target, TimePos position, NoteVector& inserted, QString& error) { return restore(document, target, position, inserted, error, nullptr, nullptr); }

bool NativeNoteTransfer::split(MidiClip* source, const NoteVector& selection, TimePos position, NoteVector& rightFragments, QString& error)
{
	CutOptions options; options.from = position; std::vector<int> positions;
	return cut(source, selection, options, rightFragments, positions, error);
}

bool NativeNoteTransfer::knife(MidiClip* source, const NoteVector& selection, TimePos from, int fromKey, TimePos to, int toKey, bool keepLongest,
	NoteVector& rightFragments, std::vector<int>& cutPositions, QString& error)
{
	CutOptions options; options.from = from; options.to = to; options.fromKey = fromKey; options.toKey = toKey; options.line = true; options.keepLongest = keepLongest;
	return cut(source, selection, options, rightFragments, cutPositions, error);
}

bool NativeNoteTransfer::cut(MidiClip* source, const NoteVector& selection, const CutOptions& options, NoteVector& rightFragments, std::vector<int>& cutPositions, QString& error)
{
	try
	{
		const NoteVector notes = selection; rightFragments.clear(); cutPositions.clear(); owner(source);
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(source);
		if (!options.line && (options.from.getTicks() < 0 || options.from.getTicks() > MaxSongLength)) { throw std::invalid_argument("Native note split position exceeds the song range"); }
		const bool horizontal = options.line && options.fromKey == options.toKey;
		const float slope = options.line && !horizontal ? static_cast<float>(static_cast<qint64>(options.to.getTicks()) - options.from.getTicks())
			/ static_cast<float>(static_cast<qint64>(options.toKey) - options.fromKey) : 0.f;
		const auto [minimumKey, maximumKey] = std::minmax(options.fromKey, options.toKey);
		const NoteVector roster = source->notes();
		struct State { Note* note; std::shared_ptr<const void> identity; int position; int length; int key; bool selected; };
		std::vector<State> original; original.reserve(roster.size());
		for (auto note : roster) { if (!note) { throw std::invalid_argument("Native split roster contains a missing note"); } original.push_back({note, note->selectionIdentity(), note->pos().getTicks(), note->length().getTicks(), note->key(), note->selected()}); }
		QHash<const Note*, const State*> states; for (const auto& item : original) { states.insert(item.note, &item); }
		const auto validate = [&] {
			if (!receiver || receiver->notes() != roster) { throw std::runtime_error("Native split roster changed during admission"); }
			for (const auto& item : original)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->length().getTicks() != item.length || item.note->key() != item.key || item.note->selected() != item.selected) { throw std::runtime_error("Native split note state changed during admission"); } }
		};
		QSet<Note*> seen; NoteVector crossing; std::vector<std::size_t> inputs; std::vector<int> leftLengths, rightLengths, leftPositions, crossings;
		std::vector<int> readyPositions(notes.size(), -1);
		for (std::size_t index = 0; index < notes.size(); ++index)
		{
			check(); if (!receiver) { throw std::runtime_error("Native split clip retired during admission"); } auto note = notes[index];
			if (!note || seen.contains(note) || !states.contains(note) || std::find(receiver->notes().begin(), receiver->notes().end(), note) == receiver->notes().end()) { throw std::invalid_argument("Native split selection has duplicate or foreign notes"); } seen.insert(note);
			const auto snapshot = states.value(note); const int start = snapshot->position, length = snapshot->length, key = snapshot->key;
			if (start < 0 || start > MaxSongLength || length < -MaxSongLength || length > MaxSongLength || (length > 0 && length > MaxSongLength - start) || key < 0 || key >= NumKeys)
			{ throw std::invalid_argument("Native split source exceeds native time or key bounds"); }
			int position = options.from.getTicks();
			if (options.line)
			{
				if (horizontal || key <= minimumKey || key > maximumKey) { continue; }
				const double intercept = slope * (key - 0.5 - options.fromKey) + options.from.getTicks();
				// Match native truncation after proving conversion representable.
				if (!std::isfinite(intercept) || intercept < std::numeric_limits<int>::min() || intercept > std::numeric_limits<int>::max()) { continue; }
				position = static_cast<int>(intercept);
			}
			const qint64 left = static_cast<qint64>(position) - start, right = static_cast<qint64>(length) - left;
			if (left > 0 && right > 0)
			{
				crossing.push_back(note); inputs.push_back(index); crossings.push_back(position); readyPositions[index] = position;
				const bool keepRight = options.keepLongest && right > left;
				leftLengths.push_back(static_cast<int>(keepRight ? right : left)); rightLengths.push_back(static_cast<int>(right)); leftPositions.push_back(keepRight ? position : start);
			}
		}
		check(); validate(); NoteVector ready(notes.size(), nullptr);
		if (crossing.empty()) { rightFragments.swap(ready); cutPositions.swap(readyPositions); error.clear(); return true; }
		std::vector<std::unique_ptr<Note>> prepared; prepared.reserve(crossing.size()); NoteVector copies; copies.reserve(crossing.size()); std::vector<std::shared_ptr<const void>> rightIdentities; rightIdentities.reserve(crossing.size());
		if (!options.keepLongest)
		{
			DataFile document(DataFile::Type::ClipboardData);
			if (!capture(receiver.data(), crossing, 0, document, error, true)) { throw std::runtime_error(error.toStdString()); } validate();
			JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope links(document);
			auto state = document.content().firstChildElement("note-list").firstChildElement("note");
			for (std::size_t index = 0; index < crossing.size(); ++index)
			{
				check(); validate(); if (state.isNull()) { throw std::runtime_error("Native split control roster changed during preparation"); }
				prepared.emplace_back(crossing[index]->clone());
				if (prepared.back()->detuning()) { prepared.back()->detuning()->loadControlSettings(state); }
				prepared.back()->setLength(rightLengths[index]); prepared.back()->setPos(crossings[index]); copies.push_back(prepared.back().get()); rightIdentities.push_back(prepared.back()->selectionIdentity()); ready[inputs[index]] = prepared.back().get(); state = state.nextSiblingElement("note");
			}
			connections(copies); if (!links.finish(document.content(), error)) { throw std::runtime_error(error.toStdString()); }
		}
		check(); validate(); NoteVector combined = roster;
		if (prepared.size() > combined.max_size() - roster.size()) { throw std::length_error("Native split roster exceeds container capacity"); } combined.reserve(roster.size() + prepared.size());
		QHash<const Note*, int> plannedPositions; for (std::size_t index = 0; index < crossing.size(); ++index) { plannedPositions.insert(crossing[index], leftPositions[index]); }
		for (const auto& note : prepared) { combined.push_back(note.get()); }
		std::stable_sort(combined.begin(), combined.end(), [&](const Note* a, const Note* b) {
			const int first = plannedPositions.value(a, a->pos().getTicks()), second = plannedPositions.value(b, b->pos().getTicks());
			return first != second ? first < second : a->key() > b->key();
		});
		check(); validate(); if (!checkpoint(receiver.data(), error)) { throw std::runtime_error(error.toStdString()); } validate();
		{
			auto track = receiver->instrumentTrack(); track->lock(); const auto unlock = qScopeGuard([&] { track->unlock(); });
			for (std::size_t index = 0; index < crossing.size(); ++index) { crossing[index]->setLength(leftLengths[index]); crossing[index]->setPos(leftPositions[index]); }
			receiver->m_notes.swap(combined); for (auto& note : prepared) { note.release(); }
		}
		receiver->checkType(); receiver->updateLength(); if (!receiver) { throw std::runtime_error("Native split clip retired during length notification"); }
		emit receiver->dataChanged(); Engine::getSong()->setModified(); if (!receiver) { throw std::runtime_error("Native split clip retired during notification"); }
		QSet<const Note*> live; for (auto note : receiver->notes()) { live.insert(note); }
		for (const auto& item : original)
		{ if (seen.contains(item.note) && (!live.contains(item.note) || item.note->selectionIdentity() != item.identity)) { throw std::runtime_error("Native split source retired or was replaced during notification"); } }
		for (std::size_t index = 0; index < copies.size(); ++index)
		{ if (!live.contains(copies[index]) || copies[index]->selectionIdentity() != rightIdentities[index]) { throw std::runtime_error("Native split fragment retired or was replaced during notification"); } }
		rightFragments.swap(ready); cutPositions.swap(readyPositions); error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native note split failed with a nonstandard exception"; }
	rightFragments.clear(); cutPositions.clear(); return false;
}

bool NativeNoteTransfer::capture(MidiClip* source, const NoteVector& selection, TimePos origin, DataFile& document, QString& error, bool controlsOnly)
{
	try
	{
		owner(source); const NoteVector notes = selection; const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> sender(source);
		if (notes.empty() || origin.getTicks() < 0 || origin.getTicks() > MaxSongLength || document.type() != DataFile::Type::ClipboardData || !document.content().firstChildElement().isNull() || !document.serializationError().isEmpty()) { throw std::invalid_argument("Native note transfer requires a nonempty selection and empty clipboard document"); }
		QSet<const Note*> seen; std::vector<std::shared_ptr<const void>> identities;
		for (auto note : notes)
		{
			if (!note || seen.contains(note) || std::find(source->notes().begin(), source->notes().end(), note) == source->notes().end() || note->pos().getTicks() < origin.getTicks()) { throw std::invalid_argument("Native note selection has duplicate, foreign or earlier-than-origin notes"); }
			const auto position = note->pos().getTicks(), length = note->length().getTicks();
			if (position > MaxSongLength || length < -MaxSongLength || length > MaxSongLength || (length > 0 && length > MaxSongLength - position)
				|| note->key() < 0 || note->key() >= NumKeys || note->volume() > MaxVolume || note->panning() < PanningLeft || note->panning() > PanningRight
				|| (note->type() != Note::Type::Regular && note->type() != Note::Type::Step)) { throw std::invalid_argument("Native note selection contains state outside supported transfer bounds"); }
			seen.insert(note); identities.push_back(note->selectionIdentity());
		}
		check(); if (!sender) { throw std::runtime_error("Native note source retired during admission"); }
		for (std::size_t index = 0; index < notes.size(); ++index) { if (std::find(sender->notes().begin(), sender->notes().end(), notes[index]) == sender->notes().end() || notes[index]->selectionIdentity() != identities[index]) { throw std::runtime_error("Native note source selection changed during admission"); } }
		connections(notes); NativeLinkedControls::SaveScope links(document, models(notes)); auto list = document.createElement("note-list"); list.setAttribute("version", 1); document.content().appendChild(list);
		for (auto note : notes)
		{
			if (controlsOnly)
			{
				auto state = document.createElement("note"); list.appendChild(state); state.setAttribute("key", note->key()); state.setAttribute("vol", note->volume()); state.setAttribute("pan", note->panning()); state.setAttribute("len", note->length()); state.setAttribute("pos", note->pos().getTicks() - origin.getTicks()); state.setAttribute("type", static_cast<int>(note->type())); if (note->detuning()) { note->detuning()->saveControlSettings(document, state); }
			}
			else { Note relative(*note); relative.setPos(note->pos().getTicks() - origin.getTicks()); relative.saveState(document, list); }
		}
		if (!links.finish(document.content(), error) || !document.serializationError().isEmpty()) { throw std::runtime_error(error.isEmpty() ? document.serializationError().toStdString() : error.toStdString()); }
		if (!sender) { throw std::runtime_error("Native note source retired during serialization"); }
		for (std::size_t index = 0; index < notes.size(); ++index) { if (std::find(sender->notes().begin(), sender->notes().end(), notes[index]) == sender->notes().end() || notes[index]->selectionIdentity() != identities[index]) { throw std::runtime_error("Native note source selection changed during serialization"); } }
		error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); } catch (...) { error = "Native note serialization failed with a nonstandard exception"; }
	DataFile::recordSerializationError(document, error); return false;
}

bool NativeNoteTransfer::checkpoint(MidiClip* clip, QString& error)
{
	try
	{
		owner(clip); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(clip);
		const auto journal = Engine::projectJournal(); if (!journal->isJournalling() || !clip->isJournalling()) { error.clear(); return true; }
		DataFile document(DataFile::Type::JournalData); const auto state = clip->saveJournalState(document, document.content());
		if (!receiver || state.isNull() || !document.serializationError().isEmpty() || !journal->addPreparedJournalCheckPoint(receiver.data(), document)) { error = document.serializationError().isEmpty() ? "Native note edit could not publish its complete clip checkpoint" : document.serializationError(); return false; } error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); } catch (...) { error = "Native note checkpoint failed with a nonstandard exception"; } return false;
}

bool MidiClip::addNotes(const std::vector<NoteSpec>& input, NoteVector& inserted, QString& error)
{
	inserted.clear();
	try
	{
		owner(this); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(this);
		const auto specs = input; const NoteVector roster = m_notes; const int count = m_steps, bar = TimePos::ticksPerBar(), steps = TimePos::stepsPerBar();
		if (roster.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || specs.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) - roster.size()) { throw std::invalid_argument("Native note insertion roster exceeds result count range"); }
		struct State { Note* note; std::shared_ptr<const void> identity; int position, length, key; volume_t volume; panning_t pan; Note::Type type; bool selected; };
		std::vector<State> original; original.reserve(roster.size()); QSet<Note*> seen;
		for (auto note : roster) { if (!note || seen.contains(note)) { throw std::invalid_argument("Native note insertion roster contains missing or duplicate ownership"); } seen.insert(note); original.push_back({note, note->selectionIdentity(), note->pos().getTicks(), note->length().getTicks(), note->key(), note->volume(), note->panning(), note->type(), note->selected()}); }
		const auto validate = [&] {
			if (!receiver || receiver->getTrack()->isNoteRecording() || receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps || receiver->notes() != roster) { throw std::runtime_error("Native note insertion roster, count or meter changed during admission"); }
			for (const auto& item : original)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->length().getTicks() != item.length || item.note->key() != item.key || item.note->volume() != item.volume || item.note->panning() != item.pan || item.note->type() != item.type || item.note->selected() != item.selected) { throw std::runtime_error("Native note insertion original scalar state changed during admission"); } }
		};
		check(); validate(); if (specs.empty()) { error.clear(); return true; }
		if (count < 1 || bar < 1 || steps < 1) { throw std::invalid_argument("Native note insertion count or meter exceeds its bounds"); }
		const qint64 product = static_cast<qint64>(count) * bar, span = product / steps;
		if (product > std::numeric_limits<int>::max() || ((span + bar - 1) / bar) * bar > MaxSongLength) { throw std::invalid_argument("Native note insertion step span exceeds arithmetic or song bounds"); }
		qint64 noteSpan = bar, stepSpan = bar;
		const auto admit = [&](int position, int length, Note::Type type) {
			if (position < 0 || position > MaxSongLength || length < -MaxSongLength || length > MaxSongLength || (length > 0 && length > MaxSongLength - position)) { throw std::invalid_argument("Native note insertion endpoint exceeds the song range"); }
			if (length > 0) { noteSpan = std::max(noteSpan, static_cast<qint64>(position) + length); } if (type == Note::Type::Step) { stepSpan = std::max(stepSpan, static_cast<qint64>(position) + 1); }
		};
		for (const auto& item : original) { admit(item.position, item.length, item.type); }
		for (const auto& item : specs)
		{
			if (item.key < 0 || item.key >= NumKeys || item.volume < MinVolume || item.volume > MaxVolume || item.panning < PanningLeft || item.panning > PanningRight || (item.type != Note::Type::Regular && item.type != Note::Type::Step)) { throw std::invalid_argument("Native new note scalar value or type exceeds its bounds"); }
			admit(item.position, item.length, item.type);
		}
		if (((noteSpan + bar - 1) / bar) * bar > MaxSongLength || (count == steps && ((stepSpan + bar - 1) / bar) * bar > MaxSongLength)) { throw std::invalid_argument("Native note insertion rounded length exceeds the song range"); }
		std::vector<std::unique_ptr<Note>> prepared; prepared.reserve(specs.size()); NoteVector ready; ready.reserve(specs.size()); std::vector<std::shared_ptr<const void>> identities; identities.reserve(specs.size());
		for (const auto& item : specs)
		{
			check(); validate(); prepared.emplace_back(std::make_unique<Note>(TimePos(item.length), TimePos(item.position), item.key, static_cast<volume_t>(item.volume), static_cast<panning_t>(item.panning)));
			prepared.back()->setType(item.type); ready.push_back(prepared.back().get()); identities.push_back(prepared.back()->selectionIdentity());
		}
		check(); validate(); receiver->appendPreparedNotes(prepared, [&] { validate(); if (!NativeNoteTransfer::checkpoint(receiver.data(), error)) { throw std::runtime_error(error.toStdString()); } validate(); });
		Engine::getSong()->setModified();
		if (!receiver || receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps || receiver->notes().size() != roster.size() + specs.size()) { throw std::runtime_error("Native note insertion result changed during notification"); }
		QSet<const Note*> live; for (auto note : receiver->notes()) { live.insert(note); }
		for (const auto& item : original)
		{ if (!live.contains(item.note) || item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->length().getTicks() != item.length || item.note->key() != item.key || item.note->volume() != item.volume || item.note->panning() != item.pan || item.note->type() != item.type || item.note->selected() != item.selected) { throw std::runtime_error("Native note insertion original changed during notification"); } }
		for (std::size_t index = 0; index < ready.size(); ++index)
		{
			auto note = ready[index]; const auto& item = specs[index];
			if (!live.contains(note) || note->selectionIdentity() != identities[index] || note->pos().getTicks() != item.position || note->length().getTicks() != item.length || note->key() != item.key || note->volume() != item.volume || note->panning() != item.panning || note->type() != item.type) { throw std::runtime_error("Native inserted note retired or changed during notification"); }
		}
		inserted.swap(ready); error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native note insertion failed with a nonstandard exception"; }
	return false;
}

bool MidiClip::clearNotes(int& removed, QString& error) { return removeNotes(m_notes, removed, error); }

bool MidiClip::removeNotes(const NoteVector& selection, int& removed, QString& error)
{
	removed = 0;
	try
	{
		owner(this); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(this);
		const NoteVector notes = selection, roster = m_notes; const int count = m_steps, bar = TimePos::ticksPerBar(), steps = TimePos::stepsPerBar();
		if (roster.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { throw std::invalid_argument("Native note removal roster exceeds result count range"); }
		struct State { Note* note; std::shared_ptr<const void> identity; int position, length, key; volume_t volume; panning_t pan; Note::Type type; bool selected; };
		std::vector<State> original; original.reserve(roster.size()); QSet<Note*> live, retiring;
		for (auto note : roster) { if (!note || live.contains(note)) { throw std::invalid_argument("Native note removal roster contains missing or duplicate ownership"); } live.insert(note); original.push_back({note, note->selectionIdentity(), note->pos().getTicks(), note->length().getTicks(), note->key(), note->volume(), note->panning(), note->type(), note->selected()}); }
		for (auto note : notes) { if (!note || !live.contains(note) || retiring.contains(note)) { throw std::invalid_argument("Native note removal selection has duplicate or foreign notes"); } retiring.insert(note); }
		const auto validate = [&] {
			if (!receiver || receiver->getTrack()->isNoteRecording() || receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps || receiver->notes() != roster) { throw std::runtime_error("Native note removal roster, count or meter changed during admission"); }
			for (const auto& item : original)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->length().getTicks() != item.length || item.note->key() != item.key || item.note->volume() != item.volume || item.note->panning() != item.pan || item.note->type() != item.type || item.note->selected() != item.selected) { throw std::runtime_error("Native note removal scalar state changed during admission"); } }
		};
		check(); validate(); if (notes.empty()) { error.clear(); return true; }
		if (count < 1 || bar < 1 || steps < 1) { throw std::invalid_argument("Native note removal count or meter exceeds its bounds"); }
		const qint64 product = static_cast<qint64>(count) * bar, span = product / steps;
		if (product > std::numeric_limits<int>::max() || ((span + bar - 1) / bar) * bar > MaxSongLength) { throw std::invalid_argument("Native note removal step span exceeds arithmetic or song bounds"); }
		NoteVector retained; retained.reserve(roster.size() - notes.size()); std::vector<std::unique_ptr<Note>> owned; owned.reserve(notes.size()); qint64 noteSpan = bar, stepSpan = bar;
		for (const auto& item : original)
		{
			if (retiring.contains(item.note)) { continue; }
			if (item.position < 0 || item.position > MaxSongLength || item.length < -MaxSongLength || item.length > MaxSongLength || (item.length > 0 && item.length > MaxSongLength - item.position)) { throw std::invalid_argument("Retained native note exceeds the song range"); }
			retained.push_back(item.note); if (item.length > 0) { noteSpan = std::max(noteSpan, static_cast<qint64>(item.position) + item.length); }
			if (item.type == Note::Type::Step) { stepSpan = std::max(stepSpan, static_cast<qint64>(item.position) + 1); }
		}
		if (((noteSpan + bar - 1) / bar) * bar > MaxSongLength || (count == steps && ((stepSpan + bar - 1) / bar) * bar > MaxSongLength)) { throw std::invalid_argument("Retained native note rounded length exceeds the song range"); }
		check(); validate(); if (!NativeNoteTransfer::checkpoint(receiver.data(), error)) { throw std::runtime_error(error.toStdString()); } validate();
		{
			auto track = receiver->instrumentTrack(); track->lock(); const auto unlock = qScopeGuard([&] { track->unlock(); });
			receiver->m_notes.swap(retained); for (auto note : notes) { owned.emplace_back(note); }
		}
		for (auto& note : owned) { note.reset(); if (!receiver) { throw std::runtime_error("Native note removal clip retired during cleanup"); } }
		receiver->checkType(); receiver->updateLength(); if (!receiver) { throw std::runtime_error("Native note removal clip retired during length notification"); }
		emit receiver->dataChanged(); Engine::getSong()->setModified();
		if (!receiver || receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps || receiver->notes().size() != roster.size() - notes.size()) { throw std::runtime_error("Native note removal result changed during notification"); }
		live.clear(); for (auto note : receiver->notes()) { live.insert(note); }
		for (const auto& item : original)
		{ if (!retiring.contains(item.note) && (!live.contains(item.note) || item.note->selectionIdentity() != item.identity)) { throw std::runtime_error("Native note removal survivor retired or was replaced during notification"); } }
		removed = static_cast<int>(notes.size()); error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native note removal failed with a nonstandard exception"; }
	return false;
}

bool MidiClip::resizeSteps(int target, StepResizeResult& result, QString& error, int limit)
{
	StepEditOptions options; options.value = target; options.limit = limit; Note* active = nullptr;
	return editSteps(options, result, active, error);
}

bool MidiClip::setStep(int step, bool enabled, StepSetResult& result, QString& error, int limit)
{
	result = {}; StepEditOptions options; options.set = true; options.enabled = enabled; options.value = step; options.limit = limit;
	StepResizeResult resized; Note* active = nullptr; if (!editSteps(options, resized, active, error)) { return false; }
	result.note = active; result.removed = resized.removed; result.changed = resized.changed; return true;
}

bool MidiClip::editSteps(const StepEditOptions& options, StepResizeResult& result, Note*& active, QString& error)
{
	result = {}; active = nullptr;
	try
	{
		owner(this); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(this);
		const int count = m_steps, bar = TimePos::ticksPerBar(), steps = TimePos::stepsPerBar();
		const int target = options.set ? count : options.value, limit = options.limit;
		if (limit < 0 || count < 1 || target < 1 || bar < 1 || steps < 1 || (limit && target > limit)) { throw std::invalid_argument("Native step edit count or meter exceeds its bounds"); }
		if (options.set && (options.value < 0 || options.value >= count)) { throw std::invalid_argument("Native step edit cell is outside its current row"); }
		for (const int size : {count, target})
		{
			const qint64 product = static_cast<qint64>(size) * bar, span = product / steps, rounded = ((span + bar - 1) / bar) * bar;
			if (product > std::numeric_limits<int>::max() || span > MaxSongLength || rounded > MaxSongLength) { throw std::invalid_argument("Edited native step span exceeds native arithmetic or song bounds"); }
		}
		const NoteVector roster = m_notes;
		if (roster.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { throw std::invalid_argument("Native step edit roster exceeds result count range"); }
		struct State { Note* note; std::shared_ptr<const void> identity; int position, length, key; volume_t volume; panning_t pan; Note::Type type; bool selected; };
		std::vector<State> original; original.reserve(roster.size()); NoteVector retained, retiring; retained.reserve(roster.size()); retiring.reserve(roster.size());
		qint64 noteSpan = bar, stepSpan = bar; const int position = options.set ? static_cast<int>(static_cast<qint64>(options.value) * bar / steps) : 0; Note* existing = nullptr;
		for (auto note : roster)
		{
			if (!note) { throw std::invalid_argument("Native step edit roster contains a missing note"); }
			original.push_back({note, note->selectionIdentity(), note->pos().getTicks(), note->length().getTicks(), note->key(), note->volume(), note->panning(), note->type(), note->selected()});
			const auto& item = original.back();
			if (item.position < 0 || item.position > MaxSongLength || item.length < -MaxSongLength || item.length > MaxSongLength || (item.length > 0 && item.length > MaxSongLength - item.position)) { throw std::invalid_argument("Native step edit note exceeds the song range"); }
			const qint64 index = (static_cast<qint64>(item.position) * steps + bar - 1) / bar;
			const bool cell = options.set && item.type == Note::Type::Step && item.position == position;
			if (cell && !existing) { existing = note; }
			if ((cell && !options.enabled) || (!options.set && target < count && item.type == Note::Type::Step && index >= target && index < count && index * bar / steps == item.position)) { retiring.push_back(note); }
			else
			{
				retained.push_back(note); if (item.length > 0) { noteSpan = std::max(noteSpan, static_cast<qint64>(item.position) + item.length); }
				if (item.type == Note::Type::Step) { stepSpan = std::max(stepSpan, static_cast<qint64>(item.position) + 1); }
			}
		}
		std::unique_ptr<Note> prepared; Note* enabled = options.set && options.enabled ? existing : nullptr; std::shared_ptr<const void> enabledIdentity;
		if (options.set && options.enabled && !existing)
		{
			if (roster.size() == static_cast<std::size_t>(std::numeric_limits<int>::max())) { throw std::invalid_argument("Native enabled step roster exceeds receipt count range"); }
			prepared = std::make_unique<Note>(TimePos(DefaultTicksPerBar / 16), TimePos(position)); prepared->setType(Note::Type::Step); enabled = prepared.get();
			if (position > MaxSongLength - prepared->length().getTicks()) { throw std::invalid_argument("Native enabled step endpoint exceeds the song range"); }
			retained.push_back(enabled); std::stable_sort(retained.begin(), retained.end(), Note::lessThan);
			noteSpan = std::max(noteSpan, static_cast<qint64>(position) + prepared->length().getTicks()); stepSpan = std::max(stepSpan, static_cast<qint64>(position) + 1);
		}
		if (enabled) { enabledIdentity = enabled->selectionIdentity(); }
		// updateLength rounds retained regular ends, and one-bar pattern rows also
		// inspect outside-prefix Step notes. Prove both native length paths safe.
		if (((noteSpan + bar - 1) / bar) * bar > MaxSongLength || (target == steps && ((stepSpan + bar - 1) / bar) * bar > MaxSongLength)) { throw std::invalid_argument("Edited native note span exceeds rounded song bounds"); }
		const auto validate = [&] {
			if (!receiver || receiver->getTrack()->isNoteRecording() || receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps || receiver->notes() != roster) { throw std::runtime_error("Native step edit roster, count or meter changed during admission"); }
			for (const auto& item : original)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->length().getTicks() != item.length || item.note->key() != item.key || item.note->volume() != item.volume || item.note->panning() != item.pan || item.note->type() != item.type || item.note->selected() != item.selected) { throw std::runtime_error("Native step edit note state changed during admission"); } }
		};
		check(); validate(); StepResizeResult ready; ready.fromCount = count;
		if (target == count && retiring.empty() && !prepared) { result = ready; active = enabled; error.clear(); return true; }
		ready.changed = true; ready.removed = static_cast<int>(retiring.size()); std::vector<std::unique_ptr<Note>> owned; owned.reserve(retiring.size());
		const auto expectedSize = roster.size() - retiring.size() + (prepared ? 1 : 0);
		if (!NativeNoteTransfer::checkpoint(receiver.data(), error)) { throw std::runtime_error(error.toStdString()); } validate();
		{
			auto track = receiver->instrumentTrack(); track->lock(); const auto unlock = qScopeGuard([&] { track->unlock(); });
			receiver->m_steps = target; receiver->m_notes.swap(retained); for (auto note : retiring) { owned.emplace_back(note); }
			prepared.release();
		}
		for (auto& note : owned) { note.reset(); if (!receiver) { throw std::runtime_error("Native step edit clip retired during removed-note cleanup"); } }
		receiver->checkType(); receiver->updateLength(); if (!receiver) { throw std::runtime_error("Native step edit clip retired during length notification"); }
		emit receiver->dataChanged(); Engine::getSong()->setModified();
		if (!receiver || receiver->stepCount() != target || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps) { throw std::runtime_error("Native step edit result changed during notification"); }
		QSet<const Note*> removed, live; for (auto note : retiring) { removed.insert(note); } for (auto note : receiver->notes()) { live.insert(note); }
		if (receiver->notes().size() != expectedSize) { throw std::runtime_error("Native step edit roster changed during notification"); }
		for (const auto& item : original)
		{ if (!removed.contains(item.note) && (!live.contains(item.note) || item.note->selectionIdentity() != item.identity)) { throw std::runtime_error("Native step edit retained note retired or was replaced during notification"); } }
		if (enabled && (!live.contains(enabled) || enabled->selectionIdentity() != enabledIdentity || enabled->type() != Note::Type::Step || enabled->pos().getTicks() != position)) { throw std::runtime_error("Native enabled step retired or moved during notification"); }
		result = ready; active = enabled; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native step edit failed with a nonstandard exception"; }
	return false;
}

bool MidiClip::cloneSteps(StepCloneResult& result, QString& error, int limit)
{
	result = {};
	try
	{
		owner(this); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(this);
		const int count = m_steps, bar = TimePos::ticksPerBar(), steps = TimePos::stepsPerBar();
		if (limit < 0 || count < 1 || count > std::numeric_limits<int>::max() / 2 || bar < 1 || steps < 1 || (limit && count > limit / 2)) { throw std::invalid_argument("Native step clone count or meter exceeds its bounds"); }
		const int target = count * 2;
		const qint64 product = static_cast<qint64>(target) * bar, span = product / steps, rounded = ((span + bar - 1) / bar) * bar;
		if (product > std::numeric_limits<int>::max() || span > MaxSongLength || rounded > MaxSongLength) { throw std::invalid_argument("Cloned native step span exceeds native arithmetic or song bounds"); }
		const NoteVector roster = m_notes;
		struct State { Note* note; std::shared_ptr<const void> identity; int position, length, key; volume_t volume; panning_t pan; Note::Type type; bool selected; };
		std::vector<State> original; original.reserve(roster.size());
		for (auto note : roster) { if (!note) { throw std::invalid_argument("Native step clone roster contains a missing note"); } original.push_back({note, note->selectionIdentity(), note->pos().getTicks(), note->length().getTicks(), note->key(), note->volume(), note->panning(), note->type(), note->selected()}); }
		const auto validate = [&] {
			if (!receiver || receiver->stepCount() != count || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps || receiver->notes() != roster) { throw std::runtime_error("Native step clone roster, count or meter changed during admission"); }
			for (const auto& item : original)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->length().getTicks() != item.length || item.note->key() != item.key || item.note->volume() != item.volume || item.note->panning() != item.pan || item.note->type() != item.type || item.note->selected() != item.selected) { throw std::runtime_error("Native step clone note state changed during admission"); } }
		};
		QMap<int, const State*> first; QSet<int> occupied;
		for (const auto& item : original)
		{
			if (item.type != Note::Type::Step) { continue; } occupied.insert(item.position); if (item.position < 0) { continue; }
			// Invert the increasing native floor(step * ticks / stepsPerBar) grid.
			const qint64 index = (static_cast<qint64>(item.position) * steps + bar - 1) / bar;
			if (index >= count || index * bar / steps != item.position || first.contains(static_cast<int>(index))) { continue; }
			first.insert(static_cast<int>(index), &item);
		}
		StepCloneResult ready; ready.fromCount = count; std::vector<int> positions;
		for (auto item = first.cbegin(); item != first.cend(); ++item)
		{
			check(); validate(); const int position = static_cast<int>((static_cast<qint64>(count) + item.key()) * bar / steps);
			if (occupied.contains(position)) { throw std::invalid_argument("Native step clone destination already contains a step note"); }
			ready.sources.push_back(item.value()->note); ready.steps.push_back(item.key()); positions.push_back(position);
		}
		if (limit && roster.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) - ready.sources.size()) { throw std::invalid_argument("Native step clone note roster exceeds receipt index range"); }
		check(); validate(); DataFile document(DataFile::Type::ClipboardData);
		if (ready.sources.empty()) { auto list = document.createElement("note-list"); list.setAttribute("version", 1); document.content().appendChild(list); }
		else if (!NativeNoteTransfer::capture(this, ready.sources, 0, document, error, true)) { throw std::runtime_error(error.toStdString()); }
		validate(); auto state = document.content().firstChildElement("note-list").firstChildElement("note");
		for (std::size_t index = 0; index < positions.size(); ++index) { if (state.isNull()) { throw std::runtime_error("Native step clone control roster changed"); } state.setAttribute("pos", positions[index]); state = state.nextSiblingElement("note"); }
		if (!NativeNoteTransfer::restore(document, this, 0, ready.copies, error, this, &ready.sources, target, validate)) { throw std::runtime_error(error.toStdString()); }
		if (!receiver || receiver->stepCount() != target || TimePos::ticksPerBar() != bar || TimePos::stepsPerBar() != steps || receiver->notes().size() != roster.size() + ready.copies.size()) { throw std::runtime_error("Native step clone result changed during notification"); }
		QSet<const Note*> live; for (auto note : receiver->notes()) { live.insert(note); }
		for (const auto& item : original) { if (!live.contains(item.note) || item.note->selectionIdentity() != item.identity) { throw std::runtime_error("Native step clone original retired during notification"); } }
		result = std::move(ready); error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native step clone failed with a nonstandard exception"; }
	return false;
}

bool NativeNoteTransfer::restore(const DataFile& document, MidiClip* target, TimePos position, NoteVector& inserted, QString& error, MidiClip* source, const NoteVector* sources, int stepCount, const std::function<void()>& validate)
{
	try
	{
		owner(target); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(target), sender(source); std::vector<std::shared_ptr<const void>> identities;
		if (sources) { owner(source); for (auto note : *sources) { if (std::find(source->notes().begin(), source->notes().end(), note) == source->notes().end()) { throw std::runtime_error("Native note source retired before preparation"); } identities.push_back(note->selectionIdentity()); } }
		check(); if (!receiver) { throw std::runtime_error("Native note destination retired during admission"); }
		if (validate) { validate(); }
		if (document.type() != DataFile::Type::ClipboardData || !document.serializationError().isEmpty() || position.getTicks() < 0 || position.getTicks() > MaxSongLength || !inserted.empty()) { throw std::invalid_argument("Invalid native note transfer document, destination or result roster"); }
		const auto list = document.content().firstChildElement("note-list"); if (list.isNull() || !list.nextSiblingElement("note-list").isNull() || (list.hasAttribute("version") && list.attribute("version") != "1")) { throw std::invalid_argument("Unsupported native note-list format"); }
		std::vector<QDomElement> states;
		const bool legacy = !list.hasAttribute("version");
		for (auto state = list.firstChildElement(); !state.isNull(); state = state.nextSiblingElement())
		{
			if (state.tagName() != Note::classNodeName()) { throw std::invalid_argument("Unexpected native note-list child"); }
			const int relative = integer(state, "pos", 0, MaxSongLength, legacy ? "0" : QString()), length = integer(state, "len", -MaxSongLength, MaxSongLength, legacy ? "0" : QString());
			if (relative > MaxSongLength - position.getTicks() || (length > 0 && length > MaxSongLength - position.getTicks() - relative)) { throw std::invalid_argument("Copied native note endpoint exceeds the song range"); }
			const auto key = integer(state, "key", 0, NumKeys - 1, legacy ? "0" : QString());
			const auto tone = integer(state, "tone", std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), "0"), octave = integer(state, "oct", std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), "0");
			const auto octaveKey = static_cast<qint64>(octave) * KeysPerOctave, oldKey = static_cast<qint64>(tone) + octaveKey;
			if (octaveKey < std::numeric_limits<int>::min() || octaveKey > std::numeric_limits<int>::max() || oldKey < std::numeric_limits<int>::min() || oldKey > std::numeric_limits<int>::max()
				|| std::max<qint64>(key, oldKey) >= NumKeys) { throw std::invalid_argument("Legacy native note key exceeds the keyboard range"); }
			integer(state, "vol", MinVolume, MaxVolume, legacy ? "0" : QString()); integer(state, "pan", PanningLeft, PanningRight, legacy ? "0" : QString()); integer(state, "type", 0, 1, "0"); states.push_back(state);
		}
		// Only MIDI controllers are owned inline. Refuse other embedded factories
		// before native loading can create an unregistered controller under Song.
		std::vector<QDomElement> pending{list};
		while (!pending.empty())
		{
			const auto element = pending.back(); pending.pop_back();
			if (element.tagName() == "connection")
			{
				for (auto binding = element.firstChildElement(); !binding.isNull(); binding = binding.nextSiblingElement())
				{
					const auto controller = binding.firstChildElement();
					if (!controller.isNull())
					{ if (!controller.nextSiblingElement().isNull() || integer(controller, "type", 0, 3) != static_cast<int>(Controller::ControllerType::Midi)) { throw std::invalid_argument("Only native MIDI controllers can be embedded in note transfer"); } }
					else
					{
						const auto id = integer(binding, "id", 0, std::numeric_limits<int>::max()); const auto& rack = Engine::getSong()->controllers();
						if (static_cast<std::size_t>(id) >= rack.size() || !rack.at(id) || rack.at(id)->type() == Controller::ControllerType::Dummy) { throw std::invalid_argument("Native note transfer controller rack reference is unavailable"); }
					}
				}
			}
			for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) { pending.push_back(child); }
		}
		if ((states.empty() && stepCount < 0) || (sources && states.size() != sources->size())) { throw std::invalid_argument("Native note transfer has no notes or a changed source roster"); }
		JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope links(document); std::vector<std::unique_ptr<Note>> prepared; prepared.reserve(states.size()); NoteVector ready; ready.reserve(states.size()); std::vector<std::shared_ptr<const void>> readyIdentities; readyIdentities.reserve(states.size());
		for (std::size_t index = 0; index < states.size(); ++index)
		{
			check(); if (!receiver) { throw std::runtime_error("Native note destination retired while preparing"); }
			if (validate) { validate(); }
			if (sources && (!sender || std::find(sender->notes().begin(), sender->notes().end(), sources->at(index)) == sender->notes().end() || sources->at(index)->selectionIdentity() != identities[index])) { throw std::runtime_error("Native note source changed while preparing"); }
			prepared.emplace_back(sources ? sources->at(index)->clone() : new Note);
			if (sources) { if (prepared.back()->detuning()) { prepared.back()->detuning()->loadControlSettings(states[index]); } } else { prepared.back()->restoreState(states[index]); }
			prepared.back()->setPos(states[index].attribute("pos").toInt() + position.getTicks()); prepared.back()->setSelected(stepCount < 0); ready.push_back(prepared.back().get()); readyIdentities.push_back(prepared.back()->selectionIdentity());
		}
		connections(ready); if (!links.finish(document.content(), error)) { throw std::runtime_error(error.toStdString()); }
		check(); if (!receiver) { throw std::runtime_error("Native note destination retired before insertion"); } owner(receiver.data());
		if (validate) { validate(); }
		receiver->appendPreparedNotes(prepared, [&] {
			if (validate) { validate(); }
			if (!checkpoint(receiver.data(), error)) { throw std::runtime_error(error.toStdString()); }
			if (validate) { validate(); }
		}, stepCount);
		Engine::getSong()->setModified();
		if (!receiver) { throw std::runtime_error("Native note destination retired during insertion"); }
		QSet<const Note*> current; for (auto note : receiver->notes()) { current.insert(note); }
		for (std::size_t index = 0; index < ready.size(); ++index)
		{ if (!current.contains(ready[index]) || ready[index]->selectionIdentity() != readyIdentities[index]) { throw std::runtime_error("Copied native note retired or was replaced during insertion"); } }
		inserted.swap(ready); error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); } catch (...) { error = "Native note transfer failed with a nonstandard exception"; } return false;
}
}

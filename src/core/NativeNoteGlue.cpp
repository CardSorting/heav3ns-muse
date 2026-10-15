/* Native note gluing shared by GUI and structured studio callers.
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
#include <utility>
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NativeNoteTransfer.h"
#include "Song.h"
namespace lmms
{
bool MidiClip::glueNotes(const NoteVector& selection, GlueResult& result, QString& error)
{
	result = {};
	try
	{
		if (!QCoreApplication::instance() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject()
			|| QThread::currentThread() != QCoreApplication::instance()->thread() || thread() != QThread::currentThread()
			|| !getTrack() || getTrack()->isNoteRecording()) { throw std::invalid_argument("Native note glue requires an idle clip on its model owner thread"); }
		const auto& clips = getTrack()->getClips(); if (std::find(clips.begin(), clips.end(), this) == clips.end()) { throw std::invalid_argument("Native note glue clip is detached"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; const QPointer<MidiClip> receiver(this);
		const NoteVector notes = selection, roster = m_notes;
		if (notes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { throw std::invalid_argument("Native glue selection exceeds result count range"); }
		struct State { Note* note; std::shared_ptr<const void> identity; int position; int key; int length; };
		std::vector<State> original; original.reserve(roster.size());
		for (auto note : roster) { if (!note) { throw std::invalid_argument("Native glue roster contains a missing note"); } original.push_back({note, note->selectionIdentity(), note->pos().getTicks(), note->key(), note->length().getTicks()}); }
		const auto check = [&] { QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } if (!receiver) { throw std::runtime_error("Native note glue clip retired during admission"); } };
		const auto validate = [&] {
			if (!receiver || receiver->notes() != roster) { throw std::runtime_error("Native glue roster changed during admission"); }
			for (const auto& item : original)
			{ if (item.note->selectionIdentity() != item.identity || item.note->pos().getTicks() != item.position || item.note->key() != item.key || item.note->length().getTicks() != item.length) { throw std::runtime_error("Native glue note state changed during admission"); } }
		};
		QSet<Note*> selected;
		for (auto note : notes)
		{
			check(); if (!note || selected.contains(note) || std::find(receiver->notes().begin(), receiver->notes().end(), note) == receiver->notes().end()) { throw std::invalid_argument("Native glue selection has duplicate or foreign notes"); }
			selected.insert(note); const int position = note->pos().getTicks(), length = note->length().getTicks();
			if (position < 0 || position > MaxSongLength || length < -MaxSongLength || length > MaxSongLength || (length > 0 && length > MaxSongLength - position) || note->key() < 0 || note->key() >= NumKeys)
			{ throw std::invalid_argument("Native glue source exceeds native time or key bounds"); }
		}
		check(); validate(); NoteVector ordered; ordered.reserve(notes.size());
		// Equal key/start ties keep native clip roster order, independent of request order.
		for (auto note : roster) { if (selected.contains(note)) { ordered.push_back(note); } }
		std::stable_sort(ordered.begin(), ordered.end(), [](const Note* a, const Note* b) { return a->key() == b->key() ? a->pos() < b->pos() : a->key() < b->key(); });
		QHash<Note*, Note*> representative; QHash<Note*, int> lengths; QSet<Note*> consumed; NoteVector retiring; Note* current = nullptr;
		for (auto note : ordered)
		{
			if (current && note->key() == current->key() && note->pos().getTicks() <= static_cast<qint64>(current->pos().getTicks()) + std::max(0, lengths.value(current)))
			{
				const qint64 length = std::max<qint64>(lengths.value(current), static_cast<qint64>(note->pos().getTicks()) + note->length().getTicks() - current->pos().getTicks());
				if (length < -MaxSongLength || length > MaxSongLength || (length > 0 && length > MaxSongLength - current->pos().getTicks())) { throw std::invalid_argument("Glued native note endpoint exceeds the song range"); }
				lengths.insert(current, static_cast<int>(length)); representative.insert(note, current); consumed.insert(note); retiring.push_back(note);
			}
			else { current = note; lengths.insert(note, note->length().getTicks()); representative.insert(note, note); }
		}
		GlueResult ready; ready.removed = static_cast<int>(retiring.size()); ready.survivors.reserve(notes.size()); std::vector<std::shared_ptr<const void>> identities; identities.reserve(notes.size());
		for (auto note : notes) { auto survivor = representative.value(note); ready.survivors.push_back(survivor); identities.push_back(survivor->selectionIdentity()); }
		check(); validate(); if (retiring.empty()) { result = std::move(ready); error.clear(); return true; }
		NoteVector combined; combined.reserve(roster.size() - retiring.size());
		for (auto note : roster) { if (!consumed.contains(note)) { combined.push_back(note); } }
		std::stable_sort(combined.begin(), combined.end(), Note::lessThan); std::vector<std::unique_ptr<Note>> owned; owned.reserve(retiring.size());
		check(); validate(); if (!NativeNoteTransfer::checkpoint(receiver.data(), error)) { throw std::runtime_error(error.toStdString()); } validate();
		{
			auto track = receiver->instrumentTrack(); track->lock(); const auto unlock = qScopeGuard([&] { track->unlock(); });
			for (auto item = lengths.cbegin(); item != lengths.cend(); ++item) { item.key()->setLength(item.value()); }
			receiver->m_notes.swap(combined); for (auto note : retiring) { owned.emplace_back(note); }
		}
		// The clip no longer owns consumed notes. Normal destruction detaches their controls.
		for (auto& note : owned) { note.reset(); if (!receiver) { throw std::runtime_error("Native glue clip retired during consumed-note cleanup"); } }
		receiver->checkType(); receiver->updateLength(); if (!receiver) { throw std::runtime_error("Native glue clip retired during length notification"); }
		emit receiver->dataChanged(); Engine::getSong()->setModified(); if (!receiver) { throw std::runtime_error("Native glue clip retired during notification"); }
		QSet<const Note*> live; for (auto note : receiver->notes()) { live.insert(note); }
		for (std::size_t index = 0; index < ready.survivors.size(); ++index)
		{ if (!live.contains(ready.survivors[index]) || ready.survivors[index]->selectionIdentity() != identities[index]) { throw std::runtime_error("Native glue survivor retired or was replaced during notification"); } }
		result = std::move(ready); error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native note glue failed with a nonstandard exception"; }
	return false;
}
}

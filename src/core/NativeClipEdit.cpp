/* Checked native clip property editing, shared with structured callers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "Clip.h"
#include <QCoreApplication>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "AudioEngine.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "Track.h"
#include "TrackContainer.h"
namespace lmms
{
bool Clip::edit(const EditOptions& input, bool& changed, QString& error)
{
	changed = false;
	try
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| thread() != QThread::currentThread() || !Engine::audioEngine() || !Engine::getSong() || Engine::getSong()->isLoadingProject()) { throw std::invalid_argument("Native clip edit requires a ready project on the model owner thread"); }
		const EditOptions options = input; const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
		const bool refresh = options.autoResize.value_or(false); const int meter = TimePos::ticksPerBar(); const auto clock = Engine::framesPerTick();
		const QPointer<Clip> receiver(this); const QPointer<Track> track(getTrack());
		const auto context = [&] {
			if (!receiver || !track || receiver->getTrack() != track.data() || !track->trackContainer()
				|| std::find(track->trackContainer()->tracks().begin(), track->trackContainer()->tracks().end(), track.data()) == track->trackContainer()->tracks().end()
				|| std::find(track->getClips().begin(), track->getClips().end(), receiver.data()) == track->getClips().end()) { throw std::runtime_error("Native clip edit owner retired or became detached"); }
			if ((refresh && TimePos::ticksPerBar() != meter) || ((refresh || options.offset) && Engine::framesPerTick() != clock)) { throw std::runtime_error("Native clip edit meter or audio clock changed during admission or callbacks"); }
			receiver->requireIdleForCopy();
		};
		context(); const int oldPosition = startPosition().getTicks(), oldLength = length().getTicks(), oldOffset = startTimeOffset().getTicks();
		const bool oldAuto = getAutoResize(); const auto oldMute = mutedModel()->storedValue(); const QString oldName = name();
		const int position = options.position.value_or(oldPosition), size = options.length.value_or(oldLength), offset = options.offset.value_or(oldOffset);
		const bool automatic = options.autoResize.value_or((options.length || options.offset) ? false : oldAuto);
		if (position < 0 || position > MaxSongLength || size < 0 || (options.length && size < 1) || size > MaxSongLength || position > MaxSongLength - size || offset < -MaxSongLength || offset > MaxSongLength) { throw std::invalid_argument("Native clip edit geometry exceeds the song range"); }
		if (options.position && isInPattern()) { throw std::invalid_argument("PatternStore owns clip positions"); }
		if (refresh && (options.length || options.offset)) { throw std::invalid_argument("Enable autoResize separately from manual length or offset edits"); }
		if ((refresh && meter < 1) || ((refresh || options.offset) && (!std::isfinite(clock) || clock <= 0))) { throw std::invalid_argument("Native automatic sizing or sample offset requires a valid meter and audio clock"); }
		const auto validate = [&] {
			context(); if (receiver->startPosition().getTicks() != oldPosition || receiver->length().getTicks() != oldLength || receiver->startTimeOffset().getTicks() != oldOffset
				|| receiver->getAutoResize() != oldAuto || receiver->mutedModel()->storedValue() != oldMute || receiver->name() != oldName) { throw std::runtime_error("Native clip properties changed during edit admission"); }
		};
		const auto check = [&] { QString failure; if (!FileResourceIO::check(failure)) { throw std::runtime_error(failure.toStdString()); } validate(); };
		check();
		const bool apply = refresh || position != oldPosition || size != oldLength || offset != oldOffset || automatic != oldAuto
			|| (options.name && *options.name != oldName) || (options.muted && oldMute != (*options.muted ? 1.f : 0.f));
		if (!apply) { error.clear(); return true; }
		auto journal = Engine::projectJournal(); const bool journalling = journal->isJournalling();
		if (journalling && isJournalling())
		{
			DataFile document(DataFile::Type::JournalData); const auto state = saveJournalState(document, document.content()); validate();
			if (state.isNull() || !document.serializationError().isEmpty() || !journal->addPreparedJournalCheckPoint(receiver.data(), document)) { throw std::runtime_error(document.serializationError().isEmpty() ? "Native clip edit could not publish a complete clip checkpoint" : document.serializationError().toStdString()); }
		}
		validate();
		{
			// One complete clip checkpoint owns this compound edit. Nested native
			// model setters must not publish independent partial checkpoints.
			journal->setJournalling(false); const auto restore = qScopeGuard([&] { journal->setJournalling(journalling); });
			if (options.position && position != oldPosition) { receiver->movePosition(position); context(); }
			if (options.length) { receiver->setAutoResize(false); if (receiver->length().getTicks() != size) { receiver->changeLength(size); context(); } }
			if (options.offset) { receiver->setAutoResize(false); if (receiver->startTimeOffset().getTicks() != offset) { receiver->setStartTimeOffset(offset); context(); } }
			if (options.autoResize) { receiver->setAutoResize(*options.autoResize); if (refresh) { receiver->updateLength(); context(); } }
			if (options.name && receiver->name() != *options.name) { receiver->setName(*options.name); context(); }
			if (options.muted && receiver->mutedModel()->storedValue() != (*options.muted ? 1.f : 0.f)) { receiver->setMuted(*options.muted); context(); }
		}
		const auto result = [&] {
			context(); const int finalLength = receiver->length().getTicks(), finalOffset = receiver->startTimeOffset().getTicks();
			if (receiver->startPosition().getTicks() != position || finalLength < 0 || finalLength > MaxSongLength - position || finalOffset < -MaxSongLength || finalOffset > MaxSongLength
				|| (!refresh && (finalLength != size || finalOffset != offset)) || receiver->getAutoResize() != automatic
				|| receiver->name() != options.name.value_or(oldName) || receiver->mutedModel()->storedValue() != (options.muted ? (*options.muted ? 1.f : 0.f) : oldMute)) { throw std::runtime_error("Native clip edit result changed or exceeded native bounds"); }
		};
		result(); emit receiver->dataChanged(); Engine::getSong()->setModified(); result(); changed = true; error.clear(); return true;
	}
	catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
	catch (...) { error = "Native clip edit failed with a nonstandard exception"; }
	return false;
}
}

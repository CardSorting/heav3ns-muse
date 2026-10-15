/*
 * TrackContainer.cpp - implementation of base class for all trackcontainers
 *                      like Song-Editor, Pattern Editor...
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


#include <QCoreApplication>
#include <QProgressDialog>
#include <QDomElement>
#include <QWriteLocker>
#include <QReadLocker>
#include <QScopedValueRollback>
#include <QThread>
#include <QPointer>
#include <QScopeGuard>
#include <stdexcept>
#include <algorithm>
#include <limits>

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "Engine.h"
#include "embed.h"
#include "TrackContainer.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"
#include "FileResourceIO.h"

#include "GuiApplication.h"
#include "MainWindow.h"
#include "TextFloat.h"

namespace lmms
{


TrackContainer::TrackContainer() :
	Model( nullptr ),
	JournallingObject(),
	m_tracksMutex(),
	m_tracks()
{
}




TrackContainer::~TrackContainer()
{
	clearAllTracks();
}




void TrackContainer::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	if (thread() != QThread::currentThread() || m_savingTracks || m_loadingTracks) { throw std::logic_error("Native track container save is already active or on another thread"); }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const QPointer<TrackContainer> owner(this);
	m_savingTracks = true;
	const auto saving = qScopeGuard([owner] { if (owner) { owner->m_savingTracks = false; } });
	std::vector<QPointer<Track>> tracks;
	{
		const QReadLocker lock(&m_tracksMutex);
		for (auto track : m_tracks) { tracks.emplace_back(track); }
	}
	std::size_t next = 0;
	const auto ready = [&](bool complete = false) {
		if (!owner || owner->m_tracks.size() != tracks.size()) { throw std::runtime_error("Native track container changed during serialization"); }
		if (!complete && next < tracks.size() && (!tracks[next] || owner->m_tracks[next] != tracks[next].data())) { throw std::runtime_error("Native track identity changed during serialization"); }
		for (std::size_t index = 0; complete && index < tracks.size(); ++index)
		{ if (!tracks[index] || owner->m_tracks[index] != tracks[index].data()) { throw std::runtime_error("Native track roster changed during serialization"); } }
	};
	_this.setTagName( classNodeName() );
	_this.setAttribute( "type", nodeName() );
	ready();
	// Native serializers may remove tracks. Never retain the roster lock across them.
	for (const auto& track : tracks)
	{
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready();
		track->saveState(_doc, _this);
		ready();
		++next;
	}
	ready(true);
}




void TrackContainer::loadSettings( const QDomElement & _this )
{
	if (thread() != QThread::currentThread() || m_loadingTracks || m_savingTracks) { throw std::logic_error("Native track container restoration cannot reenter or overlap serialization"); }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const QPointer<TrackContainer> owner(this);
	const QPointer<Song> song(Engine::getSong());
	m_loadingTracks = true;
	const auto loading = qScopeGuard([owner] { if (owner) { owner->m_loadingTracks = false; } });
	const auto ready = [&] { if (!owner || !song || Engine::getSong() != song.data()) { throw std::runtime_error("Native track container restoration context retired"); } };
	const auto check = [&] { ready(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready(); };
	bool journalRestore = _this.parentNode().nodeName() == "journaldata";
	if( journalRestore )
	{
		clearAllTracks();
		ready();
	}

	static QPointer<QProgressDialog> pd;
	bool was_null = ( pd == nullptr );
	QPointer<QProgressDialog> progress(pd);
	const auto disposeProgress = qScopeGuard([&] { if (was_null && progress) { delete progress.data(); } });
	if (!journalRestore && gui::getGUI() != nullptr)
	{
		if( pd == nullptr )
		{
			pd = new QProgressDialog( tr( "Loading project..." ),
						tr( "Cancel" ), 0,
						Engine::getSong()->getLoadingTrackCount(),
						gui::getGUI()->mainWindow());
			progress = pd;
			pd->setWindowModality( Qt::ApplicationModal );
			ready(); if (!progress) { throw std::runtime_error("Native project load progress dialog retired"); }
			pd->setWindowTitle( tr( "Please wait..." ) );
			ready(); if (!progress) { throw std::runtime_error("Native project load progress dialog retired"); }
			pd->show();
			ready();
		}
	}

	QDomNode node = _this.firstChild();
	while( !node.isNull() )
	{
		check();
		if (progress)
		{
			progress->setValue(progress->value() + 1);
			ready();
			QCoreApplication::instance()->processEvents(
						QEventLoop::AllEvents, 100 );
			ready();
			if (!progress) { throw std::runtime_error("Native project load progress dialog retired"); }
			if (progress->wasCanceled())
			{
				if (gui::getGUI() != nullptr)
				{
					gui::TextFloat::displayMessage( tr( "Loading cancelled" ),
					tr( "Project loading was cancelled." ),
					embed::getIconPixmap( "project_file", 24, 24 ),
					2000 );
					ready();
				}
				Engine::getSong()->loadingCancelled();
				break;
			}
		}

		if (node.isElement() && node.nodeName() == "track" &&
			!node.toElement().attribute( "metadata" ).toInt() )
		{
			QString trackName = node.toElement().hasAttribute( "name" ) ?
						node.toElement().attribute( "name" ) :
						node.firstChild().toElement().attribute( "name" );
			if (progress)
			{
				progress->setLabelText(tr("Loading Track %1 (%2/Total %3)").arg(trackName).
					arg(progress->value() + 1).arg(song->getLoadingTrackCount()));
				ready();
			}
			if (!Track::create(node.toElement(), this)) { ready(); song->collectError(tr("Unavailable native track type in project")); }
			ready();
		}
		node = node.nextSibling();
	}

}




int TrackContainer::countTracks( Track::Type _tt ) const
{
	int cnt = 0;
	m_tracksMutex.lockForRead();
	for (const auto& track : m_tracks)
	{
		if (track->type() == _tt || _tt == Track::Type::Count)
		{
			++cnt;
		}
	}
	m_tracksMutex.unlock();
	return( cnt );
}




void TrackContainer::addTrack( Track * _track )
{
	if( _track->type() != Track::Type::HiddenAutomation )
	{
		_track->lock();
		m_tracksMutex.lockForWrite();
		m_tracks.push_back( _track );
		m_tracksMutex.unlock();
		_track->unlock();
		emit trackAdded( _track );
	}
}




void TrackContainer::removeTrack( Track * _track )
{
	if (!_track) { return; }
	const auto engine = Engine::audioEngine();
	const auto changes = engine ? engine->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	const QPointer<TrackContainer> owner(this);
	const QPointer<Track> target(_track);
	{
		QReadLocker lock(&m_tracksMutex);
		if (std::find(m_tracks.begin(), m_tracks.end(), target.data()) == m_tracks.end()) { return; }
	}
	// Solo restoration emits native model notifications. Never hold the roster
	// lock or retain an iterator across them: callbacks may inspect, move or
	// remove peers, and may even detach this track through a nested call.
	if (target->isSolo()) { target->setSolo(false); }
	if (!owner || !target) { return; }
	{
		QWriteLocker lock(&owner->m_tracksMutex);
		const auto found = std::find(owner->m_tracks.begin(), owner->m_tracks.end(), target.data());
		if (found == owner->m_tracks.end()) { return; }
		owner->m_tracks.erase(found);
	}
	if (auto song = Engine::getSong()) { song->setModified(); }
	if (owner) { emit owner->trackRemoved(); }
}

bool TrackContainer::moveTrack(Track* track, int indexTo)
{
	if (QThread::currentThread() != thread() || m_movingTrack) { return false; }
	const auto engine = Engine::audioEngine();
	const auto guard = engine ? engine->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	const auto found = std::find(m_tracks.begin(), m_tracks.end(), track);
	if (!track || found == m_tracks.end() || track->trackContainer() != this
		|| indexTo < 0 || indexTo >= static_cast<int>(m_tracks.size())) { return false; }
	const auto indexFrom = static_cast<int>(found - m_tracks.begin());
	if (indexFrom == indexTo) { return true; }

	std::vector<Track*> crossedPatterns;
	auto pattern = dynamic_cast<PatternTrack*>(track);
	auto store = Engine::patternStore();
	PatternTrack* selectedPattern = nullptr;
	if (pattern)
	{
		if (this != Engine::getSong() || !store) { return false; }
		const int count = countTracks(Track::Type::Pattern);
		// Check every slot before swapping anything. A sparse/malformed store
		// must not create clips or partially reorder native pattern ownership.
		for (auto candidate : m_tracks)
		{
			if (auto owner = dynamic_cast<PatternTrack*>(candidate))
			{
				const int index = owner->patternIndex();
				if (index < 0 || index >= count || PatternTrack::findPatternTrack(index) != owner) { return false; }
			}
		}
		for (auto row : store->tracks())
		{
			if (row->numOfClips() < count) { return false; }
			for (int i = 0; i < count; ++i) { if (!row->getClips()[i]) { return false; } }
		}
		selectedPattern = PatternTrack::findPatternTrack(store->currentPattern());
		const int direction = indexTo > indexFrom ? 1 : -1;
		for (int index = indexFrom + direction; index != indexTo + direction; index += direction)
		{
			if (dynamic_cast<PatternTrack*>(m_tracks[index])) { crossedPatterns.push_back(m_tracks[index]); }
		}
	}

	QScopedValueRollback<bool> moving(m_movingTrack, true);
	{
		QWriteLocker lock(&m_tracksMutex);
		if (indexFrom < indexTo) { std::rotate(m_tracks.begin() + indexFrom, m_tracks.begin() + indexFrom + 1, m_tracks.begin() + indexTo + 1); }
		else { std::rotate(m_tracks.begin() + indexTo, m_tracks.begin() + indexFrom, m_tracks.begin() + indexFrom + 1); }
	}
	// Moving over several rows rotates all intervening pattern columns, rather
	// than swapping only the endpoints. Existing clip/model identities survive.
	for (auto crossed : crossedPatterns) { PatternTrack::swapPatternTracks(track, crossed); }
	if (selectedPattern) { store->setCurrentPattern(selectedPattern->patternIndex()); }
	if (auto song = Engine::getSong()) { song->setModified(); }

	emit trackMoved();
	return true;
}



void TrackContainer::updateAfterTrackAdd()
{
}




void TrackContainer::clearAllTracks()
{
	const QPointer<TrackContainer> owner(this);
	while (owner && !owner->m_tracks.empty())
	{
		delete owner->m_tracks.front();
	}
}




bool TrackContainer::isEmpty() const
{
	for (const auto& track : m_tracks)
	{
		if (!track->getClips().empty())
		{
			return false;
		}
	}
	return true;
}



AutomatedValueMap TrackContainer::automatedValuesAt(TimePos time, int clipNum) const
{
	return automatedValuesFromTracks(tracks(), time, clipNum);
}


AutomatedValueMap TrackContainer::automatedValuesFromTracks(const TrackList &tracks, TimePos time, int clipNum)
{
	Track::clipVector clips;

	for (Track* track: tracks)
	{
		if (track->isMuted()) {
			continue;
		}

		switch(track->type())
		{
		case Track::Type::Automation:
		case Track::Type::HiddenAutomation:
		case Track::Type::Pattern:
			if (clipNum < 0) {
				track->getClipsInRange(clips, 0, time);
			} else {
				if (clipNum < track->numOfClips() && track->getClips()[clipNum])
				{ clips.push_back(track->getClips()[clipNum]); }
			}
		default:
			break;
		}
	}

	AutomatedValueMap valueMap;

	// Song ranges are inserted in time order. Explicit column reads retain
	// native row precedence even when editor positions are not sorted.

	for(Clip* clip : clips)
	{
		if (clip->isMuted() || (clipNum < 0 && clip->startPosition() > time)) {
			continue;
		}

		if (auto* p = dynamic_cast<AutomationClip *>(clip))
		{
			if (! p->hasAutomation()) {
				continue;
			}
			TimePos relTime = (clipNum >= 0 ? time : time - p->startPosition()) - p->startTimeOffset();
			if (!p->isInPattern()) {
				relTime = std::min(static_cast<int>(relTime), p->length() - p->startTimeOffset());
			}
			float value = p->valueAt(relTime);

			for (AutomatableModel* model : p->objects())
			{
				if (model) { valueMap[model] = value; }
			}
		}
		else if (auto* pattern = dynamic_cast<PatternClip*>(clip))
		{
			auto patIndex = dynamic_cast<class PatternTrack*>(pattern->getTrack())->patternIndex();
			auto patStore = Engine::patternStore();

			TimePos patTime = time - clip->startPosition();
			patTime = std::min(patTime, clip->length());
			const auto patternLength = static_cast<qint64>(patStore->lengthOfPattern(patIndex)) * TimePos::ticksPerBar();
			if (patternLength <= 0 || patternLength > std::numeric_limits<tick_t>::max()) { continue; }
			// Match PatternTrack playback's signed source offset and wrapping.
			const auto sourceTime = static_cast<qint64>(patTime.getTicks()) - clip->startTimeOffset().getTicks();
			patTime = static_cast<int>((sourceTime % patternLength + patternLength) % patternLength);

			auto patValues = patStore->automatedValuesAt(patTime, patIndex);
			for (auto it=patValues.begin(); it != patValues.end(); it++)
			{
				// override old values, pattern track with the highest index takes precedence
				if (it.key()) { valueMap[it.key()] = it.value(); }
			}
		}
		else
		{
			continue;
		}
	}

	return valueMap;
};


} // namespace lmms

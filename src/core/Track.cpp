/*
 * Track.cpp - implementation of Track class
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

/** \file Track.cpp
 *  \brief Implementation of Track class
 */


#include "Track.h"
#include "NativeTrackSaveSnapshot.h"

#include <QDomElement>
#include <QCoreApplication>
#include <QThread>
#include <QPointer>
#include <QScopeGuard>
#include <QVariant>
#include <algorithm>
#include <memory>
#include <stdexcept>

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NativeLinkedControls.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "SampleTrack.h"
#include "Song.h"


namespace lmms
{

Track::Track(Type type, TrackContainer* tc)
	: Model(tc)
	, m_trackContainer(tc)
	, m_type(type)
	, m_name()
	, m_mutedModel(false, this, tr("Mute"))
	, m_soloModel(false, this, tr("Solo"))
	, m_clips()
{
	connect(&m_soloModel, &BoolModel::dataChanged, this, &Track::toggleSolo, Qt::DirectConnection);
	m_trackContainer->addTrack( this );
	m_height = -1;
}




Track::~Track()
{
	lock();
	emit destroyedTrack();

	while (!m_clips.empty())
	{
		delete m_clips.back();
	}

	m_trackContainer->removeTrack( this );
	unlock();
}

bool Track::isRenderExcluded() const
{
	return m_renderExcluded.load(std::memory_order_acquire) && Engine::getSong() && Engine::getSong()->isExporting();
}

bool Track::isMuted() const { return isMutedByModel() || isRenderExcluded(); }

bool Track::isRenderSelected() const
{
	return m_renderSelected.load(std::memory_order_acquire) && Engine::getSong() && Engine::getSong()->isExporting();
}




Track * Track::create( Type tt, TrackContainer * tc )
{
	if (!tc || tc->thread() != QThread::currentThread()) { throw std::logic_error("Native tracks require a live container on its owner thread"); }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const QPointer<TrackContainer> owner(tc);

	Track* created = nullptr;

	switch( tt )
	{
		case Type::Instrument: created = new class InstrumentTrack(tc); break;
		case Type::Pattern: created = new class PatternTrack(tc); break;
		case Type::Sample: created = new class SampleTrack(tc); break;
//		case Type::Event:
//		case Type::Video:
		case Type::Automation: created = new class AutomationTrack(tc); break;
		case Type::HiddenAutomation:
						created = new class AutomationTrack(tc, true); break;
		default: break;
	}
	if (!created) { return nullptr; }
	const QPointer<Track> target(created);
	bool adopted = false;
	const auto cleanup = qScopeGuard([&] { if (!adopted && target && target->trackContainer() == owner.data()) { delete target.data(); } });
	const auto ready = [&] {
		if (!owner || !target || target->trackContainer() != owner.data()
			|| (tt != Type::HiddenAutomation && std::find(owner->tracks().begin(), owner->tracks().end(), target.data()) == owner->tracks().end()))
		{ throw std::runtime_error("Native track retired or detached during creation"); }
	};
	ready();

	if (tc == Engine::patternStore())
	{
		target->createClipsForPattern(Engine::patternStore()->numOfPatterns() - 1);
		ready();
	}

	tc->updateAfterTrackAdd();
	ready();

	adopted = true; return target.data();
}




Track * Track::create( const QDomElement & element, TrackContainer * tc )
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const QPointer<TrackContainer> owner(tc);
	const QPointer<Track> target(create(
		static_cast<Type>( element.attribute( "type" ).toInt() ),
										tc ));
	bool adopted = false;
	const auto cleanup = qScopeGuard([&] { if (!adopted && target && target->trackContainer() == owner.data()) { delete target.data(); } });
	if (target)
	{
		target->restoreState(element);
		if (!owner || !target || target->trackContainer() != owner.data()
			|| (target->type() != Type::HiddenAutomation && std::find(owner->tracks().begin(), owner->tracks().end(), target.data()) == owner->tracks().end()))
		{ throw std::runtime_error("Native track retired or detached during restoration"); }
	}

	adopted = true; return target.data();
}




void Track::requireIdleForRemoval()
{
	if (!Engine::audioEngine() || !Engine::getSong()) { throw std::invalid_argument("Native engine is not initialized"); }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	if (isNoteRecording()) { throw std::invalid_argument("Complete native note recording before removing its track"); }
	for (auto clip : m_clips)
	{
		if (!clip || clip->getTrack() != this) { throw std::invalid_argument("Cannot remove an incomplete native clip list"); }
		clip->requireIdleForCopy();
	}
	if (const auto pattern = dynamic_cast<PatternTrack*>(this))
	{
		const int column = pattern->patternIndex();
		const auto store = Engine::patternStore();
		if (!store || trackContainer() != Engine::getSong() || column < 0
			|| PatternTrack::findPatternTrack(column) != pattern)
		{ throw std::invalid_argument("Cannot remove an inconsistent native pattern owner"); }
		for (auto row : store->tracks())
		{
			if (row->isNoteRecording()) { throw std::invalid_argument("Complete native pattern note recording before removing a column"); }
			if (row->numOfClips() != store->numOfPatterns())
			{ throw std::invalid_argument("Cannot remove an inconsistent native pattern column"); }
			// Removing one column also repositions the surviving cells.
			for (auto clip : row->getClips())
			{
				if (!clip || clip->getTrack() != row) { throw std::invalid_argument("Cannot remove an incomplete native pattern row"); }
				clip->requireIdleForCopy();
			}
		}
	}
}

Track* Track::clone()
{
	if (!QCoreApplication::instance() || QThread::currentThread() != thread()
		|| QThread::currentThread() != QCoreApplication::instance()->thread()) { return nullptr; }
	const FileResourceIO::EventSuspension events;
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const QPointer<TrackContainer> owner(m_trackContainer);
	const int selected = Engine::patternStore()->currentPattern();
	const auto previousErrors = Engine::getSong()->errorSummary();
	const auto previousFatal = Engine::getSong()->fatalErrorGeneration();
	try
	{
		const auto checkpoint = [] {
			QString error;
			if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		};
		checkpoint();
		const auto pattern = dynamic_cast<PatternTrack*>(this);
		const bool patternRow = m_trackContainer == Engine::patternStore();
		const int patterns = Engine::patternStore()->numOfPatterns();
		if (isNoteRecording()) { throw std::invalid_argument("Complete native note recording before cloning its track"); }
		for (auto clip : m_clips)
		{
			if (!clip) { throw std::invalid_argument("Cannot clone an incomplete native clip list"); }
			clip->requireIdleForCopy();
			clip->captureCopyControls();
		}
		if (patternRow && numOfClips() != patterns) { throw std::invalid_argument("Cannot clone an inconsistent pattern row"); }
		if (pattern)
		{
			for (auto row : Engine::patternStore()->tracks())
			{
				if (row->numOfClips() != patterns) { throw std::invalid_argument("Cannot clone an inconsistent pattern column"); }
				if (row->isNoteRecording()) { throw std::invalid_argument("Complete pattern note recording before adding a column"); }
				for (auto clip : row->getClips())
				{
					if (!clip) { throw std::invalid_argument("Cannot clone an incomplete pattern column"); }
					clip->requireIdleForCopy();
					clip->captureCopyControls();
				}
			}
		}
		// Track/plugin settings use their native serializers, while clip state
		// is copied directly so resident audio and object bindings are retained.
		DataFile doc(DataFile::Type::JournalData);
		auto parent = doc.createElement("clonedtrack"); doc.content().appendChild(parent); auto state = doc.createElement("track"); parent.appendChild(state);
		{
			NativeLinkedControls::SaveScope links(doc, NativeLinkedControls::trackModels(this));
			DataFile::ResourceSourceScope resources(doc, true, true);
			saveTrack(doc, state, false, false);
			if (hook()) { hook()->saveSettings(doc, state); }
			QString linkedError; if (!links.finish(parent, linkedError)) { DataFile::recordSerializationError(doc, linkedError); }
		}
		const auto error = DataFile::serializationError(doc);
		if (!error.isEmpty()) { throw std::runtime_error(error.toStdString()); }
		if (Engine::getSong()->errorSummary() != previousErrors) { throw std::runtime_error("Native track settings could not be serialized completely"); }
		checkpoint();
		if (pattern) { state.firstChildElement(nodeName()).removeAttribute("sourcepattern"); }
		auto copy = std::unique_ptr<Track>(create(type(), m_trackContainer));
		if (!copy) { throw std::runtime_error("Native track type could not be created"); }
		{
			JournallingObject::KeepIdentityScope identity;
			NativeLinkedControls::RestoreScope links(doc);
			DataFile::ResourceReadScope resources(doc);
			copy->loadTrack(state, true);
			if (copy->hook()) { copy->hook()->loadSettings(state); }
			QString linkedError; if (!links.finish(parent, linkedError)) { throw std::runtime_error(linkedError.toStdString()); }
		}
		if (Engine::getSong()->fatalErrorGeneration() != previousFatal) { throw std::runtime_error("Native track settings could not be restored completely"); }
		copy->m_height = m_height;
		for (int i = 0; i < numOfClips(); ++i)
		{
			checkpoint();
			m_clips[i]->copyTo(copy.get(), patternRow ? copy->getClips().at(i) : nullptr);
		}
		if (pattern)
		{
			const int source = pattern->patternIndex(); const int destination = static_cast<PatternTrack*>(copy.get())->patternIndex();
			for (auto row : Engine::patternStore()->tracks())
			{
				checkpoint();
				row->getClips().at(source)->copyTo(row, row->getClips().at(destination));
			}
			copy->setName(tr("Clone of %1").arg(name()));
		}
		checkpoint();
		const QPointer<Track> completed(copy.release());
		auto discard = qScopeGuard([&] { if (completed) { delete completed.data(); } });
		const auto registered = [&] {
			if (!owner || !completed || completed->trackContainer() != owner.data()
				|| std::find(owner->tracks().begin(), owner->tracks().end(), completed.data()) == owner->tracks().end())
			{ throw std::runtime_error("Native cloned track retired or detached during completion"); }
		};
		registered();
		if (completed->isSolo()) { completed->toggleSolo(); }
		registered();
		Engine::getSong()->setModified();
		registered();
		discard.dismiss(); return completed.data();
	}
	catch (const std::exception& exception)
	{
		Engine::getSong()->collectError(tr("Native track cloning failed: %1").arg(QString::fromUtf8(exception.what())));
	}
	catch (...) { Engine::getSong()->collectError(tr("Native track cloning failed with a nonstandard exception")); }
	Engine::patternStore()->setCurrentPattern(selected);
	return nullptr;
}


void Track::saveTrack(QDomDocument& doc, QDomElement& element, bool presetMode, bool includeClips, const NativeTrackSaveSnapshot* source)
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	if (m_loadingTrackState || m_loadingPresetState || (!source && m_savingTrackState) || !canSaveTrackState())
	{ throw std::logic_error("Native track serialization cannot reenter an active track transition"); }
	const QPointer<Track> receiver(this); const bool previous = m_savingTrackState; m_savingTrackState = true;
	const auto finish = qScopeGuard([&] { if (receiver) { receiver->m_savingTrackState = previous; } });
	std::unique_ptr<NativeTrackSaveSnapshot> captured;
	if (!source) { captured = std::make_unique<NativeTrackSaveSnapshot>(this, !presetMode && includeClips); source = captured.get(); }
	const auto check = [&] { source->validate(false); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } source->validate(false); };
	check();
	if (!presetMode)
	{
		element.setTagName( "track" );
	}
	element.setAttribute( "type", static_cast<int>(type()) );
	element.setAttribute( "name", name() );
	check();
	m_mutedModel.saveSettings( doc, element, "muted" );
	check();
	m_soloModel.saveSettings( doc, element, "solo" );
	check();
	// Save the mutedBeforeSolo value so we can recover the muted state if any solo was active (issue 5562)
	element.setAttribute( "mutedBeforeSolo", int(m_mutedBeforeSolo) );

	if( m_height >= MINIMAL_TRACK_HEIGHT )
	{
		element.setAttribute( "trackheight", m_height );
	}

	if (m_color.has_value())
	{
		element.setAttribute("color", m_color->name());
	}

	QDomElement tsDe = doc.createElement( nodeName() );
	check();
	// let actual track (InstrumentTrack, PatternTrack, SampleTrack etc.) save its settings
	element.appendChild( tsDe );
	saveTrackSpecificSettings(doc, tsDe, presetMode);
	check(); source->validate();

	if (presetMode || !includeClips)
	{
		return;
	}

	// now save settings of all Clip's
	std::vector<QPointer<Clip>> clips; for (auto clip : m_clips) { clips.emplace_back(clip); }
	for (const auto& clip : clips)
	{
		check(); if (!clip) { throw std::runtime_error("Native clip retired during track serialization"); }
		clip->saveState(doc, element);
		check();
	}
	source->validate();
}


void Track::loadTrack(const QDomElement& element, bool presetMode)
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events;
	if (m_loadingTrackState || m_savingTrackState || (!presetMode && m_loadingPresetState) || !canLoadTrackState())
	{ throw std::logic_error("Native track state restoration cannot reenter an active transition"); }
	const QPointer<Track> receiver(this); const bool loading = m_loadingTrackState; m_loadingTrackState = true;
	const QPointer<TrackContainer> owner(trackContainer());
	const auto restore = qScopeGuard([&] { if (receiver) { receiver->m_loadingTrackState = loading; } });
	const auto ready = [&] {
		if (!receiver || !owner || receiver->trackContainer() != owner.data()) { throw std::runtime_error("Native track retired during state restoration"); }
	};
	const auto check = [&] { ready(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready(); };
	const auto specific = [&](const QDomElement& node) {
		const bool previous = receiver->m_dispatchingTrackSpecificState; receiver->m_dispatchingTrackSpecificState = true;
		const auto finish = qScopeGuard([&] { if (receiver) { receiver->m_dispatchingTrackSpecificState = previous; } });
		receiver->loadTrackSpecificSettings(node);
		if (receiver) { receiver->m_dispatchingTrackSpecificState = previous; } check();
	};
	check();
	if( static_cast<Type>(element.attribute( "type" ).toInt()) != type() )
	{
		qWarning( "Current track-type does not match track-type of "
							"settings-node!\n" );
	}

	setName( element.hasAttribute( "name" ) ? element.attribute( "name" ) :
			element.firstChild().toElement().attribute( "name" ) );
	check();

	m_mutedModel.loadSettings( element, "muted" );
	check();
	m_soloModel.loadSettings( element, "solo" );
	check();
	// Get the mutedBeforeSolo value so we can recover the muted state if any solo was active.
	// Older project files that didn't have this attribute will set the value to false (issue 5562)
	m_mutedBeforeSolo = QVariant( element.attribute( "mutedBeforeSolo", "0" ) ).toBool();

	if (element.hasAttribute("color"))
	{
		setColor(QColor{element.attribute("color")});
		check();
	}

	if (presetMode)
	{
		QDomNode node = element.firstChild();
		while( !node.isNull() )
		{
			if( node.isElement() && node.nodeName() == nodeName() )
			{
				specific(node.toElement());
				break;
			}
			node = node.nextSibling();
		}

		return;
	}

	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		deleteClips();
	}
	check();

	QDomNode node = element.firstChild();
	while( !node.isNull() )
	{
		if( node.isElement() )
		{
			if( node.nodeName() == nodeName() )
			{
				specific(node.toElement());
			}
			else if( node.nodeName() != "muted"
			&& node.nodeName() != "solo"
			&& !node.toElement().attribute( "metadata" ).toInt() )
			{
				const QPointer<Clip> clip(createClip(TimePos(0))); check();
				if (!clip) { throw std::runtime_error("Native restored clip retired during construction"); }
				clip->restoreState( node.toElement() );
				check(); if (!clip) { throw std::runtime_error("Native restored clip retired during settings restoration"); }
			}
		}
		node = node.nextSibling();
	}

	int storedHeight = element.attribute( "trackheight" ).toInt();
	if( storedHeight >= MINIMAL_TRACK_HEIGHT )
	{
		m_height = storedHeight;
	}
}

void Track::savePreset(QDomDocument & doc, QDomElement & element)
{
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	if (m_savingTrackState || m_loadingTrackState || m_loadingPresetState || !canSaveTrackState()) { throw std::logic_error("Native preset serialization cannot reenter an active transition"); }
	const QPointer<Track> receiver(this); m_savingTrackState = true;
	const auto finish = qScopeGuard([&] { if (receiver) { receiver->m_savingTrackState = false; } });
	NativeTrackSaveSnapshot source(this, false);
	NativeLinkedControls::SaveScope links(doc, NativeLinkedControls::trackModels(this));
	saveTrack(doc, element, true, false, &source); source.validate();
	// Portable presets do not carry the track's live mute/solo state. Apply this
	// to reference-bearing model nodes as well as the legacy scalar attributes.
	for (const auto key : {"muted", "solo"})
	{ element.setAttribute(key, 0); auto node = element.firstChildElement(key); if (!node.isNull()) { node.setAttribute("value", 0); } }
	element.setAttribute("mutedBeforeSolo", 0);
	QString linkedError; if (!links.finish(element, linkedError)) { DataFile::recordSerializationError(doc, linkedError); }
	source.validate();
}

void Track::loadPreset(const QDomElement & element)
{
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	if (m_loadingPresetState || m_loadingTrackState || m_savingTrackState || !canLoadTrackState()) { throw std::logic_error("Native preset restoration cannot reenter an active track transition"); }
	const QPointer<Track> receiver(this); const QPointer<Song> song(Engine::getSong()); m_loadingPresetState = true;
	const auto restore = qScopeGuard([&] { if (receiver) { receiver->m_loadingPresetState = false; } });
	const auto ready = [&] { if (!receiver || !song || Engine::getSong() != song.data()) { throw std::runtime_error("Native preset owner retired during restoration"); } };
	QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready();
	const auto document = element.ownerDocument();
	if (NativeLinkedControls::isRestoring(document)) { loadTrack(element, true); return; }
	const bool solo = isSolo(), beforeSolo = m_mutedBeforeSolo;
	{
		JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope links(document);
		if (!element.firstChildElement("nativeLinkedControls").isNull()) { NativeLinkedControls::unlinkInternalGroups(NativeLinkedControls::trackModels(this)); }
		loadTrack(element, true);
		ready(); QString linkedError; if (!links.finish(element, linkedError)) { ready(); song->collectError("Native preset links did not restore: " + linkedError); } ready();
	}
	if (receiver && receiver->isSolo() != solo)
	{
		const bool loadedBefore = receiver->m_mutedBeforeSolo;
		if (solo) { receiver->m_mutedBeforeSolo = beforeSolo; }
		receiver->toggleSolo(); ready(); receiver->m_mutedBeforeSolo = loadedBefore;
	}
}

void Track::saveSettings(QDomDocument& doc, QDomElement& element)
{
	// Assume that everything should be saved if we are called through SerializingObject::saveSettings
	saveTrack(doc, element, false);
}

void Track::loadSettings(const QDomElement& element)
{
	// Assume that everything should be loaded if we are called through SerializingObject::loadSettings
	loadTrack(element, false);
}




Clip * Track::addClip( Clip * clip )
{
	m_clips.push_back( clip );

	emit clipAdded( clip );

	return clip; // just for convenience
}




void Track::removeClip( Clip * clip )
{
	clipVector::iterator it = std::find( m_clips.begin(), m_clips.end(), clip );
	if( it != m_clips.end() )
	{
		m_clips.erase( it );
		if( Engine::getSong() )
		{
			Engine::getSong()->updateLength();
			Engine::getSong()->setModified();
		}
	}
}


void Track::deleteClips()
{
	const QPointer<Track> receiver(this);
	while (receiver && !receiver->m_clips.empty())
	{
		delete receiver->m_clips.front();
	}
}




auto Track::getClip(std::size_t clipNum) -> Clip*
{
	if( clipNum < m_clips.size() )
	{
		return m_clips[clipNum];
	}
	printf( "called Track::getClip( %zu ), "
			"but Clip %zu doesn't exist\n", clipNum, clipNum );
	return createClip( clipNum * TimePos::ticksPerBar() );

}




int Track::getClipNum( const Clip * clip )
{
//	for( int i = 0; i < getTrackContentWidget()->numOfClips(); ++i )
	clipVector::iterator it = std::find( m_clips.begin(), m_clips.end(), clip );
	if( it != m_clips.end() )
	{
/*		if( getClip( i ) == _clip )
		{
			return i;
		}*/
		return it - m_clips.begin();
	}
	qWarning( "Track::getClipNum(...) -> _clip not found!\n" );
	return 0;
}




void Track::getClipsInRange( clipVector & clipV, const TimePos & start,
							const TimePos & end )
{
	for( Clip* clip : m_clips )
	{
		int s = clip->startPosition();
		int e = clip->endPosition();
		if( ( s <= end ) && ( e >= start ) )
		{
			// Clip is within given range
			// Insert sorted by Clip's position
			clipV.insert(std::upper_bound(clipV.begin(), clipV.end(), clip, Clip::comparePosition),
						clip);
		}
	}
}




void Track::swapPositionOfClips( int clipNum1, int clipNum2 )
{
	qSwap( m_clips[clipNum1], m_clips[clipNum2] );

	const TimePos pos = m_clips[clipNum1]->startPosition();

	m_clips[clipNum1]->movePosition( m_clips[clipNum2]->startPosition() );
	m_clips[clipNum2]->movePosition( pos );
}




void Track::createClipsForPattern(int pattern)
{
	while( numOfClips() < pattern + 1 )
	{
		TimePos position = TimePos( numOfClips(), 0 );
		Clip * clip = createClip( position );
		clip->changeLength( TimePos( 1, 0 ) );
	}
}




//! @todo If we stepped through this list last to first, and the list was in ascending order by Clip time, once we hit
//! a Clip that was earlier than the insert time, we could fall out of the loop early.
void Track::insertBar( const TimePos & pos )
{
	// we'll increase the position of every Clip, positioned behind pos, by
	// one bar
	for (const auto& clip : m_clips)
	{
		if (clip->startPosition() >= pos)
		{
			clip->movePosition(clip->startPosition() + TimePos::ticksPerBar());
		}
	}
}




void Track::removeBar( const TimePos & pos )
{
	// we'll decrease the position of every Clip, positioned behind pos, by
	// one bar
	for (const auto& clip : m_clips)
	{
		if (clip->startPosition() >= pos)
		{
			clip->movePosition(clip->startPosition() - TimePos::ticksPerBar());
		}
	}
}




bar_t Track::length() const
{
	// find last end-position
	tick_t last = 0;
	for (const auto& clip : m_clips)
	{
		if (Engine::getSong()->isExporting() && clip->isMuted())
		{
			continue;
		}

		const tick_t cur = clip->endPosition();
		if( cur > last )
		{
			last = cur;
		}
	}

	// A clip ending inside a bar still occupies that bar. Truncating here made
	// short sample-only arrangements appear empty to the native renderer.
	const auto ticksPerBar = TimePos::ticksPerBar();
	return last / ticksPerBar + (last % ticksPerBar != 0);
}



void Track::toggleSolo()
{
	// Serialized masks and their pre-solo caches are authoritative. In particular,
	// linked restoration may publish model notifications after loadTrack returns.
	if (m_loadingTrackState || NativeLinkedControls::isRestoring() || !Engine::audioEngine()
		|| !Engine::getSong() || Engine::getSong()->isLoadingProject()) { return; }
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	const QPointer<Track> receiver(this);
	const QPointer<TrackContainer> owner(m_trackContainer);
	if (!owner || owner->m_updatingTrackSolo) { return; }
	const auto attached = [&](const QPointer<Track>& track) {
		return owner && track && track->trackContainer() == owner.data()
			&& std::find(owner->tracks().begin(), owner->tracks().end(), track.data()) != owner->tracks().end();
	};
	if (!attached(receiver)) { return; }
	struct State { QPointer<Track> track; bool muted, solo; };
	std::vector<State> peers;
	peers.reserve(owner->tracks().size());
	for (auto track : owner->tracks()) { peers.push_back({track, track->isMutedByModel(), track->isSolo()}); }
	const bool solo = isSolo();
	const bool soloBefore = std::any_of(peers.begin(), peers.end(), [&](const State& entry) {
		return entry.track != receiver && entry.solo;
	});
	const bool legacy = ConfigManager::inst()->value("app", "sololegacybehavior", "0").toInt();
	owner->m_updatingTrackSolo = true;
	const auto restore = qScopeGuard([&] { if (owner) { owner->m_updatingTrackSolo = false; } });
	// Snapshot all pre-solo values before notifications or linked writes can
	// change a later peer. Internal solo clears do not start nested transitions.
	if (solo && !soloBefore)
	{
		for (const auto& entry : peers) { if (attached(entry.track)) { entry.track->m_mutedBeforeSolo = entry.muted; } }
	}
	for (const auto& entry : peers)
	{
		if (!attached(receiver)) { return; }
		const auto track = entry.track;
		if (!attached(track)) { continue; }
		if (solo)
		{
			if (track == receiver) { track->setMuted(false); }
			else if (legacy || track->type() != Type::Automation) { track->setMuted(true); }
			if (!attached(receiver)) { return; }
			if (attached(track) && track != receiver) { track->m_soloModel.setValue(false); }
		}
		else if (!soloBefore && (legacy || track->type() != Type::Automation)) { track->setMuted(track->m_mutedBeforeSolo); }
	}
}

void Track::setColor(const std::optional<QColor>& color)
{
	m_color = color;
	emit colorChanged();
}

BoolModel *Track::getMutedModel()
{
	return &m_mutedModel;
}

void Track::setName(const QString& newName)
{
	if (m_name != newName)
	{
		const QPointer<Track> receiver(this);
		m_name = newName;

		if (auto song = Engine::getSong())
		{
			song->setModified();
		}

		if (receiver) { emit receiver->nameChanged(); }
	}
}

} // namespace lmms

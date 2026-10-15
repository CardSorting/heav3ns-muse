/*
 * PatternTrack.cpp - a track representing a pattern in the PatternStore
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
#include "PatternTrack.h"

#include <QDomElement>
#include <utility>
#include <vector>
#include <limits>

#include "AudioEngine.h"
#include "Engine.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrackView.h"
#include "PlayHandle.h"


namespace lmms
{


PatternTrack::infoMap PatternTrack::s_infoMap;


PatternTrack::PatternTrack(TrackContainer* tc) :
	Track(Track::Type::Pattern, tc)
{
	auto store = Engine::patternStore();
	const int selected = store->currentPattern();
	const int patternNum = s_infoMap.size();
	std::vector<std::pair<Track*, int>> previousColumns;
	previousColumns.reserve(store->tracks().size());
	for (auto row : store->tracks()) { previousColumns.emplace_back(row, row->numOfClips()); }
	try
	{
		s_infoMap[this] = patternNum;
		setName(tr("Pattern %1").arg(patternNum));
		store->createClipsForPattern(patternNum);
		store->setCurrentPattern(patternNum);
		store->updateComboBox();
		connect(this, &Track::nameChanged, store, &PatternStore::updateComboBox);
	}
	catch (...)
	{
		// A failed derived constructor will run only Track's destructor. Undo
		// its pattern registration and every appended cell before that happens.
		for (const auto& [row, count] : previousColumns)
		{
			while (row->numOfClips() > count) { delete row->getClips().back(); }
		}
		s_infoMap.remove(this);
		trackContainer()->removeTrack(this);
		store->updateComboBox();
		store->setCurrentPattern(selected);
		throw;
	}
}




PatternTrack::~PatternTrack()
{
	Engine::audioEngine()->removePlayHandlesOfTypes( this,
					PlayHandle::Type::NotePlayHandle
					| PlayHandle::Type::InstrumentPlayHandle
					| PlayHandle::Type::SamplePlayHandle );

	const int pattern = s_infoMap[this];
	Engine::patternStore()->removePattern(pattern);
	for( infoMap::iterator it = s_infoMap.begin(); it != s_infoMap.end();
									++it )
	{
		if (it.value() > pattern)
		{
			--it.value();
		}
	}
	s_infoMap.remove( this );

	// remove us from the Song and update the pattern selection combobox to reflect the change
	trackContainer()->removeTrack( this );
	Engine::patternStore()->updateComboBox();
}




// play _frames frames of given Clip within starting with _start
bool PatternTrack::play( const TimePos & _start, const f_cnt_t _frames,
					const f_cnt_t _offset, int _clip_num )
{
	if( isMuted() )
	{
		return false;
	}

	if( _clip_num >= 0 )
	{
		return Engine::patternStore()->play(_start, _frames, _offset, s_infoMap[this]);
	}

	clipVector clips;
	getClipsInRange( clips, _start, _start + static_cast<int>( _frames / Engine::framesPerTick() ) );

	if( clips.size() == 0 )
	{
		return false;
	}

	TimePos lastPosition;
	TimePos lastLength;
	qint64 lastOffset = 0;
	qint64 lastPatternLength = 0;
	for (const auto& clip : clips)
	{
		if (!clip->isMuted() && clip->startPosition() >= lastPosition)
		{
			lastPosition = clip->startPosition();
			lastLength = clip->length();
			lastPatternLength = static_cast<qint64>(Engine::patternStore()->lengthOfPattern(static_cast<PatternClip*>(clip)->patternIndex()))
				* TimePos::ticksPerBar();
			if (lastPatternLength <= 0 || lastPatternLength > std::numeric_limits<tick_t>::max()) { return false; }
			lastOffset = lastPatternLength - clip->startTimeOffset().getTicks() % lastPatternLength;
			if (lastOffset == lastPatternLength) { lastOffset = 0; }
		}
	}

	if (lastPatternLength > 0 && _start - lastPosition < lastLength)
	{
		const auto sourceTime = static_cast<qint64>(_start.getTicks()) - lastPosition.getTicks() + lastOffset;
		// Keep native signed pre-roll semantics, narrowing only after wrapping.
		const auto wrapped = static_cast<tick_t>(sourceTime % lastPatternLength);
		return Engine::patternStore()->play(wrapped, _frames, _offset, s_infoMap[this]);
	}
	return false;
}




gui::TrackView* PatternTrack::createView(gui::TrackContainerView* tcv)
{
	return new gui::PatternTrackView(this, tcv);
}




Clip* PatternTrack::createClip(const TimePos & pos)
{
	auto pc = new PatternClip(this);
	pc->movePosition(pos);
	return pc;
}




void PatternTrack::saveTrackSpecificSettings(QDomDocument& doc, QDomElement& _this, bool presetMode)
{
//	_this.setAttribute( "icon", m_trackLabel->pixmapFile() );
/*	_this.setAttribute( "current", s_infoMap[this] ==
					engine::getPatternEditor()->currentPattern() );*/
	if( s_infoMap[this] == 0 &&
			_this.parentNode().parentNode().nodeName() != "clonedtrack" &&
			_this.parentNode().parentNode().nodeName() != "journaldata" )
	{
		Engine::patternStore()->saveState(doc, _this);
	}
	// If we are creating drag-n-drop data for Track::clone() only save pattern ID, not pattern content
	if (_this.parentNode().parentNode().nodeName() == "clonedtrack")
	{
		_this.setAttribute("sourcepattern", s_infoMap[this]);
	}
}




void PatternTrack::loadTrackSpecificSettings(const QDomElement& _this)
{
/*	if( _this.attribute( "icon" ) != "" )
	{
		m_trackLabel->setPixmapFile( _this.attribute( "icon" ) );
	}*/

	// If data was created by Track::clone(), do not add any tracks to the pattern(-editor)
	// instead create a new copy of the clip on each track
	if (_this.hasAttribute("sourcepattern"))
	{
		const int src = _this.attribute("sourcepattern").toInt();
		const int dst = s_infoMap[this];
		// copy clips of all tracks from source pattern (at bar "src") to destination
		// clips (which are created if they do not exist yet)
		for (const auto& track : Engine::patternStore()->tracks())
		{
			Clip::copyStateTo(track->getClip(src), track->getClip(dst));
		}
		setName( tr( "Clone of %1" ).arg(
					_this.parentNode().toElement().attribute( "name" ) ) );
	}
	else
	{
		QDomNode node = _this.namedItem(
					TrackContainer::classNodeName() );
		if( node.isElement() )
		{
			Engine::patternStore()->restoreState(node.toElement());
		}
	}
/*	doesn't work yet because PatternTrack-ctor also sets current pattern so if
	pattern tracks are created after this function is called, this doesn't
	help at all....
	if( _this.attribute( "current" ).toInt() )
	{
		engine::getPatternEditor()->setCurrentPattern( s_infoMap[this] );
	}*/
}




// return pointer to PatternTrack specified by pattern_num
PatternTrack* PatternTrack::findPatternTrack(int pattern_num)
{
	for( infoMap::iterator it = s_infoMap.begin(); it != s_infoMap.end();
									++it )
	{
		if (it.value() == pattern_num)
		{
			return it.key();
		}
	}
	return nullptr;
}




void PatternTrack::swapPatternTracks(Track* track1, Track* track2)
{
	auto t1 = dynamic_cast<PatternTrack*>(track1);
	auto t2 = dynamic_cast<PatternTrack*>(track2);
	if( t1 != nullptr && t2 != nullptr )
	{
		qSwap( s_infoMap[t1], s_infoMap[t2] );
		Engine::patternStore()->swapPattern(s_infoMap[t1], s_infoMap[t2]);
		Engine::patternStore()->setCurrentPattern(s_infoMap[t1]);
	}
}


} // namespace lmms

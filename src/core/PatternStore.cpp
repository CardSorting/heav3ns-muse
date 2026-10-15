/*
 * PatternStore.cpp - model-component of Pattern Editor
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


#include "PatternStore.h"

#include <algorithm>
#include <limits>
#include "AudioEngine.h"
#include "Clip.h"
#include "Engine.h"
#include "PatternTrack.h"
#include "Song.h"

namespace lmms
{


PatternStore::PatternStore() :
	TrackContainer(),
	m_patternComboBoxModel(this)
{
	setType(Type::Pattern);
	connect(Engine::getSong(), &Song::timeSignatureChanged, this, &PatternStore::fixIncorrectPositions);
}




bool PatternStore::play(TimePos start, f_cnt_t frames, f_cnt_t offset, int clipNum)
{
	bool notePlayed = false;

	if (clipNum < 0 || clipNum >= numOfPatterns()) { return false; }
	const auto length = static_cast<qint64>(lengthOfPattern(clipNum)) * TimePos::ticksPerBar();
	if (length <= 0 || length > std::numeric_limits<tick_t>::max())
	{
		return false;
	}

	start = start % static_cast<tick_t>(length);

	const TrackList& tl = tracks();
	for (Track * t : tl)
	{
		if (clipNum >= t->numOfClips() || !t->getClips()[clipNum]) { continue; }
		if (t->play(start, frames, offset, clipNum))
		{
			notePlayed = true;
		}
	}

	return notePlayed;
}




void PatternStore::updateAfterTrackAdd()
{
	if (numOfPatterns() == 0 && !Engine::getSong()->isLoadingProject())
	{
		Engine::getSong()->addPatternTrack();
	}
}




bar_t PatternStore::lengthOfPattern(int pattern) const
{
	const auto meter = TimePos::ticksPerBar();
	if (meter <= 0) { return 0; }
	// Retain the one-bar metadata fallback for absent patterns. Reading a
	// missing or negative column must never invoke the lazy cell accessor.
	if (pattern < 0 || pattern >= numOfPatterns()) { return 1; }
	qint64 maxLength = meter;

	const TrackList & tl = tracks();
	for (Track * t : tl)
	{
		// Don't create Clips here if they don't exist, and only takes into account MIDI clips
		// because the length of Automation / Sample clips is defined based on this method's output.
		if (pattern < t->numOfClips() && t->type() == Track::Type::Instrument && t->getClips()[pattern])
		{
			maxLength = std::max(maxLength, static_cast<qint64>(t->getClips()[pattern]->length().getTicks()));
		}
	}

	return static_cast<bar_t>((maxLength + meter - 1) / meter);
}




int PatternStore::numOfPatterns() const
{
	return Engine::getSong()->countTracks(Track::Type::Pattern);
}




void PatternStore::removePattern(int pattern)
{
	if (pattern < 0 || pattern >= numOfPatterns()) { return; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const TrackList& tl = tracks();
	for (Track * t : tl)
	{
		// Columns are owned by their native indices, not by a fixed 4/4 time.
		// Do not create a missing clip while retiring a sparse loaded pattern.
		if (pattern < t->numOfClips()) { delete t->getClips()[pattern]; }
		for (int i = 0; i < t->numOfClips(); ++i) { t->getClips()[i]->movePosition(TimePos(i, 0)); }
	}
	if (pattern <= currentPattern())
	{
		setCurrentPattern(std::max(currentPattern() - 1, 0));
	}
}




void PatternStore::swapPattern(int pattern1, int pattern2)
{
	const TrackList& tl = tracks();
	for (Track * t : tl)
	{
		t->swapPositionOfClips(pattern1, pattern2);
	}
	updateComboBox();
}




void PatternStore::updatePatternTrack(Clip* clip)
{
	if (!clip || !clip->getTrack() || clip->getTrack()->trackContainer() != this) { return; }
	const auto& clips = clip->getTrack()->getClips();
	const auto found = std::find(clips.begin(), clips.end(), clip);
	if (found == clips.end()) { return; }
	PatternTrack * t = PatternTrack::findPatternTrack(static_cast<int>(found - clips.begin()));
	if (t != nullptr)
	{
		t->dataChanged();
	}
	emit trackUpdated();
}




void PatternStore::fixIncorrectPositions()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const TrackList& tl = tracks();
	for (Track * t : tl)
	{
		for (int i = 0; i < numOfPatterns(); ++i)
		{
			t->getClip(i)->movePosition(TimePos(i, 0));
		}
	}
}




void PatternStore::play()
{
	if (Engine::getSong()->playMode() != Song::PlayMode::Pattern)
	{
		Engine::getSong()->playPattern();
	}
	else
	{
		Engine::getSong()->togglePause();
	}
}




void PatternStore::stop()
{
	Engine::getSong()->stop();
}




void PatternStore::updateComboBox()
{
	const int curPattern = currentPattern();

	m_patternComboBoxModel.clear();

	for (int i = 0; i < numOfPatterns(); ++i)
	{
		PatternTrack* pt = PatternTrack::findPatternTrack(i);
		m_patternComboBoxModel.addItem(pt->name());
	}
	setCurrentPattern(curPattern);
}




void PatternStore::createClipsForPattern(int pattern)
{
	const TrackList& tl = tracks();
	for (Track * t : tl)
	{
		t->createClipsForPattern(pattern);
	}
}

AutomatedValueMap PatternStore::automatedValuesAt(TimePos time, int clipNum) const
{
	if (clipNum < 0 || clipNum >= numOfPatterns() || time.getTicks() < 0) { return {}; }
	const auto lengthTicks = static_cast<qint64>(lengthOfPattern(clipNum)) * TimePos::ticksPerBar();
	if (lengthTicks <= 0 || lengthTicks > std::numeric_limits<tick_t>::max()) { return {}; }
	if (time > lengthTicks)
	{
		time = static_cast<tick_t>(lengthTicks);
	}

	// TrackContainer evaluates an explicitly selected column in pattern-local
	// time. The editor column's placement must not advance its automation.
	return TrackContainer::automatedValuesAt(time, clipNum);
}


} // namespace lmms

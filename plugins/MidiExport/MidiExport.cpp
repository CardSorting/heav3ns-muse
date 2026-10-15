/*
 * MidiExport.cpp - support for exporting MIDI files
 *
 * Copyright (c) 2015 Mohamed Abdel Maksoud <mohamed at amaksoud.com>
 * Copyright (c) 2017 Hyunjin Song <tteu.ingog/at/gmail.com>
 * Copyright (c) 2020 EmoonX
 * Copyright (c) 2026 Dalton Messmer <messmer.dalton/at/gmail.com>
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

#include "MidiExport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
#include <QDebug>
#include <QThread>

#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiFile.h"
#include "Note.h"
#include "PatternClip.h"
#include "PatternTrack.h"
#include "Song.h"
#include "plugin_export.h"

namespace lmms
{

extern "C"
{
Plugin::Descriptor PLUGIN_EXPORT midiexport_plugin_descriptor =
{
	LMMS_STRINGIFY(PLUGIN_NAME),
	"MIDI Export",
	QT_TRANSLATE_NOOP("PluginBrowser", "Filter for exporting MIDI files from LMMS"),
	"Mohamed Abdel Maksoud <mohamed at amaksoud.com> and Hyunjin Song <tteu.ingog/at/gmail.com>",
	0x0100,
	Plugin::Type::ExportFilter,
	nullptr,
	nullptr,
	nullptr,
};

PLUGIN_EXPORT Plugin* lmms_plugin_main(Model*, void*) { return new MidiExport{}; }
} // extern "C"

namespace
{
// Native positions are signed 32-bit ticks. Do intermediate clip/loop arithmetic
// in a wider type and reject an unrepresentable result before publishing a file.
using WideTick = std::int64_t;

void checkWork()
{
	QString error;
	if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
}

struct ExportNote
{
	std::uint8_t pitch, velocity;
	WideTick time, duration;
	bool step;
};

struct Segment
{
	WideTick start, end, origin;
};

struct PatternSchedule
{
	WideTick period;
	std::vector<Segment> segments;
};
using Schedules = std::map<int, PatternSchedule>;

std::vector<ExportNote> readNotes(const MidiClip& clip, WideTick basePitch, double baseVolume, bool pattern)
{
	std::vector<ExportNote> result;
	if (clip.isMuted() || clip.length() <= 0) { return result; }
	const WideTick offset = clip.startTimeOffset();
	const WideTick start = -offset;
	const WideTick end = static_cast<WideTick>(clip.length().getTicks()) - offset;
	std::size_t count = 0;
	for (const auto note : clip.notes())
	{
		if ((count++ % 1024) == 0) { checkWork(); }
		const WideTick pos = note->pos(), length = note->length();
		const bool step = note->type() == Note::Type::Step;
		if (length == 0 || (!step && length < 0) || pos >= end) { continue; }
		const double volume = note->getVolume() * baseVolume * (127.0 / 200.0);
		if (!std::isfinite(volume)) { throw std::out_of_range("Nonfinite native MIDI note volume"); }
		// Clamp in the wide source domain before narrowing to a MIDI data byte.
		const auto velocity = static_cast<std::uint8_t>(std::lround(std::clamp(volume, 0., 127.)));
		if (velocity == 0) { continue; }
		const auto pitch = static_cast<std::uint8_t>(std::clamp<WideTick>(basePitch + note->key(), 0, 127));
		const auto append = [&](WideTick onset)
		{
			const WideTick duration = step ? std::min<WideTick>(MidiExport::DefaultBeatLength, end - onset)
				: std::min(pos + length, end) - onset;
			if (duration > 0) { result.push_back({pitch, velocity, onset, duration, step}); }
		};
		if (pattern)
		{
			// PatternStore passes loop-local time directly to InstrumentTrack.
			// A cell offset does not shift its primary onsets; native playback
			// also retriggers a note spanning the cell's trim boundary.
			if (pos >= 0) { append(pos); }
			if (start >= 0 && pos < start && pos + length > start) { append(start); }
		}
		else if (pos >= start) { append(pos); }
		else if (pos + length > start) { append(start); }
	}
	std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
	WideTick next = end, current = end;
	for (auto it = result.rbegin(); it != result.rend(); ++it)
	{
		if (it->time < current) { next = current; current = it->time; }
		if (it->step) { it->duration = std::min(it->duration, next - it->time); }
	}
	return result;
}

void writeNote(MidiFile::Track& output, const ExportNote& note, WideTick time, WideTick duration)
{
	if (time < 0) { duration += time; time = 0; }
	if (duration <= 0) { return; }
	if (time > std::numeric_limits<tick_t>::max() || duration > std::numeric_limits<tick_t>::max() - time)
	{ throw std::out_of_range("Exported MIDI note exceeds the native tick range"); }
	output.addNoteTicks(note.pitch, note.velocity, static_cast<tick_t>(time), static_cast<tick_t>(duration));
}

WideTick patternPeriod(int index, const TrackContainer::TrackList& rows)
{
	const WideTick bar = TimePos::ticksPerBar();
	if (bar <= 0) { throw std::out_of_range("Invalid native pattern meter"); }
	WideTick length = bar;
	for (auto track : rows)
	{
		checkWork();
		if (track->type() != Track::Type::Instrument || index >= track->numOfClips()) { continue; }
		// Same maximum-of-all-instrument-rows rule as PatternStore::lengthOfPattern,
		// without getClip(), which can create a missing native cell.
		length = std::max(length, static_cast<WideTick>(track->getClips()[index]->length().getTicks()));
	}
	return ((length + bar - 1) / bar) * bar;
}

std::vector<Segment> patternSegments(PatternTrack& track)
{
	struct Placement { WideTick start, end, origin; };
	struct Boundary { WideTick time; std::size_t index; bool begin; };
	std::vector<Placement> placements;
	std::vector<Boundary> boundaries;
	if (track.isMuted()) { return {}; }
	for (const auto clip : track.getClips())
	{
		checkWork();
		if (!dynamic_cast<const PatternClip*>(clip) || clip->isMuted() || clip->length() <= 0) { continue; }
		const WideTick start = clip->startPosition();
		const WideTick end = start + clip->length().getTicks();
		if (end <= 0) { continue; }
		const auto index = placements.size();
		placements.push_back({start, end, start + clip->startTimeOffset().getTicks()});
		boundaries.push_back({std::max<WideTick>(0, start), index, true});
		boundaries.push_back({end, index, false});
	}
	std::sort(boundaries.begin(), boundaries.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
	// Native PatternTrack chooses the latest starting active clip; stored order
	// breaks equal-start ties. An earlier long placement resumes afterwards.
	std::set<std::pair<WideTick, std::size_t>> active;
	std::vector<Segment> result;
	for (std::size_t i = 0; i < boundaries.size();)
	{
		checkWork();
		const auto time = boundaries[i].time;
		do
		{
			const auto& boundary = boundaries[i];
			const auto key = std::pair{placements[boundary.index].start, boundary.index};
			if (boundary.begin) { active.insert(key); } else { active.erase(key); }
			++i;
		} while (i < boundaries.size() && boundaries[i].time == time);
		if (i == boundaries.size() || active.empty()) { continue; }
		const auto& placement = placements[active.rbegin()->second];
		if (!result.empty() && result.back().end == time && result.back().origin == placement.origin)
		{ result.back().end = boundaries[i].time; }
		else { result.push_back({time, boundaries[i].time, placement.origin}); }
	}
	return result;
}

void writePattern(MidiFile::Track& output, const std::vector<ExportNote>& notes, const PatternSchedule& schedule)
{
	std::size_t count = 0;
	for (const auto& segment : schedule.segments)
	{
		checkWork();
		for (const auto& note : notes)
		{
			if ((count++ % 1024) == 0) { checkWork(); }
			if (note.time >= schedule.period) { continue; }
			const WideTick base = segment.origin + note.time;
			const WideTick distance = segment.start - base;
			// Signed ceiling division: truncation alone starts some notes before
			// a trimmed/overlapped segment and loses the phase of negative offsets.
			const WideTick cycle = distance / schedule.period + (distance % schedule.period > 0 ? 1 : 0);
			for (WideTick time = base + cycle * schedule.period; time < segment.end; time += schedule.period)
			{
				if ((count++ % 1024) == 0) { checkWork(); }
				writeNote(output, note, time, note.step ? std::min(note.duration, segment.end - time) : note.duration);
			}
		}
	}
}

void writeTrack(InstrumentTrack& track, MidiFile::Track& output, int tempo, int masterPitch,
	std::uint8_t& channel, const Schedules* schedules)
{
	checkWork();
	const auto instrument = track.instrument();
	const auto patch = instrument ? instrument->midiPatch().value_or(MidiPatch{}) : MidiPatch{};
	if (patch.bank == 128) { output.setChannel(9); }
	else
	{
		output.setChannel(channel);
		channel = (channel + 1) % 16;
		if (channel == 9) { ++channel; }
	}
	output.addTempo(tempo, 0);
	output.addName(track.name().toStdString(), 0);
	output.addPatch(patch, 0);
	const WideTick basePitch = static_cast<WideTick>(DefaultBaseKey) - track.baseNoteModel()->value()
		+ (track.useMasterPitchModel()->value() ? masterPitch : 0);
	const double baseVolume = track.volumeModel()->value() / 100.;
	if (!track.isMuted())
	{
		const auto& clips = track.getClips();
		for (std::size_t index = 0; index < clips.size(); ++index)
		{
			checkWork();
			const auto clip = dynamic_cast<const MidiClip*>(clips[index]);
			if (!clip) { continue; }
			if (schedules)
			{
				if (index > static_cast<std::size_t>(std::numeric_limits<int>::max())) { break; }
				const auto found = schedules->find(static_cast<int>(index));
				if (found != schedules->end()) { writePattern(output, readNotes(*clip, basePitch, baseVolume, true), found->second); }
			}
			else
			{
				const WideTick origin = static_cast<WideTick>(clip->startPosition().getTicks()) + clip->startTimeOffset().getTicks();
				std::size_t count = 0;
				for (const auto& note : readNotes(*clip, basePitch, baseVolume, false))
				{
					if ((count++ % 1024) == 0) { checkWork(); }
					writeNote(output, note, origin + note.time, note.duration);
				}
			}
		}
	}
	output.writeToBuffer();
}
} // namespace

MidiExport::MidiExport() : ExportFilter(&midiexport_plugin_descriptor) { }

bool MidiExport::tryExport(const TrackContainer::TrackList& tracks,
	const TrackContainer::TrackList& patternStoreTracks,
	int tempo, int masterPitch, const std::filesystem::path& filePath)
{
	try
	{
		if (Engine::getSong() && QThread::currentThread() != Engine::getSong()->thread())
		{ throw std::runtime_error("MIDI export requires the native model owner thread"); }
		const FileResourceIO::EventSuspension noReentry;
		const auto guard = Engine::audioEngine() ? Engine::audioEngine()->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
		checkWork();
		Schedules schedules;
		for (auto track : tracks)
		{
			checkWork();
			if (auto pattern = dynamic_cast<PatternTrack*>(track))
			{
				const auto index = pattern->patternIndex();
				if (index < 0) { throw std::out_of_range("Invalid native pattern index"); }
				schedules.emplace(index, PatternSchedule{patternPeriod(index, patternStoreTracks), patternSegments(*pattern)});
			}
		}
		const auto isInstrument = [](const Track* track) { return track->type() == Track::Type::Instrument; };
		const auto count = std::ranges::count_if(tracks, isInstrument) + std::ranges::count_if(patternStoreTracks, isInstrument);
		// An empty song still needs a valid conductor track with tempo and EOT.
		MidiFile file(filePath, std::max<std::size_t>(1, count));
		file.m_header.writeToBuffer();
		std::size_t index = 0;
		std::uint8_t channel = 0; // Every export starts a fresh channel allocation.
		for (auto track : tracks)
		{
			if (isInstrument(track)) { writeTrack(static_cast<InstrumentTrack&>(*track), file.m_tracks[index++], tempo, masterPitch, channel, nullptr); }
		}
		for (auto track : patternStoreTracks)
		{
			if (isInstrument(track)) { writeTrack(static_cast<InstrumentTrack&>(*track), file.m_tracks[index++], tempo, masterPitch, channel, &schedules); }
		}
		if (index == 0)
		{
			file.m_tracks.front().addTempo(tempo, 0);
			file.m_tracks.front().addName("LMMS", 0);
			file.m_tracks.front().writeToBuffer();
		}
		return file.writeAllToStream();
	}
	catch (const std::exception& error) { qWarning() << "MIDI export failed:" << error.what(); }
	catch (...) { qWarning() << "MIDI export failed with an unknown native exception"; }
	return false;
}

} // namespace lmms

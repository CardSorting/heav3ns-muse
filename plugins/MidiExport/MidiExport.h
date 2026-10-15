/*
 * MidiExport.h - support for exporting MIDI files
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

#ifndef LMMS_MIDI_EXPORT_H
#define LMMS_MIDI_EXPORT_H

#include "ExportFilter.h"

namespace lmms
{

// Reads the native project models. No plugin state serialization or DSP is
// needed to export the editable note arrangement.
class MidiExport : public ExportFilter
{
public:
	MidiExport();
	~MidiExport() override = default;

	// Step-note playback length depends on InstrumentTrack::beatLen and a live
	// NotePlayHandle. MIDI uses this upper bound, shortened at the next onset
	// or clip boundary; it is not a measurement of the instrument's sound.
	static constexpr int DefaultBeatLength = 1500;

	bool tryExport(const TrackContainer::TrackList& tracks,
		const TrackContainer::TrackList& patternStoreTracks,
		int tempo, int masterPitch, const std::filesystem::path& filePath) override;

private:
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
};

} // namespace lmms

#endif // LMMS_MIDI_EXPORT_H

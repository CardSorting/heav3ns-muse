/* Native MIDI note recording state. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_MIDI_NOTE_RECORDER_H
#define LMMS_MIDI_NOTE_RECORDER_H
#include <array>
#include <limits>
#include <optional>
#include "Note.h"
#include "lmms_export.h"

namespace lmms
{
class MidiClip;

// Operated on the model thread. Input snapshots come from native NotePlayHandles;
// no widget, hardware timestamp epoch, or borrowed callback storage is required.
class LMMS_EXPORT MidiNoteRecorder
{
public:
	void reset(std::size_t maximumNotes = std::numeric_limits<std::size_t>::max());
	void configurePitch(float baselineCents, std::size_t maximumPoints);
	bool startNote(const Note& note, TimePos position);
	void recordPitch(const Note& elapsedNote, float pitchCents);
	std::optional<Note> finishNote(const Note& note, int quantization = 0);
	std::size_t acceptedNotes() const { return m_accepted; }
	std::size_t pitchPoints() const { return m_pitchPoints; }
	std::size_t pendingNotes() const;
	static TimePos positionInClip(const Note& note, const MidiClip& clip, bool songPosition);
private:
	static Note completed(const Note& onset, const Note& release, int quantization, const std::shared_ptr<DetuningHelper>& detuning);
	std::array<std::optional<Note>, NumKeys> m_pending;
	std::array<std::shared_ptr<DetuningHelper>, NumKeys> m_recordedPitch;
	std::size_t m_maximum = std::numeric_limits<std::size_t>::max();
	std::size_t m_accepted = 0;
	std::size_t m_maximumPitchPoints = 0, m_pitchPoints = 0;
	float m_pitchBaseline = 0;
};
}
#endif

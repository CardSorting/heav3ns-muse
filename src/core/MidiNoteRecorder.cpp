/* Native MIDI note recording state. SPDX-License-Identifier: GPL-2.0-or-later */
#include "MidiNoteRecorder.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "DetuningHelper.h"
#include "MidiClip.h"

namespace lmms
{
void MidiNoteRecorder::reset(std::size_t maximumNotes)
{
	if (maximumNotes == 0) { throw std::invalid_argument("MIDI recording requires a positive note limit"); }
	for (auto& note : m_pending) { note.reset(); }
	for (auto& curve : m_recordedPitch) { curve.reset(); }
	m_maximum = maximumNotes; m_accepted = 0;
	m_maximumPitchPoints = m_pitchPoints = 0; m_pitchBaseline = 0;
}

void MidiNoteRecorder::configurePitch(float baselineCents, std::size_t maximumPoints)
{
	if (!std::isfinite(baselineCents) || maximumPoints == 0 || m_accepted != 0)
	{
		throw std::invalid_argument("Configure bounded MIDI pitch recording before accepting notes");
	}
	m_pitchBaseline = baselineCents; m_maximumPitchPoints = maximumPoints;
}

TimePos MidiNoteRecorder::positionInClip(const Note& note, const MidiClip& clip, bool songPosition)
{
	// The onset snapshot survives a queued GUI/model-thread delivery. Song clips
	// use the same placement/trim offset as InstrumentTrack::play.
	return songPosition ? note.pos() - clip.startPosition() - clip.startTimeOffset() : note.pos();
}

bool MidiNoteRecorder::startNote(const Note& note, TimePos position)
{
	if (note.key() < 0 || note.key() >= NumKeys || position < 0) { return false; }
	auto& pending = m_pending[note.key()]; if (pending) { return false; }
	if (m_accepted >= m_maximum) { throw std::runtime_error("MIDI recording note limit reached"); }
	pending.emplace(TimePos(1), position, note.key(), note.getVolume(), note.getPanning(), note.detuning()); ++m_accepted;
	return true;
}

void MidiNoteRecorder::recordPitch(const Note& note, float pitchCents)
{
	const int key = note.key();
	if (!m_maximumPitchPoints || key < 0 || key >= NumKeys || !m_pending[key]) { return; }
	const float value = (pitchCents - m_pitchBaseline) / 100.f; const auto time = note.length();
	if (!std::isfinite(value) || value < -MaxDetuning || value > MaxDetuning || time < 0)
	{
		throw std::runtime_error("Recorded MIDI pitch exceeds native note detuning limits");
	}
	auto& detuning = m_recordedPitch[key];
	if (!detuning && value == 0) { return; }
	auto curve = detuning ? detuning->automationClip() : nullptr;
	if (curve && curve->valueAt(time) == value) { return; }
	const std::size_t added = curve ? !curve->getTimeMap().contains(time.getTicks()) : (time > 0 ? 2 : 1);
	if (added > m_maximumPitchPoints - m_pitchPoints) { throw std::runtime_error("MIDI recording pitch point limit reached"); }
	if (!detuning)
	{
		// Own a fresh native curve; never alter data borrowed by an input snapshot.
		Note carrier; carrier.createDetuning(); detuning = carrier.detuning(); curve = detuning->automationClip();
		curve->clear(); curve->setProgressionType(AutomationClip::ProgressionType::Discrete);
		if (time > 0) { curve->putValue(0, 0, false); }
	}
	curve->putValue(time, value, false); m_pitchPoints += added;
}

Note MidiNoteRecorder::completed(const Note& onset, const Note& release, int quantization, const std::shared_ptr<DetuningHelper>& detuning)
{
	if (quantization < 0) { throw std::invalid_argument("MIDI recording quantization must not be negative"); }
	Note result(release.length(), onset.pos(), onset.key(), onset.getVolume(), onset.getPanning(), detuning);
	if (quantization > 0) { result.quantizeLength(quantization); result.quantizePos(quantization); }
	result.setLength(std::max(result.length(), TimePos(1))); return result;
}

std::optional<Note> MidiNoteRecorder::finishNote(const Note& note, int quantization)
{
	if (note.key() < 0 || note.key() >= NumKeys || !m_pending[note.key()]) { return std::nullopt; }
	const int key = note.key();
	auto result = completed(*m_pending[key], note, quantization, m_recordedPitch[key] ? m_recordedPitch[key] : note.detuning());
	m_pending[key].reset(); m_recordedPitch[key].reset(); return result;
}

std::size_t MidiNoteRecorder::pendingNotes() const
{
	return std::count_if(m_pending.begin(), m_pending.end(), [](const auto& note) { return note.has_value(); });
}
}

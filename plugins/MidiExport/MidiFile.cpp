/*
 * MidiFile.cpp - support for exporting MIDI files
 *
 * Copyright (c) 2009 Mark Conway Wirt <emergentmusics) at (gmail . com>
 * Copyright (c) 2015 Mohamed Abdel Maksoud <mohamed at amaksoud.com>
 * Copyright (c) 2020 EmoonX
 * Copyright (c) 2026 Dalton Messmer <messmer.dalton/at/gmail.com>
 *
 * This file was originally based on the Python module MidiFile.py from MIDIUtil
 * by Mark Conway Wirt, which was later rewritten in C++ by Mohamed Abdel Maksoud.
 *
 * --------------------------------------------------------------------------
 * MIDUTIL, Copyright (c) 2009, Mark Conway Wirt
 *                        <emergentmusics) at (gmail . com>
 *
 * This software is distributed under an Open Source license, the
 * details of which follow.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 * --------------------------------------------------------------------------
 *
 */

#include "MidiFile.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <stack>
#include <stdexcept>

#include "endian_handling.h"
#include "FileResourceIO.h"

namespace lmms
{
namespace
{
constexpr std::uint32_t MaximumVariableLength = 0x0fffffff;
void checkWork()
{
	QString error;
	if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
}
tick_t checkedTime(double beats)
{
	const double ticks = std::round(beats * MidiFile::TicksPerBeat);
	if (!std::isfinite(ticks) || beats < 0 || ticks > std::numeric_limits<tick_t>::max())
	{ throw std::out_of_range("MIDI event time is outside the native tick range"); }
	return static_cast<tick_t>(ticks);
}
}

MidiFile::MidiFile(const std::filesystem::path& file, std::size_t numTracks)
	: m_header{numTracks}
{
	if (numTracks == 0 || numTracks > std::numeric_limits<std::uint16_t>::max())
	{ throw std::out_of_range("Standard MIDI files require 1..65535 tracks"); }
	// Open designated blank MIDI file as a data stream for writing
#ifdef Q_OS_WIN
	m_stream.setFileName(QString::fromStdWString(file.native()));
#else
	m_stream.setFileName(QString::fromStdString(file.native()));
#endif
	m_stream.open(QIODevice::WriteOnly);

	// Resize track list
	m_tracks.resize(numTracks);
}

bool MidiFile::writeAllToStream()
{
	if (!m_stream.isOpen()) { return false; }
	const auto writeSection = [this](const auto& bytes)
	{
		if (bytes.empty()) { return false; }
		for (std::size_t offset = 0; offset < bytes.size();)
		{
			checkWork();
			const auto count = static_cast<qint64>(std::min<std::size_t>(64 * 1024, bytes.size() - offset));
			if (m_stream.write(reinterpret_cast<const char*>(bytes.data() + offset), count) != count) { return false; }
			offset += static_cast<std::size_t>(count);
		}
		return true;
	};
	if (!writeSection(m_header.m_buffer)) { return false; }

	for (const Track& track : m_tracks)
	{
		if (!writeSection(track.m_buffer)) { return false; }
	}
	checkWork();
	return m_stream.commit();
}

MidiFile::Section::Section()
{
	m_buffer.reserve(InitialBufferSize);
}

void MidiFile::Section::writeBytes(std::span<const std::uint8_t> bytes)
{
	// Append content to the end of m_buffer
	m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
}

void MidiFile::Section::writeBytes(std::initializer_list<std::uint8_t> bytes)
{
	writeBytes(std::span{bytes.begin(), bytes.size()});
}

void MidiFile::Section::writeVarLength(std::uint32_t val)
{
	if (val > MaximumVariableLength) { throw std::out_of_range("MIDI variable-length value exceeds four bytes"); }
	// Build little endian stack from 7-bit packs
	std::uint8_t result = val & 0x7F;
	auto littleEndian = std::stack<std::uint8_t>({result});
	val >>= 7;
	while (val > 0)
	{
		result = val & 0x7F;
		result |= 0x80;
		littleEndian.push(result);
		val >>= 7;
	}
	// Add packs in reverse order to actual buffer
	while (!littleEndian.empty())
	{
		m_buffer.push_back(littleEndian.top());
		littleEndian.pop();
	}
}

void MidiFile::Section::writeBigEndian4(std::uint32_t val)
{
	const auto valBigEndian = std::bit_cast<std::array<std::uint8_t, 4>>(byteswapIfLE(val));
	writeBytes(valBigEndian);
}

void MidiFile::Section::writeBigEndian2(std::uint16_t val)
{
	const auto valBigEndian = std::bit_cast<std::array<std::uint8_t, 2>>(byteswapIfLE(val));
	writeBytes(valBigEndian);
}

MidiFile::Header::Header(std::size_t numTracks, tick_t ticksPerBeat)
	: m_numTracks{numTracks}
	, m_ticksPerBeat{ticksPerBeat}
{}

void MidiFile::Header::writeToBuffer()
{
	if (m_numTracks == 0 || m_numTracks > std::numeric_limits<std::uint16_t>::max() || m_ticksPerBeat <= 0 || m_ticksPerBeat > 0x7fff)
	{ throw std::out_of_range("Invalid Standard MIDI file header"); }
	m_buffer.clear();
	// Chunk ID
	writeBytes({'M', 'T', 'h', 'd'});

	// Chunk size (6 bytes always)
	writeBytes({0, 0, 0, 0x06});

	// Format: 1 (multitrack)
	writeBytes({0, 0x01});

	// Track and ticks info
	writeBigEndian2(static_cast<std::uint16_t>(m_numTracks));
	writeBigEndian2(static_cast<std::uint16_t>(m_ticksPerBeat));
}

void MidiFile::Track::addEvent(Event&& event, tick_t time)
{
	if (time < 0) { throw std::out_of_range("MIDI event time must not be negative"); }
	event.time = time;
	event.channel = m_channel;
	m_events.push_back(std::move(event));
}

void MidiFile::Track::setChannel(std::uint8_t channel)
{
	if (channel > 15) { throw std::out_of_range("MIDI channel exceeds 15"); }
	m_channel = channel;
}

void MidiFile::Track::addNote(std::uint8_t pitch, std::uint8_t volume,
	double realTime, double duration)
{
	if (pitch > 127 || volume > 127 || !std::isfinite(duration) || duration <= 0)
	{ throw std::out_of_range("MIDI note requires valid data bytes and a positive duration"); }
	const auto onset = checkedTime(realTime), end = checkedTime(realTime + duration);
	if (end <= onset) { throw std::out_of_range("MIDI note duration rounds to zero ticks"); }
	addNoteTicks(pitch, volume, onset, end - onset);
}

void MidiFile::Track::addNoteTicks(std::uint8_t pitch, std::uint8_t volume, tick_t time, tick_t duration)
{
	if (pitch > 127 || volume > 127 || time < 0 || duration <= 0 || duration > std::numeric_limits<tick_t>::max() - time)
	{ throw std::out_of_range("Invalid native MIDI note or endpoint"); }
	if (volume == 0) { return; }
	// Add start of note
	addEvent({
		.type = Event::NoteOn,
		.note = {.pitch = pitch, .volume = volume}
	}, time);

	// Add end of note
	addEvent({
		.type = Event::NoteOff,
		.note = {.pitch = pitch, .volume = volume}
	}, time + duration);
}

void MidiFile::Track::addTempo(std::uint32_t tempo, tick_t time)
{
	if (tempo == 0 || std::llround(6e7 / tempo) < 1 || std::llround(6e7 / tempo) > 0xffffff)
	{ throw std::out_of_range("MIDI tempo cannot be represented in three bytes"); }
	addEvent({.type = Event::Tempo, .tempo = tempo}, time);
}

void MidiFile::Track::addPatch(MidiPatch patch, tick_t time)
{
	if (patch.bank > 0x3fff || patch.program > 127) { throw std::out_of_range("Invalid MIDI bank or program"); }
	addEvent({.type = Event::BankSelectMSB, .patch = patch}, time);
	addEvent({.type = Event::BankSelectLSB, .patch = patch}, time);
	addEvent({.type = Event::ProgramChange, .patch = patch}, time);
}

void MidiFile::Track::addName(std::string name, tick_t time)
{
	if (name.size() > MaximumVariableLength) { throw std::length_error("MIDI track name exceeds the file format limit"); }
	addEvent({.type = Event::TrackName, .trackName = std::move(name)}, time);
}

void MidiFile::Track::writeToBuffer()
{
	checkWork();
	m_buffer.clear();
	// Chunk ID
	writeBytes({'M', 'T', 'r', 'k'});

	// Chunk size placeholder
	writeBigEndian4(0);
	std::size_t idx = m_buffer.size();

	// Write all events to buffer
	writeMIDIToBuffer();

	// Write correct size in placeholder place
	if (m_buffer.size() - idx > std::numeric_limits<std::uint32_t>::max()) { throw std::length_error("MIDI track exceeds the chunk size limit"); }
	const auto size = static_cast<std::uint32_t>(m_buffer.size() - idx);
	const auto sizeBigEndian = std::bit_cast<std::array<std::uint8_t, 4>>(byteswapIfLE(size));
	for (std::size_t i = 0; i < 4; ++i)
	{
		m_buffer[idx - 4 + i] = sizeBigEndian[i];
	}
}

void MidiFile::Track::writeMIDIToBuffer()
{
	// Process events in the eventList
	writeEventsToBuffer();

	// Write MIDI close event
	writeBytes({0x00, 0xFF, 0x2F, 0x00});
}

void MidiFile::Track::writeEventsToBuffer()
{
	std::stable_sort(m_events.begin(), m_events.end());

	tick_t timeLast = 0;
	std::size_t count = 0;
	for (const Event& event : m_events)
	{
		if ((count++ % 1024) == 0) { checkWork(); }
		assert(event.time >= timeLast);
		auto delta = event; delta.time -= timeLast; timeLast = event.time;
		// A native arrangement may contain a gap larger than one SMF delta.
		// Empty text meta events preserve that gap without a
		// nonstandard five-byte VLQ or an audible channel message.
		while (static_cast<std::uint32_t>(delta.time) > MaximumVariableLength)
		{
			writeVarLength(MaximumVariableLength); writeBytes({0xff, 0x01, 0x00});
			delta.time -= static_cast<tick_t>(MaximumVariableLength);
		}
		writeSingleEventToBuffer(delta);
	}
}

void MidiFile::Track::writeSingleEventToBuffer(Event& event)
{
	// First of all, write event time
	writeVarLength(event.time);

	switch (event.type)
	{
		case MidiFile::Event::NoteOn:
		{
			// A note starts playing
			std::uint8_t code = 0x90 | event.channel;
			writeBytes({code, event.note.pitch, event.note.volume});
			break;
		}
		case MidiFile::Event::NoteOff:
		{
			// A note finishes playing
			std::uint8_t code = 0x80 | event.channel;
			writeBytes({code, event.note.pitch, event.note.volume});
			break;
		}
		case MidiFile::Event::Tempo:
		{
			// A tempo measure
			std::uint8_t code = 0xFF;
			writeBytes({code, 0x51, 0x03});

			// Convert to microseconds before writing
			const auto usec = static_cast<std::uint32_t>(std::llround(6e7 / event.tempo));
			const auto usecBigEndian = std::bit_cast<std::array<std::uint8_t, 4>>(byteswapIfLE(usec));
			writeBytes({usecBigEndian[1], usecBigEndian[2], usecBigEndian[3]});
			break;
		}
		case MidiFile::Event::BankSelectMSB:
		{
			// Bank select (MSB)
			writeBytes({static_cast<std::uint8_t>(0xB0 | event.channel), 0x00, event.patch.bankMSB()});
			break;
		}
		case MidiFile::Event::BankSelectLSB:
		{
			// Bank select (LSB)
			writeBytes({static_cast<std::uint8_t>(0xB0 | event.channel), 0x20, event.patch.bankLSB()});
			break;
		}
		case MidiFile::Event::ProgramChange:
		{
			// Program change
			writeBytes({static_cast<std::uint8_t>(0xC0 | event.channel), event.patch.program});
			break;
		}
		case MidiFile::Event::TrackName:
		{
			// Name of current track
			writeBytes({0xFF, 0x03});

			// Write name string size and then copy it's content
			// to the following size bytes of buffer
			auto bytes = std::vector<std::uint8_t>(event.trackName.begin(), event.trackName.end());
			writeVarLength(static_cast<std::uint32_t>(event.trackName.size()));
			writeBytes(bytes);
			break;
		}
	}
}

} // namespace lmms

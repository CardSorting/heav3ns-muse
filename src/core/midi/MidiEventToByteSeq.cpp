/*
 * MidiEventToByteSeq.cpp - writeToByteSeq implementation
 *
 * Copyright (c) 2020-2020 Johannes Lorenz <jlsf2013$users.sourceforge.net, $=@>
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

#include "MidiEventToByteSeq.h"

#include <QtGlobal>

#include "MidiEvent.h"


namespace lmms
{

std::optional<MidiEvent> readFromByteSeq(const uint8_t* data, std::size_t size, MidiEvent::Source source)
{
	if (!data || size < 2 || size > 3 || data[0] < 0x80 || data[0] >= 0xf0) { return std::nullopt; }
	const auto type = static_cast<MidiEventTypes>(data[0] & 0xf0);
	const std::size_t expected = type == MidiProgramChange || type == MidiChannelPressure ? 2 : 3;
	if (size != expected) { return std::nullopt; }
	for (std::size_t i = 1; i < size; ++i) { if (data[i] > 127) { return std::nullopt; } }
	const auto first = type == MidiPitchBend ? data[1] | (data[2] << 7) : data[1];
	const auto second = type == MidiPitchBend || size == 2 ? 0 : data[2];
	return MidiEvent(type, data[0] & 0x0f, first, second, nullptr, source);
}

std::size_t writeToByteSeq(
	const MidiEvent& ev, uint8_t *data, std::size_t bufsize)
{
	if (!data || bufsize < 3 || ev.channel() < 0 || ev.channel() >= MidiChannelCount) { return 0; }
	switch (ev.type())
	{
	case MidiNoteOn: case MidiNoteOff: case MidiKeyPressure: case MidiControlChange:
		if (ev.param(0) < 0 || ev.param(0) > 127 || ev.param(1) < 0 || ev.param(1) > 127) { return 0; }
		break;
	case MidiProgramChange: case MidiChannelPressure:
		if (ev.param(0) < 0 || ev.param(0) > 127) { return 0; }
		break;
	case MidiPitchBend:
		if (ev.param(0) < 0 || ev.param(0) > MidiMaxPitchBend) { return 0; }
		break;
	default: return 0;
	}

	std::size_t size = 0;
	data[0] = ev.type() | (ev.channel() & 0x0F);

	switch (ev.type())
	{
		case MidiNoteOn:
			if (ev.velocity() > 0)
			{
				if (ev.key() < 0 || ev.key() > MidiMaxKey)
					break;

				data[1] = ev.key();
				data[2] = ev.velocity();
				size    = 3;
				break;
			}
			else
			{
				// Lv2 MIDI specs:
				// "Note On messages with velocity 0 are not allowed.
				// These messages are equivalent to Note Off in standard
				// MIDI streams, but here only proper Note Off messages
				// are allowed."
				data[0] = MidiNoteOff | (ev.channel() & 0x0F);
				// nobreak
			}

		case MidiNoteOff:
			if (ev.key() < 0 || ev.key() > MidiMaxKey)
				break;
			data[1] = ev.key();
			data[2] = ev.velocity(); // release time
			size    = 3;
			break;

		case MidiKeyPressure:
			data[1] = ev.key();
			data[2] = ev.velocity();
			size    = 3;
			break;

		case MidiControlChange:
			data[1] = ev.controllerNumber();
			data[2] = ev.controllerValue();
			size    = 3;
			break;

		case MidiProgramChange:
			data[1] = ev.program();
			size    = 2;
			break;

		case MidiChannelPressure:
			data[1] = ev.channelPressure();
			size    = 2;
			break;

		case MidiPitchBend:
			data[1] = ev.pitchBend() & 0x7f;
			data[2] = ev.pitchBend() >> 7;
			size    = 3;
			break;

		default:
			// unhandled
			break;
	}

	return size;
}


} // namespace lmms

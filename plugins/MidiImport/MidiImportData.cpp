/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "MidiImportData.h"
#include <QFile>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include "FileResourceIO.h"

namespace lmms
{
namespace
{
void check()
{
	QString error;
	if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
}
void require(bool condition, const char* error)
{ if (!condition) { throw std::runtime_error(error); } }

class Reader
{
	const QByteArray& m_bytes;
public:
	explicit Reader(const QByteArray& bytes) : m_bytes(bytes), end(bytes.size()) { }
	qint64 pos = 0, end;
	std::uint8_t byte()
	{
		require(pos < end, "Truncated MIDI data");
		return static_cast<std::uint8_t>(m_bytes.at(static_cast<int>(pos++)));
	}
	std::uint32_t integer(int count, bool little = false)
	{
		std::uint32_t value = 0;
		for (int i = 0; i < count; ++i)
		{
			const auto next = byte();
			if (little) { value |= std::uint32_t(next) << (i * 8); } else { value = (value << 8) | next; }
		}
		return value;
	}
	std::uint32_t vlq()
	{
		std::uint32_t value = 0;
		for (int i = 0; i < 4; ++i)
		{
			const auto next = byte(); value = (value << 7) | (next & 0x7f);
			if (!(next & 0x80)) { return value; }
		}
		throw std::runtime_error("MIDI variable-length value exceeds four bytes");
	}
	void skip(qint64 count)
	{
		require(count >= 0 && count <= end - pos, "MIDI chunk or event exceeds its container"); pos += count;
	}
	QByteArray id()
	{
		const auto start = pos; skip(4); return m_bytes.mid(static_cast<int>(start), 4);
	}
};

void validateTrack(Reader& reader)
{
	int running = 0; bool ended = false; std::size_t count = 0;
	while (reader.pos < reader.end)
	{
		if ((count++ % 1024) == 0) { check(); }
		require(!ended, "MIDI events follow end-of-track"); reader.vlq();
		int status = reader.byte(); bool firstData = false;
		if (status < 0x80)
		{
			require(running != 0, "MIDI running status has no preceding channel message");
			status = running; firstData = true;
		}
		if (status >= 0x80 && status <= 0xef)
		{
			running = status;
			const int dataBytes = (status & 0xe0) == 0xc0 ? 1 : 2;
			for (int i = firstData ? 1 : 0; i < dataBytes; ++i)
			{ require(reader.byte() < 0x80, "Invalid MIDI channel data byte"); }
		}
		else if (status == 0xf0 || status == 0xf7) { running = 0; reader.skip(reader.vlq()); }
		else if (status == 0xff)
		{
			running = 0;
			const auto kind = reader.byte(); require(kind < 0x80, "Invalid MIDI meta-event type");
			const auto length = reader.vlq();
			const auto start = reader.pos; reader.skip(length); const auto finish = reader.pos; reader.pos = start;
			switch (kind)
			{
				case 0x00: require(length == 2, "Invalid MIDI sequence-number event"); break;
				case 0x20: require(length == 1 && reader.byte() < 16, "Invalid MIDI channel prefix"); break;
				case 0x21: require(length == 1, "Invalid MIDI port event"); break;
				case 0x2f: require(length == 0, "Invalid MIDI end-of-track event"); ended = true; break;
				case 0x51: require(length == 3 && reader.integer(3) > 0, "Invalid MIDI tempo event"); break;
				case 0x54: require(length == 5, "Invalid MIDI SMPTE offset event"); break;
				case 0x58:
					require(length == 4 && reader.byte() > 0, "Invalid MIDI time signature");
					require(reader.byte() <= 30, "MIDI denominator exceeds the native integer range"); break;
				case 0x59: require(length == 2, "Invalid MIDI key signature"); break;
				default: break;
			}
			reader.pos = finish;
		}
		else { throw std::runtime_error("Unsupported status in Standard MIDI File"); }
	}
	require(ended, "MIDI track has no end-of-track event");
}

void validateSMF(const QByteArray& bytes)
{
	Reader reader(bytes); require(reader.id() == "MThd", "RIFF data does not contain a MIDI header");
	const auto size = reader.integer(4); require(size >= 6, "Short MIDI header");
	const auto format = reader.integer(2), count = reader.integer(2), division = reader.integer(2);
	require(format <= 2 && count > 0 && (format != 0 || count == 1), "Invalid MIDI format or track count");
	if (division & 0x8000)
	{
		const auto fps = static_cast<std::int8_t>(division >> 8);
		require((fps == -24 || fps == -25 || fps == -29 || fps == -30) && (division & 0xff), "Invalid MIDI SMPTE division");
	}
	else { require(division != 0, "MIDI timing division is zero"); }
	reader.skip(size - 6);
	std::uint32_t tracks = 0;
	while (reader.pos < reader.end)
	{
		check(); const auto id = reader.id(); const auto length = reader.integer(4);
		const auto start = reader.pos; reader.skip(length); const auto finish = reader.pos;
		if (id == "MTrk")
		{
			require(++tracks <= count, "MIDI has more tracks than its header declares");
			const auto fileEnd = reader.end; reader.pos = start; reader.end = finish;
			validateTrack(reader); reader.end = fileEnd;
		}
	}
	require(tracks == count, "MIDI track count does not match its header"); check();
}
}

QByteArray readMidiImportData(QFile& input)
{
	check(); require(input.isOpen() && input.seek(0), "Cannot seek MIDI input");
	const auto signature = input.peek(4);
	if (signature != "MThd" && signature != "RIFF") { return {}; }
	if (signature == "RIFF" && input.peek(12).mid(8, 4) != "RMID") { return {}; }
	const auto size = input.size();
	require(size >= 0 && size <= std::numeric_limits<int>::max(), "MIDI input exceeds the native byte-array range");
	QByteArray bytes;
	while (bytes.size() < size)
	{
		check(); const auto chunk = input.read(std::min<qint64>(64 * 1024, size - bytes.size()));
		require(!chunk.isEmpty(), "Could not read complete MIDI input"); bytes.append(chunk);
	}
	require(input.error() == QFileDevice::NoError && input.size() == size, "MIDI input changed or could not be read");
	if (signature == "RIFF")
	{
		Reader reader(bytes); reader.skip(4); const auto length = reader.integer(4, true);
		require(length >= 4 && length == size - 8, "RIFF length does not match the input file");
		if (reader.id() != "RMID") { return {}; }
		QByteArray midi; bool found = false;
		while (reader.pos < reader.end)
		{
			check(); const auto id = reader.id(); const auto count = reader.integer(4, true);
			const auto start = reader.pos; reader.skip(count);
			if (id == "data")
			{
				require(!found, "RIFF MIDI has multiple data chunks"); found = true;
				midi = bytes.mid(static_cast<int>(start), static_cast<int>(count));
			}
			reader.skip(count & 1); // RIFF alignment is outside the payload length.
		}
		require(found, "RIFF MIDI has no data chunk"); bytes = std::move(midi);
	}
	validateSMF(bytes); return bytes;
}
} // namespace lmms

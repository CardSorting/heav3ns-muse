/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "RawMidiOutput.h"
#include "MidiEventToByteSeq.h"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <stdexcept>

namespace lmms::detail
{
namespace
{
constexpr std::array<std::uint8_t, 4> controllers{120, 64, 66, 123};
bool cleanupMessage(const std::array<std::uint8_t, 3>& bytes)
{
	const auto type = bytes[0] >> 4;
	return type == 0x8 || (type == 0x9 && bytes[2] == 0)
		|| (type == 0xb && bytes[2] == 0 && (bytes[1] == 64 || bytes[1] == 66 || bytes[1] == 120 || bytes[1] == 123));
}
}

void RawMidiOutput::add(const MidiPort* source)
{
	const auto lock = std::lock_guard{m_mutex};
	if (!source || !m_sources.emplace(source, Source{}).second) { throw std::logic_error("Raw MIDI output source is already registered or null"); }
}

void RawMidiOutput::cleanupChannel(unsigned int channel)
{ m_cleanup |= std::uint64_t{0xf} << (channel * 4); }

void RawMidiOutput::cancelLocked(const MidiPort* source)
{
	const auto found = m_sources.find(source); if (found == m_sources.end()) { return; }
	m_count = static_cast<unsigned int>(std::remove_if(m_queue.begin(), m_queue.begin() + m_count,
		[source](const auto& packet) { return packet.source == source; }) - m_queue.begin());
	// A partially submitted message must finish framing before channel cleanup.
	// Unsubmitted packets can be discarded without touching the native stream.
	if (m_current.source == source && !m_current.cleanupBit && m_current.offset == 0) { m_current = {}; }
	for (unsigned int channel = 0; channel < 16; ++channel) { if (found->second.channels & (1u << channel)) { cleanupChannel(channel); } }
	found->second.cancelling = true;
}

void RawMidiOutput::cancel(const MidiPort* source)
{ const auto lock = std::lock_guard{m_mutex}; cancelLocked(source); }

void RawMidiOutput::remove(const MidiPort* source)
{ const auto lock = std::lock_guard{m_mutex}; cancelLocked(source); m_sources.erase(source); }

void RawMidiOutput::discardLocked()
{
	m_count = 0;
	if (!m_current.offset && !m_current.cleanupBit) { m_current = {}; }
	for (auto& item : m_sources)
	{
		for (unsigned int channel = 0; channel < 16; ++channel) { if (item.second.channels & (1u << channel)) { cleanupChannel(channel); } }
		item.second.cancelling = true;
	}
}

void RawMidiOutput::cancelAll()
{ const auto lock = std::lock_guard{m_mutex}; discardLocked(); }

void RawMidiOutput::failLocked(int error, bool terminal)
{
	m_error = error ? error : -EIO; m_lastError = m_error; m_terminal = m_terminal || terminal;
	discardLocked();
}

void RawMidiOutput::fail(int error)
{ const auto lock = std::lock_guard{m_mutex}; failLocked(error, true); }

void RawMidiOutput::enqueue(const MidiEvent& event, const MidiPort* source)
{
	Packet packet; const auto size = writeToByteSeq(event, packet.bytes.data(), packet.bytes.size());
	if (!size) { return; } packet.size = static_cast<std::uint8_t>(size); packet.source = source;
	const auto lock = std::lock_guard{m_mutex}; const auto found = m_sources.find(source);
	if (found == m_sources.end()) { throw std::runtime_error("Raw MIDI output source has retired"); }
	if (m_terminal) { throw std::runtime_error("Raw MIDI output backend is unavailable"); }
	if (found->second.cancelling && cleanupMessage(packet.bytes))
	{
		// Port release can generate many individual note-offs. The reserved
		// channel cleanup covers them without exhausting the music queue.
		cleanupChannel(packet.bytes[0] & 0xf); return;
	}
	if (m_error) { throw std::runtime_error("Raw MIDI output is recovering from a delivery failure"); }
	if (m_count + (m_current.size && !m_current.cleanupBit ? 1u : 0u) >= QueueLimit)
	{
		failLocked(-ENOSPC, false);
		throw std::runtime_error("Raw MIDI output queue is full; pending music was discarded and cleanup is required");
	}
	found->second.cancelling = false; m_queue[m_count++] = packet;
}

int RawMidiOutput::service(const Writer& writer)
{
	for (unsigned int attempt = 0; attempt < 64; ++attempt)
	{
		const auto lock = std::lock_guard{m_mutex};
		if (m_terminal) { return m_error ? m_error : -EIO; }
		if (m_cleanup && m_current.size && !m_current.offset && !m_current.cleanupBit)
		{
			std::move_backward(m_queue.begin(), m_queue.begin() + m_count, m_queue.begin() + m_count + 1);
			m_queue[0] = m_current; ++m_count; m_current = {};
		}
		if (!m_current.size)
		{
			if (m_cleanup)
			{
				const auto bit = static_cast<unsigned int>(std::countr_zero(m_cleanup));
				m_current = {}; m_current.bytes = {static_cast<std::uint8_t>(0xb0 | (bit / 4)), controllers[bit % 4], 0};
				m_current.size = 3; m_current.cleanupBit = std::uint64_t{1} << bit;
			}
			else if (m_count)
			{
				m_current = m_queue[0]; --m_count;
				std::move(m_queue.begin() + 1, m_queue.begin() + 1 + m_count, m_queue.begin());
			}
			else { m_error = 0; return 0; }
		}
		const auto remaining = static_cast<std::size_t>(m_current.size - m_current.offset);
		int written;
		try { written = writer(m_current.bytes.data() + m_current.offset, remaining); }
		catch (...) { written = -EIO; }
		if (written == 0 || written == -EAGAIN || written == -EINTR) { return 1; }
		if (!m_current.cleanupBit && !m_current.offset)
		{
			// A failing native call may already have submitted bytes. Retain the
			// channel for cleanup even when native acceptance is uncertain.
			if (const auto found = m_sources.find(m_current.source); found != m_sources.end())
			{ found->second.channels |= static_cast<std::uint16_t>(1u << (m_current.bytes[0] & 0xf)); }
		}
		if (written < 0 || static_cast<std::size_t>(written) > remaining)
		{ failLocked(written < 0 ? written : -EIO, true); return m_error; }
		m_current.offset += static_cast<std::uint8_t>(written);
		if (m_current.offset == m_current.size)
		{
			if (m_current.cleanupBit)
			{
				const auto channel = m_current.bytes[0] & 0xf; m_cleanup &= ~m_current.cleanupBit;
				if (!(m_cleanup & (std::uint64_t{0xf} << (channel * 4))))
				{ for (auto& item : m_sources) { item.second.channels &= static_cast<std::uint16_t>(~(1u << channel)); } }
			}
			m_current = {};
		}
	}
	return 0;
}

bool RawMidiOutput::wantsWrite() const
{ const auto lock = std::lock_guard{m_mutex}; return !m_terminal && (m_count || m_current.size || m_cleanup || m_error); }

bool RawMidiOutput::drained() const
{ const auto lock = std::lock_guard{m_mutex}; return !m_count && !m_current.size && !m_cleanup; }

bool RawMidiOutput::terminal() const
{ const auto lock = std::lock_guard{m_mutex}; return m_terminal; }

MidiClient::OutputBackendStatus RawMidiOutput::status(bool available) const
{
	const auto lock = std::lock_guard{m_mutex}; unsigned int channels = 0;
	for (unsigned int channel = 0; channel < 16; ++channel) { if (m_cleanup & (std::uint64_t{0xf} << (channel * 4))) { ++channels; } }
	const int error = m_error ? m_error : available ? 0 : -ENODEV;
	return {true, available && !m_terminal && !m_error && !m_cleanup, m_count + (m_current.size && !m_current.cleanupBit ? 1u : 0u), QueueLimit, channels, error, m_lastError};
}
}

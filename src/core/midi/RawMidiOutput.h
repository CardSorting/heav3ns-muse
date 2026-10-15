/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_RAW_MIDI_OUTPUT_H
#define LMMS_RAW_MIDI_OUTPUT_H
#include "MidiClient.h"
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace lmms::detail
{
// Bounded, untimestamped MIDI packets owned by one native byte-stream client.
// Source identities are opaque and never dereferenced by the writer thread.
class RawMidiOutput
{
public:
	static constexpr unsigned int QueueLimit = 1024;
	using Writer = std::function<int(const std::uint8_t*, std::size_t)>;
	void add(const MidiPort* source);
	void remove(const MidiPort* source);
	void cancel(const MidiPort* source);
	void cancelAll();
	void enqueue(const MidiEvent& event, const MidiPort* source);
	// Only the native worker calls service. Retain the exact suffix on short
	// writes or EAGAIN/EINTR. At most 64 nonblocking native writes per cycle.
	// Writer must not reenter this queue. Return 1 for native backpressure,
	// 0 after progress/budget exhaustion, or a negative terminal error.
	int service(const Writer& writer);
	void fail(int error);
	bool wantsWrite() const;
	bool drained() const;
	bool terminal() const;
	MidiClient::OutputBackendStatus status(bool available) const;
private:
	struct Source { std::uint16_t channels = 0; bool cancelling = false; };
	struct Packet
	{
		std::array<std::uint8_t, 3> bytes{};
		std::uint8_t size = 0, offset = 0;
		const MidiPort* source = nullptr;
		std::uint64_t cleanupBit = 0;
	};
	void cancelLocked(const MidiPort* source);
	void discardLocked();
	void failLocked(int error, bool terminal);
	void cleanupChannel(unsigned int channel);
	mutable std::mutex m_mutex;
	std::unordered_map<const MidiPort*, Source> m_sources;
	std::array<Packet, QueueLimit> m_queue{};
	unsigned int m_count = 0;
	Packet m_current;
	std::uint64_t m_cleanup = 0;
	int m_error = 0, m_lastError = 0;
	bool m_terminal = false;
};
}
#endif

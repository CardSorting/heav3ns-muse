/* Owned native VST event packets. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_MIDI_BUFFER_H
#define LMMS_VST_MIDI_BUFFER_H

#include <aeffectx.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lmms
{
class VstMidiBuffer
{
public:
	void assign(std::vector<VstMidiEvent>& queued)
	{
		constexpr auto prefix = offsetof(VstEvents, events);
		constexpr auto word = sizeof(std::uintptr_t);
		static_assert(alignof(std::uintptr_t) >= alignof(VstEvents));
		if (queued.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())
			|| queued.size() > (std::numeric_limits<std::size_t>::max() - prefix - word) / sizeof(VstEvent*))
		{ throw std::length_error("Native VST MIDI batch is too large"); }
		// VST uses a variable-length C ABI pointer array. Allocate its full storage
		// and copy pointer bytes rather than indexing past VstEvents::events[1].
		const auto bytes = std::max(sizeof(VstEvents), prefix + queued.size() * sizeof(VstEvent*));
		std::stable_sort(queued.begin(), queued.end(), [](const auto& a, const auto& b) { return a.deltaFrames < b.deltaFrames; });
		m_packet.resize((bytes + word - 1) / word); // Reuse peak capacity across audio blocks.
		m_events.swap(queued); queued.clear();
		VstEvents header{}; header.numEvents = static_cast<std::int32_t>(m_events.size());
		std::memcpy(m_packet.data(), &header, sizeof(header));
		auto destination = reinterpret_cast<std::byte*>(m_packet.data()) + prefix;
		for (auto& event : m_events)
		{
			auto pointer = reinterpret_cast<VstEvent*>(&event);
			std::memcpy(destination, &pointer, sizeof(pointer)); destination += sizeof(pointer);
		}
	}
	VstEvents* data() { return m_packet.empty() ? nullptr : reinterpret_cast<VstEvents*>(m_packet.data()); }
private:
	// Plugins may read event pointers during the following audio callback.
	// Keep both records and pointer storage alive until the next batch or teardown.
	std::vector<VstMidiEvent> m_events;
	std::vector<std::uintptr_t> m_packet;
};
} // namespace lmms
#endif

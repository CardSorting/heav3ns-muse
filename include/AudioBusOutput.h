/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_AUDIO_BUS_OUTPUT_H
#define LMMS_AUDIO_BUS_OUTPUT_H

#include <algorithm>
#include <atomic>
#include <vector>

#include "SampleFrame.h"

namespace lmms
{
class AudioBusHandle;
class AudioEngine;

// A retained native bus output, consumed only by AudioEngine::renderNextBuffer.
// Its storage outlives the bus when a backend retains a destination. Bus removal
// silences both periods under the engine model lock before releasing ownership.
// No backend may read these buffers independently of the engine's period cursor.
class AudioBusOutput final
{
public:
	bool retired() const noexcept { return m_retired.load(std::memory_order_acquire); }
private:
	explicit AudioBusOutput(f_cnt_t frames)
		: m_read(frames, SampleFrame{}), m_write(frames, SampleFrame{}) { }
	AudioBusOutput(const AudioBusOutput&) = delete;
	AudioBusOutput& operator=(const AudioBusOutput&) = delete;

	void advance()
	{
		m_read.swap(m_write);
		std::fill(m_write.begin(), m_write.end(), SampleFrame{});
	}

	void silence()
	{
		std::fill(m_read.begin(), m_read.end(), SampleFrame{});
		std::fill(m_write.begin(), m_write.end(), SampleFrame{});
	}
	void retire() { silence(); m_retired.store(true, std::memory_order_release); }

	std::vector<SampleFrame> m_read;
	std::vector<SampleFrame> m_write;
	std::atomic_bool m_retired{false};
	friend class AudioBusHandle;
	friend class AudioEngine;
};
}

#endif

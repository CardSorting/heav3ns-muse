/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "SampleRecordingBuffer.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <QObject>
#include "SampleBuffer.h"
#include "FileResourceIO.h"

namespace lmms
{
bool SampleRecordingBuffer::canAppendFrames(f_cnt_t recorded, f_cnt_t incoming) noexcept
{
	const auto limit = std::min(static_cast<f_cnt_t>(std::numeric_limits<int>::max()),
		static_cast<f_cnt_t>(std::vector<SampleFrame>().max_size()));
	return recorded <= limit && incoming <= limit - recorded;
}

bool SampleRecordingBuffer::append(const SampleFrame* input, f_cnt_t frames, sample_rate_t rate)
try
{
	if (failed()) { return false; }
	if (frames == 0) { return true; }
	if (!input || !canAppendFrames(m_frames, frames))
	{
		m_error = QObject::tr("Native audio recording input exceeds valid frame bounds"); return false;
	}
	if (rate == 0 || rate > static_cast<sample_rate_t>(std::numeric_limits<int>::max()) || (m_rate != 0 && rate != m_rate))
	{
		m_error = QObject::tr("Native audio recording input rate is invalid or changed during the take"); return false;
	}
	const auto count = static_cast<std::size_t>(frames);
	if (std::any_of(input, input + count, [](const SampleFrame& value) { return value.containsNaN() || value.containsInf(); }))
	{
		m_error = QObject::tr("Native audio recording input contains nonfinite audio"); return false;
	}
	auto copied = std::make_unique<SampleFrame[]>(count);
	std::copy_n(input, count, copied.get());
	m_blocks.push_back({std::move(copied), count});
	m_frames += frames; m_rate = rate; return true;
}
catch (const std::bad_alloc&) { m_error = QObject::tr("Unable to allocate native audio recording input"); return false; }
catch (const std::length_error&) { m_error = QObject::tr("Native audio recording exceeds allocation limits"); return false; }

std::shared_ptr<const SampleBuffer> SampleRecordingBuffer::finalize(QString& error) const
try
{
	if (failed()) { error = m_error; return {}; }
	if (m_frames == 0) { error = QObject::tr("No native audio input frames were recorded"); return {}; }
	if (!FileResourceIO::check(error)) { return {}; }
	std::vector<SampleFrame> output(static_cast<std::size_t>(m_frames));
	std::size_t copied = 0;
	for (const auto& block : m_blocks)
	{
		if (!FileResourceIO::check(error)) { return {}; }
		if (block.frames > output.size() - copied) { error = QObject::tr("Native audio recording block bounds are inconsistent"); return {}; }
		std::copy_n(block.samples.get(), block.frames, output.data() + copied); copied += block.frames;
	}
	if (copied != output.size()) { error = QObject::tr("Native audio recording is incomplete"); return {}; }
	if (!FileResourceIO::check(error)) { return {}; }
	error.clear(); return std::make_shared<SampleBuffer>(std::move(output), static_cast<int>(m_rate));
}
catch (const std::bad_alloc&) { error = QObject::tr("Unable to finalize native audio recording"); return {}; }
catch (const std::length_error&) { error = QObject::tr("Native audio recording exceeds final buffer limits"); return {}; }
}

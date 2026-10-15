/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_SAMPLE_RECORDING_BUFFER_H
#define LMMS_SAMPLE_RECORDING_BUFFER_H

#include <memory>
#include <vector>
#include <QString>
#include "SampleFrame.h"
#include "lmms_export.h"

namespace lmms
{
class SampleBuffer;

// Owns the native audio input blocks of one take. Callers serialize append/finalize.
class LMMS_EXPORT SampleRecordingBuffer
{
public:
	SampleRecordingBuffer() = default;
	SampleRecordingBuffer(SampleRecordingBuffer&&) noexcept = default;
	SampleRecordingBuffer& operator=(SampleRecordingBuffer&&) noexcept = default;
	SampleRecordingBuffer(const SampleRecordingBuffer&) = delete;
	SampleRecordingBuffer& operator=(const SampleRecordingBuffer&) = delete;

	static bool canAppendFrames(f_cnt_t recorded, f_cnt_t incoming) noexcept;
	bool append(const SampleFrame* input, f_cnt_t frames, sample_rate_t rate);
	std::shared_ptr<const SampleBuffer> finalize(QString& error) const;
	f_cnt_t framesRecorded() const { return m_frames; }
	sample_rate_t sampleRate() const { return m_rate; }
	bool failed() const { return !m_error.isEmpty(); }
	const QString& error() const { return m_error; }
	void reject(const QString& error) { if (m_error.isEmpty()) { m_error = error; } }

private:
	struct Block { std::unique_ptr<SampleFrame[]> samples; std::size_t frames; };
	std::vector<Block> m_blocks;
	f_cnt_t m_frames = 0;
	sample_rate_t m_rate = 0;
	QString m_error;
};
}
#endif

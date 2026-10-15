/*
 * SampleRecordHandle.h - play-handle for recording a sample
 *
 * Copyright (c) 2008 Csaba Hruska <csaba.hruska/at/gmail.com>
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

#ifndef LMMS_SAMPLE_RECORD_HANDLE_H
#define LMMS_SAMPLE_RECORD_HANDLE_H

#include <QPointer>
#include <atomic>
#include <memory>

#include "PlayHandle.h"
#include "TimePos.h"
#include "SampleRecordingBuffer.h"

namespace lmms
{


class PatternTrack;
class SampleBuffer;
class SampleClip;
class Track;


class SampleRecordHandle : public PlayHandle
{
public:
	SampleRecordHandle(SampleClip* clip, f_cnt_t frameLimit = 0, bool reservedCompletion = false);
	~SampleRecordHandle() override;

	void play( SampleFrame* _working_buffer ) override;
	bool isFinished() const override;

	bool isFromTrack( const Track * _track ) const override;

	f_cnt_t framesRecorded() const;
	std::shared_ptr<const SampleBuffer> createSampleBuffer();
	static bool finalizePendingRecordings(QString& error);
	bool recordsClip(const SampleClip* clip) const;
	bool reservedCompletion() const { return m_reservedCompletion; }


private:
	virtual void writeBuffer( const SampleFrame* _ab,
						const f_cnt_t _frames );

	SampleRecordingBuffer m_recording;
	std::uint64_t m_inputErrorEpoch = 0;
	std::atomic<f_cnt_t> m_framesRecorded{0};
	TimePos m_minLength;

	Track * m_track;
	PatternTrack* m_patternTrack;
	QPointer<SampleClip> m_clip;
	// Identity only, never dereferenced; retirement still finds a deleted clip's take.
	const SampleClip* m_clipIdentity = nullptr;
	bool m_registeredTake = false;
	f_cnt_t m_frameLimit = 0;
	bool m_reservedCompletion = false;

} ;


} // namespace lmms

#endif // LMMS_SAMPLE_RECORD_HANDLE_H

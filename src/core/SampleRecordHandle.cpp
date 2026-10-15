/*
 * SampleRecordHandle.cpp - implementation of class SampleRecordHandle
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


#include "SampleRecordHandle.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <QDebug>
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include "AudioEngine.h"
#include "Engine.h"
#include "PatternTrack.h"
#include "PatternStore.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "TrackContainer.h"

namespace lmms
{
SampleRecordHandle::SampleRecordHandle(SampleClip* clip, f_cnt_t frameLimit, bool reservedCompletion)
	: PlayHandle(Type::SamplePlayHandle)
	, m_minLength(clip ? clip->length() : TimePos())
	, m_track(clip ? clip->getTrack() : nullptr)
	, m_patternTrack(nullptr)
	, m_clip(clip)
	, m_clipIdentity(clip)
	, m_frameLimit(frameLimit)
	, m_reservedCompletion(reservedCompletion)
{
	setUsesBuffer(false);
	if (Engine::audioEngine()) { m_inputErrorEpoch = Engine::audioEngine()->inputErrorEpoch(); }
	if (auto track = dynamic_cast<SampleTrack*>(m_track)) { setAudioBusHandle(track->audioBusHandle()); }
	else { m_recording.append(nullptr, 1, 0); }
	if (m_clip && audioBusHandle())
	{
		m_registeredTake = m_clip->beginRecordingTake(reservedCompletion);
		if (!m_registeredTake) { m_recording.reject(QObject::tr("Native clip recording is reserved or publishing")); }
	}
}

SampleRecordHandle::~SampleRecordHandle()
{
	if (!m_clip || !m_registeredTake) { return; }
	if (Engine::audioEngine() && Engine::audioEngine()->inputErrorEpoch() != m_inputErrorEpoch)
	{
		m_recording.reject(QObject::tr("Native audio input was lost or rejected during the take"));
	}
	std::shared_ptr<SampleRecordingBuffer> recording;
	QString failure;
	try { recording = std::make_shared<SampleRecordingBuffer>(std::move(m_recording)); }
	catch (const std::exception& error)
	{
		failure = QString::fromUtf8(error.what());
	}
	try { m_clip->stageRecordingCompletion(std::move(recording), failure); }
	catch (const std::exception& error) { qWarning() << "Native recording completion notification failed:" << error.what(); }
}

bool SampleRecordHandle::finalizePendingRecordings(QString& error)
{
	const auto application = QCoreApplication::instance();
	if (!application || QThread::currentThread() != application->thread() || !Engine::audioEngine() || !Engine::getSong())
	{
		error = QObject::tr("Native recording persistence requires the application thread and engine"); return false;
	}
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	std::vector<QPointer<SampleClip>> clips;
	for (const auto container : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		if (!container) { continue; }
		for (const auto track : container->tracks())
		{
			for (const auto clip : track->getClips())
			{
				if (const auto sample = dynamic_cast<SampleClip*>(clip)) { clips.emplace_back(sample); }
			}
		}
	}
	for (const auto& clip : clips)
	{
		if (clip && (clip->hasReservedRecording() || clip->hasActiveRecording() || clip->isFinalizingRecording()))
		{
			error = QObject::tr("Finish native recording completion before saving"); return false;
		}
	}
	for (const auto& clip : clips)
	{
		if (clip && !clip->finalizeRecordings(error)) { return false; }
	}
	error.clear(); return true;
}

void SampleRecordHandle::play(SampleFrame*)
{
	const auto engine = Engine::audioEngine();
	if (engine->inputErrorEpoch() != m_inputErrorEpoch || engine->inputBufferFault())
	{
		m_recording.reject(QObject::tr("Native audio input was lost or rejected during the take")); return;
	}
	writeBuffer(engine->inputBuffer(), engine->inputBufferFrames());
	const auto frames = framesRecorded();
	const auto framesPerTick = Engine::framesPerTick();
	if (std::isfinite(framesPerTick) && framesPerTick > 0)
	{
		const auto ticks = std::min(static_cast<double>(frames) / framesPerTick,
			static_cast<double>(std::numeric_limits<tick_t>::max()));
		const TimePos length(static_cast<tick_t>(ticks));
		if (length > m_minLength) { m_minLength = length; }
	}
}

bool SampleRecordHandle::isFinished() const
{
	return !m_clip || m_recording.failed() || (m_frameLimit && framesRecorded() >= m_frameLimit);
}
bool SampleRecordHandle::recordsClip(const SampleClip* clip) const { return m_clipIdentity == clip; }
bool SampleRecordHandle::isFromTrack(const Track* track) const { return m_track == track || m_patternTrack == track; }
f_cnt_t SampleRecordHandle::framesRecorded() const { return m_framesRecorded.load(std::memory_order_relaxed); }

std::shared_ptr<const SampleBuffer> SampleRecordHandle::createSampleBuffer()
{
	if (Engine::audioEngine() && Engine::audioEngine()->inputErrorEpoch() != m_inputErrorEpoch)
	{
		m_recording.reject(QObject::tr("Native audio input was lost or rejected during the take"));
	}
	QString error; const auto result = m_recording.finalize(error);
	if (!result) { qWarning() << error; }
	return result;
}

void SampleRecordHandle::writeBuffer(const SampleFrame* input, f_cnt_t frames)
{
	if (m_frameLimit) { frames = std::min(frames, m_frameLimit - std::min(m_frameLimit, framesRecorded())); }
	m_recording.append(input, frames, Engine::audioEngine()->inputSampleRate());
	m_framesRecorded.store(m_recording.framesRecorded(), std::memory_order_relaxed);
}
} // namespace lmms

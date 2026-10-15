/*
 * AudioBusHandle.cpp - ThreadableJob between PlayHandle and MixerChannel
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2025 Johannes Lorenz <jlsf2013$users.sourceforge.net, $=@>
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

#include "AudioBusHandle.h"

#include <QMutexLocker>
#include <stdexcept>

#include "AudioDevice.h"
#include "AudioEngine.h"
#include "EffectChain.h"
#include "Mixer.h"
#include "Engine.h"
#include "MixHelpers.h"
#include "Track.h"

namespace lmms
{

AudioBusHandle::AudioBusHandle(const QString& name, bool hasEffectChain,
	FloatModel* volumeModel, FloatModel* panningModel,
	BoolModel* mutedModel, const Track* renderTrack) :
	m_bufferUsage(false),
	m_buffer(Engine::audioEngine()->framesPerPeriod()),
	m_extOutputEnabled(false),
	m_nextMixerChannel(0),
	m_name(name),
	m_effects(hasEffectChain ? new EffectChain(nullptr) : nullptr),
	m_volumeModel(volumeModel),
	m_panningModel(panningModel),
	m_mutedModel(mutedModel),
	m_renderTrack(renderTrack)
{
	m_buffer.allocateInterleavedBuffer();

	const auto guard = Engine::audioEngine()->requestChangesGuard();
	Engine::audioEngine()->addAudioBusHandle(this);
	try { setExtOutputEnabled(true); }
	catch (...) { Engine::audioEngine()->removeAudioBusHandle(this); throw; }
}




AudioBusHandle::~AudioBusHandle()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_externalOutput) { m_externalOutput->retire(); }
	setExtOutputEnabled(false);
	Engine::audioEngine()->removeAudioBusHandle(this);
}




std::shared_ptr<const AudioBusOutput> AudioBusHandle::externalOutput()
{
	const auto engine = Engine::audioEngine();
	if (QThread::currentThread() != engine->thread())
	{ throw std::logic_error("Native bus outputs must be acquired on the model owner thread"); }
	const auto guard = engine->requestChangesGuard();
	if (!m_externalOutput) { m_externalOutput.reset(new AudioBusOutput(engine->framesPerPeriod())); }
	return m_externalOutput;
}

void AudioBusHandle::setExtOutputEnabled(bool enabled)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (enabled != m_extOutputEnabled)
	{
		if (enabled)
		{
			Engine::audioEngine()->audioDev()->registerPort(this);
		}
		else
		{
			Engine::audioEngine()->audioDev()->unregisterPort(this);
		}
		if (m_externalOutput) { m_externalOutput->silence(); }
		m_extOutputEnabled = enabled;
	}
}




void AudioBusHandle::setName(const QString& newName)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	m_name = newName;
	Engine::audioEngine()->audioDev()->renamePort(this);
}




bool AudioBusHandle::processEffects()
{
	if (m_effects)
	{
		bool more = m_effects->processAudioBuffer(m_buffer);
		return more;
	}
	return false;
}


void AudioBusHandle::doProcessing()
{
	// Muted and render-excluded jobs still discard their previous audio. The
	// external write period was cleared by AudioEngine before jobs were queued.
	m_buffer.silenceAllChannels();
	m_bufferUsage = false;
	if ((m_mutedModel && m_mutedModel->value()) || (m_renderTrack && m_renderTrack->isRenderExcluded()))
	{
		return;
	}

	const f_cnt_t fpp = Engine::audioEngine()->framesPerPeriod();

	//qDebug( "Playhandles: %d", m_playHandles.size() );
	for (PlayHandle* ph : m_playHandles) // now we mix all playhandle buffers into our internal buffer
	{
		if (ph->buffer())
		{
			if (ph->usesBuffer()
				&& (ph->type() == PlayHandle::Type::NotePlayHandle
					|| !MixHelpers::isSilent(ph->buffer(), fpp)))
			{
				m_bufferUsage = true;

				// Writing to temporary interleaved buffer until PlayHandle and MixHelpers switch to planar
				MixHelpers::add(m_buffer.interleavedBuffer().asSampleFrames().data(), ph->buffer(), fpp);
			}
			ph->releaseBuffer(); 	// gets rid of playhandle's buffer and sets
									// pointer to null, so if it doesn't get re-acquired we know to skip it next time
		}
	}

	if (m_bufferUsage)
	{
		// PlayHandle buffers were written to the temporary interleaved buffer
		auto buffer = m_buffer.interleavedBuffer();

		// handle volume and panning
		// has both vol and pan models
		if (m_volumeModel && m_panningModel)
		{
			ValueBuffer* volBuf = m_volumeModel->valueBuffer();
			ValueBuffer* panBuf = m_panningModel->valueBuffer();

			// both vol and pan have s.ex.data:
			if (volBuf && panBuf)
			{
				for (f_cnt_t f = 0; f < fpp; ++f)
				{
					float v = volBuf->values()[f] * 0.01f;
					float p = panBuf->values()[f] * 0.01f;
					buffer[f][0] *= (p <= 0 ? 1.0f : 1.0f - p) * v;
					buffer[f][1] *= (p >= 0 ? 1.0f : 1.0f + p) * v;
				}
			}

			// only vol has s.ex.data:
			else if (volBuf)
			{
				float p = m_panningModel->value() * 0.01f;
				float l = (p <= 0 ? 1.0f : 1.0f - p);
				float r = (p >= 0 ? 1.0f : 1.0f + p);
				for (f_cnt_t f = 0; f < fpp; ++f)
				{
					float v = volBuf->values()[f] * 0.01f;
					buffer[f][0] *= v * l;
					buffer[f][1] *= v * r;
				}
			}

			// only pan has s.ex.data:
			else if (panBuf)
			{
				float v = m_volumeModel->value() * 0.01f;
				for (f_cnt_t f = 0; f < fpp; ++f)
				{
					float p = panBuf->values()[f] * 0.01f;
					buffer[f][0] *= (p <= 0 ? 1.0f : 1.0f - p) * v;
					buffer[f][1] *= (p >= 0 ? 1.0f : 1.0f + p) * v;
				}
			}

			// neither has s.ex.data:
			else
			{
				float p = m_panningModel->value() * 0.01f;
				float v = m_volumeModel->value() * 0.01f;
				for (f_cnt_t f = 0; f < fpp; ++f)
				{
					buffer[f][0] *= (p <= 0 ? 1.0f : 1.0f - p) * v;
					buffer[f][1] *= (p >= 0 ? 1.0f : 1.0f + p) * v;
				}
			}
		}

		// has vol model only
		else if (m_volumeModel)
		{
			ValueBuffer* volBuf = m_volumeModel->valueBuffer();

			if (volBuf)
			{
				for (f_cnt_t f = 0; f < fpp; ++f)
				{
					float v = volBuf->values()[f] * 0.01f;
					buffer[f][0] *= v;
					buffer[f][1] *= v;
				}
			}
			else
			{
				float v = m_volumeModel->value() * 0.01f;
				for (f_cnt_t f = 0; f < fpp; ++f)
				{
					buffer[f][0] *= v;
					buffer[f][1] *= v;
				}
			}
		}

		// Copy from temporary interleaved buffer to the main planar buffer
		// so they stay in sync
		toPlanar(buffer, m_buffer.groupBuffers(0));

		const bool corrupted = m_buffer.hasNonfiniteAudio();
		if (Engine::audioEngine()->sanitizationEnabled()) { m_buffer.sanitizeAll(); }
		m_corrupted.store(corrupted, std::memory_order_relaxed);
		if (corrupted) { Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::NonfiniteOutput); }

		// Update silence status of all channels for instrument output
		m_buffer.updateAllSilenceFlags();
	}
	// as of now there's no situation where we only have panning model but no volume model
	// if we have neither, we don't have to do anything here - just pass the audio as is

	// handle effects
	const bool anyOutputAfterEffects = processEffects();
	if (anyOutputAfterEffects || m_bufferUsage)
	{
		if (m_extOutputEnabled && m_externalOutput)
		{
			// This is after native track gain/pan and effects, before mixer sends,
			// mixer effects and master gain. Publish a whole, finite stereo period.
			if (m_buffer.hasNonfiniteAudio())
			{
				Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::NonfiniteOutput);
			}
			else
			{
				for (f_cnt_t frame = 0; frame < fpp; ++frame)
				{
					m_externalOutput->m_write[frame] = SampleFrame(m_buffer.buffer(0)[frame], m_buffer.buffer(1)[frame]);
				}
			}
		}
		// TODO: improve the flow here - convert to pull model
		Engine::mixer()->mixToChannel(m_buffer, m_nextMixerChannel); // send output to mixer
		m_bufferUsage = false;
	}
}


void AudioBusHandle::addPlayHandle(PlayHandle* handle)
{
	QMutexLocker lockGuard(&m_playHandleLock);
	m_playHandles.append(handle);
}


void AudioBusHandle::removePlayHandle(PlayHandle* handle)
{
	QMutexLocker lockGuard(&m_playHandleLock);
	PlayHandleList::Iterator it = std::find(m_playHandles.begin(), m_playHandles.end(), handle);
	if (it != m_playHandles.end())
	{
		m_playHandles.erase(it);
	}
}

} // namespace lmms

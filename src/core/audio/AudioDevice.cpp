/*
 * AudioDevice.cpp - base-class for audio-devices used by LMMS audio engine
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <cstring>

#include "AudioDevice.h"
#include <stdexcept>
#include "AudioEngine.h"

namespace lmms
{

AudioDevice::AudioDevice(const ch_cnt_t _channels, AudioEngine* _audioEngine)
	: m_supportsCapture(false)
	, m_sampleRate(_audioEngine->outputSampleRate())
	, m_channels(_channels)
	, m_audioEngine(_audioEngine)
{
}



AudioDevice::~AudioDevice()
{
	assert(!isRunning() && "device should have been stopped before being destroyed");
}

AudioDevice::BackendRoute AudioDevice::setBackendRoute(bool, unsigned int, const QString&, bool, const std::function<bool()>&)
{
	throw std::runtime_error("The current audio backend does not expose native master graph routing");
}

void AudioDevice::configureBusOutput(AudioBusHandle*, bool, const std::function<bool()>&)
{
	throw std::runtime_error("The current audio backend does not expose native track audio ports");
}

AudioDevice::BackendRoute AudioDevice::setBusRoute(AudioBusHandle*, unsigned int, const QString&, bool, const std::function<bool()>&)
{
	throw std::runtime_error("The current audio backend does not expose native track audio routing");
}

void AudioDevice::startProcessing()
{
	auto status = m_processingStatus.load(std::memory_order_acquire);
	do { if (status & RunningFlag) { return; } }
	while (!m_processingStatus.compare_exchange_weak(status, RunningFlag, std::memory_order_acq_rel));
	try { startProcessingImpl(); }
	catch (...) { reportProcessingFailure(ProcessingFailure::Exception); throw; }
}

void AudioDevice::stopProcessing()
{
	m_processingStatus.fetch_and(~RunningFlag, std::memory_order_acq_rel);
	try { stopProcessingImpl(); }
	catch (...) { reportProcessingFailure(ProcessingFailure::Exception); throw; }
}

const char* AudioDevice::processingFailureName(ProcessingFailure failure)
{
	switch (failure)
	{
	case ProcessingFailure::Start: return "start";
	case ProcessingFailure::Stop: return "stop";
	case ProcessingFailure::Disconnected: return "disconnected";
	case ProcessingFailure::InvalidCallback: return "invalidCallback";
	case ProcessingFailure::Exception: return "exception";
	default: return "none";
	}
}

void AudioDevice::reportProcessingFailure(ProcessingFailure failure, std::int32_t nativeError) noexcept
{
	if (failure == ProcessingFailure::None) { return; }
	auto status = m_processingStatus.load(std::memory_order_acquire);
	if (((status >> 32) & 0x7fffffff) != 0) { return; }
	// Publish input loss before failure becomes observable to the control thread.
	if (m_supportsCapture && m_audioEngine) { m_audioEngine->invalidateInput(); }
	const auto failed = (static_cast<std::uint64_t>(failure) << 32) | static_cast<std::uint32_t>(nativeError);
	do { if (((status >> 32) & 0x7fffffff) != 0) { return; } }
	while (!m_processingStatus.compare_exchange_weak(status, failed, std::memory_order_acq_rel));
}

void AudioDevice::stopProcessingThread( QThread * thread )
{
	if( !thread->wait( 30000 ) )
	{
		fprintf( stderr, "Terminating audio device thread\n" );
		thread->terminate();
		if( !thread->wait( 1000 ) )
		{
			fprintf( stderr, "Thread not terminated yet\n" );
		}
	}
}



void AudioDevice::registerPort(AudioBusHandle*)
{
}




void AudioDevice::unregisterPort(AudioBusHandle*)
{
}




void AudioDevice::renamePort(AudioBusHandle*)
{
}

int AudioDevice::convertToS16(const SampleFrame* _ab,
								const f_cnt_t _frames,
								int_sample_t * _output_buffer,
								const bool _convert_endian )
{
	if( _convert_endian )
	{
		for( f_cnt_t frame = 0; frame < _frames; ++frame )
		{
			for( ch_cnt_t chnl = 0; chnl < channels(); ++chnl )
			{
				const auto sample = chnl < DEFAULT_CHANNELS ? _ab[frame][chnl] : 0.f;
				auto temp = static_cast<int_sample_t>(AudioEngine::clip(sample) * OUTPUT_SAMPLE_MULTIPLIER);

				( _output_buffer + frame * channels() )[chnl] =
						( temp & 0x00ff ) << 8 |
						( temp & 0xff00 ) >> 8;
			}
		}
	}
	else
	{
		for( f_cnt_t frame = 0; frame < _frames; ++frame )
		{
			for( ch_cnt_t chnl = 0; chnl < channels(); ++chnl )
			{
				(_output_buffer + frame * channels())[chnl]
						= static_cast<int_sample_t>(AudioEngine::clip(chnl < DEFAULT_CHANNELS ? _ab[frame][chnl] : 0.f) * OUTPUT_SAMPLE_MULTIPLIER);
			}
		}
	}

	return _frames * channels() * BYTES_PER_INT_SAMPLE;
}




void AudioDevice::clearS16Buffer( int_sample_t * _outbuf, const f_cnt_t _frames )
{

	assert( _outbuf != nullptr );

	memset( _outbuf, 0,  _frames * channels() * BYTES_PER_INT_SAMPLE );
}

} // namespace lmms

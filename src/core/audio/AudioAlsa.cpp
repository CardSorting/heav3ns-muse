/*
 * AudioAlsa.cpp - device-class which implements ALSA-PCM-output
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


#include "AudioAlsa.h"

#ifdef LMMS_HAVE_ALSA

#include "endian_handling.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "FileResourceIO.h"
#include "AlsaPcmIo.h"
#include <QScopeGuard>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lmms
{

namespace
{
const detail::AlsaPcmIo nativeIo;
constexpr int ProgressTimeoutMs = 10000;
constexpr int WaitIntervalMs = 25;
}

AudioAlsa::AudioAlsa(bool& successful, AudioEngine* engine)
	: AudioAlsa(successful, engine, nativeIo, probeDevice(), ProgressTimeoutMs)
{
}

AudioAlsa::AudioAlsa(bool& successful, AudioEngine* engine, const detail::AlsaPcmIo& io, const QString& device, int timeoutMs)
	: AudioDevice(DEFAULT_CHANNELS, engine), m_io(io), m_timeoutMs(timeoutMs)
{
	successful = false;
	const auto cleanup = qScopeGuard([&] { if (!successful) { closeResources(); } });
	const auto check = [] { QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } };
	check();
	if (setenv("PULSE_ALSA_HOOK_CONF", "/dev/null", 0))
	{ std::fprintf(stderr, "Could not avoid possible interception by PulseAudio\n"); }

	if (const auto error = snd_pcm_open(&m_handle, device.toUtf8().constData(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK); error < 0)
	{ std::fprintf(stderr, "Playback open error: %s\n", snd_strerror(error)); return; }
	check();
	if (snd_pcm_hw_params_malloc(&m_hwParams) < 0 || snd_pcm_sw_params_malloc(&m_swParams) < 0) { return; }
	if (const auto error = setHWParams(channels(), SND_PCM_ACCESS_RW_INTERLEAVED); error < 0)
	{ std::fprintf(stderr, "Setting of hwparams failed: %s\n", snd_strerror(error)); return; }
	check();
	if (const auto error = setSWParams(); error < 0)
	{ std::fprintf(stderr, "Setting of swparams failed: %s\n", snd_strerror(error)); return; }

	// Touch only descriptors actually returned by this PCM. Some plugins expose
	// no poll descriptors; neighbouring descriptor numbers belong to other owners.
	const auto count = snd_pcm_poll_descriptors_count(m_handle);
	if (count < 0) { return; }
	if (count > 0)
	{
		std::vector<pollfd> descriptors(static_cast<std::size_t>(count));
		const auto filled = snd_pcm_poll_descriptors(m_handle, descriptors.data(), count);
		if (filled < 0 || filled > count) { return; }
		for (int i = 0; i < filled; ++i)
		{
			check();
			const auto fd = descriptors[i].fd;
			if (fd < 0) { continue; }
			const auto flags = fcntl(fd, F_GETFD, 0);
			if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) { return; }
		}
	}
	check(); successful = true;
}

AudioAlsa::~AudioAlsa()
{
	AudioDevice::stopProcessing();
	closeResources();
}

void AudioAlsa::closeResources() noexcept
{
	if (m_handle) { snd_pcm_close(m_handle); m_handle = nullptr; }
	if (m_hwParams) { snd_pcm_hw_params_free(m_hwParams); m_hwParams = nullptr; }
	if (m_swParams) { snd_pcm_sw_params_free(m_swParams); m_swParams = nullptr; }
}


QString AudioAlsa::probeDevice()
{
	QString dev = ConfigManager::inst()->value( "audioalsa", "device" );
	if( dev == "" )
	{
		if( getenv( "AUDIODEV" ) != nullptr )
		{
			return getenv( "AUDIODEV" );
		}
		return "default";
	}
	return dev;
}




AudioAlsa::DeviceInfoCollection AudioAlsa::getAvailableDevices()
{
	DeviceInfoCollection deviceInfos;

	char** hints = nullptr;

	/* Enumerate sound devices */
	int err = snd_device_name_hint(-1, "pcm", (void***)&hints);
	if (err != 0)
	{
		return deviceInfos;
	}

	char** n = hints;
	while (*n != nullptr)
	{
		char *name = snd_device_name_get_hint(*n, "NAME");
		char *description = snd_device_name_get_hint(*n, "DESC");

		if (name != 0 && description != 0)
		{
			deviceInfos.push_back(DeviceInfo(QString(name), QString(description)));
		}

		free(name);
		free(description);

		n++;
	}

	//Free the hint buffer
	snd_device_name_free_hint((void**)hints);

	return deviceInfos;
}




bool AudioAlsa::processing() const
{
	return !m_stopRequested.load(std::memory_order_acquire) && AudioDevice::isRunning();
}

void AudioAlsa::requestStop()
{
	m_stopRequested.store(true, std::memory_order_release);
	m_stopSemaphore.release();
}

void AudioAlsa::fail(ProcessingFailure failure, int error) noexcept
{
	if (m_stopRequested.exchange(true, std::memory_order_acq_rel)) { return; }
	reportProcessingFailure(failure, error); m_startupSemaphore.release();
}

void AudioAlsa::startProcessingImpl()
{
	requestStop(); wait();
	while (m_stopSemaphore.tryAcquire()) { }
	while (m_startupSemaphore.tryAcquire()) { }
	m_cleanupFailure = ProcessingFailure::None; m_cleanupError = 0;
	m_prepared.store(false, std::memory_order_release);
	m_stopRequested.store(false, std::memory_order_release);
	QElapsedTimer deadline; deadline.start();
	start(QThread::HighPriority);
	try
	{
		for (;;)
		{
			QString error;
			if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
			if (!AudioDevice::isRunning() || m_prepared.load(std::memory_order_acquire)) { break; }
			if (deadline.elapsed() >= m_timeoutMs) { fail(ProcessingFailure::Start, -ETIMEDOUT); break; }
			m_startupSemaphore.tryAcquire(1, WaitIntervalMs);
		}
	}
	catch (...)
	{
		fail(ProcessingFailure::Start, -ECANCELED); requestStop(); wait(); throw;
	}
	if (!AudioDevice::isRunning()) { requestStop(); wait(); }
}

void AudioAlsa::stopProcessingImpl()
{
	requestStop(); wait();
	reportProcessingFailure(m_cleanupFailure, m_cleanupError);
}

bool AudioAlsa::recover(int error, QElapsedTimer& progress)
{
	if (!processing()) { return false; }
	if (error == -EINTR || error == -EAGAIN) { return true; }
	if (error == -EPIPE) { error = m_io.prepare(m_handle); }
#ifdef ESTRPIPE
	else if (error == -ESTRPIPE)
	{
		while (processing() && (error = m_io.resume(m_handle)) == -EAGAIN)
		{
			if (progress.elapsed() >= m_timeoutMs) { fail(ProcessingFailure::Disconnected, -ETIMEDOUT); return false; }
			m_stopSemaphore.tryAcquire(1, WaitIntervalMs);
		}
		if (!processing()) { return false; }
		if (error < 0) { error = m_io.prepare(m_handle); }
	}
#endif
	if (error < 0) { fail(ProcessingFailure::Disconnected, error); return false; }
	return processing();
}

bool AudioAlsa::writeBuffer(const float* data, snd_pcm_uframes_t frames)
{
	snd_pcm_uframes_t offset = 0;
	QElapsedTimer progress; progress.start();
	while (offset < frames && processing())
	{
		const auto remaining = frames - offset;
		const auto written = m_io.write(m_handle, data + offset * channels(), remaining);
		if (!processing()) { return false; }
		if (written > 0)
		{
			if (static_cast<snd_pcm_uframes_t>(written) > remaining)
			{ fail(ProcessingFailure::InvalidCallback, -EOVERFLOW); return false; }
			offset += static_cast<snd_pcm_uframes_t>(written); progress.restart(); continue;
		}
		if (written < std::numeric_limits<int>::min())
		{ fail(ProcessingFailure::InvalidCallback, -EOVERFLOW); return false; }
		if (progress.elapsed() >= m_timeoutMs) { fail(ProcessingFailure::Disconnected, -ETIMEDOUT); return false; }
		if (written == 0 || written == -EAGAIN)
		{
			const auto ready = m_io.wait(m_handle, WaitIntervalMs);
			if (ready < 0 && !recover(ready, progress)) { return false; }
		}
		else if (!recover(static_cast<int>(written), progress)) { return false; }
		// A plugin can report immediate readiness yet accept no frames. Avoid
		// spinning while preserving the unwritten suffix and the stop wakeup.
		m_stopSemaphore.tryAcquire(1, 1);
	}
	return offset == frames && processing();
}

void AudioAlsa::run()
{
	bool prepared = false;
	const auto cleanup = qScopeGuard([&]
	{
		if (prepared)
		{
			try
			{
				if (const auto error = m_io.drop(m_handle); error < 0)
				{ m_cleanupError = error; m_cleanupFailure = ProcessingFailure::Stop; }
			}
			catch (...) { m_cleanupError = -EIO; m_cleanupFailure = ProcessingFailure::Exception; }
		}
		m_startupSemaphore.release();
	});
	try
	{
		if (!m_handle) { fail(ProcessingFailure::Start, -ENODEV); return; }
		const auto frames = audioEngine()->framesPerAudioBuffer();
		auto buffer = std::vector<float>(static_cast<std::size_t>(frames) * channels());
		if (!processing()) { return; }
		if (const auto error = m_io.prepare(m_handle); error < 0) { fail(ProcessingFailure::Start, error); return; }
		prepared = true;
		if (!processing()) { return; }
		m_prepared.store(true, std::memory_order_release); m_startupSemaphore.release();
		while (processing())
		{
			audioEngine()->renderNextBuffer({buffer.data(), channels(), frames});
			if (!writeBuffer(buffer.data(), frames)) { break; }
		}
	}
	catch (...) { fail(ProcessingFailure::Exception, -EIO); }
}


int AudioAlsa::setHWParams( const ch_cnt_t _channels, snd_pcm_access_t _access )
{
	// choose all parameters
	if (int err = snd_pcm_hw_params_any(m_handle, m_hwParams); err < 0)
	{
		printf( "Broken configuration for playback: no configurations "
				"available: %s\n", snd_strerror( err ) );
		return err;
	}

	// set the interleaved read/write format
	if (int err = snd_pcm_hw_params_set_access(m_handle, m_hwParams, _access); err < 0)
	{
		printf( "Access type not available for playback: %s\n",
							snd_strerror( err ) );
		return err;
	}

	// set the sample format
	if (int err = snd_pcm_hw_params_set_format(m_handle, m_hwParams, SND_PCM_FORMAT_FLOAT); err < 0)
	{
		printf("Failed to set PCM format: %s", snd_strerror(err));
		return err;
	}

	// set the count of channels
	if (int err = snd_pcm_hw_params_set_channels(m_handle, m_hwParams, _channels); err < 0)
	{
		printf( "Channel count (%i) not available for playbacks: %s\n"
				"(Does your soundcard not support surround?)\n",
					_channels, snd_strerror( err ) );
		return err;
	}

	// set the sample rate
	if (int err = snd_pcm_hw_params_set_rate(m_handle, m_hwParams, sampleRate(), 0); err < 0)
	{
		if (int err = snd_pcm_hw_params_set_rate(m_handle, m_hwParams, audioEngine()->baseSampleRate(), 0); err < 0)
		{
			printf( "Could not set sample rate: %s\n",
							snd_strerror( err ) );
			return err;
		}
		setSampleRate(audioEngine()->baseSampleRate());
	}

	m_periodSize = audioEngine()->framesPerPeriod();
	m_bufferSize = m_periodSize * 8;
	int dir = 0;
	if (int err = snd_pcm_hw_params_set_period_size_near(m_handle, m_hwParams, &m_periodSize, &dir); err < 0)
	{
		printf( "Unable to set period size %lu for playback: %s\n",
					m_periodSize, snd_strerror( err ) );
		return err;
	}
	dir = 0;
	if (int err = snd_pcm_hw_params_get_period_size(m_hwParams, &m_periodSize, &dir); err < 0)
	{
		printf( "Unable to get period size for playback: %s\n",
							snd_strerror( err ) );
		return err;
	}

	dir = 0;
	if (int err = snd_pcm_hw_params_set_buffer_size_near(m_handle, m_hwParams, &m_bufferSize); err < 0)
	{
		printf( "Unable to set buffer size %lu for playback: %s\n",
					m_bufferSize, snd_strerror( err ) );
		return ( err );
	}

	if (const auto error = snd_pcm_hw_params_get_buffer_size(m_hwParams, &m_bufferSize); error < 0) { return error; }
	if (m_periodSize == 0 || m_bufferSize / 2 < m_periodSize)
	{ std::fprintf(stderr, "Playback buffer is smaller than two periods\n"); return -EINVAL; }


	// write the parameters to device
	if (int err = snd_pcm_hw_params(m_handle, m_hwParams); err < 0)
	{
		printf( "Unable to set hw params for playback: %s\n",
							snd_strerror( err ) );
		return ( err );
	}

	return ( 0 );	// all ok
}




int AudioAlsa::setSWParams()
{
	// get the current swparams
	if (int err = snd_pcm_sw_params_current(m_handle, m_swParams); err < 0)
	{
		printf( "Unable to determine current swparams for playback: %s"
						"\n", snd_strerror( err ) );
		return err;
	}

	// start the transfer when a period is full
	if (int err = snd_pcm_sw_params_set_start_threshold(m_handle, m_swParams, m_periodSize); err < 0)
	{
		printf( "Unable to set start threshold mode for playback: %s\n",
							snd_strerror( err ) );
		return err;
	}

	// allow the transfer when at least m_periodSize samples can be
	// processed
	if (int err = snd_pcm_sw_params_set_avail_min(m_handle, m_swParams, m_periodSize); err < 0)
	{
		printf( "Unable to set avail min for playback: %s\n",
							snd_strerror( err ) );
		return err;
	}

	// align all transfers to 1 sample
	
#if SND_LIB_VERSION < ((1<<16)|(0)|16)
	if (int err = snd_pcm_sw_params_set_xfer_align(m_handle, m_swParams, 1); err < 0)
	{
		printf( "Unable to set transfer align for playback: %s\n",
							snd_strerror( err ) );
		return err;
	}
#endif

	// write the parameters to the playback device
	if (int err = snd_pcm_sw_params(m_handle, m_swParams); err < 0)
	{
		printf( "Unable to set sw params for playback: %s\n",
							snd_strerror( err ) );
		return err;
	}

	return 0;	// all ok
}

} // namespace lmms

#endif // LMMS_HAVE_ALSA

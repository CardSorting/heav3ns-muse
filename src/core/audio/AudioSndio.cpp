/*
 * AudioSndio.cpp - base-class that implements sndio audio support
 *
 * Copyright (c) 2010-2016 jackmsr@openbsd.net
 * Copyright (c) 2016-2017 David Carlier <devnexen@gmail.com>
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

#include "AudioSndio.h"

#ifdef LMMS_HAVE_SNDIO

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <QScopeGuard>
#include <QFormLayout>
#include <QLineEdit>

#include "endian_handling.h"
#include "LcdSpinBox.h"
#include "AudioEngine.h"

#include "ConfigManager.h"
#include "FileResourceIO.h"
#include "SndioApi.h"


namespace lmms
{

namespace
{
const detail::SndioApi nativeApi;
constexpr int ProgressTimeoutMs = 10000;
constexpr int PollIntervalMs = 25;
class OperationCancelled : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};
void checkWork()
{
	QString error;
	if (!FileResourceIO::check(error)) { throw OperationCancelled(error.toStdString()); }
}
}

AudioSndio::AudioSndio(bool& successful, AudioEngine* engine)
	: AudioSndio(successful, engine, nativeApi, ProgressTimeoutMs)
{
}

AudioSndio::AudioSndio(bool& successful, AudioEngine* engine, const detail::SndioApi& api, int timeoutMs)
	: AudioDevice(DEFAULT_CHANNELS, engine), m_api(api), m_timeoutMs(timeoutMs)
	, m_device(ConfigManager::inst()->value("audiosndio", "device").toUtf8())
{
	successful = false;
	successful = configureDevice(false);
}

bool AudioSndio::configureDevice(bool preserveRate)
{
	bool successful = false;
	const auto cleanup = qScopeGuard([&] { if (!successful) { closeDevice(); } });
	checkWork();
	m_hdl = m_api.open(m_device.isEmpty() ? SIO_DEVANY : m_device.constData(), SIO_PLAY, 1);
	if (!m_hdl) { std::fprintf(stderr, "sndio: could not open playback device\n"); return false; }
	checkWork();
	m_api.initParameters(&m_par);
	m_par.pchan = channels(); m_par.bits = sizeof(int_sample_t) * 8; m_par.bps = sizeof(int_sample_t);
	m_par.sig = 1; m_par.le = SIO_LE_NATIVE; m_par.rate = sampleRate();
	if (m_par.rate == 0) { return false; }
	m_par.round = audioEngine()->framesPerAudioBuffer();
	if (m_par.round == 0 || m_par.round > std::numeric_limits<unsigned int>::max() / 2) { return false; }
	m_par.appbufsz = m_par.round * 2;
	const auto requested = m_par;
	if (!m_api.setParameters(m_hdl, &m_par) || !m_api.getParameters(m_hdl, &m_par)) { return false; }
	checkWork();
	const auto difference = m_par.rate > requested.rate ? m_par.rate - requested.rate : requested.rate - m_par.rate;
	if (m_par.pchan != requested.pchan || m_par.bits != requested.bits || m_par.bps != requested.bps
		|| m_par.sig != requested.sig || m_par.le != requested.le || m_par.rate == 0
		|| m_par.round == 0 || m_par.appbufsz == 0
		// Preserve the existing integer-percent tolerance without signed overflow.
		|| static_cast<std::uint64_t>(difference) * 100 / requested.rate > 2
		|| (preserveRate && m_par.rate != requested.rate))
	{
		std::fprintf(stderr, "sndio: negotiated playback format is incompatible\n"); return false;
	}
	if (m_api.eof(m_hdl)) { return false; }
	setSampleRate(m_par.rate); successful = true; return true;
}

AudioSndio::~AudioSndio()
{
	AudioDevice::stopProcessing(); closeDevice();
}

void AudioSndio::closeDevice() noexcept
{
	if (m_hdl) { m_api.close(m_hdl); m_hdl = nullptr; }
}

bool AudioSndio::processing() const
{
	return !m_stopRequested.load(std::memory_order_acquire) && AudioDevice::isRunning();
}

void AudioSndio::requestStop()
{
	m_stopRequested.store(true, std::memory_order_release); m_stopSemaphore.release();
}

void AudioSndio::fail(ProcessingFailure failure, int error) noexcept
{
	if (m_stopRequested.exchange(true, std::memory_order_acq_rel)) { return; }
	reportProcessingFailure(failure, error); m_startupSemaphore.release();
}

void AudioSndio::startProcessingImpl()
{
	requestStop(); wait();
	while (m_startupSemaphore.tryAcquire()) { }
	while (m_stopSemaphore.tryAcquire()) { }
	const bool reopen = m_cleanupFailure != ProcessingFailure::None;
	m_cleanupFailure = ProcessingFailure::None; m_cleanupError = 0;
	m_started.store(false, std::memory_order_release); m_stopRequested.store(false, std::memory_order_release);
	try
	{
		checkWork();
		// EOF permanently poisons a sndio handle. Reopen the same configured sink
		// after joining; a different rate needs normal backend selection so the
		// engine can deliver its rate/quality notifications before starting DSP.
		if (reopen || !m_hdl || m_api.eof(m_hdl))
		{
			closeDevice();
			if (!configureDevice(true)) { fail(ProcessingFailure::Start, -EIO); return; }
		}
		QElapsedTimer deadline; deadline.start(); start(QThread::HighPriority);
		for (;;)
		{
			checkWork();
			if (!AudioDevice::isRunning() || m_started.load(std::memory_order_acquire)) { break; }
			if (deadline.elapsed() >= m_timeoutMs) { fail(ProcessingFailure::Start, -ETIMEDOUT); break; }
			m_startupSemaphore.tryAcquire(1, PollIntervalMs);
		}
	}
	catch (const OperationCancelled&)
	{
		fail(ProcessingFailure::Start, -ECANCELED); requestStop(); wait(); throw;
	}
	catch (...)
	{
		fail(ProcessingFailure::Exception, -EIO); requestStop(); wait(); throw;
	}
	if (!AudioDevice::isRunning()) { requestStop(); wait(); }
}

void AudioSndio::stopProcessingImpl()
{
	requestStop(); wait(); reportProcessingFailure(m_cleanupFailure, m_cleanupError);
}

bool AudioSndio::waitWritable(std::vector<pollfd>& descriptors, QElapsedTimer& progress)
{
	while (processing())
	{
		if (progress.elapsed() >= m_timeoutMs) { fail(ProcessingFailure::Disconnected, -ETIMEDOUT); return false; }
		std::fill(descriptors.begin(), descriptors.end(), pollfd{});
		const auto count = m_api.descriptors(m_hdl, descriptors.data(), POLLOUT);
		if (count < 0 || static_cast<std::size_t>(count) > descriptors.size())
		{ fail(ProcessingFailure::InvalidCallback, -EOVERFLOW); return false; }
		if (m_api.eof(m_hdl)) { fail(ProcessingFailure::Disconnected, -EIO); return false; }
		if (count == 0) { m_stopSemaphore.tryAcquire(1, PollIntervalMs); continue; }
		errno = 0;
		const auto ready = m_api.poll(descriptors.data(), static_cast<nfds_t>(count), PollIntervalMs);
		const auto error = errno;
		if (!processing()) { return false; }
		if (ready < 0)
		{
			if (error == EINTR) { m_stopSemaphore.tryAcquire(1, 1); continue; }
			fail(ProcessingFailure::Disconnected, error ? -error : -EIO); return false;
		}
		if (ready > count) { fail(ProcessingFailure::InvalidCallback, -EOVERFLOW); return false; }
		// revents also processes sndiod control messages. OS readiness alone is
		// not evidence that the audio stream can accept another byte.
		const auto events = m_api.events(m_hdl, descriptors.data());
		if (events < 0 || (events & (POLLHUP | POLLERR | POLLNVAL)) || m_api.eof(m_hdl))
		{ fail(ProcessingFailure::Disconnected, -EIO); return false; }
		if (events & POLLOUT) { return processing(); }
		if (ready > 0) { m_stopSemaphore.tryAcquire(1, 1); }
	}
	return false;
}

bool AudioSndio::writeBuffer(const int_sample_t* data, std::size_t samples, std::vector<pollfd>& descriptors)
{
	const auto bytes = samples * sizeof(int_sample_t);
	const auto source = reinterpret_cast<const unsigned char*>(data);
	std::size_t offset = 0;
	QElapsedTimer progress; progress.start();
	while (offset < bytes && processing())
	{
		const auto written = m_api.write(m_hdl, source + offset, bytes - offset);
		if (!processing()) { return false; }
		if (written > bytes - offset) { fail(ProcessingFailure::InvalidCallback, -EOVERFLOW); return false; }
		if (m_api.eof(m_hdl)) { fail(ProcessingFailure::Disconnected, -EIO); return false; }
		if (written > 0) { offset += written; progress.restart(); continue; }
		if (!waitWritable(descriptors, progress)) { return false; }
		// A ready stream which accepts zero bytes must not spin or render ahead.
		m_stopSemaphore.tryAcquire(1, 1);
	}
	return offset == bytes && processing();
}

void AudioSndio::run()
{
	bool started = false;
	const auto cleanup = qScopeGuard([&]
	{
		if (started)
		{
			try
			{
				if (!m_api.stop(m_hdl)) { m_cleanupFailure = ProcessingFailure::Stop; m_cleanupError = -EIO; }
			}
			catch (...) { m_cleanupFailure = ProcessingFailure::Exception; m_cleanupError = -EIO; }
		}
		m_startupSemaphore.release();
	});
	try
	{
		const auto frames = audioEngine()->framesPerAudioBuffer();
		const auto samples = static_cast<std::size_t>(frames) * channels();
		auto floats = std::vector<float>(samples); auto integers = std::vector<int_sample_t>(samples);
		const auto count = m_api.descriptorCount(m_hdl);
		if (count <= 0) { fail(ProcessingFailure::Start, -EINVAL); return; }
		auto descriptors = std::vector<pollfd>(static_cast<std::size_t>(count));
		if (!processing()) { return; }
		if (!m_api.start(m_hdl)) { fail(ProcessingFailure::Start, -EIO); return; }
		started = true;
		if (!processing()) { return; }
		if (m_api.eof(m_hdl)) { fail(ProcessingFailure::Start, -EIO); return; }
		m_started.store(true, std::memory_order_release); m_startupSemaphore.release();
		while (processing())
		{
			audioEngine()->renderNextBuffer({floats.data(), channels(), frames});
			for (std::size_t i = 0; i < samples; ++i)
			{
				if (!std::isfinite(floats[i])) { fail(ProcessingFailure::InvalidCallback, -EDOM); return; }
				integers[i] = static_cast<int_sample_t>(AudioEngine::clip(floats[i]) * OUTPUT_SAMPLE_MULTIPLIER);
			}
			if (!writeBuffer(integers.data(), integers.size(), descriptors)) { break; }
		}
	}
	catch (...) { fail(ProcessingFailure::Exception, -EIO); }
}


AudioSndio::setupWidget::setupWidget( QWidget * _parent ) :
	AudioDeviceSetupWidget( AudioSndio::name(), _parent )
{
	QFormLayout * form = new QFormLayout(this);

	m_device = new QLineEdit(ConfigManager::inst()->value("audiosndio", "device"), this);
	form->addRow(tr("Device"), m_device);

	gui::LcdSpinBoxModel * m = new gui::LcdSpinBoxModel( /* this */ );
	m->setRange(DEFAULT_CHANNELS, DEFAULT_CHANNELS);
	m->setStep( 2 );
	m->setValue( ConfigManager::inst()->value( "audiosndio",
	    "channels" ).toInt() );

	m_channels = new gui::LcdSpinBox( 1, this );
	m_channels->setModel( m );

	form->addRow(tr("Channels"), m_channels);
}


void AudioSndio::setupWidget::saveSettings()
{
	ConfigManager::inst()->setValue( "audiosndio", "device",
	    m_device->text() );
	ConfigManager::inst()->setValue( "audiosndio", "channels",
	    QString::number( m_channels->value<int>() ) );
}


} // namespace lmms

#endif	// LMMS_HAVE_SNDIO

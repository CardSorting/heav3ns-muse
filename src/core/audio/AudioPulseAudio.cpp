/*
 * AudioPulseAudio.cpp - device-class which implements PulseAudio-output
 *
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include <QFormLayout>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <algorithm>
#include <limits>
#include <stdexcept>

#include "AudioPulseAudio.h"

#ifdef LMMS_HAVE_PULSEAUDIO

#include "ConfigManager.h"
#include "LcdSpinBox.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "PulseAudioApi.h"

namespace lmms
{

namespace
{
const detail::PulseAudioApi nativeApi;
constexpr int StartupTimeoutMs = 10000;
}

AudioPulseAudio::AudioPulseAudio(bool& successful, AudioEngine* engine)
	: AudioPulseAudio(successful, engine, nativeApi, StartupTimeoutMs)
{
}

AudioPulseAudio::AudioPulseAudio(bool& successful, AudioEngine* engine, const detail::PulseAudioApi& api, int startupTimeoutMs)
	: AudioDevice(DEFAULT_CHANNELS, engine)
	, m_api(api)
	, m_startupTimeoutMs(startupTimeoutMs)
	, m_device(probeDevice().toUtf8())
	, m_periodBytes(static_cast<std::size_t>(engine->framesPerAudioBuffer()) * channels() * sizeof(float))
{
	m_sampleSpec.format = PA_SAMPLE_FLOAT32;
	m_sampleSpec.rate = sampleRate();
	m_sampleSpec.channels = channels();
	successful = pa_sample_spec_valid(&m_sampleSpec) && m_periodBytes > 0
		&& m_periodBytes < std::numeric_limits<std::uint32_t>::max();
}

AudioPulseAudio::~AudioPulseAudio()
{
	AudioDevice::stopProcessing();
}

QString AudioPulseAudio::probeDevice()
{
	const auto configured = ConfigManager::inst()->value("audiopa", "device");
	if (!configured.isEmpty()) { return configured; }
	const auto environment = qEnvironmentVariable("AUDIODEV");
	return environment.isEmpty() ? QStringLiteral("default") : environment;
}

void AudioPulseAudio::requestStop()
{
	m_stopRequested.store(true, std::memory_order_release);
	// wakeup is the cross-thread operation supported by the simple mainloop.
	// Hold the pointer lock only for wakeup; never join while holding it.
	const std::lock_guard lock{m_mainLoopMutex};
	if (m_mainLoop) { m_api.mainloopWakeup(m_mainLoop); }
}

void AudioPulseAudio::startProcessingImpl()
{
	// A failed previous run may still be releasing its context. Join it before
	// resetting readiness or consuming any notifications from the new run.
	requestStop();
	wait();
	while (m_startupSemaphore.tryAcquire()) { }
	m_streamReady.store(false, std::memory_order_release);
	m_stopRequested.store(false, std::memory_order_release);
	QElapsedTimer deadline; deadline.start();
	start(QThread::HighPriority);
	try
	{
		for (;;)
		{
			QString error;
			if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
			if (!AudioDevice::isRunning() || m_streamReady.load(std::memory_order_acquire)) { break; }
			if (deadline.elapsed() >= m_startupTimeoutMs)
			{
				fail(ProcessingFailure::Start, PA_ERR_TIMEOUT);
				break;
			}
			m_startupSemaphore.tryAcquire(1, 25);
		}
	}
	catch (...)
	{
		fail(ProcessingFailure::Start, PA_ERR_KILLED);
		requestStop(); wait();
		throw;
	}
	if (!AudioDevice::isRunning()) { requestStop(); wait(); }
}

void AudioPulseAudio::stopProcessingImpl()
{
	requestStop();
	// No forced termination: cleanup and every callback must finish before
	// the engine can release this device or install a replacement.
	wait();
}

void AudioPulseAudio::fail(ProcessingFailure failure, int error) noexcept
{
	if (m_stopRequested.exchange(true, std::memory_order_acq_rel)) { return; }
	reportProcessingFailure(failure, error);
	m_startupSemaphore.release();
}

void AudioPulseAudio::connectionFailed(pa_context* context) noexcept
{
	const auto error = context ? m_api.contextError(context) : PA_ERR_INTERNAL;
	fail(m_streamReady.load(std::memory_order_acquire) ? ProcessingFailure::Disconnected : ProcessingFailure::Start,
		error == PA_OK ? PA_ERR_CONNECTIONTERMINATED : error);
}

void AudioPulseAudio::contextStateCallback(pa_context* context, void* userdata) noexcept
{
	auto device = static_cast<AudioPulseAudio*>(userdata);
	try { device->contextStateChanged(context); }
	catch (...) { device->fail(ProcessingFailure::Exception, PA_ERR_INTERNAL); }
}

void AudioPulseAudio::streamStateCallback(pa_stream* stream, void* userdata) noexcept
{
	auto device = static_cast<AudioPulseAudio*>(userdata);
	try { device->streamStateChanged(stream); }
	catch (...) { device->fail(ProcessingFailure::Exception, PA_ERR_INTERNAL); }
}

void AudioPulseAudio::streamWriteCallback(pa_stream* stream, std::size_t length, void* userdata) noexcept
{
	auto device = static_cast<AudioPulseAudio*>(userdata);
	try { device->writeStream(stream, length); }
	catch (...) { device->fail(ProcessingFailure::Exception, PA_ERR_INTERNAL); }
}

void AudioPulseAudio::contextStateChanged(pa_context* context)
{
	if (m_stopRequested.load(std::memory_order_acquire)) { return; }
	switch (m_api.contextState(context))
	{
	case PA_CONTEXT_UNCONNECTED:
	case PA_CONTEXT_CONNECTING:
	case PA_CONTEXT_AUTHORIZING:
	case PA_CONTEXT_SETTING_NAME:
		return;
	case PA_CONTEXT_READY:
	{
		if (m_stream) { return; }
		m_stream = m_api.streamNew(context, "lmms", &m_sampleSpec, nullptr);
		if (!m_stream) { connectionFailed(context); return; }
		m_api.streamSetStateCallback(m_stream, streamStateCallback, this);
		m_api.streamSetWriteCallback(m_stream, streamWriteCallback, this);
		pa_buffer_attr attributes{};
		attributes.maxlength = attributes.minreq = attributes.fragsize = std::numeric_limits<std::uint32_t>::max();
		attributes.prebuf = 0; // Play silence instead of rewinding on underrun.
		// One native period in bytes, avoiding a seconds/milliseconds conversion.
		attributes.tlength = static_cast<std::uint32_t>(m_periodBytes);
		const auto sink = m_device.isEmpty() || m_device == "default" ? nullptr : m_device.constData();
		if (m_api.streamConnect(m_stream, sink, &attributes, PA_STREAM_ADJUST_LATENCY, nullptr, nullptr) < 0)
		{ connectionFailed(context); }
		return;
	}
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		connectionFailed(context);
		return;
	}
}

void AudioPulseAudio::streamStateChanged(pa_stream* stream)
{
	if (m_stopRequested.load(std::memory_order_acquire)) { return; }
	switch (m_api.streamState(stream))
	{
	case PA_STREAM_UNCONNECTED:
	case PA_STREAM_CREATING:
		return;
	case PA_STREAM_READY:
		if (!m_streamReady.exchange(true, std::memory_order_acq_rel)) { m_startupSemaphore.release(); }
		return;
	case PA_STREAM_FAILED:
	case PA_STREAM_TERMINATED:
		connectionFailed(m_api.streamContext(stream));
		return;
	}
}

void AudioPulseAudio::run()
{
	pa_mainloop* loop = nullptr;
	pa_context* context = nullptr;
	const auto cleanup = qScopeGuard([&]
	{
		if (m_stream)
		{
			m_api.streamSetWriteCallback(m_stream, nullptr, nullptr);
			m_api.streamSetStateCallback(m_stream, nullptr, nullptr);
			m_api.streamDisconnect(m_stream); m_api.streamUnref(m_stream); m_stream = nullptr;
		}
		if (context)
		{
			m_api.contextSetCallback(context, nullptr, nullptr);
			m_api.contextDisconnect(context); m_api.contextUnref(context);
		}
		{
			const std::lock_guard lock{m_mainLoopMutex};
			m_mainLoop = nullptr;
		}
		if (loop) { m_api.mainloopFree(loop); }
		m_startupSemaphore.release();
	});
	try
	{
		loop = m_api.mainloopNew();
		if (!loop) { fail(ProcessingFailure::Start, PA_ERR_INTERNAL); return; }
		{
			const std::lock_guard lock{m_mainLoopMutex};
			m_mainLoop = loop;
		}
		if (m_stopRequested.load(std::memory_order_acquire)) { return; }
		context = m_api.contextNew(m_api.mainloopGetApi(loop), "lmms");
		if (!context) { fail(ProcessingFailure::Start, PA_ERR_INTERNAL); return; }
		m_api.contextSetCallback(context, contextStateCallback, this);
		if (m_api.contextConnect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
		{ connectionFailed(context); return; }
		while (!m_stopRequested.load(std::memory_order_acquire) && AudioDevice::isRunning())
		{
			// prepare clears old wakeups. Recheck stop after it so a stop racing
			// with prepare cannot be lost immediately before the blocking poll.
			if (m_api.mainloopPrepare(loop, -1) < 0) { connectionFailed(context); break; }
			if (m_stopRequested.load(std::memory_order_acquire) || !AudioDevice::isRunning()) { break; }
			if (m_api.mainloopPoll(loop) < 0 || m_api.mainloopDispatch(loop) < 0)
			{ connectionFailed(context); break; }
		}
	}
	catch (...) { fail(ProcessingFailure::Exception, PA_ERR_INTERNAL); }
}

void AudioPulseAudio::writeStream(pa_stream* stream, std::size_t length)
{
	if (m_stopRequested.load(std::memory_order_acquire) || !AudioDevice::isRunning() || length == 0) { return; }
	const auto frameBytes = sizeof(float) * channels();
	if (stream != m_stream || !m_streamReady.load(std::memory_order_acquire) || length % frameBytes != 0)
	{ fail(ProcessingFailure::InvalidCallback, PA_ERR_INVALID); return; }
	while (length > 0 && !m_stopRequested.load(std::memory_order_acquire) && AudioDevice::isRunning())
	{
		const auto requested = std::min(length, m_periodBytes);
		auto bytes = requested;
		void* data = nullptr;
		if (m_api.streamBeginWrite(stream, &data, &bytes) < 0)
		{ connectionFailed(m_api.streamContext(stream)); return; }
		bool submitted = false;
		const auto cancel = qScopeGuard([&] { if (!submitted) { m_api.streamCancelWrite(stream); } });
		if (!data || reinterpret_cast<std::uintptr_t>(data) % alignof(float) != 0
			|| bytes == 0 || bytes > requested || bytes % frameBytes != 0)
		{ fail(ProcessingFailure::InvalidCallback, PA_ERR_INVALID); return; }
		if (m_stopRequested.load(std::memory_order_acquire) || !AudioDevice::isRunning()) { return; }
		audioEngine()->renderNextBuffer({static_cast<float*>(data), channels(), static_cast<f_cnt_t>(bytes / frameBytes)});
		if (m_stopRequested.load(std::memory_order_acquire) || !AudioDevice::isRunning()) { return; }
		// A write consumes the borrowed buffer, including the error path. Never
		// cancel or touch it after calling write (see PulseAudio stream.h).
		submitted = true;
		if (m_api.streamWrite(stream, data, bytes, nullptr, 0, PA_SEEK_RELATIVE) < 0)
		{ connectionFailed(m_api.streamContext(stream)); return; }
		length -= bytes;
	}
}


AudioPulseAudio::setupWidget::setupWidget( QWidget * _parent ) :
	AudioDeviceSetupWidget( AudioPulseAudio::name(), _parent )
{
	QFormLayout * form = new QFormLayout(this);

	m_device = new QLineEdit( AudioPulseAudio::probeDevice(), this );
	form->addRow(tr("Device"), m_device);

	auto m = new gui::LcdSpinBoxModel();
	m->setRange(DEFAULT_CHANNELS, DEFAULT_CHANNELS);
	m->setStep( 2 );
	m->setValue( ConfigManager::inst()->value( "audiopa",
										 "channels" ).toInt() );

	m_channels = new gui::LcdSpinBox( 1, this );
	m_channels->setModel( m );

	form->addRow(tr("Channels"), m_channels);
}




AudioPulseAudio::setupWidget::~setupWidget()
{
	delete m_channels->model();
}




void AudioPulseAudio::setupWidget::saveSettings()
{
	ConfigManager::inst()->setValue( "audiopa", "device",
							m_device->text() );
	ConfigManager::inst()->setValue( "audiopa", "channels",
				QString::number( m_channels->value<int>() ) );
}

} // namespace lmms

#endif // LMMS_HAVE_PULSEAUDIO


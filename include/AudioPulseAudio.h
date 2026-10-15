/*
 * AudioPulseAudio.h - device-class which implements PulseAudio-output
 *
 * Copyright (c) 2008-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_AUDIO_PULSEAUDIO_H
#define LMMS_AUDIO_PULSEAUDIO_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_PULSEAUDIO

#include <pulse/pulseaudio.h>
#include <QByteArray>
#include <QSemaphore>
#include <QThread>
#include <mutex>

#include "AudioDevice.h"
#include "AudioDeviceSetupWidget.h"

class QLineEdit;

namespace lmms
{

namespace detail { struct PulseAudioApi; }

namespace gui
{
class LcdSpinBox;
}


class AudioPulseAudio : public QThread, public AudioDevice
{
	Q_OBJECT
public:
	AudioPulseAudio( bool & _success_ful, AudioEngine* audioEngine );
	~AudioPulseAudio() override;

	inline static QString name()
	{
		return QT_TRANSLATE_NOOP( "AudioDeviceSetupWidget", "PulseAudio" );
	}

	static QString probeDevice();


	class setupWidget : public gui::AudioDeviceSetupWidget
	{
	public:
		setupWidget( QWidget * _parent );
		~setupWidget() override;

		void saveSettings() override;

	private:
		QLineEdit * m_device;
		gui::LcdSpinBox * m_channels;

	} ;


private:
	friend class PulseAudioTestAccess;
	AudioPulseAudio(bool& successful, AudioEngine* engine, const detail::PulseAudioApi& api, int startupTimeoutMs);
	void startProcessingImpl() override;
	void stopProcessingImpl() override;
	void run() override;
	void requestStop();
	void fail(ProcessingFailure failure, int error) noexcept;
	void connectionFailed(pa_context* context) noexcept;
	void contextStateChanged(pa_context* context);
	void streamStateChanged(pa_stream* stream);
	void writeStream(pa_stream* stream, std::size_t length);
	static void contextStateCallback(pa_context* context, void* userdata) noexcept;
	static void streamStateCallback(pa_stream* stream, void* userdata) noexcept;
	static void streamWriteCallback(pa_stream* stream, std::size_t length, void* userdata) noexcept;

	const detail::PulseAudioApi& m_api;
	const int m_startupTimeoutMs;
	const QByteArray m_device;
	const std::size_t m_periodBytes;
	pa_sample_spec m_sampleSpec{};
	pa_stream* m_stream = nullptr; // Worker thread only, including cleanup.
	std::mutex m_mainLoopMutex;
	pa_mainloop* m_mainLoop = nullptr; // Protected against concurrent wakeup/free.
	std::atomic_bool m_stopRequested{true};
	std::atomic_bool m_streamReady{false};
	QSemaphore m_startupSemaphore;

} ;

} // namespace lmms

#endif // LMMS_HAVE_PULSEAUDIO

#endif // LMMS_AUDIO_PULSEAUDIO_H

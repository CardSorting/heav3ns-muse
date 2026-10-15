/*
 * AudioAlsa.h - device-class that implements ALSA-PCM-output
 *
 * Copyright (c) 2004-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_AUDIO_ALSA_H
#define LMMS_AUDIO_ALSA_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_ALSA

// older ALSA-versions might require this
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <alsa/asoundlib.h>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <vector>

#include "AudioDevice.h"

namespace lmms
{

namespace detail { struct AlsaPcmIo; }

class AudioAlsa : public QThread, public AudioDevice
{
	Q_OBJECT
public:
	/**
	 * @brief Contains the relevant information about available ALSA devices
	 */
	class DeviceInfo
	{
	public:
		DeviceInfo(QString const & deviceName, QString const & deviceDescription) :
			m_deviceName(deviceName),
			m_deviceDescription(deviceDescription)
		{}
		~DeviceInfo() = default;

		QString const & getDeviceName() const { return m_deviceName; }
		QString const & getDeviceDescription() const { return m_deviceDescription; }

	private:
		QString m_deviceName;
		QString m_deviceDescription;

	};

	using DeviceInfoCollection = std::vector<DeviceInfo>;

public:
	AudioAlsa( bool & _success_ful, AudioEngine* audioEngine );
	~AudioAlsa() override;

	inline static QString name()
	{
		return QT_TRANSLATE_NOOP( "AudioDeviceSetupWidget",
			"ALSA (Advanced Linux Sound Architecture)" );
	}

	static QString probeDevice();

	//! @brief Creates a list of all available devices.
	//!
	//! Uses the hints API of ALSA to collect all devices. This also includes plug devices. The reason to collect these
	//! and not the raw hardware devices (e.g. hw:0,0) is that hardware devices often have a very limited number of
	//! supported formats, etc. Plugs on the other hand are software components that map all types of formats and inputs
	//! to the hardware and therefore they are much more flexible and more what we want.
	//!
	//! @see Further helpful info at [http://jan.newmarch.name/LinuxSound/Sampled/Alsa](https://web.archive.org/web/20211204221331/http://jan.newmarch.name/LinuxSound/Sampled/Alsa/).
	//! @return A collection of devices found on the system.
	static DeviceInfoCollection getAvailableDevices();

private:
	friend class AudioAlsaTestAccess;
	AudioAlsa(bool& successful, AudioEngine* engine, const detail::AlsaPcmIo& io, const QString& device, int timeoutMs);
	void startProcessingImpl() override;
	void stopProcessingImpl() override;
	void run() override;
	void requestStop();
	void closeResources() noexcept;
	void fail(ProcessingFailure failure, int error) noexcept;
	bool processing() const;
	bool recover(int error, QElapsedTimer& progress);
	bool writeBuffer(const float* data, snd_pcm_uframes_t frames);

	int setHWParams( const ch_cnt_t _channels, snd_pcm_access_t _access );
	int setSWParams();
	const detail::AlsaPcmIo& m_io;
	const int m_timeoutMs;
	std::atomic_bool m_stopRequested{true};
	std::atomic_bool m_prepared{false};
	QSemaphore m_startupSemaphore;
	QSemaphore m_stopSemaphore;
	// Written by the worker; read/reset only after joining it.
	ProcessingFailure m_cleanupFailure = ProcessingFailure::None;
	int m_cleanupError = 0;
	snd_pcm_t* m_handle = nullptr;

	snd_pcm_uframes_t m_bufferSize = 0;
	snd_pcm_uframes_t m_periodSize = 0;

	snd_pcm_hw_params_t* m_hwParams = nullptr;
	snd_pcm_sw_params_t* m_swParams = nullptr;
} ;

} // namespace lmms

#endif // LMMS_HAVE_ALSA

#endif // LMMS_AUDIO_ALSA_H

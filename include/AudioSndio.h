/*
 * AudioSndio.h - base-class that implements sndio audio support
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

#ifndef LMMS_AUDIO_SNDIO_H
#define LMMS_AUDIO_SNDIO_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_SNDIO

#include <QThread>
#include <QByteArray>
#include <QElapsedTimer>
#include <QSemaphore>
#include <vector>
#include <poll.h>
#include <sndio.h>

#include "AudioDevice.h"
#include "AudioDeviceSetupWidget.h"

class QLineEdit;

namespace lmms
{

namespace detail { struct SndioApi; }

namespace gui
{
class LcdSpinBox;
}


class AudioSndio : public QThread, public AudioDevice
{
	Q_OBJECT
public:
	AudioSndio( bool & _success_ful, AudioEngine * _audioEngine );
	~AudioSndio() override;

	inline static QString name()
	{
		return QT_TRANSLATE_NOOP( "AudioDeviceSetupWidget", "sndio" );
	}

	class setupWidget : public gui::AudioDeviceSetupWidget
	{
	public:
		setupWidget( QWidget * _parent );
		~setupWidget() override = default;

		void saveSettings() override;

	private:
		QLineEdit * m_device;
		gui::LcdSpinBox * m_channels;
	} ;

private:
	friend class AudioSndioTestAccess;
	AudioSndio(bool& successful, AudioEngine* engine, const detail::SndioApi& api, int timeoutMs);
	void startProcessingImpl() override;
	void stopProcessingImpl() override;
	void run() override;
	bool configureDevice(bool preserveRate);
	void closeDevice() noexcept;
	void requestStop();
	bool processing() const;
	void fail(ProcessingFailure failure, int error) noexcept;
	bool waitWritable(std::vector<pollfd>& descriptors, QElapsedTimer& progress);
	bool writeBuffer(const int_sample_t* data, std::size_t samples, std::vector<pollfd>& descriptors);

	const detail::SndioApi& m_api;
	const int m_timeoutMs;
	const QByteArray m_device;
	struct sio_hdl* m_hdl = nullptr;
	struct sio_par m_par{};
	std::atomic_bool m_stopRequested{true};
	std::atomic_bool m_started{false};
	QSemaphore m_startupSemaphore;
	QSemaphore m_stopSemaphore;
	// Written by the worker, read/reset only after joining it.
	ProcessingFailure m_cleanupFailure = ProcessingFailure::None;
	int m_cleanupError = 0;
};

} // namespace lmms

#endif // LMMS_HAVE_SNDIO

#endif // LMMS_AUDIO_SNDIO_H

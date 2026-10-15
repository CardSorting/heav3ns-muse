/*
 * AudioFileDevice.cpp - base-class for audio-device-classes which write
 *                       their output into a file
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

#include <QMessageBox>
#include <algorithm>
#include <cmath>
#include <limits>
#include "SampleFrame.h"
#include "lmms_constants.h"

#include "AudioFileDevice.h"
#include "ExportProjectDialog.h"
#include "GuiApplication.h"

namespace lmms
{

AudioFileDevice::AudioFileDevice( OutputSettings const & outputSettings,
					const ch_cnt_t _channels,
					const QString & _file,
					AudioEngine*  _audioEngine ) :
	AudioDevice( _channels, _audioEngine ),
	m_outputFile( _file ),
	m_outputSettings(outputSettings)
{
	using gui::ExportProjectDialog;

	setSampleRate( outputSettings.getSampleRate() );
	if (_channels == 0 || sampleRate() == 0 || sampleRate() > static_cast<sample_rate_t>(std::numeric_limits<int>::max()))
	{ recordWriteError(); return; }

	if( m_outputFile.open( QFile::WriteOnly | QFile::Truncate ) == false )
	{
		QString title, message;
		title = ExportProjectDialog::tr( "Could not open file" );
		message = ExportProjectDialog::tr( "Could not open file %1 "
						"for writing.\nPlease make "
						"sure you have write "
						"permission to the file and "
						"the directory containing the "
						"file and try again!"
								).arg( _file );

		if (gui::getGUI() != nullptr)
		{
			QMessageBox::critical( nullptr, title, message,
						QMessageBox::Ok,
						QMessageBox::NoButton );
		}
		else
		{
			fprintf( stderr, "%s\n", message.toUtf8().constData() );
		}
		recordWriteError();
	}
}




AudioFileDevice::~AudioFileDevice()
{
	m_outputFile.close();
}




bool AudioFileDevice::admitBuffer(const SampleFrame* data, f_cnt_t frames)
{
	if (frames == 0) { return false; }
	if (m_finalized || !outputFileOpened() || hasWriteError() || !data || channels() == 0
		|| frames > static_cast<f_cnt_t>(std::numeric_limits<int>::max()) / channels() / sizeof(float))
	{ recordWriteError(); return false; }
	for (f_cnt_t frame = 0; frame < frames; ++frame)
	{
		for (ch_cnt_t channel = 0; channel < std::min<ch_cnt_t>(channels(), DEFAULT_CHANNELS); ++channel)
		{ if (!std::isfinite(data[frame][channel])) { recordWriteError(); return false; } }
	}
	return true;
}

int AudioFileDevice::writeData( const void* data, int len )
{
	if (len >= 0 && (data || len == 0) && m_outputFile.isOpen())
	{
		const auto written = m_outputFile.write(static_cast<const char*>(data), len);
		if (written != len) { recordWriteError(); }
		return static_cast<int>(written);
	}

	recordWriteError();
	return -1;
}

bool AudioFileDevice::finalizeOutput()
{
	if (m_finalized) { return !hasWriteError(); }
	if (!m_outputFile.isOpen() || !m_outputFile.flush()) { recordWriteError(); }
	m_outputFile.close();
	if (m_outputFile.error() != QFile::NoError) { recordWriteError(); }
	m_finalized = true;
	return !hasWriteError();
}

} // namespace lmms

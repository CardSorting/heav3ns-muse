/*
 * AudioFileMP3.cpp - Audio-device which encodes a wave stream into
 *                    an MP3 file. This is used for song export.
 *
 * Copyright (c) 2017 to present Michael Gregorius <michael.gregorius.git/at/arcor[dot]de>
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

#include "AudioFileMP3.h"

#include "SampleFrame.h"
#include <array>

#ifdef LMMS_HAVE_MP3LAME


namespace lmms
{

AudioFileMP3::AudioFileMP3(	OutputSettings const & outputSettings,
				const ch_cnt_t channels,
				bool & successful,
				const QString & file,
				AudioEngine* audioEngine ) :
	AudioFileDevice( outputSettings, channels, file, audioEngine )
{
	// For now only accept stereo sources
	m_initialized = channels == 2 && outputFileOpened() && initEncoder();
	if (!m_initialized) { recordWriteError(); }
	successful = m_initialized;
}

AudioFileMP3::~AudioFileMP3()
{
	finalizeOutput();
	tearDownEncoder();
}

bool AudioFileMP3::finalizeOutput()
{
	if (!m_finalized)
	{
		if (m_initialized) { flushRemainingBuffers(); }
		else { recordWriteError(); }
		m_finalized = true;
	}
	return AudioFileDevice::finalizeOutput();
}

void AudioFileMP3::writeBuffer(const SampleFrame* _buf, const f_cnt_t _frames)
{
	if (!admitBuffer(_buf, _frames)) { return; }
	if (!m_initialized || m_finalized || !m_lame) { recordWriteError(); return; }

	m_interleavedData.resize(_frames * 2); auto& interleavedDataBuffer = m_interleavedData;
	for (f_cnt_t i = 0; i < _frames; ++i)
	{
		interleavedDataBuffer[2*i] = _buf[i][0];
		interleavedDataBuffer[2*i + 1] = _buf[i][1];
	}

	const auto minimumBufferSize = static_cast<std::size_t>((_frames * 5 + 3) / 4 + 7200);
	m_encodingBuffer.resize(minimumBufferSize); auto& encodingBuffer = m_encodingBuffer;

	int bytesWritten = lame_encode_buffer_interleaved_ieee_float(m_lame, interleavedDataBuffer.data(), static_cast<int>(_frames), encodingBuffer.data(), static_cast<int>(encodingBuffer.size()));
	if (bytesWritten < 0) { recordWriteError(); return; }

	writeData(&encodingBuffer[0], bytesWritten);
}

void AudioFileMP3::flushRemainingBuffers()
{
	// The documentation states that flush should have at least 7200 bytes. So let's be generous.
	std::array<unsigned char, 7200 * 4> encodingBuffer{};

	int bytesWritten = lame_encode_flush(m_lame, &encodingBuffer[0], static_cast<int>(encodingBuffer.size()));
	if (bytesWritten < 0) { recordWriteError(); return; }

	writeData(&encodingBuffer[0], bytesWritten);
}

MPEG_mode mapToMPEG_mode(OutputSettings::StereoMode stereoMode)
{
	switch (stereoMode)
	{
	case OutputSettings::StereoMode::Stereo:
		return STEREO;
	case OutputSettings::StereoMode::JointStereo:
		return JOINT_STEREO;
	case OutputSettings::StereoMode::Mono:
		return MONO;
	default:
		return NOT_SET;
	}
}

bool AudioFileMP3::initEncoder()
{
	m_lame = lame_init();
	if (!m_lame) { return false; }

	// Handle stereo/joint/mono settings
	OutputSettings::StereoMode stereoMode = getOutputSettings().getStereoMode();
	lame_set_mode(m_lame, mapToMPEG_mode(stereoMode));

	// Handle bit rate settings
	int bitRate = static_cast<int>(getOutputSettings().bitrate());

	lame_set_VBR(m_lame, vbr_off);
	lame_set_brate(m_lame, bitRate);
	lame_set_in_samplerate(m_lame, static_cast<int>(getOutputSettings().getSampleRate()));

	// Add a comment
	id3tag_init(m_lame);
	id3tag_set_comment(m_lame, "Created with LMMS");

	return lame_init_params(m_lame) >= 0;
}

void AudioFileMP3::tearDownEncoder()
{
	if (m_lame) { lame_close(m_lame); m_lame = nullptr; }
}

} // namespace lmms

#endif // LMMS_HAVE_MP3LAME

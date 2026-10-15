/*
 * AudioFileOgg.cpp - audio-device which encodes wave-stream and writes it
 *                    into an OGG-file. This is used for song-export.
 *
 * This file is based on encode.c from vorbis-tools-source, for more information
 * see below.
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

#include "AudioFileOgg.h"

#ifdef LMMS_HAVE_OGGVORBIS

#include <vorbis/vorbisenc.h>
#include <QUuid>

#include "SampleFrame.h"
#include "lmms_constants.h"

namespace lmms
{

AudioFileOgg::AudioFileOgg(OutputSettings const& outputSettings, const ch_cnt_t channels, bool& successful,
	const QString& file, AudioEngine* audioEngine)
	: AudioFileDevice(outputSettings, channels, file, audioEngine)
{
	successful = false;
	vorbis_info_init(&m_vi);
	vorbis_comment_init(&m_vc);
	if (!outputFileOpened()) { return; }

	const auto bitrate = outputSettings.bitrate();
	static constexpr auto maxBitrate = 320;

	if (vorbis_encode_init_vbr(&m_vi, channels, sampleRate(), static_cast<float>(bitrate) / maxBitrate))
	{
		return;
	}

	if (vorbis_analysis_init(&m_vds, &m_vi) != 0) { return; }
	m_dspInitialized = true;
	if (vorbis_block_init(&m_vds, &m_vb) != 0) { return; }
	m_blockInitialized = true;
	vorbis_comment_add_tag(&m_vc, "Cool", "This song has been made using LMMS");

	auto headerPackets = std::array<ogg_packet, 3>{};
	if (vorbis_analysis_headerout(&m_vds, &m_vc, &headerPackets[0], &headerPackets[1], &headerPackets[2]) != 0)
	{
		return;
	}

	const auto serial = static_cast<int>(QUuid::createUuid().data1 & 0x7fffffffu);
	if (ogg_stream_init(&m_oss, serial) != 0) { return; }
	m_streamInitialized = true;

	for (auto& packet : headerPackets)
	{
		if (ogg_stream_packetin(&m_oss, &packet) != 0) { recordWriteError(); return; }
	}

	while (ogg_stream_flush(&m_oss, &m_page))
	{
		writeData(m_page.header, m_page.header_len);
		writeData(m_page.body, m_page.body_len);
	}

	successful = !hasWriteError();
}

AudioFileOgg::~AudioFileOgg()
{
	finalizeOutput();
	if (m_streamInitialized) { ogg_stream_clear(&m_oss); }
	if (m_blockInitialized) { vorbis_block_clear(&m_vb); }
	if (m_dspInitialized) { vorbis_dsp_clear(&m_vds); }
	vorbis_comment_clear(&m_vc);
	vorbis_info_clear(&m_vi);
}

bool AudioFileOgg::finalizeOutput()
{
	if (!m_finalized)
	{
		if (m_dspInitialized && m_blockInitialized && m_streamInitialized)
		{
			// Zero frames flush the remaining codec packets exactly once.
			encodeBuffer(nullptr, 0);
		}
		else { recordWriteError(); }
		m_finalized = true;
	}
	return AudioFileDevice::finalizeOutput();
}

void AudioFileOgg::writeBuffer(const SampleFrame* _ab, const f_cnt_t _frames)
{
	if (!admitBuffer(_ab, _frames)) { return; }
	if (m_finalized || !m_dspInitialized || !m_blockInitialized || !m_streamInitialized) { recordWriteError(); return; }
	encodeBuffer(_ab, static_cast<int>(_frames));
}

void AudioFileOgg::encodeBuffer(const SampleFrame* _ab, int _frames)
{
	if (_frames == 0)
	{
		if (vorbis_analysis_wrote(&m_vds, 0) != 0) { recordWriteError(); return; }
	}
	else
	{
		const auto vab = vorbis_analysis_buffer(&m_vds, _frames);
		if (!vab) { recordWriteError(); return; }
		for (auto c = 0; c < channels(); ++c)
		{
			if (c < DEFAULT_CHANNELS)
			{
				for (int i = 0; i < _frames; ++i)
				{
					vab[c][i] = _ab[i][c];
				}
			}
			else
			{
				std::fill_n(vab[c], _frames, 0.0f);
			}
		}

		if (vorbis_analysis_wrote(&m_vds, _frames) != 0) { recordWriteError(); return; }
	}

	int blockStatus;
	while ((blockStatus = vorbis_analysis_blockout(&m_vds, &m_vb)) == 1)
	{
		if (vorbis_analysis(&m_vb, nullptr) != 0 || vorbis_bitrate_addblock(&m_vb) != 0)
		{
			recordWriteError(); return;
		}

		int packetStatus;
		while ((packetStatus = vorbis_bitrate_flushpacket(&m_vds, &m_packet)) == 1)
		{
			if (ogg_stream_packetin(&m_oss, &m_packet) != 0) { recordWriteError(); return; }

			do
			{
				if (ogg_stream_pageout(&m_oss, &m_page) == 0) { break; }
				writeData(m_page.header, m_page.header_len);
				writeData(m_page.body, m_page.body_len);
			} while (!ogg_page_eos(&m_page));
		}
		if (packetStatus < 0) { recordWriteError(); return; }
	}
	if (blockStatus < 0) { recordWriteError(); }
}

} // namespace lmms

#endif // LMMS_HAVE_OGGVORBIS



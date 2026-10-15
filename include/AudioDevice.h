/*
 * AudioDevice.h - base-class for audio-devices, used by LMMS audio engine
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

#ifndef LMMS_AUDIO_DEVICE_H
#define LMMS_AUDIO_DEVICE_H

#include <QMutex>
#include <QString>
#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <vector>
#if __has_include(<samplerate.h>)
#include <samplerate.h>
#endif

#include "LmmsTypes.h"

class QThread;

namespace lmms
{

class AudioEngine;
class AudioBusHandle;
class SampleFrame;


class AudioDevice
{
public:
	struct BackendGraph
	{
		struct Channel { unsigned int index; QString localPort, startupPeer; bool startupPeerTruncated = false; };
		struct Endpoint { QString name; std::vector<unsigned int> connectedChannels; };
		bool supported = false, observed = false;
		std::vector<Channel> channels;
		std::vector<Endpoint> endpoints;
		int nativeErrorCode = 0;
	};
	struct BackendRoute
	{
		QString localPort, remotePort;
		bool connected = false, changed = false, processingInterrupted = false;
	};
	// Live native master input/output graph, independent of project mixer routes.
	virtual BackendGraph inspectBackendGraph(bool) { return {}; }
	virtual BackendRoute setBackendRoute(bool input, unsigned int channel, const QString& remote, bool enabled,
		const std::function<bool()>& cancelled = {});
	struct BusGraph
	{
		BackendGraph graph;
		QString id, label, error;
		bool enabled = false, published = false, pending = false, labelTruncated = false;
		int nativeErrorCode = 0;
	};
	virtual BusGraph inspectBusGraph(AudioBusHandle*) { return {}; }
	virtual void configureBusOutput(AudioBusHandle*, bool enabled, const std::function<bool()>& cancelled = {});
	virtual BackendRoute setBusRoute(AudioBusHandle*, unsigned int channel, const QString& remote, bool enabled,
		const std::function<bool()>& cancelled = {});
	enum class ProcessingFailure : std::uint32_t
	{
		None, Start, Stop, Disconnected, InvalidCallback, Exception
	};
	struct ProcessingStatus
	{
		bool running;
		ProcessingFailure failure;
		std::int32_t nativeError;
	};
	AudioDevice( const ch_cnt_t _channels, AudioEngine* audioEngine );
	virtual ~AudioDevice();

	// if audio-driver supports ports, classes inheriting AudioBusHandle
	// (e.g. channel-tracks) can register themselves for making
	// audio-driver able to collect their individual output and provide
	// them at a specific port. JACK stages these hooks without native calls;
	// native port publication/retirement happens outside the model lock.
	virtual void registerPort(AudioBusHandle* port);
	virtual void unregisterPort(AudioBusHandle* port);
	virtual void renamePort(AudioBusHandle* port);

	inline bool supportsCapture() const
	{
		return m_supportsCapture;
	}

	inline sample_rate_t sampleRate() const
	{
		return m_sampleRate;
	}

	void startProcessing();

	void stopProcessing();

	ProcessingStatus processingStatus() const
	{
		const auto status = m_processingStatus.load(std::memory_order_acquire);
		return {static_cast<bool>(status & RunningFlag), static_cast<ProcessingFailure>((status >> 32) & 0x7fffffff),
			std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(status))};
	}
	bool isRunning() const { return processingStatus().running; }
	static const char* processingFailureName(ProcessingFailure failure);
	// Backends publish numeric failure state without allocating or entering Qt.
	// Control-thread start/stop calls must remain serialized by their owner.
	void reportProcessingFailure(ProcessingFailure failure, std::int32_t nativeError = 0) noexcept;

protected:
	// convert a given audio-buffer to a buffer in signed 16-bit samples
	// returns num of bytes in outbuf
	int convertToS16(const SampleFrame* _ab, const f_cnt_t _frames, int_sample_t* _output_buffer,
		const bool _convert_endian = false);

	// clear given signed-int-16-buffer
	void clearS16Buffer(int_sample_t* _outbuf, const f_cnt_t _frames);

	ch_cnt_t channels() const { return m_channels; }

	AudioEngine* audioEngine() { return m_audioEngine; }

	void setSampleRate(const sample_rate_t _new_sr) { m_sampleRate = _new_sr; }
	void setChannels(const ch_cnt_t channels) { m_channels = channels; }

	static void stopProcessingThread( QThread * thread );
protected:
	bool m_supportsCapture;

private:
	virtual void startProcessingImpl() = 0;
	virtual void stopProcessingImpl() = 0;

	sample_rate_t m_sampleRate;
	ch_cnt_t m_channels;
	AudioEngine* m_audioEngine = nullptr;
	static constexpr std::uint64_t RunningFlag = std::uint64_t{1} << 63;
	std::atomic<std::uint64_t> m_processingStatus{0};
};

} // namespace lmms

#endif // LMMS_AUDIO_DEVICE_H

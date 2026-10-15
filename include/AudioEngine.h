/*
 * AudioEngine.h - device-independent audio engine for LMMS
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

#ifndef LMMS_AUDIO_ENGINE_H
#define LMMS_AUDIO_ENGINE_H

#include <mutex>
#include <functional>
#include <exception>
#include <algorithm>

#include <QThread>
#include <QStringList>
#if __has_include(<samplerate.h>)
#include <samplerate.h>
#endif

#include <memory>
#include <vector>

#include "AudioBufferView.h"
#include "AudioDevice.h"
#include "LmmsTypes.h"
#include "SampleFrame.h"
#include "LocklessList.h"
#include "AudioEngineProfiler.h"
#include "PlayHandle.h"
#include "MidiClient.h"


namespace lmms
{

class AudioBusHandle;  // IWYU pragma: keep
class AudioBusOutput;
class AudioEngineWorkerThread;
class SampleClip;

constexpr f_cnt_t MINIMUM_BUFFER_SIZE = 32;
constexpr f_cnt_t DEFAULT_BUFFER_SIZE = 256;
constexpr f_cnt_t MAXIMUM_BUFFER_SIZE = 4096;

constexpr int BYTES_PER_SAMPLE = sizeof(sample_t);
constexpr int BYTES_PER_INT_SAMPLE = sizeof(int_sample_t);
constexpr int BYTES_PER_FRAME = sizeof(SampleFrame);

constexpr float OUTPUT_SAMPLE_MULTIPLIER = 32767.0f;

constexpr auto SUPPORTED_SAMPLERATES = std::array{44100, 48000, 88200, 96000, 192000};
constexpr auto SUPPORTED_BITRATES = std::array{64, 128, 160, 192, 256, 320};

class LMMS_EXPORT AudioEngine : public QObject
{
	Q_OBJECT
public:
	enum class DspFailure : std::uint8_t
	{
		None, NoteSetupException, InstrumentJobException, EffectJobException,
		MixerJobException, QueueOverflow, MixException, NonfiniteOutput, CorruptedEffect,
		MixerDependency
	};
	struct DspStatus { DspFailure failure; std::uint64_t failures; };
	DspStatus dspStatus() const
	{
		const auto state = m_dspState.load(std::memory_order_acquire);
		return {static_cast<DspFailure>(state & 0xff), state >> 8};
	}
	static const char* dspFailureName(DspFailure failure);
	// Numeric publication from native processing; no Qt callback or allocation.
	void reportDspFailure(DspFailure failure) noexcept;
	bool canResetDspFailure();
	// Brief native model admission; release its lock before JACK joins/drains.
	bool canChangeBackendGraph();
	// Joining callbacks from an owner-thread model edit would invert the lock order.
	bool canJoinBackendCallbacks() const;
	// Control-thread retry only, after transport, recording and export stop.
	bool resetDspFailure();
	/**
	 * @brief RAII helper for requestChangesInModel.
	 * Used by AudioEngine::requestChangesGuard.
	 */
	class RequestChangesGuard {
		friend class AudioEngine;

	private:
		RequestChangesGuard(AudioEngine* audioEngine)
			: m_audioEngine{audioEngine}
		{
			m_audioEngine->requestChangeInModel();
		}
	public:

		RequestChangesGuard()
			: m_audioEngine{nullptr}
		{
		}

		RequestChangesGuard(RequestChangesGuard&& other)
			: RequestChangesGuard()
		{
			std::swap(other.m_audioEngine, m_audioEngine);
		}

		// Disallow copy.
		RequestChangesGuard(const RequestChangesGuard&) = delete;
		RequestChangesGuard& operator=(const RequestChangesGuard&) = delete;

		~RequestChangesGuard() {
			if (m_audioEngine) {
				m_audioEngine->doneChangeInModel();
			}
		}

	private:
		AudioEngine* m_audioEngine;
	};

	void initDevices();
	void clear();
	void clearNewPlayHandles();
	void finishSampleRecordings(const SampleClip* clip = nullptr);


	// audio-device-stuff

	bool renderOnly() const { return m_renderOnly; }
	// Returns the current audio device's name. This is not necessarily
	// the user's preferred audio device, in case you were thinking that.
	inline const QString & audioDevName() const
	{
		return m_audioDevName;
	}
	inline bool audioDevStartFailed() const
	{
		return m_audioDevStartFailed;
	}
	const QString& requestedAudioBackend() const { return m_requestedAudioBackend; }
	const QString& requestedMidiBackend() const { return m_requestedMidiBackend; }
	struct MidiBackendStartupFailure
	{
		QString name;
		bool nativeCodeAvailable;
		int nativeErrorCode;
	};
	const std::vector<MidiBackendStartupFailure>& midiBackendStartupFailures() const { return m_midiBackendStartupFailures; }

	//! Set new audio device. Old device will be deleted,
	//! unless it's stored using storeAudioDevice
	void setAudioDevice(AudioDevice* _dev, bool startNow);
	// Native notification receivers catch their own exceptions and report the
	// first failure here. The owner rethrows only after Qt dispatch returns.
	bool reportDeviceNotificationFailure(std::exception_ptr failure) noexcept;
	// Select exactly one compiled factory using its loaded native settings.
	// Requires stopped/disarmed transport; failure restores the prior device.
	void selectAudioBackend(const QString& name);
	bool storeAudioDevice();
	// Retain a failed reservation for control-thread retry; safe in cleanup.
	bool restoreAudioDevice() noexcept;
	enum class DeviceRestoreFailure { None, WrongThread, Busy, StopException, NotificationException, RestartException, RestartFailed, ExportStopException };
	static const char* deviceRestoreFailureName(DeviceRestoreFailure failure);
	DeviceRestoreFailure deviceRestoreFailure() const { return m_deviceRestoreFailure.load(); }
	bool isRestoringAudioDevice() const { return m_restoringAudioDevice.load(); }
	bool isChangingAudioDevice() const { return m_changingAudioDevice.load(); }
	bool hasStoredAudioDevice() const { return m_oldAudioDev != nullptr; }
	// Monotonic identity for owner-thread cleanup; release does not reuse an ID.
	std::uint64_t audioDeviceReservationGeneration() const noexcept { return m_audioDeviceReservationGeneration.load(); }
	inline AudioDevice * audioDev()
	{
		return m_audioDev;
	}


	// audio-bus-handle-stuff
	inline void addAudioBusHandle(AudioBusHandle* busHandle)
	{
		const auto guard = requestChangesGuard();
		m_audioBusHandles.push_back(busHandle);
	}

	void removeAudioBusHandle(AudioBusHandle* busHandle);


	// MIDI-client-stuff
	inline const QString & midiClientName() const
	{
		return m_midiClientName;
	}

	inline MidiClient * midiClient()
	{
		return m_midiClient;
	}
	struct MidiBackendSelection
	{
		unsigned int ports = 0, inputResetsRequired = 0, retirementExceptions = 0, notificationFailures = 0;
		bool audioRestored = false;
		MidiClient::OutputAbandonment outputAbandonment;
		QString previousBackend;
	};
	// Exact, process-local selection. A returned result means MIDI committed;
	// audioRestored=false retains audio.device.restore recovery, not MIDI rollback.
	// Explicit abandonment retires eligible terminal output without claiming
	// physical cleanup; the returned receipt retains its uncertainty.
	MidiBackendSelection selectMidiBackend(const QString& name, bool abandonFailedOutput = false);
	bool isChangingMidiBackend() const { return m_changingMidiClient.load(); }


	// play-handle stuff
	bool addPlayHandle( PlayHandle* handle );

	void removePlayHandle( PlayHandle* handle );

	inline PlayHandleList& playHandles()
	{
		return m_playHandles;
	}

	void removePlayHandlesOfTypes(Track* track, PlayHandle::Types types, bool preserveReservedRecordings = false);

	AudioEngineProfiler& profiler()
	{
		return m_profiler;
	}

	int cpuLoad() const
	{
		return m_profiler.cpuLoad();
	}

	int detailLoad(const AudioEngineProfiler::DetailType type) const
	{
		return m_profiler.detailLoad(type);
	}

	sample_rate_t baseSampleRate() const { return m_baseSampleRate; }


	sample_rate_t outputSampleRate() const
	{
		return m_audioDev != nullptr ? m_audioDev->sampleRate() : m_baseSampleRate;
	}


	sample_rate_t inputSampleRate() const
	{
		return m_audioDev != nullptr ? m_audioDev->sampleRate() : m_baseSampleRate;
	}


	inline float masterGain() const
	{
		return m_masterGain;
	}

	inline void setMasterGain(const float mo)
	{
		m_masterGain = mo;
	}


	static inline sample_t clip(const sample_t s)
	{
		if (s > 1.0f)
		{
			return 1.0f;
		}
		else if (s < -1.0f)
		{
			return -1.0f;
		}
		return s;
	}


	bool criticalXRuns() const;

	void pushInputFrames( SampleFrame* _ab, const f_cnt_t _frames );
	std::uint64_t inputErrorEpoch() const { return m_inputErrorEpoch.load(std::memory_order_relaxed); }
	// Safe for backend shutdown callbacks; active takes and stale queued input fail.
	void invalidateInput() { m_inputErrorEpoch.fetch_add(1, std::memory_order_relaxed); }
	void discardInputBuffers();
	bool inputBufferFault() const
	{
		return m_inputBufferFault[m_inputBufferRead] || m_inputBufferEpoch[m_inputBufferRead] != inputErrorEpoch();
	}

	inline const SampleFrame* inputBuffer()
	{
		return m_inputBuffer[ m_inputBufferRead ];
	}

	inline f_cnt_t inputBufferFrames() const
	{
		return m_inputBufferFrames[ m_inputBufferRead ];
	}

	/**
	 * @returns The internal buffer size used by audio plugins and other processing done within the audio engine.
	 * Its value is @ref DEFAULT_BUFFER_SIZE or @ref framesPerAudioBuffer(), whichever is lower.
	 * @see renderNextPeriod()
	 */
	f_cnt_t framesPerPeriod() const { return m_framesPerPeriod; }

	/**
	 * @returns The buffer size used by the configured @ref AudioDevice "output audio device". It is in the range
	 * of @ref MINIMUM_BUFFER_SIZE and @ref MAXIMUM_BUFFER_SIZE.
	 * @note This should not be needed for a majority of cases. Currently, it's only being used to set up the
	 * audio devices. This member function may be removed in a later refactor.
	 */
	f_cnt_t framesPerAudioBuffer() const { return m_framesPerAudioBuffer; }

	/**
	 * @brief Renders the next audio period.
	 *
	 * An audio period is a fixed-size chunk of audio the engine generates, and represents a single cycle of the
	 * engine's output. The rendering is chunked into smaller periods to timely handle per-buffer updates like
	 * non-sample-accurate automation, as well as to improve memory cache performance.
	 *
	 * The audio period generated is interleaved and stereo.
	 *
	 * @note The audio period returned is non-owning and will be changed on subsequent calls to @ref renderNextPeriod()
	 * and @ref renderNextBuffer(). Callers must copy the data into their own local buffers if they need it to persist.
	 *
	 * @returns A non-owning buffer to the next audio period.
	 */
	std::span<const SampleFrame> renderNextPeriod();

	/**
	 * @brief Renders an audio buffer into @a dst.
	 *
	 * Renders @ref renderNextPeriod() "audio periods" into @a dst. If @a dst is not a multiple of the period
	 * size, the remaining frames are partially rendered (an extra period may be rendered in such cases, which can
	 * degrade performance).
	 *
	 * If @a dst has 1 channel, the channels are averaged to mono.
	 * If @a dst has 2 channels, the channels are directly copied.
	 * If @a dst has more than 2 channels, the stereo channels are copied and the rest are zero-filled.
	 *
	 * @param dst An audio buffer view to write into. Both interleaved and planar overloads are provided.
	 */
	void renderNextBuffer(InterleavedBufferView<float> dst) { renderNextBuffer<InterleavedBufferView<float>>(dst); }

	//! @copydoc renderNextBuffer(InterleavedBufferView<float>)
	void renderNextBuffer(PlanarBufferView<float> dst) { renderNextBuffer<PlanarBufferView<float>>(dst); }

	struct BusDestination
	{
		std::shared_ptr<const AudioBusOutput> source;
		PlanarBufferView<float> output;
	};
	/**
	 * Render master and native stereo buses with one shared period cursor.
	 * Destinations must have the same frame count as master and disjoint writable
	 * buffers. A null, disabled or retired bus source produces silence. Sources
	 * are retained on the model owner thread before the device callback starts.
	 * This holds the model lock through copying, so bus retirement cannot race
	 * a partially consumed period. It does not manage native device port lifetime.
	 */
	void renderNextBuffer(PlanarBufferView<float> dst, std::span<const BusDestination> buses);

	//! Block until a change in model can be done (i.e. wait for audio thread)
	void requestChangeInModel();
	void doneChangeInModel();

	RequestChangesGuard requestChangesGuard()
	{
		return RequestChangesGuard{this};
	}

	static bool isAudioDevNameValid(QString name);
	static bool isMidiDevNameValid(QString name);
	// Compiled native factories; discovery does not open or probe hardware.
	static QStringList availableAudioDeviceNames();
	static QStringList availableMidiClientNames();

	//! @returns true if sanitization of inf/nan values is enabled
	bool sanitizationEnabled() { return m_sanitizationEnabled.load(std::memory_order_relaxed); }

	//! Enable/disable sanitization of inf/nan values
	void setSanitizationEnabled(bool enabled) { m_sanitizationEnabled.store(enabled, std::memory_order_relaxed); }

signals:
	void qualitySettingsChanged();
	void sampleRateChanged();
	void nextAudioBuffer(const lmms::SampleFrame* buffer);


private:
	void copyBusOutputs(std::span<const BusDestination> buses, f_cnt_t sourceOffset, f_cnt_t outputOffset, f_cnt_t frames);
	static void silenceBusOutputs(std::span<const BusDestination> buses);
	std::uint64_t refreshOutputFailure();

	void renderNextBuffer(AudioBufferView<float> auto dst, std::span<const BusDestination> buses = {})
	{
		const auto failures = refreshOutputFailure();
		for (auto offset = f_cnt_t{0}; offset < dst.frames(); )
		{
			if (m_outputBufferReadIndex == m_framesPerPeriod) { m_outputBufferReadIndex = 0; }
			if (m_outputBufferReadIndex == 0) { renderNextPeriod(); }
			const auto frames = std::min(dst.frames() - offset, m_framesPerPeriod - m_outputBufferReadIndex);
			for (auto frame = f_cnt_t{0}; frame < frames; ++frame)
			{
				const auto& sample = m_outputBufferRead[m_outputBufferReadIndex + frame];
				switch (dst.channels())
				{
				case 0:
					assert(false);
					break;
				case 1:
					dst.sample(0, offset + frame) = sample.average();
					break;
				default:
					dst.sample(0, offset + frame) = sample[0];
					dst.sample(1, offset + frame) = sample[1];
					for (auto channel = 2; channel < dst.channels(); ++channel)
					{
						dst.sample(channel, offset + frame) = 0.f;
					}
					break;
				}
			}
			if (!buses.empty()) { copyBusOutputs(buses, m_outputBufferReadIndex, offset, frames); }
			m_outputBufferReadIndex += frames;
			offset += frames;
		}
		// A fault in a later native period invalidates the entire callback,
		// including an already-copied prefix and all independent bus outputs.
		const auto status = dspStatus();
		if (status.failure != DspFailure::None || status.failures != failures)
		{
			for (auto channel = ch_cnt_t{0}; channel < dst.channels(); ++channel)
			{
				for (auto frame = f_cnt_t{0}; frame < dst.frames(); ++frame) { dst.sample(channel, frame) = 0.f; }
			}
			silenceBusOutputs(buses);
			refreshOutputFailure();
		}
	}

	AudioEngine( bool renderOnly );
	~AudioEngine() override;

	void startProcessing() { m_audioDev->startProcessing(); }
	void stopProcessing() { m_audioDev->stopProcessing(); }


	AudioDevice * tryAudioDevices();
	std::unique_ptr<AudioDevice> createAudioDevice(const QString& name);
	void setAudioDeviceImpl(AudioDevice* device, bool startNow);
	void selectAudioBackend(const QString& name, const std::function<std::unique_ptr<AudioDevice>()>& factory);
	MidiClient * tryMidiClients();
	std::unique_ptr<MidiClient> createMidiClient(const QString& name, bool& successful);
	MidiBackendSelection selectMidiBackend(const QString& name, const std::function<std::unique_ptr<MidiClient>()>& factory, bool abandonFailedOutput = false);
	bool backendGraphIdle();
	bool canTransferMidiClient(const MidiClient* source);

	void renderStageNoteSetup();
	void renderStageInstruments();
	void renderStageEffects();
	void renderStageMix();


	void swapBuffers();

	void clearInternal();

	bool m_renderOnly;

	std::vector<AudioBusHandle*> m_audioBusHandles;

	f_cnt_t m_framesPerAudioBuffer;
	f_cnt_t m_framesPerPeriod;
	sample_rate_t m_baseSampleRate;
	std::atomic<std::uint64_t> m_inputErrorEpoch{0};
	// Low byte is the latched first cause; the high 56 bits count failures.
	std::atomic<std::uint64_t> m_dspState{0};
	std::uint64_t m_outputFailureCount = 0; // only the single native render consumer writes this
	bool m_inputBufferFault[2]{};
	std::uint64_t m_inputBufferEpoch[2]{};

	SampleFrame* m_inputBuffer[2];
	f_cnt_t m_inputBufferFrames[2];
	f_cnt_t m_inputBufferSize[2];
	int m_inputBufferRead;
	int m_inputBufferWrite;

	std::unique_ptr<SampleFrame[]> m_outputBufferRead;
	std::unique_ptr<SampleFrame[]> m_outputBufferWrite;
	f_cnt_t m_outputBufferReadIndex;

	// worker thread stuff
	std::vector<AudioEngineWorkerThread *> m_workers;
	int m_numWorkers;

	// playhandle stuff
	PlayHandleList m_playHandles;
	// place where new playhandles are added temporarily
	LocklessList<PlayHandle *> m_newPlayHandles;
	ConstPlayHandleList m_playHandlesToRemove;

	float m_masterGain;

	// audio device stuff
	AudioDevice * m_audioDev;
	AudioDevice * m_oldAudioDev;
	// Private test-peer checkpoints run only after Qt notification dispatch has
	// returned. Fault fixtures must never throw through a connected Qt slot.
	enum class DeviceTransitionPoint { SetupAfterQuality, SetupAfterRate, RestoreAfterQuality, RestoreAfterRate };
	void deviceTransitionCheckpoint(DeviceTransitionPoint point);
	std::function<void(DeviceTransitionPoint)> m_deviceTransitionProbe;
	void notifyDeviceChange(bool quality);
	std::exception_ptr* m_deviceNotificationFailure = nullptr;
	std::atomic<std::uint64_t> m_audioDeviceReservationGeneration{0};
	void updateMidiInputAdmission();
	bool m_storedAudioDeviceWasRunning = false;
	bool m_audioDeviceReservationTouched = false;
	bool m_restoredAudioDeviceInstalled = false;
	bool m_restoreQualityNotified = false;
	bool m_restoreRateNotified = false;
	std::atomic_bool m_restoringAudioDevice{false};
	std::atomic_bool m_changingAudioDevice{false};
	std::atomic<DeviceRestoreFailure> m_deviceRestoreFailure{DeviceRestoreFailure::None};
	QString m_audioDevName;
	QString m_requestedAudioBackend;
	QString m_requestedMidiBackend;
	bool m_audioDevStartFailed;

	// MIDI device stuff
	MidiClient * m_midiClient;
	QString m_midiClientName;
	std::atomic_bool m_changingMidiClient{false};
	std::vector<MidiBackendStartupFailure> m_midiBackendStartupFailures;

	AudioEngineProfiler m_profiler;

	bool m_clearSignal;
	std::atomic<bool> m_sanitizationEnabled = false;

	std::recursive_mutex m_changeMutex;

	friend class Engine;
	friend class Song;
	friend class AudioEngineWorkerThread;
	friend class ProjectRenderer;
	friend class StudioMidiLearnTestAccess;
	friend class StudioBackendSelectionTestAccess;
	friend class StudioMidiBackendSelectionTestAccess;
	friend class MidiClient;
	friend class MidiJackTestAccess;
	friend class AudioDeviceTransitionTestAccess;
} ;

} // namespace lmms

#endif // LMMS_AUDIO_ENGINE_H

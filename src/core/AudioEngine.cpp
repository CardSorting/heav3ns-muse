/*
 * AudioEngine.cpp - device-independent audio engine for LMMS
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

#include "AudioEngine.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include "AutomationClip.h"
#include "AutomatableModel.h"
#include "Controller.h"
#include "AutomationTrack.h"
#include "AudioFileDevice.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "RemotePlugin.h"
#include "Track.h"
#include "TrackRenderScope.h"
#include "PatternStore.h"
#include "SampleClip.h"
#include "SampleRecordingBuffer.h"
#include "SampleRecordHandle.h"

#include "MixHelpers.h"

#include "lmmsconfig.h"

#include "AudioEngineWorkerThread.h"
#include "AudioBusHandle.h"
#include "Hardware.h"
#include "Mixer.h"
#include "Song.h"
#include "EnvelopeAndLfoParameters.h"
#include "NotePlayHandle.h"
#include "ConfigManager.h"

// platform-specific audio-interface-classes
#include "AudioAlsa.h"
#include "AudioJack.h"
#include "AudioSndio.h"
#include "AudioPortAudio.h"
#include "AudioPulseAudio.h"
#include "AudioSdl.h"
#include "AudioDummy.h"

// platform-specific midi-interface-classes
#include "MidiAlsaRaw.h"
#include "MidiAlsaSeq.h"
#include "MidiJack.h"
#include "MidiSndio.h"
#include "MidiWinMM.h"
#include "MidiApple.h"
#include "MidiDummy.h"

#include "BufferManager.h"

namespace lmms
{

using LocklessListElement = LocklessList<PlayHandle*>::Element;

static thread_local bool s_renderingThread = false;
static thread_local unsigned int s_modelChangeDepth = 0;

const char* AudioEngine::dspFailureName(DspFailure failure)
{
	switch (failure)
	{
	case DspFailure::None: return "none";
	case DspFailure::NoteSetupException: return "note-setup-exception";
	case DspFailure::InstrumentJobException: return "instrument-job-exception";
	case DspFailure::EffectJobException: return "effect-job-exception";
	case DspFailure::MixerJobException: return "mixer-job-exception";
	case DspFailure::QueueOverflow: return "job-queue-overflow";
	case DspFailure::MixException: return "mix-exception";
	case DspFailure::NonfiniteOutput: return "nonfinite-output";
	case DspFailure::CorruptedEffect: return "corrupted-effect";
	case DspFailure::MixerDependency: return "mixer-dependency";
	}
	return "unknown";
}

void AudioEngine::reportDspFailure(DspFailure failure) noexcept
{
	if (failure == DspFailure::None) { return; }
	auto previous = m_dspState.load(std::memory_order_acquire);
	for (;;)
	{
		const auto count = previous >> 8;
		const auto nextCount = count < (std::numeric_limits<std::uint64_t>::max() >> 8) ? count + 1 : count;
		const auto cause = (previous & 0xff) ? previous & 0xff : static_cast<std::uint64_t>(failure);
		if (m_dspState.compare_exchange_weak(previous, (nextCount << 8) | cause, std::memory_order_acq_rel)) { return; }
	}
}

bool AudioEngine::canResetDspFailure()
{
	if (QThread::currentThread() != thread() || s_renderingThread) { return false; }
	const auto guard = std::lock_guard{m_changeMutex};
	const auto song = Engine::getSong();
	if (!song || !song->isStopped() || song->isRecording() || song->isExporting() || song->hasPendingExportCleanup()
		|| hasStoredAudioDevice() || dynamic_cast<AudioFileDevice*>(audioDev())) { return false; }
	const auto safeTrack = [](const Track* track)
	{
		if (!track) { return true; }
		for (const auto clip : track->getClips())
		{
			if (const auto sample = dynamic_cast<SampleClip*>(clip))
			{
				if (sample->isRecord() || sample->hasActiveRecording() || sample->hasPendingRecording()
					|| sample->isFinalizingRecording() || sample->hasReservedRecording()) { return false; }
			}
			if (const auto automation = dynamic_cast<AutomationClip*>(clip); automation && automation->isRecording()) { return false; }
		}
		return true;
	};
	for (const auto owner : {static_cast<TrackContainer*>(song), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		if (!owner) { return false; }
		for (const auto track : owner->tracks()) { if (!safeTrack(track)) { return false; } }
	}
	if (!safeTrack(song->globalAutomationTrack())) { return false; }
	return dspStatus().failures != (std::numeric_limits<std::uint64_t>::max() >> 8);
}

bool AudioEngine::canJoinBackendCallbacks() const
{
	return QThread::currentThread() == thread() && !s_renderingThread && s_modelChangeDepth == 0;
}

bool AudioEngine::canChangeBackendGraph()
{
	return !m_changingMidiClient && backendGraphIdle();
}

bool AudioEngine::canTransferMidiClient(const MidiClient* source)
{
	if (!canJoinBackendCallbacks()) { return false; }
	return (!m_changingMidiClient || source == m_midiClient) && backendGraphIdle();
}

bool AudioEngine::backendGraphIdle()
{
	if (!canJoinBackendCallbacks()) { return false; }
	const auto guard = std::lock_guard{m_changeMutex};
	const auto song = Engine::getSong(); const auto patterns = Engine::patternStore();
	if (!song || !patterns || !m_audioDev || m_changingAudioDevice || m_restoringAudioDevice || m_oldAudioDev
		|| !song->isStopped() || song->isPlaying() || song->isPaused() || song->isRecording() || song->isExporting() || song->hasPendingExportCleanup()
		|| dynamic_cast<AudioFileDevice*>(audioDev()) || RemotePlugin::hasActiveRenderContext() || TrackRenderScope::hasActiveSelection())
	{ return false; }
	const auto idleTrack = [](const Track* track) {
		if (!track || track->isNoteRecording()) { return false; }
		for (const auto clip : track->getClips())
		{
			if (const auto sample = dynamic_cast<SampleClip*>(clip); sample && (sample->isRecord() || sample->hasReservedRecording()
				|| sample->hasActiveRecording() || sample->hasPendingRecording() || sample->isFinalizingRecording())) { return false; }
			if (const auto curve = dynamic_cast<AutomationClip*>(clip); curve && (curve->isRecording() || curve->hasReservedRecording())) { return false; }
		}
		return true;
	};
	for (const auto owner : {static_cast<TrackContainer*>(song), static_cast<TrackContainer*>(patterns)})
	{ for (const auto track : owner->tracks()) { if (!idleTrack(track)) { return false; } } }
	return idleTrack(song->globalAutomationTrack());
}

bool AudioEngine::resetDspFailure()
{
	if (QThread::currentThread() != thread() || s_renderingThread) { return false; }
	const auto guard = std::lock_guard{m_changeMutex};
	if (!canResetDspFailure()) { return false; }
	auto previous = m_dspState.load(std::memory_order_acquire);
	if (!(previous & 0xff)) { return true; }
	if ((previous >> 8) == (std::numeric_limits<std::uint64_t>::max() >> 8)) { return false; }
	// Do not rewrite output buffers while a backend may be copying the silent
	// fault period. Clearing the latch permits the next period to retry DSP.
	if (!m_dspState.compare_exchange_strong(previous, previous & ~std::uint64_t{0xff}, std::memory_order_acq_rel)) { return false; }
	// A failed period can leave parameter/controller buffers marked current.
	// Retry must observe repaired values even before either musical clock moves.
	Controller::invalidateValueBuffers();
	AutomatableModel::invalidateValueBuffers();
	return true;
}

AudioEngine::AudioEngine(bool renderOnly)
	: m_renderOnly(renderOnly)
	, m_framesPerAudioBuffer(std::clamp(
		static_cast<f_cnt_t>(ConfigManager::inst()->value("audioengine", "framesperaudiobuffer",
		QString::number(DEFAULT_BUFFER_SIZE)).toUInt()),
		MINIMUM_BUFFER_SIZE, MAXIMUM_BUFFER_SIZE))
	, m_framesPerPeriod(std::min(m_framesPerAudioBuffer, DEFAULT_BUFFER_SIZE))
	, m_baseSampleRate(
		std::max(ConfigManager::inst()->value("audioengine", "samplerate").toInt(), SUPPORTED_SAMPLERATES.front()))
	, m_inputBufferRead(0)
	, m_inputBufferWrite(1)
	, m_outputBufferRead(nullptr)
	, m_outputBufferWrite(nullptr)
	, m_outputBufferReadIndex(0)
	, m_workers()
	, m_numWorkers(std::max(0, QThread::idealThreadCount() - 1))
	, m_newPlayHandles(PlayHandle::MaxNumber)
	, m_masterGain(1.0f)
	, m_audioDev(nullptr)
	, m_oldAudioDev(nullptr)
	, m_audioDevStartFailed(false)
	, m_profiler()
	, m_clearSignal(false)
	, m_sanitizationEnabled(ConfigManager::inst()->value("audioengine", "sanitizemix", "1").toInt())
{
	for( int i = 0; i < 2; ++i )
	{
		m_inputBufferFrames[i] = 0;
		m_inputBufferSize[i] = DEFAULT_BUFFER_SIZE * 100;
		m_inputBuffer[i] = new SampleFrame[ DEFAULT_BUFFER_SIZE * 100 ];
		zeroSampleFrames(m_inputBuffer[i], m_inputBufferSize[i]);
	}

	BufferManager::init( m_framesPerPeriod );
	m_outputBufferRead = std::make_unique<SampleFrame[]>(m_framesPerPeriod);
	m_outputBufferWrite = std::make_unique<SampleFrame[]>(m_framesPerPeriod);


	for( int i = 0; i < m_numWorkers; ++i )
	{
		auto wt = new AudioEngineWorkerThread(this);
		wt->start( QThread::TimeCriticalPriority );
		m_workers.push_back( wt );
	}
}




AudioEngine::~AudioEngine()
{
	for( int w = 0; w < m_numWorkers; ++w )
	{
		m_workers[w]->quit();
	}

	for( int w = 0; w < m_numWorkers; ++w )
	{
		m_workers[w]->wait();
	}

	delete m_midiClient;
	if (m_oldAudioDev && m_oldAudioDev != m_audioDev) { delete m_oldAudioDev; }
	delete m_audioDev;


	for (const auto& input : m_inputBuffer)
	{
		delete[] input;
	}
}




void AudioEngine::initDevices()
{
	bool success_ful = false;
	m_requestedAudioBackend = m_renderOnly ? AudioDummy::name() : ConfigManager::inst()->value("audioengine", "audiodev");
	m_requestedMidiBackend = m_renderOnly ? MidiDummy::name() : ConfigManager::inst()->value("audioengine", "mididev");
	if( m_renderOnly ) {
		m_audioDev = new AudioDummy( success_ful, this );
		m_audioDevName = AudioDummy::name();
		m_midiClient = new MidiDummy;
		m_midiClientName = MidiDummy::name();
	} else {
		m_audioDev = tryAudioDevices();
		m_midiClient = tryMidiClients();
	}
	// Loading audio device may have changed the sample rate
	notifyDeviceChange(false);
}

bool AudioEngine::criticalXRuns() const
{
	return cpuLoad() >= 99 && Engine::getSong()->isExporting() == false;
}




void AudioEngine::discardInputBuffers()
{
	const auto guard = requestChangesGuard(); invalidateInput();
	for (int index = 0; index < 2; ++index)
	{
		m_inputBufferFrames[index] = 0; m_inputBufferFault[index] = false; m_inputBufferEpoch[index] = inputErrorEpoch();
	}
}

void AudioEngine::pushInputFrames(SampleFrame* input, f_cnt_t incoming)
{
	if (incoming == 0) { return; }
	const auto guard = requestChangesGuard();
	const auto index = m_inputBufferWrite;
	auto& frames = m_inputBufferFrames[index];
	const auto epoch = inputErrorEpoch();
	if (m_inputBufferEpoch[index] != epoch) { frames = 0; }
	if (frames == 0) { m_inputBufferEpoch[index] = epoch; }
	const auto reject = [&] {
		m_inputBufferFrames[index] = 0;
		m_inputBufferFault[index] = true;
		m_inputErrorEpoch.fetch_add(1, std::memory_order_relaxed);
	};
	if (!input || !SampleRecordingBuffer::canAppendFrames(frames, incoming)) { reject(); return; }
	const auto count = static_cast<std::size_t>(incoming);
	if (std::any_of(input, input + count, [](const SampleFrame& frame) { return frame.containsNaN() || frame.containsInf(); }))
	{
		reject(); return;
	}
	try
	{
		const auto required = frames + incoming;
		if (required > m_inputBufferSize[index])
		{
			const auto limit = std::min(static_cast<f_cnt_t>(std::numeric_limits<int>::max()),
				static_cast<f_cnt_t>(std::vector<SampleFrame>().max_size()));
			const auto grown = m_inputBufferSize[index] <= limit / 2 ? m_inputBufferSize[index] * 2 : limit;
			const auto capacity = std::max(required, grown);
			auto replacement = std::make_unique<SampleFrame[]>(static_cast<std::size_t>(capacity));
			std::copy_n(m_inputBuffer[index], static_cast<std::size_t>(frames), replacement.get());
			delete[] m_inputBuffer[index]; m_inputBuffer[index] = replacement.release();
			m_inputBufferSize[index] = capacity;
		}
		std::copy_n(input, count, m_inputBuffer[index] + static_cast<std::size_t>(frames));
		m_inputBufferFrames[index] = required;
	}
	catch (const std::bad_alloc&) { reject(); }
	catch (const std::length_error&) { reject(); }
}



void AudioEngine::renderStageNoteSetup()
{
	AudioEngineProfiler::Probe profilerProbe(m_profiler, AudioEngineProfiler::DetailType::NoteSetup);

	if( m_clearSignal )
	{
		m_clearSignal = false;
		clearInternal();
	}

	// remove all play-handles that have to be deleted and delete
	// them if they still exist...
	// maybe this algorithm could be optimized...
	while (!m_playHandlesToRemove.isEmpty())
	{
		const auto requested = m_playHandlesToRemove.takeFirst();
		auto it = std::find(m_playHandles.begin(), m_playHandles.end(), requested);

		if( it != m_playHandles.end() )
		{
			const auto handle = *it;
			m_playHandles.erase(it);
			handle->audioBusHandle()->removePlayHandle(handle);
			if (handle->type() == PlayHandle::Type::NotePlayHandle)
			{
				NotePlayHandleManager::release(static_cast<NotePlayHandle*>(handle));
			}
			else { delete handle; }
		}
	}

	swapBuffers();

	// prepare master mix (clear internal buffers etc.)
	Mixer * mixer = Engine::mixer();
	mixer->prepareMasterMix();

	// create play-handles for new notes, samples etc.
	Engine::getSong()->processNextBuffer();

	// add all play-handles that have to be added
	for( LocklessListElement * e = m_newPlayHandles.popList(); e; )
	{
		m_playHandles += e->value;
		LocklessListElement * next = e->next;
		m_newPlayHandles.free( e );
		e = next;
	}
}



void AudioEngine::renderStageInstruments()
{
	AudioEngineProfiler::Probe profilerProbe(m_profiler, AudioEngineProfiler::DetailType::Instruments);

	AudioEngineWorkerThread::fillJobQueue(m_playHandles);
	AudioEngineWorkerThread::startAndWaitForJobs();
}



void AudioEngine::renderStageEffects()
{
	AudioEngineProfiler::Probe profilerProbe(m_profiler, AudioEngineProfiler::DetailType::Effects);

	// STAGE 2: process effects of all instrument- and sampletracks
	AudioEngineWorkerThread::fillJobQueue(m_audioBusHandles);
	AudioEngineWorkerThread::startAndWaitForJobs();

	// removed all play handles which are done
	const auto handles = m_playHandles;
	for (const auto handle : handles)
	{
		if (!m_playHandles.contains(handle)) { continue; }
		if (handle->affinityMatters() && handle->affinity() != QThread::currentThread())
		{
			continue;
		}
		if (handle->isFinished())
		{
			m_playHandles.removeAll(handle);
			handle->audioBusHandle()->removePlayHandle(handle);
			if (handle->type() == PlayHandle::Type::NotePlayHandle)
			{
				NotePlayHandleManager::release(static_cast<NotePlayHandle*>(handle));
			}
			else { delete handle; }
		}
	}
}



void AudioEngine::renderStageMix()
{
	AudioEngineProfiler::Probe profilerProbe(m_profiler, AudioEngineProfiler::DetailType::Mixing);

	Mixer *mixer = Engine::mixer();
	mixer->masterMix(m_outputBufferWrite.get());

	MixHelpers::multiply(m_outputBufferWrite.get(), m_masterGain, m_framesPerPeriod);

	emit nextAudioBuffer(m_outputBufferRead.get());

	// and trigger LFOs
	EnvelopeAndLfoParameters::instances()->trigger();
	Controller::triggerFrameCounter();
	AutomatableModel::incrementPeriodCounter();
}



std::uint64_t AudioEngine::refreshOutputFailure()
{
	if (dspStatus().failures == m_outputFailureCount) { return m_outputFailureCount; }
	const auto lock = std::lock_guard{m_changeMutex};
	// Resetting a latch does not erase its failure count. Discard cached audio
	// even if repair/reset happened before the next device callback observed it.
	zeroSampleFrames(m_outputBufferRead.get(), m_framesPerPeriod);
	zeroSampleFrames(m_outputBufferWrite.get(), m_framesPerPeriod);
	for (const auto bus : m_audioBusHandles) { if (bus->m_externalOutput) { bus->m_externalOutput->silence(); } }
	m_outputFailureCount = dspStatus().failures;
	return m_outputFailureCount;
}

void AudioEngine::renderNextBuffer(PlanarBufferView<float> dst, std::span<const BusDestination> buses)
{
	const auto lock = std::lock_guard{m_changeMutex};
	if (s_renderingThread) { throw std::logic_error("Cannot recursively render native bus outputs"); }
	const auto validate = [](PlanarBufferView<float> output)
	{
		if (output.channels() == 0 || (output.frames() && !output.data()))
		{ throw std::invalid_argument("Native audio destination has no channels or buffer"); }
		if (output.frames())
		{
			for (ch_cnt_t channel = 0; channel < output.channels(); ++channel)
			{
				if (!output.bufferPtr(channel)) { throw std::invalid_argument("Native audio destination has a null channel buffer"); }
			}
		}
	};
	// Validate every destination before consuming audio or touching any buffer.
	validate(dst);
	for (const auto& bus : buses)
	{
		if (bus.output.channels() != DEFAULT_CHANNELS || bus.output.frames() != dst.frames()
			|| (bus.source && !bus.source->m_retired && bus.source->m_read.size() != m_framesPerPeriod))
		{ throw std::invalid_argument("Native bus destination requires stereo audio and matching frame counts"); }
		validate(bus.output);
	}
	renderNextBuffer<PlanarBufferView<float>>(dst, buses);
}

void AudioEngine::copyBusOutputs(std::span<const BusDestination> buses, f_cnt_t sourceOffset, f_cnt_t outputOffset, f_cnt_t frames)
{
	for (const auto& bus : buses)
	{
		for (f_cnt_t frame = 0; frame < frames; ++frame)
		{
			const auto sample = bus.source && !bus.source->m_retired ? bus.source->m_read[sourceOffset + frame] : SampleFrame{};
			bus.output.sample(0, outputOffset + frame) = sample[0];
			bus.output.sample(1, outputOffset + frame) = sample[1];
		}
	}
}

void AudioEngine::silenceBusOutputs(std::span<const BusDestination> buses)
{
	for (const auto& bus : buses)
	{
		for (ch_cnt_t channel = 0; channel < bus.output.channels(); ++channel)
		{
			if (bus.output.frames()) { std::fill_n(bus.output.bufferPtr(channel), bus.output.frames(), 0.f); }
		}
	}
}

std::span<const SampleFrame> AudioEngine::renderNextPeriod()
{
	const auto lock = std::lock_guard{m_changeMutex};
	refreshOutputFailure();

	m_profiler.startPeriod();
	QScopedValueRollback<bool> rendering(s_renderingThread, true);
	const bool previouslyFailed = dspStatus().failure != DspFailure::None;
	if (!previouslyFailed)
	{
		auto stageFailure = DspFailure::NoteSetupException;
		const auto checkJobs = [&](DspFailure stage)
		{
			const auto failure = AudioEngineWorkerThread::jobQueueFailure();
			if (failure != AudioEngineWorkerThread::JobQueue::Failure::None)
			{
				reportDspFailure(failure == AudioEngineWorkerThread::JobQueue::Failure::QueueOverflow ? DspFailure::QueueOverflow : stage);
			}
		};
		try
		{
			renderStageNoteSetup();
			stageFailure = DspFailure::InstrumentJobException;
			renderStageInstruments(); checkJobs(stageFailure);
			if (dspStatus().failure == DspFailure::None)
			{
				stageFailure = DspFailure::EffectJobException;
				renderStageEffects(); checkJobs(stageFailure);
			}
			if (dspStatus().failure == DspFailure::None)
			{
				stageFailure = DspFailure::MixException;
				renderStageMix(); checkJobs(DspFailure::MixerJobException);
			}
			if (dspStatus().failure == DspFailure::None)
			{
				for (f_cnt_t frame = 0; frame < m_framesPerPeriod; ++frame)
				{
					if (m_outputBufferRead[frame].containsNaN() || m_outputBufferRead[frame].containsInf()
						|| m_outputBufferWrite[frame].containsNaN() || m_outputBufferWrite[frame].containsInf())
					{
						reportDspFailure(DspFailure::NonfiniteOutput); break;
					}
				}
			}
		}
		catch (...) { reportDspFailure(stageFailure); }
	}
	if (dspStatus().failure != DspFailure::None)
	{
		zeroSampleFrames(m_outputBufferRead.get(), m_framesPerPeriod);
		zeroSampleFrames(m_outputBufferWrite.get(), m_framesPerPeriod);
		for (const auto bus : m_audioBusHandles) { if (bus->m_externalOutput) { bus->m_externalOutput->silence(); } }
		m_outputFailureCount = dspStatus().failures;
		if (!previouslyFailed) { Engine::mixer()->discardProcessingPeriod(); invalidateInput(); }
	}
	m_profiler.finishPeriod(outputSampleRate(), m_framesPerPeriod);
	m_outputBufferReadIndex = 0;

	return {m_outputBufferRead.get(), m_framesPerPeriod};
}

void AudioEngine::swapBuffers()
{
	m_inputBufferWrite = (m_inputBufferWrite + 1) % 2;
	m_inputBufferRead = (m_inputBufferRead + 1) % 2;
	m_inputBufferFrames[m_inputBufferWrite] = 0;
	m_inputBufferFault[m_inputBufferWrite] = false;
	m_inputBufferEpoch[m_inputBufferWrite] = inputErrorEpoch();

	std::swap(m_outputBufferRead, m_outputBufferWrite);
	zeroSampleFrames(m_outputBufferWrite.get(), m_framesPerPeriod);
	for (const auto bus : m_audioBusHandles) { if (bus->m_externalOutput) { bus->m_externalOutput->advance(); } }
}

void AudioEngine::clear()
{
	m_clearSignal = true;
}




void AudioEngine::clearNewPlayHandles()
{
	requestChangeInModel();
	for( LocklessListElement * e = m_newPlayHandles.popList(); e; )
	{
		LocklessListElement * next = e->next;
		const auto handle = e->value;
		m_newPlayHandles.free( e );
		if (handle->audioBusHandle()) { handle->audioBusHandle()->removePlayHandle(handle); }
		if (handle->type() == PlayHandle::Type::NotePlayHandle) { NotePlayHandleManager::release(static_cast<NotePlayHandle*>(handle)); }
		else { delete handle; }
		e = next;
	}
	doneChangeInModel();
}



// removes all play-handles. this is necessary, when the song is stopped ->
// all remaining notes etc. would be played until their end
void AudioEngine::finishSampleRecordings(const SampleClip* clip)
{
	const auto guard = requestChangesGuard();
	for (;;)
	{
		const auto active = std::find_if(m_playHandles.begin(), m_playHandles.end(), [clip](const PlayHandle* handle) {
			const auto recording = dynamic_cast<const SampleRecordHandle*>(handle);
			return recording && (!clip || recording->recordsClip(clip));
		});
		if (active == m_playHandles.end()) { break; }
		const auto handle = *active; m_playHandles.erase(active);
		handle->audioBusHandle()->removePlayHandle(handle); delete handle;
	}
	for (;;)
	{
		LocklessListElement* found = nullptr;
		LocklessListElement* previous = nullptr;
		for (auto item = m_newPlayHandles.first(); item; previous = item, item = item->next)
		{
			if (const auto recording = dynamic_cast<SampleRecordHandle*>(item->value); recording && (!clip || recording->recordsClip(clip))) { found = item; break; }
		}
		if (!found) { break; }
		if (previous) { previous->next = found->next; }
		else { m_newPlayHandles.setFirst(found->next); }
		const auto handle = found->value; m_newPlayHandles.free(found);
		handle->audioBusHandle()->removePlayHandle(handle); delete handle;
	}
}

void AudioEngine::clearInternal()
{
	// TODO: m_midiClient->noteOffAll();
	for (auto ph : m_playHandles)
	{
		if (ph->type() != PlayHandle::Type::InstrumentPlayHandle)
		{
			m_playHandlesToRemove.push_back(ph);
		}
	}
}

const char* AudioEngine::deviceRestoreFailureName(DeviceRestoreFailure failure)
{
	switch (failure)
	{
	case DeviceRestoreFailure::None: return "none";
	case DeviceRestoreFailure::WrongThread: return "wrong-thread";
	case DeviceRestoreFailure::Busy: return "busy";
	case DeviceRestoreFailure::StopException: return "stop-exception";
	case DeviceRestoreFailure::ExportStopException: return "export-stop-exception";
	case DeviceRestoreFailure::NotificationException: return "notification-exception";
	case DeviceRestoreFailure::RestartException: return "restart-exception";
	case DeviceRestoreFailure::RestartFailed: return "restart-failed";
	}
	return "unknown";
}

void AudioEngine::setAudioDevice(AudioDevice* device, bool startNow)
{
	if (QThread::currentThread() != thread() || s_renderingThread || m_restoringAudioDevice || m_changingMidiClient
		|| (Engine::getSong() && (Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup())) || m_changingAudioDevice.exchange(true))
	{
		throw std::logic_error("Native audio device switching requires the idle owner thread");
	}
	struct Transition
	{
		std::atomic_bool& active;
		~Transition() { active = false; }
	} transition{m_changingAudioDevice};
	setAudioDeviceImpl(device, startNow);
}

bool AudioEngine::reportDeviceNotificationFailure(std::exception_ptr failure) noexcept
{
	if (QThread::currentThread() != thread() || !m_deviceNotificationFailure || !failure) { return false; }
	if (!*m_deviceNotificationFailure) { *m_deviceNotificationFailure = std::move(failure); }
	return true;
}

void AudioEngine::notifyDeviceChange(bool quality)
{
	std::exception_ptr failure;
	const auto previous = m_deviceNotificationFailure;
	m_deviceNotificationFailure = &failure;
	const auto restore = qScopeGuard([this, previous] { m_deviceNotificationFailure = previous; });
	if (quality) { emit qualitySettingsChanged(); }
	else { emit sampleRateChanged(); }
	if (failure) { std::rethrow_exception(failure); }
}

void AudioEngine::deviceTransitionCheckpoint(DeviceTransitionPoint point)
{
	if (!m_deviceTransitionProbe) { return; }
	// A probe may reset its installation or retire an export owner. Keep its
	// executing callable alive until the ordinary native call returns/throws.
	const auto probe = m_deviceTransitionProbe;
	probe(point);
}

void AudioEngine::setAudioDeviceImpl(AudioDevice* device, bool startNow)
{
	std::unique_ptr<AudioDevice> fallback;
	if (!device) { fallback.reset(tryAudioDevices()); device = fallback.get(); }
	if (!device) { throw std::runtime_error("Native audio device factory did not produce a device"); }
	// Joining a device thread while holding m_changeMutex can deadlock a
	// callback waiting to render. Stop first; publish only after it returns.
	if (m_oldAudioDev) { m_audioDeviceReservationTouched = true; }
	stopProcessing();
	AudioDevice* previous;
	{
		const auto guard = requestChangesGuard();
		previous = m_audioDev; m_audioDev = device;
		if (fallback) { fallback.release(); }
	}
	if (previous != device && previous != m_oldAudioDev) { delete previous; }
	if (previous != device)
	{
		const auto guard = requestChangesGuard();
		for (const auto bus : m_audioBusHandles) { if (bus->extOutputEnabled()) { device->registerPort(bus); } }
	}
	discardInputBuffers();
	notifyDeviceChange(true);
	deviceTransitionCheckpoint(DeviceTransitionPoint::SetupAfterQuality);
	notifyDeviceChange(false);
	deviceTransitionCheckpoint(DeviceTransitionPoint::SetupAfterRate);
	if (startNow) { device->startProcessing(); }
}

void AudioEngine::selectAudioBackend(const QString& name)
{
	selectAudioBackend(name, [this, name] { return createAudioDevice(name); });
}

void AudioEngine::selectAudioBackend(const QString& name, const std::function<std::unique_ptr<AudioDevice>()>& factory)
{
	const auto song = Engine::getSong();
	if (QThread::currentThread() != thread() || s_renderingThread || !song || !m_audioDev
		|| m_changingAudioDevice || m_restoringAudioDevice || m_oldAudioDev || m_changingMidiClient
		|| RemotePlugin::hasActiveRenderContext() || TrackRenderScope::hasActiveSelection())
	{ throw std::runtime_error("Native audio backend selection requires the idle device owner with no export/restoration reservation"); }
	if (!isAudioDevNameValid(name)) { throw std::invalid_argument("Unknown compiled native audio backend"); }
#ifdef LMMS_HAVE_JACK
	if (const auto jackMidi = dynamic_cast<MidiJack*>(m_midiClient); jackMidi && jackMidi->sharedAudioDevice() == m_audioDev)
	{ throw std::runtime_error("JACK MIDI shares this audio connection; select an independent MIDI backend before replacing its audio device"); }
#endif
	const auto checkWork = [] {
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
	};
	{
		FileResourceIO::EventSuspension events;
		const auto guard = requestChangesGuard(); checkWork();
		if (!song->isStopped() || song->isRecording() || song->isExporting() || song->hasPendingExportCleanup())
		{ throw std::runtime_error("Stop and disarm native transport before selecting an audio backend"); }
		const auto checkTrack = [&](Track* track) {
			checkWork();
			if (track->isNoteRecording()) { throw std::runtime_error("Complete native MIDI recording before changing audio backend"); }
			for (const auto clip : track->getClips())
			{
				checkWork();
				if (const auto sample = dynamic_cast<SampleClip*>(clip); sample && (sample->isRecord() || sample->hasReservedRecording()
					|| sample->hasActiveRecording() || sample->hasPendingRecording() || sample->isFinalizingRecording()))
				{ throw std::runtime_error("Complete and disarm native sample capture before changing audio backend"); }
				if (const auto curve = dynamic_cast<AutomationClip*>(clip); curve && (curve->isRecording() || curve->hasReservedRecording()))
				{ throw std::runtime_error("Complete and disarm native automation recording before changing audio backend"); }
			}
		};
		for (const auto owner : {static_cast<TrackContainer*>(song), static_cast<TrackContainer*>(Engine::patternStore())})
		{ for (const auto track : owner->tracks()) { checkTrack(track); } }
		checkTrack(song->globalAutomationTrack());
	}
	// Prepare metadata before touching the current device. No configuration file
	// or project state is changed by this process-local selection.
	QString selectedName = name, requestedName = name;
	if (!storeAudioDevice()) { throw std::runtime_error("Another native device reservation is active"); }
	QString failure;
	try
	{
		m_changingAudioDevice = true;
		struct Transition { std::atomic_bool& active; ~Transition() { active = false; } } transition{m_changingAudioDevice};
		m_audioDeviceReservationTouched = true;
		stopProcessing(); checkWork();
		auto candidate = factory();
		if (!candidate) { throw std::runtime_error("Requested native audio backend could not be opened; no fallback was selected"); }
		checkWork();
		try { setAudioDeviceImpl(candidate.get(), false); }
		catch (...) { if (m_audioDev == candidate.get()) { candidate.release(); } throw; }
		candidate.release();
		checkWork();
		m_audioDev->startProcessing();
		checkWork();
		const auto status = m_audioDev->processingStatus();
		if (!status.running)
		{
			throw std::runtime_error(QString("Requested native audio backend failed to start (%1, native error %2)")
				.arg(AudioDevice::processingFailureName(status.failure)).arg(status.nativeError).toStdString());
		}
		// The reservation owns the old stopped device until the new backend has
		// passed construction, port registration, notifications and startup.
		auto retired = m_oldAudioDev;
		m_audioDevName.swap(selectedName); m_requestedAudioBackend.swap(requestedName);
		m_audioDevStartFailed = false; m_renderOnly = false;
		{
			const auto guard = requestChangesGuard();
			m_oldAudioDev = nullptr; m_deviceRestoreFailure = DeviceRestoreFailure::None; updateMidiInputAdmission();
		}
		delete retired;
		return;
	}
	catch (const std::exception& error) { failure = QString::fromUtf8(error.what()); }
	catch (...) { failure = "Unknown native audio backend selection failure"; }
	FileResourceIO::Suspension recovery;
	if (!restoreAudioDevice())
	{
		failure += QString("; previous audio device restoration failed (%1); use audio.device.restore before retrying selection")
			.arg(deviceRestoreFailureName(deviceRestoreFailure()));
	}
	throw std::runtime_error(failure.toStdString());
}

AudioEngine::MidiBackendSelection AudioEngine::selectMidiBackend(const QString& name, bool abandonFailedOutput)
{
	return selectMidiBackend(name, [this, name] {
		bool successful = false;
		auto candidate = createMidiClient(name, successful);
		if (!candidate || !successful)
		{
			const auto state = candidate ? candidate->inputBackendStatus() : MidiClient::InputBackendStatus{};
			const auto output = candidate ? candidate->outputBackendStatus() : MidiClient::OutputBackendStatus{};
			const auto code = state.nativeErrorCode ? state.nativeErrorCode : output.nativeErrorCode ? output.nativeErrorCode : output.lastNativeErrorCode;
			throw std::runtime_error(QString("Requested native MIDI backend could not be opened (native code available %1, code %2); no fallback was selected")
				.arg(state.observed || output.observed).arg(code).toStdString());
		}
		return candidate;
	}, abandonFailedOutput);
}

AudioEngine::MidiBackendSelection AudioEngine::selectMidiBackend(const QString& name,
	const std::function<std::unique_ptr<MidiClient>()>& factory, bool abandonFailedOutput)
{
	if (!canChangeBackendGraph() || !m_midiClient)
	{ throw std::runtime_error("Native MIDI backend selection requires the stopped/disarmed owner outside model edits, export and device recovery"); }
	if (!isMidiDevNameValid(name)) { throw std::invalid_argument("Unknown compiled native MIDI backend"); }
	if (abandonFailedOutput)
	{
		const auto output = m_midiClient->outputBackendStatus();
		if (!output.observed || output.active || !output.abandonmentSupported || !output.abandonmentEligible)
		{ throw std::runtime_error("MIDI output abandonment requires a supported terminal backend whose output worker has stopped"); }
	}
	FileResourceIO::EventSuspension events;
	const auto checkWork = [] {
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
	};
	checkWork();
	// Allocate metadata before touching any native device. Startup preferences
	// and startup-failure history remain unchanged by this process-local choice.
	QString selectedName = name, requestedName = name, failure;
	MidiBackendSelection result;
	result.previousBackend = m_midiClientName;
	const bool wasRunning = m_audioDev->isRunning();
	bool stopped = false;
	m_changingMidiClient = true;
	{
		const auto transition = qScopeGuard([&] { m_changingMidiClient = false; });
		try
		{
			stopProcessing(); stopped = true; discardInputBuffers(); checkWork();
			auto candidate = factory();
			if (!candidate) { throw std::runtime_error("Requested native MIDI backend produced no client; no fallback was selected"); }
			checkWork();
			const auto transferred = m_midiClient->transferPortsTo(*candidate, [&] { checkWork(); return false; }, abandonFailedOutput);
			// The primitive's commit is the last cancellable boundary. It has
			// already rebound every model; publish matching engine ownership and
			// names, then retire the source before any queued notification runs.
			const auto retired = m_midiClient; m_midiClient = candidate.release();
			m_midiClientName.swap(selectedName); m_requestedMidiBackend.swap(requestedName);
			result.ports = transferred.ports; result.inputResetsRequired = transferred.inputResetsRequired;
			result.retirementExceptions = transferred.retirementExceptions; result.notificationFailures = transferred.notificationFailures;
			result.outputAbandonment = transferred.outputAbandonment;
			delete retired;
		}
		catch (const std::exception& error) { failure = QString::fromUtf8(error.what()); }
		catch (...) { failure = "Unknown native MIDI backend selection failure"; }
		// Keep the same audio device reserved until its original processing
		// state is restored. A failed join is retried before restart. There was
		// no rate/quality change, so no corresponding model notification is due.
		{
			const auto guard = requestChangesGuard();
			m_oldAudioDev = m_audioDev; m_storedAudioDeviceWasRunning = wasRunning; updateMidiInputAdmission();
		}
		m_audioDeviceReservationTouched = true; m_restoredAudioDeviceInstalled = stopped;
		m_restoreQualityNotified = m_restoreRateNotified = true;
		m_restoringAudioDevice = true; m_deviceRestoreFailure = DeviceRestoreFailure::None;
	}
	{
		FileResourceIO::Suspension recovery;
		result.audioRestored = restoreAudioDevice();
	}
	if (!failure.isEmpty())
	{
		if (!result.audioRestored)
		{
			failure += QString("; original audio processing state could not be restored (%1); use audio.device.restore")
				.arg(deviceRestoreFailureName(deviceRestoreFailure()));
		}
		throw std::runtime_error(failure.toStdString());
	}
	// MIDI is committed even when audio startup fails. Never claim rollback
	// or run another cancellable callback after this irreversible handoff.
	return result;
}

void AudioEngine::updateMidiInputAdmission()
{
	const auto guard = requestChangesGuard();
	if (m_midiClient)
	{ m_midiClient->setExternalInputBlocked(hasStoredAudioDevice() || (Engine::getSong() && (Engine::getSong()->isExporting() || Engine::getSong()->hasPendingExportCleanup()))); }
}

bool AudioEngine::storeAudioDevice()
{
	if (QThread::currentThread() != thread() || s_renderingThread || m_changingAudioDevice || m_changingMidiClient || m_oldAudioDev || !m_audioDev) { return false; }
	const auto guard = requestChangesGuard();
	if (m_audioDeviceReservationGeneration == std::numeric_limits<std::uint64_t>::max()) { return false; }
	m_oldAudioDev = m_audioDev;
	++m_audioDeviceReservationGeneration;
	updateMidiInputAdmission();
	m_storedAudioDeviceWasRunning = m_audioDev->isRunning();
	m_audioDeviceReservationTouched = false; m_restoredAudioDeviceInstalled = false;
	m_restoreQualityNotified = false; m_restoreRateNotified = false;
	m_restoringAudioDevice = false; m_deviceRestoreFailure = DeviceRestoreFailure::None;
	return true;
}

bool AudioEngine::restoreAudioDevice() noexcept
{
	if (QThread::currentThread() != thread() || s_renderingThread)
	{
		m_deviceRestoreFailure = DeviceRestoreFailure::WrongThread; return false;
	}
	if (m_changingAudioDevice || m_changingMidiClient || RemotePlugin::hasRunningRenderExecution())
	{
		m_deviceRestoreFailure = DeviceRestoreFailure::Busy; return false;
	}
	const auto song = Engine::getSong();
	bool exportCleanupPending = false;
	{
		const auto guard = requestChangesGuard();
		exportCleanupPending = song && song->hasPendingExportCleanup();
		if (song && song->isExporting() && !exportCleanupPending)
		{ m_deviceRestoreFailure = DeviceRestoreFailure::Busy; return false; }
	}
	m_changingAudioDevice = true;
	struct Transition
	{
		std::atomic_bool& active;
		~Transition() { active = false; }
	} transition{m_changingAudioDevice};
	if (exportCleanupPending)
	{
		m_restoringAudioDevice = true;
		try { song->stopExport(); }
		catch (...) { m_deviceRestoreFailure = DeviceRestoreFailure::ExportStopException; return false; }
		if (RemotePlugin::hasRunningRenderExecution()) { m_deviceRestoreFailure = DeviceRestoreFailure::Busy; return false; }
	}
	if (!m_oldAudioDev) { m_restoringAudioDevice = false; m_deviceRestoreFailure = DeviceRestoreFailure::None; return true; }
	if (!m_audioDeviceReservationTouched && m_audioDev == m_oldAudioDev)
	{
		const auto guard = requestChangesGuard();
		m_oldAudioDev = nullptr; m_restoringAudioDevice = false;
		updateMidiInputAdmission();
		m_deviceRestoreFailure = DeviceRestoreFailure::None; return true;
	}
	m_restoringAudioDevice = true;
	auto failure = DeviceRestoreFailure::StopException;
	try
	{
		if (!m_restoredAudioDeviceInstalled)
		{
			stopProcessing();
			if (m_audioDev != m_oldAudioDev)
			{
				AudioDevice* retired;
				{
					const auto guard = requestChangesGuard();
					retired = m_audioDev; m_audioDev = m_oldAudioDev;
				}
				delete retired;
			}
			m_restoredAudioDeviceInstalled = true;
			discardInputBuffers();
		}
		else if (m_audioDev->isRunning() || deviceRestoreFailure() == DeviceRestoreFailure::RestartException
			|| deviceRestoreFailure() == DeviceRestoreFailure::RestartFailed) { stopProcessing(); }
		// Once a switch/restoration has happened, failed callbacks are retried.
		failure = DeviceRestoreFailure::NotificationException;
		if (!m_restoreQualityNotified)
		{ notifyDeviceChange(true); deviceTransitionCheckpoint(DeviceTransitionPoint::RestoreAfterQuality); m_restoreQualityNotified = true; }
		if (!m_restoreRateNotified)
		{ notifyDeviceChange(false); deviceTransitionCheckpoint(DeviceTransitionPoint::RestoreAfterRate); m_restoreRateNotified = true; }
		failure = DeviceRestoreFailure::RestartException;
		if (m_storedAudioDeviceWasRunning)
		{
			m_audioDev->startProcessing();
			if (!m_audioDev->isRunning()) { m_deviceRestoreFailure = DeviceRestoreFailure::RestartFailed; return false; }
		}
		else if (m_audioDev->isRunning())
		{
			failure = DeviceRestoreFailure::StopException; stopProcessing();
		}
		{
			const auto guard = requestChangesGuard();
			m_oldAudioDev = nullptr; m_restoringAudioDevice = false; updateMidiInputAdmission();
		}
		m_deviceRestoreFailure = DeviceRestoreFailure::None; return true;
	}
	catch (...) { m_deviceRestoreFailure = failure; return false; }
}




void AudioEngine::removeAudioBusHandle(AudioBusHandle* busHandle)
{
	const auto guard = requestChangesGuard();

	auto it = std::find(m_audioBusHandles.begin(), m_audioBusHandles.end(), busHandle);
	if (it != m_audioBusHandles.end())
	{
		m_audioBusHandles.erase(it);
	}
}


bool AudioEngine::addPlayHandle( PlayHandle* handle )
{
	if (!handle) { return false; }
	// Only add play handles if we have the CPU capacity to process them.
	// Instrument play handles are not added during playback, but when the
	// associated instrument is created, so add those unconditionally.
	if (handle->audioBusHandle() && (handle->type() == PlayHandle::Type::InstrumentPlayHandle || !criticalXRuns()))
	{
		m_newPlayHandles.push( handle );
		handle->audioBusHandle()->addPlayHandle(handle);
		return true;
	}

	if( handle->type() == PlayHandle::Type::NotePlayHandle )
	{
		NotePlayHandleManager::release( (NotePlayHandle*)handle );
	}
	else delete handle;

	return false;
}


void AudioEngine::removePlayHandle(PlayHandle * ph)
{
	requestChangeInModel();
	// check thread affinity as we must not delete play-handles
	// which were created in a thread different than the audio engine thread
	if (ph->affinityMatters() && ph->affinity() == QThread::currentThread())
	{
		ph->audioBusHandle()->removePlayHandle(ph);
		bool removedFromList = false;
		// Check m_newPlayHandles first because doing it the other way around
		// creates a race condition
		for( LocklessListElement * e = m_newPlayHandles.first(),
				* ePrev = nullptr; e; ePrev = e, e = e->next )
		{
			if (e->value == ph)
			{
				if( ePrev )
				{
					ePrev->next = e->next;
				}
				else
				{
					m_newPlayHandles.setFirst( e->next );
				}
				m_newPlayHandles.free( e );
				removedFromList = true;
				break;
			}
		}
		// Now check m_playHandles
		PlayHandleList::Iterator it = std::find(m_playHandles.begin(), m_playHandles.end(), ph);
		if (it != m_playHandles.end())
		{
			m_playHandles.erase(it);
			removedFromList = true;
		}
		// Only deleting PlayHandles that were actually found in the list
		// "fixes crash when previewing a preset under high load"
		// (See tobydox's 2008 commit 4583e48)
		if ( removedFromList )
		{
			if (ph->type() == PlayHandle::Type::NotePlayHandle)
			{
				NotePlayHandleManager::release(dynamic_cast<NotePlayHandle*>(ph));
			}
			else { delete ph; }
		}
	}
	else
	{
		m_playHandlesToRemove.push_back(ph);
	}
	doneChangeInModel();
}




void AudioEngine::removePlayHandlesOfTypes(Track* track, PlayHandle::Types types, bool preserveReservedRecordings)
{
	const auto guard = requestChangesGuard();
	const auto matches = [&](const PlayHandle* handle) {
		if (preserveReservedRecordings)
		{
			if (const auto recording = dynamic_cast<const SampleRecordHandle*>(handle); recording && recording->reservedCompletion()) { return false; }
		}
		return handle->isFromTrack(track) && (handle->type() & types);
	};
	for (;;)
	{
		// A MIDI note can be silenced before its first render. Remove queued
		// handles too, or they would start playing in the next audio buffer.
		LocklessListElement* found = nullptr;
		LocklessListElement* previous = nullptr;
		for (auto item = m_newPlayHandles.first(); item; previous = item, item = item->next)
		{
			if (matches(item->value)) { found = item; break; }
		}
		PlayHandle* handle = nullptr;
		if (found)
		{
			if (previous) { previous->next = found->next; }
			else { m_newPlayHandles.setFirst(found->next); }
			handle = found->value;
			m_newPlayHandles.free(found);
		}
		else
		{
			const auto it = std::find_if(m_playHandles.begin(), m_playHandles.end(), matches);
			if (it == m_playHandles.end()) { break; }
			handle = *it;
			m_playHandles.erase(it);
		}
		// Unlink before destruction: releasing one note can retire other
		// handles. Restart the search after each callback instead of retaining
		// iterators into either list or stale deferred-removal requests.
		m_playHandlesToRemove.removeAll(handle);
		handle->audioBusHandle()->removePlayHandle(handle);
		if (handle->type() == PlayHandle::Type::NotePlayHandle) { NotePlayHandleManager::release(static_cast<NotePlayHandle*>(handle)); }
		else { delete handle; }
	}
}




void AudioEngine::requestChangeInModel()
{
	if (s_renderingThread) { return; }
	m_changeMutex.lock();
	++s_modelChangeDepth;
}

void AudioEngine::doneChangeInModel()
{
	if (s_renderingThread) { return; }
	assert(s_modelChangeDepth > 0);
	--s_modelChangeDepth;
	m_changeMutex.unlock();
}

QStringList AudioEngine::availableAudioDeviceNames()
{
	QStringList names;
#ifdef LMMS_HAVE_SDL
	names.append(AudioSdl::name());
#endif
#ifdef LMMS_HAVE_ALSA
	names.append(AudioAlsa::name());
#endif
#ifdef LMMS_HAVE_PULSEAUDIO
	names.append(AudioPulseAudio::name());
#endif
#ifdef LMMS_HAVE_SNDIO
	names.append(AudioSndio::name());
#endif
#ifdef LMMS_HAVE_JACK
	names.append(AudioJack::name());
#endif
#ifdef LMMS_HAVE_PORTAUDIO
	names.append(AudioPortAudio::name());
#endif
	names.append(AudioDummy::name());
	return names;
}

QStringList AudioEngine::availableMidiClientNames()
{
	QStringList names;
#ifdef LMMS_HAVE_ALSA
	names.append(MidiAlsaSeq::name());
	names.append(MidiAlsaRaw::name());
#endif
#ifdef LMMS_HAVE_JACK
	names.append(MidiJack::name());
#endif
#ifdef LMMS_HAVE_SNDIO
	names.append(MidiSndio::name());
#endif
#ifdef LMMS_HAVE_WINMM
	names.append(MidiWinMM::name());
#endif
#ifdef LMMS_BUILD_APPLE
	names.append(MidiApple::name());
#endif
	names.append(MidiDummy::name());
	return names;
}

bool AudioEngine::isAudioDevNameValid(QString name)
{
	return availableAudioDeviceNames().contains(name);
}

bool AudioEngine::isMidiDevNameValid(QString name)
{
	return availableMidiClientNames().contains(name);
}

std::unique_ptr<AudioDevice> AudioEngine::createAudioDevice(const QString& name)
{
	bool successful = false;
	std::unique_ptr<AudioDevice> device;
#ifdef LMMS_HAVE_SDL
	if (name == AudioSdl::name()) { device = std::make_unique<AudioSdl>(successful, this); }
#endif
#ifdef LMMS_HAVE_ALSA
	if (name == AudioAlsa::name()) { device = std::make_unique<AudioAlsa>(successful, this); }
#endif
#ifdef LMMS_HAVE_PULSEAUDIO
	if (name == AudioPulseAudio::name()) { device = std::make_unique<AudioPulseAudio>(successful, this); }
#endif
#ifdef LMMS_HAVE_SNDIO
	if (name == AudioSndio::name()) { device = std::make_unique<AudioSndio>(successful, this); }
#endif
#ifdef LMMS_HAVE_JACK
	if (name == AudioJack::name()) { device = std::make_unique<AudioJack>(successful, this); }
#endif
#ifdef LMMS_HAVE_PORTAUDIO
	if (name == AudioPortAudio::name()) { device = std::make_unique<AudioPortAudio>(successful, this); }
#endif
	if (name == AudioDummy::name()) { device = std::make_unique<AudioDummy>(successful, this); }
	if (!successful) { device.reset(); }
	return device;
}

AudioDevice * AudioEngine::tryAudioDevices()
{
	auto selected = ConfigManager::inst()->value("audioengine", "audiodev");
	if (!isAudioDevNameValid(selected)) { selected.clear(); }
	m_audioDevStartFailed = false;
	// Preserve native startup probing order and its explicit dummy fallback.
	// Runtime selection calls the same factory for exactly one requested name.
	for (const auto& name : availableAudioDeviceNames())
	{
		if (name == AudioDummy::name() || (!selected.isEmpty() && selected != name)) { continue; }
		auto device = createAudioDevice(name);
		if (device) { m_audioDevName = name; return device.release(); }
	}
	if (selected != AudioDummy::name())
	{
		printf("No audio-driver working - falling back to dummy-audio-driver\nYou can render your songs and listen to the output files...\n");
		m_audioDevStartFailed = true;
	}
	m_audioDevName = AudioDummy::name();
	return createAudioDevice(AudioDummy::name()).release();
}


std::unique_ptr<MidiClient> AudioEngine::createMidiClient(const QString& name, bool& successful)
{
	successful = false;
#ifdef LMMS_HAVE_ALSA
	if (name == MidiAlsaSeq::name())
	{ auto client = std::make_unique<MidiAlsaSeq>(); successful = client->isRunning(); return client; }
	if (name == MidiAlsaRaw::name())
	{ auto client = std::make_unique<MidiAlsaRaw>(); successful = client->isAvailable(); return client; }
#endif
#ifdef LMMS_HAVE_JACK
	if (name == MidiJack::name())
	{ auto client = std::make_unique<MidiJack>(); successful = client->isAvailable(); return client; }
#endif
#ifdef LMMS_HAVE_SNDIO
	if (name == MidiSndio::name())
	{ auto client = std::make_unique<MidiSndio>(); successful = client->isAvailable(); return client; }
#endif
#ifdef LMMS_HAVE_WINMM
	if (name == MidiWinMM::name())
	{ auto client = std::make_unique<MidiWinMM>(); successful = client->isAvailable(); return client; }
#endif
#ifdef LMMS_BUILD_APPLE
	if (name == MidiApple::name())
	{ auto client = std::make_unique<MidiApple>(); successful = client->isAvailable(); return client; }
#endif
	if (name == MidiDummy::name()) { successful = true; return std::make_unique<MidiDummy>(); }
	return {};
}

MidiClient * AudioEngine::tryMidiClients()
{
	m_midiBackendStartupFailures.clear();
	auto selected = ConfigManager::inst()->value("audioengine", "mididev");
	if (!isMidiDevNameValid(selected)) { selected.clear(); }
	// Keep native startup probing order and dummy fallback. Runtime selection
	// calls the same factory once and never falls back to another backend.
	for (const auto& name : availableMidiClientNames())
	{
		if (name == MidiDummy::name() || (!selected.isEmpty() && selected != name)) { continue; }
		bool successful = false; auto client = createMidiClient(name, successful);
		if (client && successful) { m_midiClientName = name; return client.release(); }
		const auto state = client ? client->inputBackendStatus() : MidiClient::InputBackendStatus{};
		const auto output = client ? client->outputBackendStatus() : MidiClient::OutputBackendStatus{};
		const auto code = state.nativeErrorCode ? state.nativeErrorCode : output.nativeErrorCode ? output.nativeErrorCode : output.lastNativeErrorCode;
		m_midiBackendStartupFailures.push_back({name, state.observed || output.observed, code});
	}
	if (selected != MidiDummy::name())
	{
		if (selected.isEmpty()) { printf("Unknown MIDI-client. "); }
		else { printf("Couldn't create %s MIDI-client. ", selected.toUtf8().constData()); }
		printf("Will use dummy-MIDI-client.\n");
	}
	m_midiClientName = MidiDummy::name();
	bool successful = false;
	return createMidiClient(MidiDummy::name(), successful).release();
}

} // namespace lmms

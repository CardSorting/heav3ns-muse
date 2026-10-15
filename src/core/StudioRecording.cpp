/* Native bounded recording operation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioRecording.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "AudioFileDevice.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "PatternStore.h"
#include "SampleClip.h"
#include "SampleRecordHandle.h"
#include "SampleRecordingBuffer.h"
#include "SampleTrack.h"
#include "Song.h"

namespace lmms
{
namespace
{
[[noreturn]] void fail(const QString& error) { throw std::runtime_error(error.toStdString()); }

int option(const QJsonObject& arguments, const char* key, int fallback, int minimum, int maximum)
{
	if (!arguments.contains(key)) { return fallback; }
	const auto value = arguments.value(key); const auto number = value.toDouble();
	if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum)
	{
		fail(QString("%1 is outside the supported integer range").arg(key));
	}
	return static_cast<int>(number);
}

void requireIdleRecording()
{
	const auto song = Engine::getSong();
	if (!song->isStopped() || song->isRecording() || song->isExporting()) { fail("Stop native transport before starting a recording transaction"); }
	for (const auto owner : {static_cast<TrackContainer*>(song), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (const auto track : owner->tracks())
		{
			if (track->isNoteRecording()) { fail("Complete native MIDI note recording first"); }
			for (const auto clip : track->getClips())
			{
				if (const auto sample = dynamic_cast<SampleClip*>(clip); sample && (sample->isRecord() || sample->hasReservedRecording()
					|| sample->hasActiveRecording() || sample->hasPendingRecording() || sample->isFinalizingRecording()))
				{
					fail("Complete or disarm the existing native recording before starting another");
				}
				if (const auto curve = dynamic_cast<AutomationClip*>(clip); curve && (curve->isRecording() || curve->hasReservedRecording())) { fail("Complete or disarm native automation recording first"); }
			}
		}
	}
	for (const auto clip : song->globalAutomationTrack()->getClips())
	{
		if (const auto curve = dynamic_cast<AutomationClip*>(clip); curve && (curve->isRecording() || curve->hasReservedRecording())) { fail("Complete or disarm native global automation recording first"); }
	}
}
}

void StudioRecording::requireIdle()
{
	if (!Engine::audioEngine() || !Engine::getSong()) { fail("Native engine is not initialized"); }
	const auto guard = Engine::audioEngine()->requestChangesGuard(); requireIdleRecording();
}

void StudioRecording::requireNoCapture()
{
	if (!Engine::audioEngine() || !Engine::getSong()) { fail("Native engine is not initialized"); }
	const auto guard = Engine::audioEngine()->requestChangesGuard(); const auto song = Engine::getSong();
	if (song->isRecording()) { fail("Complete native recording before rendering"); }
	for (const auto owner : {static_cast<TrackContainer*>(song), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (const auto track : owner->tracks())
		{
			if (track->isNoteRecording()) { fail("Complete native MIDI note recording before rendering"); }
			for (const auto clip : track->getClips())
			{
				if (const auto sample = dynamic_cast<SampleClip*>(clip); sample && (sample->hasReservedRecording()
					|| sample->hasActiveRecording() || sample->hasPendingRecording() || sample->isFinalizingRecording()))
				{
					fail("Complete native capture before rendering");
				}
				if (const auto curve = dynamic_cast<AutomationClip*>(clip); curve && (curve->isRecording() || curve->hasReservedRecording())) { fail("Complete or disarm automation recording before rendering"); }
			}
		}
	}
	for (const auto clip : song->globalAutomationTrack()->getClips())
	{
		if (const auto curve = dynamic_cast<AutomationClip*>(clip); curve && (curve->isRecording() || curve->hasReservedRecording())) { fail("Complete or disarm global automation recording before rendering"); }
	}
}

QJsonObject StudioRecording::capture(SampleClip* clip, const QJsonObject& arguments, const std::function<bool()>& cancelled)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
		|| !Engine::audioEngine() || !Engine::getSong()) { fail("Native recording requires the initialized application thread"); }
	if (!clip || !dynamic_cast<SampleTrack*>(clip->getTrack())) { fail("Recording requires a native sample clip"); }
	if (!arguments.contains("durationMs")) { fail("durationMs is required for bounded native recording"); }
	const int duration = option(arguments, "durationMs", 0, 1, 86400000);
	const int timeout = option(arguments, "timeoutMs", std::min(86400000, duration + 10000), 1, 86400000);
	if (arguments.contains("playback") && !arguments.value("playback").isBool()) { fail("playback must be a boolean"); }
	const bool playback = arguments.value("playback").toBool();
	if (arguments.contains("position") && !playback) { fail("position applies to accompaniment playback"); }
	const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
	if (engine->dspStatus().failure != AudioEngine::DspFailure::None) { fail("Native DSP has a latched failure; repair and explicitly reset audio processing before recording"); }
	QPointer<SampleClip> target(clip); const auto identity = clip;
	AudioDevice* device = nullptr; sample_rate_t rate = 0; f_cnt_t frames = 0;
	int position = 0; bool priorRecord = false, priorLoop = false, startedPlayback = false, reserved = false;
	QElapsedTimer elapsed; elapsed.start();
	const auto abortRequested = [&] { return elapsed.elapsed() >= timeout || (cancelled && cancelled()); };
	FileResourceIO::Scope recordingWork(abortRequested, false);
	const auto check = [&] {
		if (cancelled && cancelled()) { fail("Native recording cancelled"); }
		if (elapsed.elapsed() >= timeout) { fail("Native recording timed out before a complete take"); }
		if (!target) { fail("Native recording target was removed"); }
		if (engine->dspStatus().failure != AudioEngine::DspFailure::None) { fail("Native DSP failed during recording; the prior sample was preserved"); }
		if (engine->audioDev() != device || !device->isRunning() || !device->supportsCapture() || engine->inputSampleRate() != rate
			|| song->isExporting() || engine->hasStoredAudioDevice()) { fail("Native recording backend stopped, changed or became reserved"); }
	};
	const auto release = [&](bool publish) {
		FileResourceIO::Suspension cleanupWork; const auto guard = engine->requestChangesGuard();
		if (reserved)
		{
			engine->finishSampleRecordings(identity);
			if (target)
			{
				if (!publish) { target->discardReservedRecording(); }
				target->setRecord(priorRecord);
			}
			reserved = false;
		}
		if (startedPlayback)
		{
			song->stop(); song->getTimeline(Song::PlayMode::Song).setLoopEnabled(priorLoop); startedPlayback = false;
		}
	};
	const auto cleanup = qScopeGuard([&] {
		try { release(false); } catch (const std::exception& error) { song->collectError(QString::fromUtf8(error.what())); }
	});
	{
		const auto guard = engine->requestChangesGuard(); requireIdleRecording(); device = engine->audioDev();
		if (!device || !device->isRunning() || !device->supportsCapture() || dynamic_cast<AudioFileDevice*>(device)
			|| engine->hasStoredAudioDevice()) { fail("Native recording requires a running capture-capable backend"); }
		rate = engine->inputSampleRate();
		if (rate == 0 || rate > static_cast<sample_rate_t>(std::numeric_limits<int>::max())) { fail("Native recording sample rate is invalid"); }
		frames = (static_cast<f_cnt_t>(duration) * rate + 999) / 1000;
		if (!SampleRecordingBuffer::canAppendFrames(0, frames)) { fail("Requested recording exceeds native sample frame limits"); }
		const auto budgetValue = arguments.value("maximumBytes");
		const auto budget = arguments.contains("maximumBytes") ? budgetValue.toDouble() : 256. * 1024 * 1024;
		if ((arguments.contains("maximumBytes") && !budgetValue.isDouble()) || !std::isfinite(budget)
			|| budget < sizeof(SampleFrame) || budget > 9007199254740991. || std::floor(budget) != budget
			|| frames > static_cast<f_cnt_t>(budget) / sizeof(SampleFrame)) { fail("Requested recording exceeds its PCM byte budget"); }
		position = playback ? option(arguments, "position", clip->startPosition().getTicks(), 0, MaxSongLength) : 0;
		if (playback && (position < 0 || position > MaxSongLength)) { fail("Native accompaniment position is outside the song range"); }
		check(); QString error; if (!clip->reserveRecordingCompletion(error)) { fail(error); } reserved = true;
		priorRecord = clip->isRecord(); clip->setRecord(true); engine->discardInputBuffers();
		if (playback)
		{
			priorLoop = song->getTimeline(Song::PlayMode::Song).loopEnabled(); startedPlayback = true;
			// Native song playback suppresses the armed target's prior sample;
			// the one reserved recorder below owns capture, without duplicate takes.
			song->getTimeline(Song::PlayMode::Song).setLoopEnabled(false);
			song->setPlayPos(position, Song::PlayMode::Song); song->playSong();
		}
		auto handle = std::make_unique<SampleRecordHandle>(clip, frames, true);
		if (!clip->hasActiveRecording() || !engine->addPlayHandle(handle.release())) { fail("Native recording handle was not accepted"); }
	}
	for (;;)
	{
		{ const auto guard = engine->requestChangesGuard(); check(); if (!target->hasActiveRecording()) { break; } }
		QEventLoop quantum; QTimer::singleShot(10, &quantum, &QEventLoop::quit); quantum.exec(QEventLoop::ExcludeUserInputEvents);
	}
	{
		const auto guard = engine->requestChangesGuard(); check(); QString error;
		if (!target->finishReservedRecording(frames, rate, error)) { fail(error); }
	}
	release(true); song->setModified();
	return {{"frames", static_cast<double>(frames)}, {"sampleRate", static_cast<int>(rate)},
		{"bytes", static_cast<double>(frames * sizeof(SampleFrame))}, {"durationSeconds", static_cast<double>(frames) / rate},
		{"playback", playback}, {"elapsedMs", static_cast<double>(elapsed.elapsed())},
		{"source", "native audio input"}, {"perceptualListening", false}};
}
}

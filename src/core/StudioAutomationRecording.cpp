/* Native bounded automation recording. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioAutomationRecording.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "AudioFileDevice.h"
#include "AutomatableModel.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "MidiClient.h"
#include "MidiController.h"
#include "PatternStore.h"
#include "Song.h"
#include "StudioRecording.h"

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
}

QJsonObject StudioAutomationRecording::capture(AutomationClip* clip, const QJsonObject& arguments, const std::function<bool()>& cancelled)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
		|| !Engine::audioEngine() || !Engine::getSong()) { fail("Native automation recording requires the initialized application thread"); }
	if (!clip || !clip->getTrack() || clip->getTrack()->type() != Track::Type::Automation)
	{
		fail("Recording requires an arrangement or pattern automation clip");
	}
	if (!arguments.contains("durationMs")) { fail("durationMs is required for bounded automation recording"); }
	const int duration = option(arguments, "durationMs", 0, 1, 86400000);
	const int timeout = option(arguments, "timeoutMs", std::min(86400000, duration + 10000), 1, 86400000);
	const int maximumPoints = option(arguments, "maximumPoints", 65536, 1, 1048576);
	const int quantization = option(arguments, "quantization", 0, 0, DefaultTicksPerBar * 16);
	const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
	QPointer<AutomationClip> target(clip); QPointer<Track> track(clip->getTrack());
	const bool pattern = track->trackContainer() == Engine::patternStore();
	const auto mode = pattern ? Song::PlayMode::Pattern : Song::PlayMode::Song;
	const auto placement = clip->startPosition(); const auto offset = clip->startTimeOffset(); const auto length = clip->length();
	const auto bindings = clip->objects();
	QPointer<AutomatableModel> primary;
	struct Driver
	{
		QPointer<const AutomatableModel> model;
		QPointer<ControllerConnection> connection;
		QPointer<Controller> controller;
		MidiClient* client = nullptr;
		MidiClient::InputDeliveryStatus delivery;
	};
	std::vector<Driver> drivers;
	AudioDevice* device = nullptr; sample_rate_t rate = 0; f_cnt_t targetFrames = 0, actualFrames = 0;
	std::uint64_t generation = 0, samples = 0; int points = 0;
	bool reserved = false, started = false, savedTimeline = false, savedControl = false, priorControl = true;
	bool priorLoop = false, clipEnd = false; int priorPosition = 0, priorPattern = 0, selectedPattern = 0;
	QElapsedTimer elapsed; elapsed.start();
	FileResourceIO::Scope recordingWork([&] { return elapsed.elapsed() >= timeout || (cancelled && cancelled()); }, false);
	const auto check = [&](bool resources = true) {
		if (cancelled && cancelled()) { fail("Native automation recording cancelled"); }
		if (elapsed.elapsed() >= timeout) { fail("Native automation recording timed out"); }
		QString error; if (resources && !FileResourceIO::check(error)) { fail(error); }
		if (!target || !track || target->getTrack() != track.data() || target->startPosition() != placement
			|| target->startTimeOffset() != offset || target->length() != length || target->isMuted() || track->isMuted()
			|| target->objects() != bindings || !primary) { fail("Native automation recording target or binding changed"); }
		for (const auto& model : bindings)
		{
			if (!model || !model->supportsAutomation()) { fail("An automation recording target is unavailable or no longer automatable"); }
		}
		const auto linked = primary->linkedModels();
		if (linked.size() != drivers.size()) { fail("Automation recording controller links changed"); }
		for (std::size_t index = 0; index < drivers.size(); ++index)
		{
			const auto& driver = drivers[index];
			if (!driver.model || linked[index] != driver.model.data() || driver.model->controllerConnection() != driver.connection.data()
				|| (driver.connection && driver.connection->getController() != driver.controller.data()))
			{
				fail("Automation recording controller binding changed");
			}
			if (driver.connection && !driver.controller) { fail("Automation recording controller was removed"); }
			if (const auto midi = dynamic_cast<MidiController*>(driver.controller.data()))
			{
				const auto port = midi->midiPort(); const auto client = port->client();
				if (!client || client != driver.client || !port->isInputEnabled() || !client->hasQueuedInputDelivery()) { fail("Automation MIDI input changed or is disabled"); }
				const auto backend = client->inputBackendStatus(); const auto delivery = client->inputDeliveryStatus(port);
				if (!backend.observed || !backend.active || backend.nativeErrorCode || !delivery.registered || delivery.failed
					|| delivery.failureVersion != driver.delivery.failureVersion || delivery.droppedEvents != driver.delivery.droppedEvents)
				{
					fail("Automation MIDI input stopped or lost events");
				}
			}
		}
		if (engine->dspStatus().failure != AudioEngine::DspFailure::None) { fail("Native DSP failed during automation recording"); }
		if (engine->audioDev() != device || !device || !device->isRunning() || engine->outputSampleRate() != rate
			|| song->isExporting() || engine->hasStoredAudioDevice()) { fail("Native automation recording audio clock stopped or changed"); }
		if (reserved && (!target->hasReservedRecording() || !target->recordingError().isEmpty()))
		{
			fail(target->recordingError().isEmpty() ? "Native automation take was discarded" : target->recordingError());
		}
		if (started && (!song->isPlaying() || !song->isRecording() || song->playMode() != mode
			|| song->playbackGeneration() != generation || !target->isRecording()
			|| (pattern && Engine::patternStore()->currentPattern() != selectedPattern)))
		{
			fail("Native automation recording transport or arm state changed");
		}
	};
	const auto release = [&](bool discard) {
		FileResourceIO::Suspension cleanupWork; const auto guard = engine->requestChangesGuard(); QString error;
		const auto attempt = [&](const auto& action) {
			try { action(); }
			catch (const std::exception& failure) { if (error.isEmpty()) { error = QString::fromUtf8(failure.what()).left(1024); if (error.isEmpty()) { error = "Automation recording cleanup failed"; } } }
			catch (...) { if (error.isEmpty()) { error = "Automation recording cleanup failed"; } }
		};
		if (reserved && target) { target->setRecording(false); }
		if (started)
		{
			started = false; attempt([&] { song->stop(); });
			if (error.isEmpty()) { error = song->midiOutputCleanupError(); }
		}
		if (savedTimeline)
		{
			savedTimeline = false; attempt([&] { song->getTimeline(mode).setLoopEnabled(priorLoop); song->setPlayPos(priorPosition, mode); });
			if (pattern) { attempt([&] { Engine::patternStore()->setCurrentPattern(priorPattern); }); }
		}
		if (savedControl)
		{
			savedControl = false; if (primary) { attempt([&] { primary->setUseControllerValue(priorControl); }); }
		}
		if (discard && reserved)
		{
			reserved = false; if (target) { attempt([&] { target->discardRecordingTake(); }); }
		}
		if (!error.isEmpty()) { fail(error); }
	};
	const auto cleanup = qScopeGuard([&] {
		try { release(true); }
		catch (const std::exception& error) { song->collectError(QString::fromUtf8(error.what())); }
		catch (...) { song->collectError("Native automation recording cleanup failed"); }
	});
	{
		const auto guard = engine->requestChangesGuard(); StudioRecording::requireIdle();
		if (!pattern && track->trackContainer() != song) { fail("Unsupported automation recording container"); }
		if (bindings.empty() || !bindings.front()) { fail("Connect a native parameter before recording automation"); }
		primary = bindings.front();
		for (const auto model : primary->linkedModels())
		{
			Driver driver; driver.model = model; driver.connection = model->controllerConnection();
			if (driver.connection) { driver.controller = driver.connection->getController(); }
			if (const auto midi = dynamic_cast<MidiController*>(driver.controller.data()))
			{
				driver.client = midi->midiPort()->client();
				if (driver.client) { driver.delivery = driver.client->inputDeliveryStatus(midi->midiPort()); }
			}
			drivers.push_back(driver);
		}
		if (placement < 0 || placement > MaxSongLength || offset < -MaxSongLength || offset > MaxSongLength || length <= 0
			|| qint64(placement.getTicks()) + length.getTicks() > MaxSongLength) { fail("Automation recording clip exceeds the native song range"); }
		const int position = option(arguments, "position", pattern ? 0 : placement.getTicks(), 0, MaxSongLength);
		if (position < (pattern ? 0 : placement.getTicks()) || qint64(position) >= (pattern ? 0 : placement.getTicks()) + qint64(length.getTicks()))
		{
			fail("Automation recording must start inside the clip window");
		}
		selectedPattern = pattern ? track->getClipNum(clip) : 0;
		if (pattern && (selectedPattern < 0 || selectedPattern >= Engine::patternStore()->numOfPatterns())) { fail("Automation target has no native pattern"); }
		device = engine->audioDev(); rate = engine->outputSampleRate();
		if (!device || dynamic_cast<AudioFileDevice*>(device) || !rate || rate > static_cast<sample_rate_t>(std::numeric_limits<int>::max()))
		{
			fail("Native automation recording requires a running audio clock");
		}
		check(); targetFrames = (static_cast<f_cnt_t>(duration) * rate + 999) / 1000;
		target->reserveRecordingTake(maximumPoints, quantization); reserved = true;
		priorControl = primary->useControllerValue(); savedControl = true; primary->setUseControllerValue(true); target->setRecording(true);
		priorLoop = song->getTimeline(mode).loopEnabled(); priorPosition = song->getPlayPos(mode).getTicks();
		priorPattern = Engine::patternStore()->currentPattern(); savedTimeline = true;
		if (pattern) { Engine::patternStore()->setCurrentPattern(selectedPattern); }
		song->getTimeline(mode).setLoopEnabled(false); song->setPlayPos(position, mode); started = true;
		if (pattern) { song->playPattern(); } else { song->playSong(); }
		song->record(); generation = song->playbackGeneration();
	}
	for (;;)
	{
		{
			const auto guard = engine->requestChangesGuard(); check();
			clipEnd = !pattern && song->getPlayPos(mode) >= placement + length;
			if (song->playbackFrames() >= targetFrames || clipEnd)
			{
				actualFrames = song->playbackFrames(); samples = target->recordingSamples(); break;
			}
		}
		QEventLoop quantum; QTimer::singleShot(10, &quantum, &QEventLoop::quit); quantum.exec(QEventLoop::ExcludeUserInputEvents);
	}
	release(false);
	{
		const auto guard = engine->requestChangesGuard(); check();
		target->commitRecordingTake([&] { check(false); }); reserved = false;
		points = target->getTimeMap().size(); song->setModified();
	}
	return {{"samples", static_cast<double>(samples)}, {"points", points}, {"maximumPoints", maximumPoints},
		{"quantization", quantization}, {"frames", static_cast<double>(actualFrames)}, {"requestedFrames", static_cast<double>(targetFrames)},
		{"sampleRate", static_cast<int>(rate)},
		{"durationSeconds", static_cast<double>(actualFrames) / rate}, {"elapsedMs", static_cast<double>(elapsed.elapsed())},
		{"playMode", pattern ? "pattern" : "song"}, {"stopReason", clipEnd ? "clipEnd" : "duration"},
		{"source", "native automation parameter"}, {"hardwareDeliveryVerified", false}};
}
}

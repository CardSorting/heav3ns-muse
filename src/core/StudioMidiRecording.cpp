/* Native bounded MIDI recording. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioMidiRecording.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
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
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiClient.h"
#include "MidiNoteRecorder.h"
#include "MidiPort.h"
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

QJsonObject StudioMidiRecording::capture(MidiClip* clip, const QJsonObject& arguments, const std::function<bool()>& cancelled)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
		|| !Engine::audioEngine() || !Engine::getSong()) { fail("Native MIDI recording requires the initialized application thread"); }
	if (!clip || !clip->instrumentTrack()) { fail("MIDI recording requires a native instrument MIDI clip"); }
	if (!arguments.contains("durationMs")) { fail("durationMs is required for bounded native MIDI recording"); }
	const int duration = option(arguments, "durationMs", 0, 1, 86400000);
	const int timeout = option(arguments, "timeoutMs", std::min(86400000, duration + 10000), 1, 86400000);
	const int maximumNotes = option(arguments, "maximumNotes", 65536, 1, 1048576);
	const int quantization = option(arguments, "quantization", 0, 0, DefaultTicksPerBar * 16);
	const int maximumPitchPoints = option(arguments, "maximumPitchPoints", 65536, 1, 1048576);
	if (arguments.contains("recordPitch") && !arguments.value("recordPitch").isBool()) { fail("recordPitch must be a boolean"); }
	const bool recordPitch = !arguments.contains("recordPitch") || arguments.value("recordPitch").toBool();
	const auto modeName = arguments.contains("playMode") ? arguments.value("playMode").toString() : QString("midiClip");
	Song::PlayMode mode;
	if (modeName == "song") { mode = Song::PlayMode::Song; }
	else if (modeName == "pattern") { mode = Song::PlayMode::Pattern; }
	else if (modeName == "midiClip") { mode = Song::PlayMode::MidiClip; }
	else { fail("playMode must be song, pattern or midiClip"); }
	const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
	QPointer<MidiClip> target(clip); QPointer<InstrumentTrack> instrument(clip->instrumentTrack());
	QPointer<Track> reservation(clip->instrumentTrack()); QObject receiver;
	MidiNoteRecorder recorder; recorder.reset(maximumNotes); std::vector<Note> staged;
	AudioDevice* device = nullptr; MidiClient* client = nullptr; MidiPort* port = nullptr; sample_rate_t rate = 0;
	MidiClient::InputDeliveryStatus inputStatus;
	bool accepting = false, reserved = false, started = false, inputSuspended = false, savedTimeline = false;
	bool priorLoop = false; int priorPosition = 0, priorPattern = 0, selectedPattern = 0;
	bool restorePitch = false; float priorPitch = 0;
	const auto clipPosition = clip->startPosition(); const auto clipOffset = clip->startTimeOffset();
	f_cnt_t targetFrames = 0, actualFrames = 0; std::size_t heldNotes = 0;
	std::uint64_t playbackGeneration = 0;
	QString callbackError; QElapsedTimer elapsed; elapsed.start();
	const auto abortRequested = [&] { return elapsed.elapsed() >= timeout || (cancelled && cancelled()); };
	FileResourceIO::Scope recordingWork(abortRequested, false);
	const auto check = [&](bool resources = true) {
		if (cancelled && cancelled()) { fail("Native MIDI recording cancelled"); }
		if (elapsed.elapsed() >= timeout) { fail("Native MIDI recording timed out before a complete take"); }
		// An onset signal can run inside NotePlayHandle construction. Never pump
		// events there: nested stop/deletion could retire that unfinished handle.
		QString resourceError; if (resources && !FileResourceIO::check(resourceError)) { fail(resourceError); }
		if (!callbackError.isEmpty()) { fail(callbackError); }
		if (!target || !instrument || target->instrumentTrack() != instrument.data()
			|| target->startPosition() != clipPosition || target->startTimeOffset() != clipOffset) { fail("Native MIDI recording target was removed or changed"); }
		if (engine->dspStatus().failure != AudioEngine::DspFailure::None) { fail("Native DSP failed; MIDI take was not published"); }
		if (recordPitch && !instrument->canRecordMidiPitch())
		{
			fail("Per-note MIDI pitch recording requires an unautomated, uncontrolled, unlinked track pitch model; recordPitch false records notes alone");
		}
		if (engine->audioDev() != device || !device || !device->isRunning() || engine->outputSampleRate() != rate
			|| engine->hasStoredAudioDevice() || song->isExporting()) { fail("Native MIDI recording audio clock stopped or changed"); }
		if (instrument->midiPort() != port || port->client() != client || !port->isInputEnabled()) { fail("Native MIDI recording input changed"); }
		const auto backend = client->inputBackendStatus(); const auto delivery = client->inputDeliveryStatus(port);
		if (!backend.observed || !backend.active || backend.nativeErrorCode != 0 || !delivery.registered || delivery.failed
			|| delivery.failureVersion != inputStatus.failureVersion || delivery.droppedEvents != inputStatus.droppedEvents)
		{
			fail("Native MIDI input stopped or lost events; reset input delivery before retrying");
		}
		if (started && (!song->isPlaying() || !song->isRecording() || song->playMode() != mode
			|| song->playbackGeneration() != playbackGeneration
			|| (mode == Song::PlayMode::MidiClip && song->playingMidiClip() != target.data())
			|| (mode == Song::PlayMode::Pattern && Engine::patternStore()->currentPattern() != selectedPattern)))
		{
			fail("Native MIDI recording transport changed before completion");
		}
	};
	const auto release = [&] {
		FileResourceIO::Suspension cleanupWork; const auto guard = engine->requestChangesGuard();
		accepting = false;
		if (instrument) { QObject::disconnect(instrument.data(), nullptr, &receiver, nullptr); }
		QString error;
		const auto attempt = [&](const auto& action) {
			try { action(); }
			catch (const std::exception& failure)
			{
				if (error.isEmpty())
				{
					error = QString::fromUtf8(failure.what()).left(1024);
					if (error.isEmpty()) { error = "Native MIDI recording cleanup failed"; }
				}
			}
			catch (...) { if (error.isEmpty()) { error = "Native MIDI recording cleanup failed"; } }
		};
		if (reserved)
		{
			if (reservation) { reservation->endNoteRecording(&receiver); } reserved = false;
			if (instrument) { attempt([&] { instrument->silenceAllNotes(); }); }
		}
		if (started)
		{
			started = false; attempt([&] { song->stop(); });
			if (!song->midiOutputCleanupError().isEmpty() && error.isEmpty()) { error = song->midiOutputCleanupError(); }
		}
		if (savedTimeline)
		{
			savedTimeline = false;
			attempt([&] { song->getTimeline(mode).setLoopEnabled(priorLoop); song->setPlayPos(priorPosition, mode); });
			if (mode == Song::PlayMode::Pattern) { attempt([&] { Engine::patternStore()->setCurrentPattern(priorPattern); }); }
		}
		if (restorePitch)
		{
			restorePitch = false;
			if (instrument) { attempt([&] { instrument->pitchModel()->setValue(priorPitch); }); }
		}
		if (inputSuspended)
		{
			inputSuspended = false;
			if (instrument && instrument->midiPort()->client() == client) { attempt([&] { client->setInputDeliveryEnabled(port, port->isInputEnabled()); }); }
		}
		if (!error.isEmpty()) { fail(error); }
	};
	const auto cleanup = qScopeGuard([&] {
		try { release(); }
		catch (const std::exception& error) { song->collectError(QString::fromUtf8(error.what())); }
		catch (...) { song->collectError("Native MIDI recording cleanup failed"); }
	});
	{
		const auto guard = engine->requestChangesGuard(); StudioRecording::requireIdle();
		if (mode == Song::PlayMode::Song && instrument->trackContainer() != song) { fail("Song recording requires a song instrument track"); }
		if (mode == Song::PlayMode::Pattern && instrument->trackContainer() != Engine::patternStore()) { fail("Pattern recording requires a pattern instrument track"); }
		if (mode == Song::PlayMode::Pattern && (instrument->getClipNum(clip) < 0
			|| instrument->getClipNum(clip) >= Engine::patternStore()->numOfPatterns())) { fail("Recording target has no native pattern"); }
		if (clipPosition < 0 || clipPosition > MaxSongLength || clipOffset < -MaxSongLength || clipOffset > MaxSongLength)
		{
			fail("MIDI recording clip placement exceeds the native song range");
		}
		device = engine->audioDev(); rate = engine->outputSampleRate(); port = instrument->midiPort(); client = port->client();
		if (!device || dynamic_cast<AudioFileDevice*>(device) || !rate || rate > static_cast<sample_rate_t>(std::numeric_limits<int>::max()))
		{
			fail("Native MIDI recording requires a running audio clock with a valid sample rate");
		}
		if (!client || !client->hasQueuedInputDelivery()) { fail("Native MIDI recording requires a queued native input backend"); }
		inputStatus = client->inputDeliveryStatus(port); check();
		const int position = option(arguments, "position", mode == Song::PlayMode::Song ? clipPosition.getTicks() : 0, 0, MaxSongLength);
		if (position < 0 || position > MaxSongLength) { fail("MIDI recording position exceeds the native song range"); }
		targetFrames = (static_cast<f_cnt_t>(duration) * rate + 999) / 1000;
		priorPitch = instrument->pitchModel()->storedValue();
		if (recordPitch) { recorder.configurePitch(priorPitch, maximumPitchPoints); }
		if (!reservation->beginNoteRecording(&receiver)) { fail("The native instrument already has a note recorder"); } reserved = true;
		restorePitch = recordPitch;
		// Discard input queued before this take without changing saved subscriptions.
		inputSuspended = true; client->setInputDeliveryEnabled(port, false); instrument->silenceAllNotes();
		QObject::connect(instrument.data(), &InstrumentTrack::midiNoteOn, &receiver, [&](const Note& note) {
			if (!accepting || !callbackError.isEmpty()) { return; }
			try
			{
				const auto modelGuard = engine->requestChangesGuard(); check(false);
				const auto position = MidiNoteRecorder::positionInClip(note, *target, mode == Song::PlayMode::Song);
				if (position > MaxSongLength) { fail("Recorded MIDI note exceeds the native song range"); }
				recorder.startNote(note, position);
			}
			catch (const std::exception& error) { callbackError = QString::fromUtf8(error.what()).left(1024); }
			catch (...) { callbackError = "Native MIDI note onset could not be recorded"; }
		});
		QObject::connect(instrument.data(), &InstrumentTrack::midiNotePitch, &receiver, [&](const Note& note, float pitch) {
			if (!recordPitch || !accepting || !callbackError.isEmpty()) { return; }
			try
			{
				const auto modelGuard = engine->requestChangesGuard(); check(false); recorder.recordPitch(note, pitch);
			}
			catch (const std::exception& error) { callbackError = QString::fromUtf8(error.what()).left(1024); }
			catch (...) { callbackError = "Native MIDI pitch could not be recorded"; }
		});
		QObject::connect(instrument.data(), &InstrumentTrack::midiNoteOff, &receiver, [&](const Note& note) {
			if (!accepting || !callbackError.isEmpty()) { return; }
			try
			{
				const auto modelGuard = engine->requestChangesGuard(); check(false);
				if (auto completed = recorder.finishNote(note, quantization))
				{
					if (completed->pos() < 0 || completed->length() < 1
						|| qint64(completed->pos().getTicks()) + completed->length().getTicks() > MaxSongLength)
					{
						fail("Recorded MIDI note exceeds the native song range");
					}
					staged.push_back(*completed);
				}
			}
			catch (const std::exception& error) { callbackError = QString::fromUtf8(error.what()).left(1024); }
			catch (...) { callbackError = "Native MIDI note release could not be recorded"; }
		});
		priorLoop = song->getTimeline(mode).loopEnabled(); priorPosition = song->getPlayPos(mode).getTicks();
		priorPattern = Engine::patternStore()->currentPattern(); savedTimeline = true;
		if (mode == Song::PlayMode::Pattern)
		{
			selectedPattern = instrument->getClipNum(target.data()); Engine::patternStore()->setCurrentPattern(selectedPattern);
		}
		song->getTimeline(mode).setLoopEnabled(false); song->setPlayPos(position, mode); started = true;
		if (mode == Song::PlayMode::Song) { song->playSong(); }
		else if (mode == Song::PlayMode::Pattern) { song->playPattern(); }
		else { song->playMidiClip(target.data(), false); }
		song->record(); playbackGeneration = song->playbackGeneration(); accepting = true;
		client->setInputDeliveryEnabled(port, true); inputSuspended = false;
	}
	for (;;)
	{
		{
			const auto guard = engine->requestChangesGuard(); check();
			if (song->playbackFrames() >= targetFrames)
			{
				inputSuspended = true; client->setInputDeliveryEnabled(port, false);
				QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall); check(false);
				heldNotes = recorder.pendingNotes(); instrument->finishMidiInputRecording(); check(false);
				if (recorder.pendingNotes()) { fail("Native input notes could not all be completed"); }
				actualFrames = song->playbackFrames(); accepting = false; break;
			}
		}
		QEventLoop quantum; QTimer::singleShot(10, &quantum, &QEventLoop::quit); quantum.exec(QEventLoop::ExcludeUserInputEvents);
	}
	// Cleanup must succeed before any notes become part of the editable clip.
	release();
	{
		const auto guard = engine->requestChangesGuard(); check();
		target->appendRecordedNotes(staged, [&] { check(false); }); song->setModified();
	}
	return {{"noteCount", static_cast<int>(staged.size())}, {"acceptedNotes", static_cast<int>(recorder.acceptedNotes())},
		{"completedHeldNotes", static_cast<int>(heldNotes)}, {"frames", static_cast<double>(actualFrames)},
		{"requestedFrames", static_cast<double>(targetFrames)}, {"sampleRate", static_cast<int>(rate)},
		{"durationSeconds", static_cast<double>(actualFrames) / rate}, {"elapsedMs", static_cast<double>(elapsed.elapsed())},
		{"playMode", modeName}, {"maximumNotes", maximumNotes}, {"quantization", quantization},
		{"recordPitch", recordPitch}, {"pitchPoints", static_cast<int>(recorder.pitchPoints())}, {"maximumPitchPoints", maximumPitchPoints},
		{"pitchBaselineCents", priorPitch},
		{"source", "native MIDI input"}, {"hardwareDeliveryVerified", false}};
}
}

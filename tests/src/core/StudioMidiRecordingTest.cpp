/* Native MIDI recording transaction tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "MidiClip.h"
#include "MidiNoteRecorder.h"
#include "MidiPort.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace lmms
{
class MidiPortTestAccess
{
public:
	static void useClient(MidiPort* port, MidiClient* client)
	{
		port->m_midiClient->removePort(port); port->m_midiClient = client; client->addPort(port);
	}
};
}
namespace
{
// Real native byte decoding, queued model delivery and engine periods, with
// fixture device boundaries. This does not verify a physical MIDI/audio device.
class RecordingInput final : public MidiClientRaw
{
public:
	bool observed = true, active = true, failOutput = false;
	InputBackendStatus inputBackendStatus() const override { return {observed, active, 0}; }
	void feed(unsigned char status, unsigned char first, unsigned char second, bool pump = true)
	{
		parseData(status); parseData(first); parseData(second);
		if (pump) { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
	}
	void failDelivery() { failInputEvents(InputDeliveryFailure::BackendInput); }
	void cancelOutputEvents(const MidiPort*) override
	{
		if (failOutput) { throw std::runtime_error("Recording fixture output cleanup failed"); }
	}
private:
	void sendByte(unsigned char) override { }
};
class RecordingClock final : public AudioDevice
{
public:
	explicit RecordingClock(AudioEngine* engine) : AudioDevice(DEFAULT_CHANNELS, engine) { }
	~RecordingClock() override { stopProcessing(); }
private:
	void startProcessingImpl() override { }
	void stopProcessingImpl() override { }
};
}

class StudioMidiRecordingTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	RecordingInput m_input;
	RecordingClock* m_clock = nullptr;
	InstrumentTrack* m_track = nullptr;
	MidiClip* m_clip = nullptr;
	void attach()
	{
		MidiPortTestAccess::useClient(m_track->midiPort(), &m_input);
		m_track->midiPort()->setReadable(true); m_track->midiPort()->setInputChannel(0);
	}
	QJsonObject options() const
	{
		return {{"track", 0}, {"clip", 0}, {"durationMs", 100}, {"timeoutMs", 1000}, {"maximumNotes", 16}};
	}
	void period() { Engine::audioEngine()->renderNextPeriod(); }
	void assertReleased()
	{
		QVERIFY(Engine::getSong()->isStopped()); QVERIFY(!Engine::getSong()->isRecording());
		QVERIFY(!m_track->isNoteRecording()); QVERIFY(!m_track->isSustainPedalPressed());
	}
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) const
	{
		return {{"id", id}, {"op", op}, {"args", args}};
	}
	QJsonObject manifest(const QTemporaryDir& directory) const
	{
		QFile file(directory.filePath("session.json"));
		return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
	}
	void refresh()
	{
		m_track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		m_clip = static_cast<MidiClip*>(m_track->getClip(0));
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", "256"); NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		m_input.observed = m_input.active = true; m_input.failOutput = false;
		Engine::init(true); m_clock = new RecordingClock(Engine::audioEngine()); Engine::audioEngine()->setAudioDevice(m_clock, true);
		Engine::getSong()->clearProject(); Engine::getSong()->setTempo(120);
		m_track = new InstrumentTrack(Engine::getSong()); m_clip = static_cast<MidiClip*>(m_track->createClip(0));
		m_clip->clearNotes(); m_clip->addNote(Note(12, 0, 48, 75), false); attach();
	}
	void cleanup() { m_input.failOutput = false; Engine::destroy(); m_track = nullptr; m_clip = nullptr; m_clock = nullptr; }

	void recorderPreservesOnsetAndNativeReleaseWithOptionalQuantization_data()
	{
		QTest::addColumn<int>("grid"); QTest::newRow("native-ticks") << 0; QTest::newRow("quantized") << 12;
	}
	void recorderPreservesOnsetAndNativeReleaseWithOptionalQuantization()
	{
		QFETCH(int, grid); MidiNoteRecorder recorder; recorder.reset(2);
		Note on(0, 777, 64, 73, -19); QVERIFY(recorder.startNote(on, 25));
		QVERIFY(!recorder.startNote(on, 999)); Note off(17, 900, 64, 1, 50);
		const auto completed = recorder.finishNote(off, grid); QVERIFY(completed);
		QCOMPARE(completed->pos().getTicks(), grid ? 24 : 25); QCOMPARE(completed->length().getTicks(), grid ? 12 : 17);
		QCOMPARE(completed->getVolume(), 73); QCOMPARE(completed->getPanning(), -19);
		QCOMPARE(recorder.acceptedNotes(), std::size_t(1)); QCOMPARE(recorder.pendingNotes(), std::size_t(0));
		QVERIFY(!recorder.finishNote(off)); QVERIFY(recorder.startNote(on, 0));
		QVERIFY(recorder.finishNote(Note(0, 0, 64))->length() >= 1);
		QVERIFY_EXCEPTION_THROWN(recorder.startNote(on, 1), std::runtime_error);
		recorder.reset(1); QVERIFY(!recorder.startNote(on, -1)); QVERIFY(recorder.startNote(on, 0));
	}

	void songPlacementUsesSnapshotAndClipTrim()
	{
		m_clip->movePosition(192); m_clip->setStartTimeOffset(-24); const Note snapshot(0, 205, 60);
		Engine::getSong()->setPlayPos(999, Song::PlayMode::Song);
		QCOMPARE(MidiNoteRecorder::positionInClip(snapshot, *m_clip, true).getTicks(), 37);
		QCOMPARE(MidiNoteRecorder::positionInClip(snapshot, *m_clip, false).getTicks(), 205);
	}

	void nativeNoteLengthAccumulatesTempoAndHonorsInitialFrameOffset()
	{
		const auto engine = Engine::audioEngine(); const auto frames = engine->framesPerPeriod(); const f_cnt_t offset = 32;
		NotePlayHandle handle(m_track, offset, 1000000, Note(0, 0, 60), nullptr, 0, NotePlayHandle::Origin::MidiInput);
		QCOMPARE(handle.recordedMidiLength(16).getTicks(), 0);
		QCOMPARE(handle.recordedMidiLength(96).getTicks(), int(64 / Engine::framesPerTick()));
		std::vector<SampleFrame> buffer(frames); const double firstClock = Engine::framesPerTick();
		handle.play(buffer.data()); Engine::getSong()->setTempo(240); const double secondClock = Engine::framesPerTick();
		handle.play(buffer.data()); const double expected = (frames - offset) / firstClock + frames / secondClock;
		QCOMPARE(handle.recordedMidiLength().getTicks(), int(expected));
		QCOMPARE(handle.recordedMidiLength(64).getTicks(), int(expected + 64 / secondClock));
		QVERIFY(int(expected) != int((2 * frames - offset) / secondClock));
	}

	void pitchCurvesOwnTheirDataAndUseElapsedNoteTicks()
	{
		MidiNoteRecorder recorder; recorder.reset(2); recorder.configurePitch(50, 4);
		Note first(0, 777, 60); first.createDetuning(); first.detuning()->automationClip()->putValue(0, 2, false);
		QVERIFY(recorder.startNote(first, 192)); recorder.recordPitch(Note(0, 0, 60), 50); QCOMPARE(recorder.pitchPoints(), std::size_t(0));
		recorder.recordPitch(Note(12, 0, 60), 75); recorder.recordPitch(Note(12, 0, 60), 125);
		recorder.recordPitch(Note(24, 0, 60), 125); QCOMPARE(recorder.pitchPoints(), std::size_t(2));
		QVERIFY(recorder.startNote(Note(0, 0, 64), 200)); recorder.recordPitch(Note(0, 0, 64), 125);
		const auto accepted = recorder.finishNote(Note(36, 0, 60)); QVERIFY(accepted && accepted->detuning());
		const auto curve = accepted->detuning()->automationClip(); QCOMPARE(curve->getTimeMap().size(), 2);
		QCOMPARE(curve->progressionType(), AutomationClip::ProgressionType::Discrete);
		QCOMPARE(curve->valueAt(11), 0.f); QCOMPARE(curve->valueAt(12), .75f);
		QCOMPARE(first.detuning()->automationClip()->valueAt(0), 2.f);
		recorder.recordPitch(Note(12, 0, 64), 25); QCOMPARE(recorder.pitchPoints(), std::size_t(4));
		const auto other = recorder.finishNote(Note(30, 0, 64)); QVERIFY(other && other->detuning());
		QCOMPARE(other->detuning()->automationClip()->valueAt(0), .75f);
		QCOMPARE(other->detuning()->automationClip()->valueAt(12), -.25f);
		recorder.reset(); QCOMPARE(curve->valueAt(12), .75f);
	}

	void pitchLimitsFailBeforeAddingCurveData_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("point-limit") << 0; QTest::newRow("native-range") << 1;
		QTest::newRow("nonfinite-pitch") << 2; QTest::newRow("negative-elapsed") << 3;
	}
	void pitchLimitsFailBeforeAddingCurveData()
	{
		QFETCH(int, failure); MidiNoteRecorder recorder; recorder.reset(1); recorder.configurePitch(0, failure == 0 ? 1 : 16);
		QVERIFY(recorder.startNote(Note(0, 0, 60), 0));
		const auto pitch = failure == 1 ? (MaxDetuning + 1) * 100.f
			: failure == 2 ? std::numeric_limits<float>::quiet_NaN() : 100.f;
		QVERIFY_EXCEPTION_THROWN(recorder.recordPitch(Note(failure == 3 ? -1 : 12, 0, 60), pitch), std::runtime_error);
		QCOMPARE(recorder.pitchPoints(), std::size_t(0)); const auto note = recorder.finishNote(Note(24, 0, 60));
		QVERIFY(note); QVERIFY(!note->hasDetuningInfo());
	}

	void pitchCaptureRestoresBaselineAndReplaysNativeDetuning_data()
	{
		QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("enabled"); QTest::addColumn<bool>("sustain");
		QTest::newRow("clip-chord") << QString("midiClip") << true << false;
		QTest::newRow("sustained-chord") << QString("midiClip") << true << true;
		QTest::newRow("pattern-after-wrap") << QString("pattern") << true << false;
		QTest::newRow("notes-only") << QString("midiClip") << false << false;
	}
	void pitchCaptureRestoresBaselineAndReplaysNativeDetuning()
	{
		QFETCH(QString, mode); QFETCH(bool, enabled); QFETCH(bool, sustain); auto args = options();
		args.insert("recordPitch", enabled); args.insert("playMode", mode);
		if (mode == "pattern")
		{
			Engine::getSong()->clearProject(); m_track = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::patternStore()));
			m_clip = static_cast<MidiClip*>(m_track->getClip(0)); m_clip->clearNotes(); attach();
			Engine::getSong()->setTempo(999); args.insert("container", "pattern"); args.insert("durationMs", 600);
		}
		m_track->pitchModel()->setValue(50); int periods = 0, firstTick = 0, finalTick = 0;
		const int finalPeriod = mode == "pattern" ? 75 : 8;
		QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (!Engine::getSong()->isRecording()) { return; }
			if (periods == 0)
			{
				m_input.feed(0x90, 60, 96); m_input.feed(0x90, 64, 96);
				if (sustain) { m_input.feed(0xb0, MidiControllerSustain, 127); }
			}
			if (periods == 4)
			{
				firstTick = int(periods * Engine::audioEngine()->framesPerPeriod() / double(Engine::framesPerTick()));
				m_input.feed(0xe0, 0, 0);
				if (sustain) { m_input.feed(0x80, 60, 0); m_input.feed(0x80, 64, 0); }
			}
			if (periods == finalPeriod)
			{
				finalTick = int(periods * Engine::audioEngine()->framesPerPeriod() / double(Engine::framesPerTick())); m_input.feed(0xe0, 127, 127);
			}
			period(); ++periods;
		}); clock.start(); const auto response = StudioCapabilities::execute("midi.record", args); clock.stop();
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(finalTick > firstTick);
		QCOMPARE(m_track->pitchModel()->storedValue(), enabled ? 50.f : 100.f);
		QCOMPARE(response.value("result").toObject().value("pitchPoints").toInt(), enabled ? 6 : 0);
		int recorded = 0;
		for (const auto note : m_clip->notes())
		{
			if (note->key() != 60 && note->key() != 64) { continue; } ++recorded;
			QCOMPARE(note->hasDetuningInfo(), enabled); if (!enabled) { continue; }
			const auto curve = note->detuning()->automationClip(); QCOMPARE(curve->valueAt(firstTick - 1), 0.f);
			QCOMPARE(curve->valueAt(firstTick), -1.5f); QCOMPARE(curve->valueAt(finalTick), .5f);
			NotePlayHandle playback(m_track, 0, 1000000, *note); playback.processTimePos(note->pos() + finalTick);
			const auto expected = m_track->nominalFrequency(note->key()) * std::exp2(1.f / 12.f);
			QVERIFY(std::abs(playback.frequency() - expected) < expected * .00001f);
		}
		QCOMPARE(recorded, 2); assertReleased();
	}

	void pitchBudgetFailurePreservesPriorNotesAndRestoresTrackPitch()
	{
		auto args = options(); args.insert("maximumPitchPoints", 1); m_track->pitchModel()->setValue(50);
		int periods = 0; QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (periods == 0) { m_input.feed(0x90, 60, 96); }
			if (periods == 4) { m_input.feed(0xe0, 127, 127); } period(); ++periods;
		}); clock.start(); const auto response = StudioCapabilities::execute("midi.record", args); clock.stop();
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("pitch point limit"));
		QCOMPARE(m_clip->notes().size(), std::size_t(1)); QCOMPARE(m_track->pitchModel()->storedValue(), 50.f); assertReleased();
	}

	void automatedOrLinkedPitchRequiresExplicitNotesOnlyRecording_data()
	{
		QTest::addColumn<bool>("linked"); QTest::addColumn<bool>("recordPitch");
		QTest::newRow("automation-pitch-refused") << false << true;
		QTest::newRow("automation-notes-accepted") << false << false;
		QTest::newRow("linked-pitch-refused") << true << true;
		QTest::newRow("linked-notes-accepted") << true << false;
	}
	void automatedOrLinkedPitchRequiresExplicitNotesOnlyRecording()
	{
		QFETCH(bool, linked); QFETCH(bool, recordPitch); FloatModel linkedModel(0, -100, 100, 1);
		if (linked) { m_track->pitchModel()->linkToModel(&linkedModel); }
		else
		{
			const auto track = new AutomationTrack(Engine::getSong());
			const auto curve = static_cast<AutomationClip*>(track->createClip(0));
			QVERIFY(curve->addObject(m_track->pitchModel())); curve->putValue(12, 50, false);
		}
		QVERIFY(!m_track->canRecordMidiPitch()); auto args = options(); args.insert("recordPitch", recordPitch);
		QTimer clock; clock.setInterval(1); bool delivered = false;
		QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (!Engine::getSong()->isRecording()) { return; }
			if (!delivered) { m_input.feed(0x90, 60, 96); delivered = true; } period();
		}); clock.start(); const auto response = StudioCapabilities::execute("midi.record", args); clock.stop();
		QCOMPARE(response.value("ok").toBool(), !recordPitch);
		QCOMPARE(m_clip->notes().size(), recordPitch ? std::size_t(1) : std::size_t(2)); assertReleased();
	}

	void nativeCaptureAppendsAndCompletesHeldOrSustainedNotes_data()
	{
		QTest::addColumn<QString>("mode"); QTest::addColumn<int>("release");
		QTest::newRow("clip-note-off") << QString("midiClip") << 0;
		QTest::newRow("clip-held-at-end") << QString("midiClip") << 1;
		QTest::newRow("clip-sustained-at-end") << QString("midiClip") << 2;
		QTest::newRow("song-trimmed-placement") << QString("song") << 0;
		QTest::newRow("pattern-wrap-held") << QString("pattern") << 1;
	}
	void nativeCaptureAppendsAndCompletesHeldOrSustainedNotes()
	{
		QFETCH(QString, mode); QFETCH(int, release); auto args = options(); args.insert("playMode", mode);
		if (mode == "song") { m_clip->movePosition(192); m_clip->setStartTimeOffset(-24); args.insert("position", 205); }
		if (mode == "pattern")
		{
			Engine::getSong()->clearProject(); m_track = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::patternStore()));
			m_clip = static_cast<MidiClip*>(m_track->getClip(0)); m_clip->clearNotes(); m_clip->addNote(Note(12, 0, 48, 75), false); attach();
			Engine::getSong()->setTempo(999); args.insert("container", "pattern"); args.insert("durationMs", 600);
		}
		const auto playMode = mode == "song" ? Song::PlayMode::Song : mode == "pattern" ? Song::PlayMode::Pattern : Song::PlayMode::MidiClip;
		Engine::getSong()->getTimeline(playMode).setLoopEnabled(true); Engine::getSong()->setPlayPos(37, playMode);
		int periods = 0; QTimer clock; clock.setInterval(1); int onset = -1; double expectedTicks = 0;
		QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (!Engine::getSong()->isRecording()) { return; }
			if (periods == 0)
			{
				onset = Engine::getSong()->getPlayPos().getTicks();
				if (release == 2) { m_input.feed(0xb0, MidiControllerSustain, 127); }
				m_input.feed(0x90, 60, 96);
			}
			if (periods == 4 && release != 1) { m_input.feed(0x80, 60, 0); }
			if (release != 0 || periods < 4) { expectedTicks += Engine::audioEngine()->framesPerPeriod() / double(Engine::framesPerTick()); }
			period(); ++periods;
		});
		clock.start(); const auto response = StudioCapabilities::execute("midi.record", args); clock.stop();
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QCOMPARE(m_clip->notes().size(), std::size_t(2)); const Note* recorded = nullptr;
		for (const auto note : m_clip->notes()) { if (note->key() == 60) { recorded = note; } }
		QVERIFY(recorded); QCOMPARE(recorded->pos().getTicks(), mode == "song" ? onset - 192 + 24 : onset);
		QCOMPARE(recorded->length().getTicks(), std::max(1, int(expectedTicks)));
		QCOMPARE(response.value("result").toObject().value("completedHeldNotes").toInt(), release == 0 ? 0 : 1);
		QVERIFY(response.value("result").toObject().value("frames").toDouble() >= response.value("result").toObject().value("requestedFrames").toDouble());
		QVERIFY(Engine::getSong()->getTimeline(playMode).loopEnabled()); QCOMPARE(Engine::getSong()->getPlayPos(playMode).getTicks(), 37);
		if (mode == "pattern") { QVERIFY(recorded->length() > DefaultTicksPerBar); }
		assertReleased();
	}

	void failureDoesNotPublishStagedNotesAndReleasesOwnership_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("cancel") << 0; QTest::newRow("timeout") << 1; QTest::newRow("input-disconnected") << 2;
		QTest::newRow("delivery-failed") << 3; QTest::newRow("audio-disconnected") << 4; QTest::newRow("dsp-failed") << 5;
		QTest::newRow("note-budget") << 6; QTest::newRow("transport-stopped") << 7; QTest::newRow("output-cleanup-failed") << 8;
		QTest::newRow("clip-retimed") << 9; QTest::newRow("input-disabled") << 10;
		QTest::newRow("transport-restarted") << 11;
	}
	void failureDoesNotPublishStagedNotesAndReleasesOwnership()
	{
		QFETCH(int, failure); auto args = options(); bool cancel = false; int periods = 0;
		if (failure == 1) { args.insert("timeoutMs", 30); }
		if (failure == 6) { args.insert("maximumNotes", 1); }
		QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (periods == 0) { m_input.feed(0x90, 60, 96); }
			if (periods == 2)
			{
				m_input.feed(0x80, 60, 0);
				if (failure == 0) { cancel = true; }
				if (failure == 2) { m_input.active = false; }
				if (failure == 3) { m_input.failDelivery(); }
				if (failure == 4) { m_clock->reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected); }
				if (failure == 5) { Engine::audioEngine()->reportDspFailure(AudioEngine::DspFailure::InstrumentJobException); }
				if (failure == 6) { m_input.feed(0x90, 61, 96); }
				if (failure == 7) { Engine::getSong()->stop(); }
				if (failure == 8) { m_input.failOutput = true; }
				if (failure == 9) { m_clip->movePosition(192); }
				if (failure == 10) { m_track->midiPort()->setReadable(false); }
				if (failure == 11) { Engine::getSong()->playMidiClip(m_clip, false); Engine::getSong()->record(); }
			}
			if (failure != 1) { period(); } ++periods;
		});
		clock.start(); const auto response = StudioCapabilities::execute("midi.record", args, [&] { return cancel; }); clock.stop();
		m_input.failOutput = false;
		QVERIFY(!response.value("ok").toBool()); QCOMPARE(m_clip->notes().size(), std::size_t(1));
		QCOMPARE(m_clip->notes().front()->key(), 48); assertReleased();
	}

	void preflightRejectsUnobservableOrCompetingInput_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("backend-unobserved") << 0; QTest::newRow("disabled") << 1;
		QTest::newRow("already-recording") << 2; QTest::newRow("invalid-pattern-owner") << 3; QTest::newRow("invalid-duration") << 4;
	}
	void preflightRejectsUnobservableOrCompetingInput()
	{
		QFETCH(int, failure); auto args = options(); int otherOwner = 0;
		if (failure == 0) { m_input.observed = false; }
		if (failure == 1) { m_track->midiPort()->setReadable(false); }
		if (failure == 2) { QVERIFY(m_track->beginNoteRecording(&otherOwner)); }
		if (failure == 3) { args.insert("playMode", "pattern"); }
		if (failure == 4) { args.insert("durationMs", 1.5); }
		const auto response = StudioCapabilities::execute("midi.record", args); QVERIFY(!response.value("ok").toBool());
		QCOMPARE(m_clip->notes().size(), std::size_t(1));
		if (failure == 2) { QVERIFY(m_track->isNoteRecording()); m_track->endNoteRecording(&otherOwner); }
		assertReleased();
	}

	void pendingInputBeforeCaptureIsDiscardedAndReservationHasOneOwner()
	{
		m_input.feed(0x90, 61, 96, false); int first = 0, second = 0;
		QVERIFY(m_track->beginNoteRecording(&first)); QVERIFY(!m_track->beginNoteRecording(&second));
		m_track->endNoteRecording(&second); QVERIFY(m_track->isNoteRecording()); m_track->endNoteRecording(&first);
		QTimer clock; clock.setInterval(1); QObject::connect(&clock, &QTimer::timeout, this, [&] { period(); }); clock.start();
		const auto response = StudioCapabilities::execute("midi.record", options()); clock.stop();
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QCOMPARE(response.value("result").toObject().value("noteCount").toInt(), 0); QCOMPARE(m_clip->notes().size(), std::size_t(1)); assertReleased();
	}

	void batchPublicationPreservesExistingNotesAndOwnsRecordedDetuning()
	{
		const auto prior = m_clip->notes().front(); Note recorded(17, 0, 60, 90); recorded.createDetuning();
		recorded.detuning()->automationClip()->putValue(0, 1.25f, false);
		m_clip->appendRecordedNotes({recorded, Note(12, 96, 62, 80)});
		QCOMPARE(m_clip->notes().size(), std::size_t(3)); QCOMPARE(m_clip->notes().at(1), prior);
		const auto accepted = m_clip->notes().front(); QCOMPARE(accepted->key(), 60);
		QVERIFY(accepted->detuning() != recorded.detuning()); recorded.detuning()->automationClip()->putValue(0, 2.5f, false);
		QCOMPARE(accepted->detuning()->automationClip()->valueAt(0), 1.25f);
	}

	void deletedTargetAbortsWithoutLeavingAnInstrumentReservation()
	{
		QTimer::singleShot(1, this, [&] { delete m_clip; m_clip = nullptr; });
		const auto response = StudioCapabilities::execute("midi.record", options());
		QVERIFY(!response.value("ok").toBool()); QVERIFY(m_track->getClips().empty()); assertReleased();
	}

	void cancellationBeforeBatchPublicationKeepsOriginalNoteIdentity()
	{
		const auto prior = m_clip->notes().front(); std::vector<Note> take(600, Note(12, 24, 60)); int checkpoints = 0;
		QVERIFY_EXCEPTION_THROWN(m_clip->appendRecordedNotes(take, [&] {
			if (++checkpoints == 5) { throw std::runtime_error("cancel before publication"); }
		}), std::runtime_error);
		QCOMPARE(m_clip->notes().size(), std::size_t(1)); QCOMPARE(m_clip->notes().front(), prior);
	}

	void committedTakeReplaysUndoesAndRecoversWithoutInput()
	{
		QTemporaryDir session; QTemporaryDir fixture; const auto project = fixture.filePath("prior.mmp");
		QVERIFY(Engine::getSong()->saveProjectFile(project)); const auto take = request("take", "midi.record", options());
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("load", "project.load", {{"path", project}})).value("ok").toBool()); refresh(); attach();
			int periods = 0, busy = 0; QTimer clock; clock.setInterval(1);
			QObject::connect(&clock, &QTimer::timeout, this, [&] {
				if (!Engine::getSong()->isRecording()) { return; }
				if (!runtime.process(request("busy", "song.set", {{"tempo", 80}})).value("ok").toBool()) { ++busy; }
				if (periods == 0) { m_input.feed(0x90, 60, 96); }
				if (periods == 4) { m_input.feed(0xe0, 127, 127); } period(); ++periods;
			}); clock.start(); const auto response = runtime.process(take); clock.stop();
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(busy > 0);
			QCOMPARE(m_clip->notes().size(), std::size_t(2)); const int revision = response.value("revision").toInt();
			const auto replay = runtime.process(take); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("revision").toInt(), revision);
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); refresh(); QCOMPARE(m_clip->notes().size(), std::size_t(1));
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); refresh(); QCOMPARE(m_clip->notes().size(), std::size_t(2));
			for (const auto note : m_clip->notes()) { if (note->key() == 60) { QVERIFY(note->hasDetuningInfo()); } }
		}
		StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error)); refresh();
		QCOMPARE(m_clip->notes().size(), std::size_t(2)); QVERIFY(recovered.process(take).value("replayed").toBool()); assertReleased();
		for (const auto note : m_clip->notes())
		{
			if (note->key() == 60)
			{
				QVERIFY(note->hasDetuningInfo()); QCOMPARE(note->detuning()->automationClip()->valueAt(note->length()), 1.f);
			}
		}
	}

	void cancelledTakeRestoresCheckpointAndHistory()
	{
		QTemporaryDir session; QTemporaryDir fixture; const auto project = fixture.filePath("prior.mmp");
		QVERIFY(Engine::getSong()->saveProjectFile(project)); StudioRuntime runtime(session.path()); QString error;
		QVERIFY2(runtime.open(error), qPrintable(error)); QVERIFY(runtime.process(request("load", "project.load", {{"path", project}})).value("ok").toBool());
		refresh(); attach(); const auto prior = manifest(session); int periods = 0; QTimer clock; clock.setInterval(1);
		QObject::connect(&clock, &QTimer::timeout, this, [&] {
			if (!Engine::getSong()->isRecording()) { return; }
			if (periods == 0) { m_input.feed(0x90, 60, 96); }
			if (periods == 2) { runtime.process(request("cancel", "session.shutdown")); }
			period(); ++periods;
		}); clock.start(); const auto response = runtime.process(request("cancelled-take", "midi.record", options())); clock.stop();
		QVERIFY(!response.value("ok").toBool()); refresh(); QCOMPARE(m_clip->notes().size(), std::size_t(1));
		QCOMPARE(manifest(session).value("history"), prior.value("history")); QCOMPARE(manifest(session).value("cursor"), prior.value("cursor")); assertReleased();
	}
};
QTEST_GUILESS_MAIN(StudioMidiRecordingTest)
#include "StudioMidiRecordingTest.moc"

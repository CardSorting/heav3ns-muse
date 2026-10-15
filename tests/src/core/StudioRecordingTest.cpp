/* Native bounded recording transaction tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <limits>
#include <stdexcept>
#include <vector>
#include "AudioDevice.h"
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleRecordHandle.h"
#include "SampleRecordingBuffer.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "StudioAudio.h"

using namespace lmms;

namespace
{
// Native hardware adapter fixture. Input and every processing period are handled
// by the real LMMS engine; this does not establish microphone/device operation.
class CaptureDevice final : public AudioDevice
{
public:
	explicit CaptureDevice(AudioEngine* engine) : AudioDevice(DEFAULT_CHANNELS, engine) { m_supportsCapture = true; }
	~CaptureDevice() override { stopProcessing(); }
	void disableCapture() { m_supportsCapture = false; }
private:
	void startProcessingImpl() override { }
	void stopProcessingImpl() override { }
};

class FailingCaptureAccompaniment final : public PlayHandle
{
public:
	explicit FailingCaptureAccompaniment(AudioBusHandle* bus) : PlayHandle(Type::SamplePlayHandle) { setAudioBusHandle(bus); }
	bool isFinished() const override { return false; }
	bool affinityMatters() const override { return true; }
	bool isFromTrack(const Track*) const override { return false; }
	void play(SampleFrame*) override { throw std::runtime_error("Native capture accompaniment DSP fixture"); }
};
}

class StudioRecordingTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	SampleClip* m_clip = nullptr;
	CaptureDevice* m_device = nullptr;
	std::shared_ptr<const SampleBuffer> m_previous;
	std::size_t m_inputFrame = 0;

	static SampleFrame inputValue(std::size_t frame)
	{
		return SampleFrame(.25f + static_cast<float>(frame % 64) / 1024, -.125f - static_cast<float>(frame % 32) / 512);
	}
	void feed()
	{
		std::vector<SampleFrame> input(256);
		for (auto& frame : input) { frame = inputValue(m_inputFrame++); }
		Engine::audioEngine()->pushInputFrames(input.data(), input.size()); Engine::audioEngine()->renderNextPeriod();
	}
	QJsonObject args(bool playback = false)
	{
		return {{"track", 0}, {"clip", 0}, {"durationMs", 20}, {"timeoutMs", 1000}, {"maximumBytes", 8192}, {"playback", playback}};
	}
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", op}, {"args", args}};
	}
	QJsonObject manifest(const QTemporaryDir& directory)
	{
		QFile file(directory.filePath("session.json"));
		return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
	}
	SampleClip* currentClip() { return static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0)); }

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		Engine::init(true); m_device = new CaptureDevice(Engine::audioEngine()); Engine::audioEngine()->setAudioDevice(m_device, true);
		Engine::getSong()->clearProject(); m_inputFrame = 0;
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		m_clip = currentClip(); m_previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(8, SampleFrame(.75f)), 24000);
		m_clip->setSampleBuffer(m_previous);
	}
	void cleanup() { Engine::destroy(); m_clip = nullptr; m_device = nullptr; m_previous.reset(); }

	void dspFailureBeforeOrDuringCaptureCannotReplacePriorPcm_data()
	{
		QTest::addColumn<bool>("before"); QTest::newRow("latched-before-start") << true; QTest::newRow("native-job-during-capture") << false;
	}
	void dspFailureBeforeOrDuringCaptureCannotReplacePriorPcm()
	{
		QFETCH(bool, before); const auto engine = Engine::audioEngine(); AudioBusHandle bus("Capture DSP failure");
		FailingCaptureAccompaniment* handle = nullptr;
		if (before) { engine->reportDspFailure(AudioEngine::DspFailure::InstrumentJobException); }
		else { handle = new FailingCaptureAccompaniment(&bus); QVERIFY(engine->addPlayHandle(handle)); }
		const auto remove = qScopeGuard([&] { if (handle) { engine->removePlayHandle(handle); } });
		QTimer input; input.setInterval(1); QObject::connect(&input, &QTimer::timeout, this, [&] { feed(); });
		input.start(); const auto result = StudioCapabilities::execute("audio.record", args()); input.stop();
		QVERIFY(!result.value("ok").toBool()); QVERIFY(result.value("error").toString().contains("DSP"));
		QCOMPARE(m_clip->sample().sampleSize(), std::size_t(8)); QCOMPARE(m_clip->sample().sampleRate(), 24000);
		QCOMPARE(m_clip->sample().data()[0].left(), .75f);
		QVERIFY(!m_clip->hasReservedRecording()); QVERIFY(!m_clip->hasPendingRecording()); QVERIFY(!m_clip->hasActiveRecording());
		QVERIFY(!m_clip->isRecord()); QVERIFY(Engine::getSong()->isStopped());
	}

	void aCompleteNativeTakePublishesExactlyTheRequestedFrames_data()
	{
		QTest::addColumn<bool>("playback"); QTest::addColumn<bool>("tempoChange");
		QTest::newRow("input-only-operation") << false << false;
		QTest::newRow("native-song-accompaniment") << true << false;
		QTest::newRow("native-accompaniment-tempo-refresh") << true << true;
	}
	void aCompleteNativeTakePublishesExactlyTheRequestedFrames()
	{
		QFETCH(bool, playback); QFETCH(bool, tempoChange); bool changed = false; QTimer input; input.setInterval(1);
		QObject::connect(&input, &QTimer::timeout, this, [&] {
			if (tempoChange && !changed && m_inputFrame >= 256) { Engine::getSong()->setTempo(200); changed = true; } feed();
		});
		Engine::getSong()->getTimeline(Song::PlayMode::Song).setLoopEnabled(true); input.start();
		const auto result = StudioCapabilities::execute("audio.record", args(playback)); input.stop();
		QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		const auto frames = (20ull * Engine::audioEngine()->inputSampleRate() + 999) / 1000;
		QCOMPARE(m_clip->sample().sampleSize(), static_cast<std::size_t>(frames)); QCOMPARE(result.value("result").toObject().value("frames").toDouble(), static_cast<double>(frames));
		for (std::size_t frame = 0; frame < frames; ++frame)
		{
			QCOMPARE(m_clip->sample().data()[frame].left(), inputValue(frame).left()); QCOMPARE(m_clip->sample().data()[frame].right(), inputValue(frame).right());
		}
		QVERIFY(!m_clip->hasReservedRecording()); QVERIFY(!m_clip->hasPendingRecording()); QVERIFY(!m_clip->hasActiveRecording()); QVERIFY(!m_clip->isRecord());
		QVERIFY(Engine::getSong()->isStopped()); QVERIFY(Engine::getSong()->getTimeline(Song::PlayMode::Song).loopEnabled());
		if (tempoChange) { QVERIFY(changed); }
	}

	void rejectedOrCancelledRecordingPreservesThePriorNativeSample_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("no-input-timeout") << 0; QTest::newRow("cancelled-after-input") << 1;
		QTest::newRow("device-disconnected") << 2; QTest::newRow("nonfinite-input") << 3;
		QTest::newRow("insufficient-byte-budget") << 4; QTest::newRow("noncapture-backend") << 5;
	}
	void rejectedOrCancelledRecordingPreservesThePriorNativeSample()
	{
		QFETCH(int, failure); bool cancelled = false; int periods = 0; QTimer input; input.setInterval(1);
		QObject::connect(&input, &QTimer::timeout, this, [&] {
			if (failure == 3) { SampleFrame invalid(std::numeric_limits<float>::quiet_NaN()); Engine::audioEngine()->pushInputFrames(&invalid, 1); Engine::audioEngine()->renderNextPeriod(); }
			else { feed(); }
			if (++periods == 1 && failure == 1) { cancelled = true; }
			if (periods == 1 && failure == 2) { m_device->reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected); }
		});
		auto options = args(); if (failure == 0) { options.insert("timeoutMs", 30); }
		if (failure == 4) { options.insert("maximumBytes", 8); } if (failure == 5) { m_device->disableCapture(); }
		if (failure >= 1 && failure <= 3) { input.start(); }
		const auto result = StudioCapabilities::execute("audio.record", options, [&] { return cancelled; }); input.stop();
		QVERIFY(!result.value("ok").toBool()); QCOMPARE(m_clip->sample().toBase64(), m_previous->toBase64());
		QVERIFY(!m_clip->hasReservedRecording()); QVERIFY(!m_clip->hasActiveRecording()); QVERIFY(!m_clip->hasPendingRecording()); QVERIFY(!m_clip->isRecord());
	}

	void reservedCompletionBlocksSavingAndCompetingTakes()
	{
		QString error; QVERIFY(m_clip->reserveRecordingCompletion(error));
		QVERIFY(!m_clip->reserveRecordingCompletion(error)); QVERIFY(!m_clip->beginRecordingTake());
		QVERIFY(!SampleRecordHandle::finalizePendingRecordings(error));
		QVERIFY_EXCEPTION_THROWN(StudioAudio::render({{"path", "reserved-capture.wav"}}), std::runtime_error);
		QTemporaryDir directory; QFile existing(directory.filePath("protected.mmp")); QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("prior-file"); existing.close();
		QVERIFY(!Engine::getSong()->saveProjectFile(existing.fileName()));
		QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("prior-file")); existing.close();
		auto capture = std::make_shared<SampleRecordingBuffer>(); SampleFrame recorded(.25f); QVERIFY(capture->append(&recorded, 1, 24000));
		QVERIFY(m_clip->beginRecordingTake(true)); m_clip->stageRecordingCompletion(std::move(capture));
		QCoreApplication::processEvents(); QCOMPARE(m_clip->sample().toBase64(), m_previous->toBase64());
		QVERIFY(!m_clip->finalizeRecordings(error)); QVERIFY(!m_clip->finishReservedRecording(2, 24000, error));
		m_clip->discardReservedRecording(); QVERIFY(!m_clip->hasReservedRecording()); QVERIFY(!m_clip->hasPendingRecording());
		QCOMPARE(m_clip->sample().toBase64(), m_previous->toBase64());
	}

	void recordingCommitsNativeHistoryAndReplaysWithoutAnotherTake()
	{
		QTemporaryDir session; QTemporaryDir fixture; const auto project = fixture.filePath("prior.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(project));
		QByteArray accepted; int committedRevision = 0; int priorCursor = 0;
		{
			StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("prior", "project.load", {{"path", project}})).value("ok").toBool()); m_clip = currentClip();
			const auto prior = manifest(session); priorCursor = prior.value("cursor").toInt(); int rejectedBusy = 0;
			QTimer input; input.setInterval(1); QObject::connect(&input, &QTimer::timeout, this, [&] {
				const auto blocked = runtime.process(request("busy", "song.set", {{"tempo", 90}})); if (!blocked.value("ok").toBool()) { ++rejectedBusy; } feed();
			}); input.start(); const auto capture = request("take", "audio.record", args()); const auto result = runtime.process(capture); input.stop();
			QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QVERIFY(rejectedBusy > 0);
			committedRevision = result.value("revision").toInt(); QCOMPARE(committedRevision, prior.value("revision").toInt() + 1);
			QCOMPARE(manifest(session).value("cursor").toInt(), priorCursor + 1); accepted = m_clip->sample().toBase64().toUtf8();
			QVERIFY(runtime.process(capture).value("replayed").toBool()); QCOMPARE(currentClip()->sample().toBase64().toUtf8(), accepted);
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); QCOMPARE(currentClip()->sample().toBase64(), m_previous->toBase64());
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); QCOMPARE(currentClip()->sample().toBase64().toUtf8(), accepted);
		}
		{
			StudioRuntime recovered(session.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
			QCOMPARE(currentClip()->sample().toBase64().toUtf8(), accepted); QVERIFY(Engine::getSong()->isStopped());
			QVERIFY(recovered.process(request("take", "audio.record", args())).value("replayed").toBool()); QCOMPARE(currentClip()->sample().toBase64().toUtf8(), accepted);
		}
	}

	void failedNativeRecordingRollsBackThePriorCheckpointAndHistory()
	{
		QTemporaryDir session; QTemporaryDir fixture; const auto project = fixture.filePath("prior.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(project));
		StudioRuntime runtime(session.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("prior", "project.load", {{"path", project}})).value("ok").toBool()); const auto prior = manifest(session);
		auto options = args(); options.insert("timeoutMs", 30); const auto failed = runtime.process(request("failed-take", "audio.record", options));
		QVERIFY(!failed.value("ok").toBool()); QCOMPARE(failed.value("revision").toInt(), prior.value("revision").toInt() + 1);
		QCOMPARE(manifest(session).value("history"), prior.value("history")); QCOMPARE(manifest(session).value("cursor"), prior.value("cursor"));
		QCOMPARE(currentClip()->sample().toBase64(), m_previous->toBase64()); QVERIFY(!currentClip()->hasReservedRecording());
	}
};

QTEST_GUILESS_MAIN(StudioRecordingTest)
#include "StudioRecordingTest.moc"

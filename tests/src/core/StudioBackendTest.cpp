/* Native backend initialization tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QSet>
#include <QFile>
#include <cstdio>
#include <thread>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <vector>

#include "AudioDummy.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "MidiDummy.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "SampleClip.h"
#include "SampleBuffer.h"
#include "SampleRecordHandle.h"
#include "SampleTrack.h"

namespace
{
// Hardware status fixture only; PCM and transport still use native LMMS.
class ControlledDevice final : public lmms::AudioDevice
{
public:
	explicit ControlledDevice(lmms::AudioEngine* engine, bool capture = true)
		: AudioDevice(lmms::DEFAULT_CHANNELS, engine) { m_supportsCapture = capture; }
	~ControlledDevice() override { stopProcessing(); }
	int starts = 0;
	bool failStart = false, throwStart = false;

private:
	void startProcessingImpl() override
	{
		++starts;
		if (throwStart) { throw std::runtime_error("native device fixture start"); }
		if (failStart) { reportProcessingFailure(ProcessingFailure::Start, -99); }
	}
	void stopProcessingImpl() override { }
};
}

using namespace lmms;

class StudioBackendTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid());
		ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		NotePlayHandleManager::init();
	}

	void cleanupTestCase() { NotePlayHandleManager::free(); }

	void compiledFactoryDiscoveryNeedsNoEngineAndPreservesConfiguration()
	{
		QVERIFY(!Engine::audioEngine()); QCOMPARE(QString(QCoreApplication::instance()->metaObject()->className()), QString("QCoreApplication"));
		ConfigManager::inst()->setValue("audioengine", "audiodev", "unchanged diagnostic selection");
		const auto audio = AudioEngine::availableAudioDeviceNames(); const auto midi = AudioEngine::availableMidiClientNames();
		QVERIFY(audio.contains(AudioDummy::name())); QVERIFY(midi.contains(MidiDummy::name()));
		QSet<QString> distinctAudio, distinctMidi;
		for (const auto& name : audio) { QVERIFY(!name.isEmpty()); QVERIFY(AudioEngine::isAudioDevNameValid(name)); distinctAudio.insert(name); }
		for (const auto& name : midi) { QVERIFY(!name.isEmpty()); QVERIFY(AudioEngine::isMidiDevNameValid(name)); distinctMidi.insert(name); }
		QCOMPARE(distinctAudio.size(), audio.size()); QCOMPARE(distinctMidi.size(), midi.size());
		QVERIFY(!AudioEngine::isAudioDevNameValid("not a compiled audio factory"));
		QVERIFY(!AudioEngine::isMidiDevNameValid("not a compiled MIDI factory"));
		QCOMPARE(ConfigManager::inst()->value("audioengine", "audiodev"), QString("unchanged diagnostic selection"));
		QVERIFY(!Engine::audioEngine());
	}

	void nativeLiveInitializationUsesExplicitDummyFactoriesWithoutWindows()
	{
		ConfigManager::inst()->setValue("audioengine", "audiodev", AudioDummy::name());
		ConfigManager::inst()->setValue("audioengine", "mididev", MidiDummy::name());
		ConfigManager::inst()->setValue("audioengine", "samplerate", "48000");
		ConfigManager::inst()->setValue("audioengine", "framesperaudiobuffer", "256");
		Engine::init(false); const auto cleanup = qScopeGuard([] { Engine::destroy(); });
		const auto engine = Engine::audioEngine(); QVERIFY(!engine->renderOnly());
		QVERIFY(dynamic_cast<AudioDummy*>(engine->audioDev())); QVERIFY(dynamic_cast<MidiDummy*>(engine->midiClient()));
		QVERIFY(engine->audioDev()->isRunning()); QVERIFY(!engine->audioDevStartFailed());
		const auto inspected = StudioCapabilities::execute("audio.backend.list", {}); QVERIFY(inspected.value("ok").toBool());
		const auto result = inspected.value("result").toObject();
		QCOMPARE(result.value("configuredAudioBackend").toString(), AudioDummy::name());
		QCOMPARE(result.value("configuredMidiBackend").toString(), MidiDummy::name());
		const auto backend = result.value("current").toObject().value("backend").toObject();
		QVERIFY(!backend.value("renderOnly").toBool()); QCOMPARE(backend.value("sampleRate").toInt(), 48000);
		QVERIFY(StudioCapabilities::execute("transport.play", {}).value("ok").toBool());
		QTRY_VERIFY_WITH_TIMEOUT(StudioCapabilities::execute("transport.inspect", {}).value("result").toObject().value("position").toInt() > 0, 2000);
		QVERIFY(StudioCapabilities::execute("transport.stop", {}).value("ok").toBool());
	}

	void nativeRenderOnlyInitializationBypassesConfiguredFactories()
	{
		ConfigManager::inst()->setValue("audioengine", "audiodev", "unavailable diagnostic audio factory");
		ConfigManager::inst()->setValue("audioengine", "mididev", "unavailable diagnostic MIDI factory");
		Engine::init(true); const auto cleanup = qScopeGuard([] { Engine::destroy(); });
		const auto engine = Engine::audioEngine(); QVERIFY(engine->renderOnly());
		QVERIFY(dynamic_cast<AudioDummy*>(engine->audioDev())); QVERIFY(dynamic_cast<MidiDummy*>(engine->midiClient()));
		QVERIFY(engine->audioDev()->isRunning()); QVERIFY(!engine->audioDevStartFailed());
		QCOMPARE(ConfigManager::inst()->value("audioengine", "audiodev"), QString("unavailable diagnostic audio factory"));
	}

	void failedProtocolOutputStopsNativeRuntimeBeforeInputPolling()
	{
		Engine::init(true); const auto cleanup = qScopeGuard([] { Engine::destroy(); }); QTemporaryDir session;
		QVERIFY(session.isValid()); StudioRuntime runtime(session.path(), {}, nullptr);
		QCOMPARE(runtime.run(), EXIT_FAILURE); QVERIFY(runtime.stopping());
		QFile manifest(session.filePath("session.json")); QVERIFY(manifest.exists());
		// The native initial checkpoint exists, but readiness was never delivered
		// and the run exits without starting an input timer/event loop.
	}

	void nativeDeviceFailuresAreVisibleAndRecoverOnlyOnAnExplicitStart()
	{
		Engine::init(true); const auto cleanup = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		auto device = new ControlledDevice(engine); device->failStart = true; engine->setAudioDevice(device, true);
		QVERIFY(!device->isRunning()); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Start);
		QCOMPARE(device->processingStatus().nativeError, -99);
		const auto backend = StudioCapabilities::execute("transport.inspect", {}).value("result").toObject().value("backend").toObject();
		QVERIFY(!backend.value("running").toBool()); QCOMPARE(backend.value("processingFailure").toString(), QString("start"));
		QCOMPARE(backend.value("nativeError").toInt(), -99);
		QVERIFY(!StudioCapabilities::execute("transport.play", {}).value("ok").toBool());
		device->reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected, 7); QCOMPARE(device->processingStatus().nativeError, -99);
		device->stopProcessing(); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Start);
		device->failStart = false; device->startProcessing(); QVERIFY(device->isRunning());
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None); QCOMPARE(device->processingStatus().nativeError, 0);
		device->startProcessing(); QCOMPARE(device->starts, 2);
		device->stopProcessing(); device->throwStart = true; QVERIFY_EXCEPTION_THROWN(device->startProcessing(), std::runtime_error);
		QVERIFY(!device->isRunning()); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Exception);
	}

	void asynchronousDeviceFailurePublishesOneCoherentStatus_data()
	{
		QTest::addColumn<bool>("capture"); QTest::newRow("capture") << true; QTest::newRow("output-only") << false;
	}

	void asynchronousDeviceFailurePublishesOneCoherentStatus()
	{
		QFETCH(bool, capture); Engine::init(true); const auto cleanup = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		ControlledDevice device(engine, capture); device.startProcessing(); const auto epoch = engine->inputErrorEpoch();
		std::atomic<bool> completed{false}; const auto code = std::numeric_limits<std::int32_t>::min();
		std::thread callback([&] {
			device.reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected, code);
			for (int repeat = 0; repeat < 1000; ++repeat) { device.reportProcessingFailure(AudioDevice::ProcessingFailure::Stop, 17); }
			completed.store(true);
		});
		bool coherent = true;
		do
		{
			const auto status = device.processingStatus();
			coherent &= status.failure == AudioDevice::ProcessingFailure::None ? status.running && status.nativeError == 0
				: !status.running && status.failure == AudioDevice::ProcessingFailure::Disconnected && status.nativeError == code;
			if (capture && status.failure != AudioDevice::ProcessingFailure::None) { coherent &= engine->inputErrorEpoch() > epoch; }
		} while (!completed.load());
		callback.join(); QVERIFY(coherent); QCOMPARE(device.processingStatus().nativeError, code);
		QCOMPARE(engine->inputErrorEpoch(), epoch + (capture ? 1 : 0));
	}

	void captureDiscontinuityRejectsActiveAndLateStartingNativeTakes_data()
	{
		QTest::addColumn<bool>("late"); QTest::newRow("active-take") << false; QTest::newRow("late-take-with-old-input") << true;
	}

	void captureDiscontinuityRejectsActiveAndLateStartingNativeTakes()
	{
		QFETCH(bool, late); Engine::init(true); const auto cleanup = qScopeGuard([] { Engine::destroy(); }); const auto engine = Engine::audioEngine();
		auto device = new ControlledDevice(engine); engine->setAudioDevice(device, true); const auto song = Engine::getSong();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(song->tracks().at(0)->getClip(0));
		const auto previous = std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32, SampleFrame(.125f)), 24000); clip->setSampleBuffer(previous);
		std::vector<SampleFrame> input(101, SampleFrame(.25f, -.5f)); engine->pushInputFrames(input.data(), input.size());
		if (!late) { QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip))); engine->renderNextPeriod(); }
		device->reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected);
		if (late) { QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip))); engine->renderNextPeriod(); }
		engine->finishSampleRecordings(); QString error; SampleRecordHandle::finalizePendingRecordings(error);
		QVERIFY(song->hasErrors()); QCOMPARE(clip->sample().toBase64(), previous->toBase64()); song->clearErrors();
		device->startProcessing(); QVERIFY(device->isRunning());
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(clip))); engine->pushInputFrames(input.data(), input.size()); engine->renderNextPeriod();
		engine->finishSampleRecordings(); QVERIFY(SampleRecordHandle::finalizePendingRecordings(error));
		QVERIFY(!song->hasErrors()); QCOMPARE(clip->sample().sampleSize(), input.size());
		QCOMPARE(clip->sample().sampleRate(), static_cast<int>(engine->inputSampleRate()));
		for (std::size_t frame = 0; frame < input.size(); ++frame)
		{
			QCOMPARE(clip->sample().data()[frame].left(), input[frame].left()); QCOMPARE(clip->sample().data()[frame].right(), input[frame].right());
		}
	}
};

QTEST_GUILESS_MAIN(StudioBackendTest)
#include "StudioBackendTest.moc"

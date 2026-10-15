/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <atomic>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <limits>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AudioEngineWorkerThread.h"
#include "ConfigManager.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

namespace lmms
{
namespace
{
class ControlledJob final : public ThreadableJob
{
public:
	bool throws = false;
	bool nonstandard = false;
	ControlledJob* nested = nullptr;
	std::atomic<int> calls{0};
	bool requiresProcessing() const override { return true; }
protected:
	void doProcessing() override
	{
		++calls;
		if (nested) { nested->queue(); nested->process(); }
		if (throws)
		{
			if (nonstandard) { throw 17; }
			throw std::runtime_error("Native job fixture exception");
		}
	}
};

enum class FaultMode { Healthy, Exception, Nonfinite };
class ControlledPlay final : public PlayHandle
{
public:
	explicit ControlledPlay(AudioBusHandle* bus) : PlayHandle(Type::SamplePlayHandle) { setAudioBusHandle(bus); }
	std::atomic<FaultMode> mode{FaultMode::Exception};
	std::atomic<int> calls{0};
	bool isFinished() const override { return false; }
	bool affinityMatters() const override { return true; }
	bool isFromTrack(const Track*) const override { return false; }
	void play(SampleFrame* output) override
	{
		++calls;
		std::fill_n(output, Engine::audioEngine()->framesPerPeriod(), SampleFrame(.25f, -.125f));
		if (mode == FaultMode::Exception) { throw std::runtime_error("Native play fixture exception"); }
		if (mode == FaultMode::Nonfinite) { output[0][0] = std::numeric_limits<float>::quiet_NaN(); }
	}
};

const Plugin::Descriptor faultDescriptor{"native-fault-fixture", "Native fault fixture", "Processing failure fixture", "LMMS tests",
	1, Plugin::Type::Effect, nullptr, "", nullptr};
class ControlledEffect final : public Effect
{
public:
	explicit ControlledEffect(EffectChain* chain) : Effect(&faultDescriptor, chain, nullptr) { }
	FaultMode mode = FaultMode::Exception;
	EffectControls* controls() override { return nullptr; }
protected:
	ProcessStatus processImpl(SampleFrame* output, f_cnt_t) override
	{
		if (mode == FaultMode::Exception) { throw std::runtime_error("Native effect fixture exception"); }
		if (mode == FaultMode::Nonfinite) { output[0][1] = std::numeric_limits<float>::quiet_NaN(); }
		return ProcessStatus::Continue;
	}
};
}

class AudioEngineFaultTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static bool silent(std::span<const SampleFrame> output)
	{
		return std::all_of(output.begin(), output.end(), [](const auto& frame) { return frame[0] == 0.f && frame[1] == 0.f; });
	}
	static QJsonObject request(const QString& id, const QString& op) { return {{"id", id}, {"op", op}, {"args", QJsonObject{}}}; }
	static QJsonObject manifest(const QString& session)
	{
		QFile file(QDir(session).filePath("session.json"));
		return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stop(); Engine::getSong()->clearProject();
		QVERIFY(Engine::audioEngine()->resetDspFailure()); Engine::audioEngine()->setSanitizationEnabled(true);
	}
	void nestedExceptionsCompleteJobsAndPermitAnotherQueue_data()
	{
		QTest::addColumn<bool>("dynamic"); QTest::addColumn<bool>("nonstandard");
		QTest::newRow("static-standard") << false << false; QTest::newRow("dynamic-standard") << true << false;
		QTest::newRow("static-nonstandard") << false << true; QTest::newRow("dynamic-nonstandard") << true << true;
	}
	void nestedExceptionsCompleteJobsAndPermitAnotherQueue()
	{
		QFETCH(bool, dynamic); QFETCH(bool, nonstandard); using Queue = AudioEngineWorkerThread::JobQueue;
		Queue queue; queue.reset(dynamic ? Queue::OperationMode::Dynamic : Queue::OperationMode::Static);
		ControlledJob outer, nested, skipped; nested.throws = true; nested.nonstandard = nonstandard; outer.nested = &nested;
		queue.addJob(&outer); queue.addJob(&skipped); queue.run(); queue.wait();
		QCOMPARE(queue.failure(), Queue::Failure::ProcessingException);
		QCOMPARE(outer.state(), ThreadableJob::ProcessingState::Done); QCOMPARE(nested.state(), ThreadableJob::ProcessingState::Done);
		QCOMPARE(skipped.state(), ThreadableJob::ProcessingState::Done); QCOMPARE(skipped.calls.load(), 0);
		queue.reset(Queue::OperationMode::Static); ControlledJob healthy; queue.addJob(&healthy); queue.run(); queue.wait();
		QCOMPARE(healthy.calls.load(), 1); QCOMPARE(queue.failure(), Queue::Failure::None);
	}
	void concurrentWorkersFinishAllClaimsAfterAnException()
	{
		using Queue = AudioEngineWorkerThread::JobQueue; Queue queue;
		std::vector<std::unique_ptr<ControlledJob>> jobs;
		for (int index = 0; index < 129; ++index)
		{
			auto job = std::make_unique<ControlledJob>(); job->throws = index == 0; queue.addJob(job.get()); jobs.push_back(std::move(job));
		}
		std::vector<std::thread> workers; for (int index = 0; index < 4; ++index) { workers.emplace_back([&] { queue.run(); }); }
		for (auto& worker : workers) { worker.join(); } queue.wait();
		QCOMPARE(queue.failure(), Queue::Failure::ProcessingException); QCOMPARE(jobs[0]->calls.load(), 1);
		for (const auto& job : jobs) { QCOMPARE(job->state(), ThreadableJob::ProcessingState::Done); QVERIFY(job->calls.load() <= 1); }
	}
	void queueOverflowEndsRejectedJobsAndDiscardsThePartialPeriod()
	{
		using Queue = AudioEngineWorkerThread::JobQueue; Queue queue;
		std::vector<std::unique_ptr<ControlledJob>> jobs;
		for (std::size_t index = 0; index <= Queue::JOB_QUEUE_SIZE; ++index)
		{
			auto job = std::make_unique<ControlledJob>(); queue.addJob(job.get()); jobs.push_back(std::move(job));
		}
		QCOMPARE(jobs.back()->state(), ThreadableJob::ProcessingState::Done);
		queue.run(); queue.wait(); QCOMPARE(queue.failure(), Queue::Failure::QueueOverflow);
		for (const auto& job : jobs) { QCOMPARE(job->state(), ThreadableJob::ProcessingState::Done); QCOMPARE(job->calls.load(), 0); }
	}
	void nativePeriodsLatchFailureUntilExplicitRepairAndRetry_data()
	{
		QTest::addColumn<bool>("nonfinite"); QTest::newRow("exception") << false; QTest::newRow("nonfinite") << true;
	}
	void nativePeriodsLatchFailureUntilExplicitRepairAndRetry()
	{
		QFETCH(bool, nonfinite); const auto engine = Engine::audioEngine(); AudioBusHandle bus("Native period fault");
		auto play = new ControlledPlay(&bus); play->mode = nonfinite ? FaultMode::Nonfinite : FaultMode::Exception;
		QVERIFY(engine->addPlayHandle(play)); const auto cleanup = qScopeGuard([&] { engine->removePlayHandle(play); });
		const auto before = engine->dspStatus().failures; QVERIFY(silent(engine->renderNextPeriod()));
		const auto fault = engine->dspStatus(); QVERIFY(fault.failures > before);
		QCOMPARE(fault.failure, nonfinite ? AudioEngine::DspFailure::NonfiniteOutput : AudioEngine::DspFailure::InstrumentJobException);
		QCOMPARE(play->state(), ThreadableJob::ProcessingState::Done); const auto calls = play->calls.load();
		QVERIFY(silent(engine->renderNextPeriod())); QCOMPARE(play->calls.load(), calls); QCOMPARE(engine->dspStatus().failures, fault.failures);
		play->mode = FaultMode::Healthy; QVERIFY(engine->resetDspFailure()); QCOMPARE(engine->dspStatus().failures, fault.failures);
		engine->renderNextPeriod(); const auto recovered = engine->renderNextPeriod();
		QVERIFY(!silent(recovered)); QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}
	void nativeBusAndMixerEffectFailuresCannotBeHiddenBySanitization_data()
	{
		QTest::addColumn<bool>("master"); QTest::addColumn<bool>("nonfinite"); QTest::addColumn<bool>("sanitize");
		for (const auto sanitize : {false, true})
		{
			const auto suffix = sanitize ? "-sanitized" : "-unsanitized";
			QTest::newRow(qPrintable(QString("bus-exception") + suffix)) << false << false << sanitize;
			QTest::newRow(qPrintable(QString("master-exception") + suffix)) << true << false << sanitize;
			QTest::newRow(qPrintable(QString("bus-nonfinite") + suffix)) << false << true << sanitize;
			QTest::newRow(qPrintable(QString("master-nonfinite") + suffix)) << true << true << sanitize;
		}
	}
	void nativeBusAndMixerEffectFailuresCannotBeHiddenBySanitization()
	{
		QFETCH(bool, master); QFETCH(bool, nonfinite); QFETCH(bool, sanitize);
		const auto engine = Engine::audioEngine(); engine->setSanitizationEnabled(sanitize); AudioBusHandle bus("Native FX fault");
		auto play = new ControlledPlay(&bus); play->mode = FaultMode::Healthy;
		QVERIFY(engine->addPlayHandle(play)); const auto cleanup = qScopeGuard([&] { engine->removePlayHandle(play); });
		const auto chain = master ? &Engine::mixer()->mixerChannel(0)->m_fxChain : bus.effects();
		auto effect = new ControlledEffect(chain); effect->mode = nonfinite ? FaultMode::Nonfinite : FaultMode::Exception; chain->appendEffect(effect);
		const auto removeEffect = qScopeGuard([&] { chain->removeEffect(effect); delete effect; });
		QVERIFY(silent(engine->renderNextPeriod()));
		QCOMPARE(engine->dspStatus().failure, nonfinite ? AudioEngine::DspFailure::CorruptedEffect
			: master ? AudioEngine::DspFailure::MixerJobException : AudioEngine::DspFailure::EffectJobException);
		effect->mode = FaultMode::Healthy; QVERIFY(engine->resetDspFailure()); engine->renderNextPeriod();
		QVERIFY(!silent(engine->renderNextPeriod())); QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}
	void resetRejectsNonownerPlaybackRecordingAndExportReservation()
	{
		const auto engine = Engine::audioEngine(); engine->reportDspFailure(AudioEngine::DspFailure::InstrumentJobException);
		bool reset = true; std::thread worker([&] { reset = engine->resetDspFailure(); }); worker.join(); QVERIFY(!reset);
		Engine::getSong()->playSong(); QVERIFY(!engine->resetDspFailure()); Engine::getSong()->stop();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		clip->setRecord(true); QVERIFY(!engine->resetDspFailure()); clip->setRecord(false);
		engine->storeAudioDevice(); QVERIFY(!engine->resetDspFailure()); engine->restoreAudioDevice(); engine->audioDev()->stopProcessing();
		QVERIFY(engine->resetDspFailure()); QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}
	void failedNativeRenderPreservesTheExistingTarget_data()
	{
		QTest::addColumn<bool>("nonfinite"); QTest::newRow("exception") << false; QTest::newRow("nonfinite") << true;
	}
	void failedNativeRenderPreservesTheExistingTarget()
	{
		QFETCH(bool, nonfinite); QTemporaryDir directory; QVERIFY(directory.isValid()); const auto target = directory.filePath("preserve.wav");
		QFile existing(target); QVERIFY(existing.open(QIODevice::WriteOnly)); QCOMPARE(existing.write("Prior authoritative export"), qint64(26)); existing.close();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 192}}).value("ok").toBool());
		const auto engine = Engine::audioEngine(); AudioBusHandle bus("Native export fault"); auto play = new ControlledPlay(&bus);
		play->mode = nonfinite ? FaultMode::Nonfinite : FaultMode::Exception;
		QVERIFY(engine->addPlayHandle(play)); const auto cleanup = qScopeGuard([&] { engine->removePlayHandle(play); });
		const auto result = StudioCapabilities::execute("audio.render", {{"path", target}, {"timeoutMs", 5000}});
		QVERIFY(!result.value("ok").toBool()); QVERIFY(result.value("error").toString().contains("DSP"));
		QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("Prior authoritative export"));
		QVERIFY(!Engine::getSong()->isExporting()); QVERIFY(!engine->hasStoredAudioDevice());
		QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 1); QVERIFY(engine->dspStatus().failure != AudioEngine::DspFailure::None);
	}
	void transientRetryReceiptsCannotClearANewerFailureOrCreateARevision()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto session = directory.filePath("session");
		StudioRuntime runtime(session); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); const auto before = manifest(session);
		const auto engine = Engine::audioEngine(); engine->reportDspFailure(AudioEngine::DspFailure::InstrumentJobException);
		const auto reset = request("retry", "audio.processing.reset"); QVERIFY(runtime.process(reset).value("ok").toBool());
		const auto repairedCount = engine->dspStatus().failures;
		engine->reportDspFailure(AudioEngine::DspFailure::CorruptedEffect);
		const auto replay = runtime.process(reset); QVERIFY(replay.value("replayed").toBool());
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::CorruptedEffect); QVERIFY(engine->dspStatus().failures > repairedCount);
		const auto after = manifest(session); QCOMPARE(after.value("revision"), before.value("revision")); QCOMPARE(after.value("history"), before.value("history"));
		const auto observed = runtime.process(request("observe", "audio.processing.inspect")); QVERIFY(observed.value("result").toObject().value("failed").toBool());
	}
};
} // namespace lmms
QTEST_GUILESS_MAIN(lmms::AudioEngineFaultTest)
#include "AudioEngineFaultTest.moc"

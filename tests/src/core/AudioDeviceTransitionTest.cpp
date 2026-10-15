/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>
#include <functional>
#include <utility>
#include <thread>
#include <stdexcept>
#include <vector>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AudioDeviceTransitionTestUtils.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "RemotePlugin.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"

namespace lmms
{
namespace
{
struct Counts { int starts = 0, stops = 0, destroyed = 0; };
// Native lifecycle adapter only; no physical driver or device is represented.
class LifecycleDevice final : public AudioDevice
{
public:
	LifecycleDevice(AudioEngine* engine, Counts& counts, sample_rate_t rate = 32000)
		: AudioDevice(DEFAULT_CHANNELS, engine), counts(counts) { setSampleRate(rate); }
	~LifecycleDevice() override { throwStop = false; stopProcessing(); ++counts.destroyed; }
	bool throwStop = false, throwStart = false, failStart = false;
	std::function<void()> onStop;
private:
	Counts& counts;
	void startProcessingImpl() override
	{
		++counts.starts;
		if (throwStart) { throw std::runtime_error("Native restart fixture"); }
		if (failStart) { reportProcessingFailure(ProcessingFailure::Start, -77); }
	}
	void stopProcessingImpl() override
	{
		++counts.stops;
		if (auto callback = std::exchange(onStop, {})) { callback(); }
		if (throwStop) { throw std::runtime_error("Native stop fixture"); }
	}
};
}

class AudioDeviceTransitionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static void destroyEngine()
	{
		if (const auto engine = Engine::audioEngine())
		{
			if (const auto device = dynamic_cast<LifecycleDevice*>(engine->audioDev())) { device->throwStop = false; device->onStop = {}; }
			Engine::destroy();
		}
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init() { Engine::init(true); Engine::getSong()->clearProject(); }
	void cleanup() { destroyEngine(); }

	void restoredBackendPreservesItsOriginalRunningState_data()
	{
		QTest::addColumn<bool>("running"); QTest::newRow("stopped") << false; QTest::newRow("running") << true;
	}
	void restoredBackendPreservesItsOriginalRunningState()
	{
		QFETCH(bool, running); const auto engine = Engine::audioEngine(); Counts priorCounts, temporaryCounts;
		const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto prior = new LifecycleDevice(engine, priorCounts); engine->setAudioDevice(prior, running);
		QVERIFY(engine->storeAudioDevice()); QVERIFY(!engine->storeAudioDevice());
		engine->setAudioDevice(new LifecycleDevice(engine, temporaryCounts, 48000), false);
		QVERIFY(engine->restoreAudioDevice()); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior));
		QCOMPARE(prior->isRunning(), running); QCOMPARE(priorCounts.starts, running ? 2 : 0);
		QCOMPARE(temporaryCounts.destroyed, 1); QVERIFY(!engine->hasStoredAudioDevice());
		QVERIFY(engine->restoreAudioDevice()); QCOMPARE(priorCounts.starts, running ? 2 : 0);
	}

	void samePointerSwitchAndCancelledReservationDoNotDeleteOrRestartTheDevice()
	{
		const auto engine = Engine::audioEngine(); Counts counts; const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto device = new LifecycleDevice(engine, counts); engine->setAudioDevice(device, false);
		engine->setAudioDevice(device, false); QCOMPARE(counts.destroyed, 0); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(device));
		QSignalSpy rate(engine, &AudioEngine::sampleRateChanged); QVERIFY(engine->storeAudioDevice());
		QVERIFY(engine->restoreAudioDevice()); QCOMPARE(rate.count(), 0); QCOMPARE(counts.starts, 0); QCOMPARE(counts.destroyed, 0);
	}

	void deviceStopCanJoinNativeRenderingWithoutHoldingTheModelMutex_data()
	{
		QTest::addColumn<bool>("restore"); QTest::newRow("switch") << false; QTest::newRow("restore-operation") << true;
	}
	void deviceStopCanJoinNativeRenderingWithoutHoldingTheModelMutex()
	{
		QFETCH(bool, restore); const auto engine = Engine::audioEngine(); Counts priorCounts, temporaryCounts;
		const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto prior = new LifecycleDevice(engine, priorCounts); engine->setAudioDevice(prior, true); QVERIFY(engine->storeAudioDevice());
		bool rendered = false;
		const auto joinNativeRender = [&] {
			std::thread callback([&] { engine->renderNextPeriod(); rendered = true; }); callback.join();
		};
		if (!restore) { prior->onStop = joinNativeRender; }
		auto temporary = new LifecycleDevice(engine, temporaryCounts); engine->setAudioDevice(temporary, false);
		if (restore) { temporary->onStop = joinNativeRender; }
		QVERIFY(StudioCapabilities::execute("audio.device.restore", {}).value("ok").toBool());
		QVERIFY(rendered); QCOMPARE(temporaryCounts.destroyed, 1); QVERIFY(prior->isRunning());
	}

	void failedNotificationsRetainTheRestoredOwnerForRetry_data()
	{
		QTest::addColumn<bool>("quality"); QTest::addColumn<bool>("nonstandard");
		QTest::newRow("quality-standard") << true << false; QTest::newRow("rate-standard") << false << false;
		QTest::newRow("quality-nonstandard") << true << true; QTest::newRow("rate-nonstandard") << false << true;
	}
	void failedNotificationsRetainTheRestoredOwnerForRetry()
	{
		QFETCH(bool, quality); QFETCH(bool, nonstandard); const auto engine = Engine::audioEngine(); Counts priorCounts, temporaryCounts;
		const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto prior = new LifecycleDevice(engine, priorCounts); engine->setAudioDevice(prior, true); QVERIFY(engine->storeAudioDevice());
		engine->setAudioDevice(new LifecycleDevice(engine, temporaryCounts, 48000), false);
		bool fail = true;
		AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point != (quality ? DeviceTransitionPoint::RestoreAfterQuality : DeviceTransitionPoint::RestoreAfterRate)) { return; } if (fail) { if (nonstandard) { throw 19; } throw std::runtime_error("Native notification fixture"); } });
		QVERIFY(!engine->restoreAudioDevice()); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior));
		QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::NotificationException);
		QVERIFY(engine->hasStoredAudioDevice()); QVERIFY(engine->isRestoringAudioDevice()); QVERIFY(!prior->isRunning());
		QCOMPARE(temporaryCounts.destroyed, 1); QVERIFY(!engine->resetDspFailure());
		fail = false; const auto retry = StudioCapabilities::execute("audio.device.restore", {}); QVERIFY(retry.value("ok").toBool());
		QVERIFY(prior->isRunning()); QCOMPARE(priorCounts.starts, 2); QVERIFY(!engine->hasStoredAudioDevice());
		QVERIFY(!retry.value("result").toObject().value("failed").toBool());
	}

	void reportedNotificationFailuresLeaveQtBeforeRethrowing_data()
	{
		QTest::addColumn<bool>("quality"); QTest::addColumn<bool>("nonstandard");
		for (bool quality : {false, true}) for (bool nonstandard : {false, true})
		{ QTest::newRow(qPrintable(QString::number(quality) + QString::number(nonstandard))) << quality << nonstandard; }
	}
	void reportedNotificationFailuresLeaveQtBeforeRethrowing()
	{
		QFETCH(bool, quality); QFETCH(bool, nonstandard); const auto engine = Engine::audioEngine();
		const auto first = nonstandard ? std::make_exception_ptr(233) : std::make_exception_ptr(std::runtime_error("First native notification failure"));
		const auto second = std::make_exception_ptr(std::runtime_error("Later native notification failure"));
		int calls = 0; bool accepted = true;
		const auto receive = [&](const std::exception_ptr& failure) { ++calls; accepted = engine->reportDeviceNotificationFailure(failure) && accepted; };
		const auto one = quality ? connect(engine, &AudioEngine::qualitySettingsChanged, this, [&] { receive(first); }) : connect(engine, &AudioEngine::sampleRateChanged, this, [&] { receive(first); });
		const auto two = quality ? connect(engine, &AudioEngine::qualitySettingsChanged, this, [&] { receive(second); }) : connect(engine, &AudioEngine::sampleRateChanged, this, [&] { receive(second); });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(one); QObject::disconnect(two); });
		bool caught = false;
		try { AudioDeviceTransitionTestAccess::notify(engine, quality); }
		catch (const std::runtime_error& error) { QVERIFY(!nonstandard); QCOMPARE(QString::fromUtf8(error.what()), QString("First native notification failure")); caught = true; }
		catch (int value) { QVERIFY(nonstandard); QCOMPARE(value, 233); caught = true; }
		QVERIFY(caught); QVERIFY(accepted); QCOMPARE(calls, 2); QObject::disconnect(one); QObject::disconnect(two);
		AudioDeviceTransitionTestAccess::notify(engine, quality); QVERIFY(!engine->reportDeviceNotificationFailure(first));
	}
	void nestedNotificationFailuresStayInTheirOwnFrame_data()
	{
		QTest::addColumn<bool>("inner"); QTest::addColumn<bool>("outer");
		QTest::newRow("inner-handled") << true << false; QTest::newRow("outer-only") << false << true; QTest::newRow("both") << true << true;
	}
	void nestedNotificationFailuresStayInTheirOwnFrame()
	{
		QFETCH(bool, inner); QFETCH(bool, outer); const auto engine = Engine::audioEngine();
		const auto innerFailure = std::make_exception_ptr(std::runtime_error("Inner native notification failure"));
		const auto outerFailure = std::make_exception_ptr(std::runtime_error("Outer native notification failure"));
		bool innerCaught = false, accepted = true; int rate = 0, quality = 0;
		const auto one = connect(engine, &AudioEngine::sampleRateChanged, this, [&] { ++rate; if (inner) { accepted = engine->reportDeviceNotificationFailure(innerFailure) && accepted; } });
		const auto two = connect(engine, &AudioEngine::qualitySettingsChanged, this, [&] {
			++quality;
			try { AudioDeviceTransitionTestAccess::notify(engine, false); }
			catch (const std::runtime_error& error) { innerCaught = QString::fromUtf8(error.what()) == "Inner native notification failure"; }
			if (outer) { accepted = engine->reportDeviceNotificationFailure(outerFailure) && accepted; }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(one); QObject::disconnect(two); });
		bool outerCaught = false;
		try { AudioDeviceTransitionTestAccess::notify(engine, true); }
		catch (const std::runtime_error& error) { QCOMPARE(QString::fromUtf8(error.what()), QString("Outer native notification failure")); outerCaught = true; }
		QCOMPARE(innerCaught, inner); QCOMPARE(outerCaught, outer); QVERIFY(accepted); QCOMPARE(rate, 1); QCOMPARE(quality, 1);
		QVERIFY(!engine->reportDeviceNotificationFailure(outerFailure));
	}
	void outOfFrameAndForeignThreadReportsDoNotChangeNotificationState()
	{
		const auto engine = Engine::audioEngine(); const auto failure = std::make_exception_ptr(std::runtime_error("Rejected notification report"));
		QVERIFY(!engine->reportDeviceNotificationFailure(failure)); bool rejected = false;
		const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] {
			std::thread worker([&] { rejected = !engine->reportDeviceNotificationFailure(failure); }); worker.join();
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		AudioDeviceTransitionTestAccess::notify(engine, false); QVERIFY(rejected); QVERIFY(!engine->reportDeviceNotificationFailure({}));
	}
	void transitionProbeRemainsAliveAfterRemovingItsInstallation_data()
	{
		QTest::addColumn<int>("point"); QTest::addColumn<int>("failure");
		for (int point = 0; point < 4; ++point) for (int failure = 0; failure < 3; ++failure)
		{ QTest::newRow(qPrintable(QString::number(point) + QString::number(failure))) << point << failure; }
	}
	void transitionProbeRemainsAliveAfterRemovingItsInstallation()
	{
		QFETCH(int, point); QFETCH(int, failure); const auto engine = Engine::audioEngine(); Counts priorCounts, temporaryCounts;
		const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto prior = new LifecycleDevice(engine, priorCounts); engine->setAudioDevice(prior, false); QVERIFY(engine->storeAudioDevice());
		auto candidate = std::make_unique<LifecycleDevice>(engine, temporaryCounts, 48000);
		const auto requested = static_cast<DeviceTransitionPoint>(point); const bool restoration = point >= 2;
		if (restoration) { engine->setAudioDevice(candidate.get(), false); candidate.release(); }
		auto witness = std::make_shared<int>(1); const std::weak_ptr<int> observed(witness);
		std::unique_ptr<AudioDeviceTransitionProbe> transitionProbe; int calls = 0; bool survived = false;
		transitionProbe = std::make_unique<AudioDeviceTransitionProbe>(engine, [&, witness](DeviceTransitionPoint current) {
			if (current != requested) { return; }
			++calls; transitionProbe->reset(); survived = witness && !observed.expired();
			if (failure == 1) { throw std::runtime_error("Reset native transition probe"); }
			if (failure == 2) { throw 229; }
		});
		witness.reset(); bool restored = false, standard = false, nonstandard = false;
		try { if (restoration) { restored = engine->restoreAudioDevice(); } else { engine->setAudioDevice(candidate.get(), false); } }
		catch (const std::runtime_error&) { standard = true; }
		catch (int value) { nonstandard = value == 229; }
		if (candidate && engine->audioDev() == candidate.get()) { candidate.release(); }
		QCOMPARE(calls, 1); QVERIFY(survived); QVERIFY(observed.expired());
		if (restoration) { QCOMPARE(restored, failure == 0); QVERIFY(!standard && !nonstandard); }
		else { QCOMPARE(standard, failure == 1); QCOMPARE(nonstandard, failure == 2); }
		QVERIFY(engine->restoreAudioDevice()); QVERIFY(!engine->hasStoredAudioDevice()); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior));
		QCOMPARE(temporaryCounts.destroyed, 1);
	}

	void failedStopsRetainOwnershipBeforeAndAfterCandidatePublication_data()
	{
		QTest::addColumn<bool>("setup"); QTest::newRow("setup-stop") << true; QTest::newRow("restoration-stop") << false;
	}
	void failedStopsRetainOwnershipBeforeAndAfterCandidatePublication()
	{
		QFETCH(bool, setup); const auto engine = Engine::audioEngine(); Counts priorCounts, temporaryCounts;
		const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto prior = new LifecycleDevice(engine, priorCounts); engine->setAudioDevice(prior, true); QVERIFY(engine->storeAudioDevice());
		auto candidate = std::make_unique<LifecycleDevice>(engine, temporaryCounts, 48000);
		if (setup)
		{
			prior->throwStop = true; QVERIFY_EXCEPTION_THROWN(engine->setAudioDevice(candidate.get(), false), std::runtime_error);
			QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior)); QCOMPARE(temporaryCounts.destroyed, 0); prior->throwStop = false;
		}
		else
		{
			const auto temporary = candidate.release(); engine->setAudioDevice(temporary, false); temporary->throwStop = true;
			QVERIFY(!engine->restoreAudioDevice()); QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::StopException);
			QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(temporary)); QCOMPARE(temporaryCounts.destroyed, 0); temporary->throwStop = false;
		}
		QVERIFY(engine->restoreAudioDevice()); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior)); QVERIFY(prior->isRunning());
		QVERIFY(!engine->hasStoredAudioDevice());
		if (!setup) { QCOMPARE(temporaryCounts.destroyed, 1); }
	}

	void restartFailuresRetainTheReservationAndDoNotReplayCompletedNotifications_data()
	{
		QTest::addColumn<bool>("exception"); QTest::newRow("failure-status") << false; QTest::newRow("returned-exception") << true;
	}
	void restartFailuresRetainTheReservationAndDoNotReplayCompletedNotifications()
	{
		QFETCH(bool, exception); const auto engine = Engine::audioEngine(); Counts priorCounts, temporaryCounts;
		const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto prior = new LifecycleDevice(engine, priorCounts); engine->setAudioDevice(prior, true); QVERIFY(engine->storeAudioDevice());
		engine->setAudioDevice(new LifecycleDevice(engine, temporaryCounts, 48000), false);
		QSignalSpy rate(engine, &AudioEngine::sampleRateChanged); prior->throwStart = exception; prior->failStart = !exception;
		QVERIFY(!engine->restoreAudioDevice()); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(rate.count(), 1);
		QCOMPARE(engine->deviceRestoreFailure(), exception ? AudioEngine::DeviceRestoreFailure::RestartException : AudioEngine::DeviceRestoreFailure::RestartFailed);
		prior->throwStart = false; prior->failStart = false; QVERIFY(engine->restoreAudioDevice());
		QCOMPARE(rate.count(), 1); QCOMPARE(priorCounts.starts, 3); QVERIFY(prior->isRunning()); QCOMPARE(temporaryCounts.destroyed, 1);
	}

	void restorationRejectsNonownerAndExecutingRenderButPermitsFinishedExecution()
	{
		const auto engine = Engine::audioEngine(); Counts priorCounts, temporaryCounts; const auto destroy = qScopeGuard([&] { destroyEngine(); });
		engine->setAudioDevice(new LifecycleDevice(engine, priorCounts), false); QVERIFY(engine->storeAudioDevice());
		engine->setAudioDevice(new LifecycleDevice(engine, temporaryCounts), false);
		const auto context = RemotePlugin::beginRenderContext(); QVERIFY(context); const auto retire = qScopeGuard([&] { RemotePlugin::endRenderContext(context); });
		QVERIFY(RemotePlugin::hasRunningRenderExecution()); QVERIFY(!engine->restoreAudioDevice()); QCOMPARE(temporaryCounts.destroyed, 0);
		RemotePlugin::finishRenderExecution(context); QVERIFY(!RemotePlugin::hasRunningRenderExecution()); QVERIFY(RemotePlugin::hasActiveRenderContext());
		QVERIFY(!StudioCapabilities::execute("audio.device.restore", {}).value("ok").toBool()); QCOMPARE(temporaryCounts.destroyed, 0);
		bool restored = true; std::thread worker([&] { restored = engine->restoreAudioDevice(); }); worker.join(); QVERIFY(!restored);
		QVERIFY(engine->hasStoredAudioDevice()); QVERIFY(engine->restoreAudioDevice()); QCOMPARE(temporaryCounts.destroyed, 1);
	}

	void studioExportDoesNotPublishWhenBackendRestorationFails_data()
	{
		QTest::addColumn<bool>("reportedDsp");
		QTest::newRow("notification-exception") << false; QTest::newRow("notification-dsp-report") << true;
	}
	void studioExportDoesNotPublishWhenBackendRestorationFails()
	{
		QFETCH(bool, reportedDsp);
		const auto engine = Engine::audioEngine(); Counts counts; const auto destroy = qScopeGuard([&] { destroyEngine(); });
		auto prior = new LifecycleDevice(engine, counts); engine->setAudioDevice(prior, true);
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		const auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks().at(0)->getClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(256, SampleFrame(.25f)), 44100));
		QVERIFY(StudioCapabilities::execute("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 192}}).value("ok").toBool());
		bool fail = true; AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::RestoreAfterRate) { return; }
			if (engine->audioDev() == prior && fail)
			{
				if (reportedDsp) { engine->reportDspFailure(AudioEngine::DspFailure::NonfiniteOutput); }
				else { throw std::runtime_error("Native restore notification fixture"); }
			}
		});
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto target = directory.filePath("preserved.wav");
		QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("Prior export"), qint64(12)); file.close();
		const auto result = StudioCapabilities::execute("audio.render", {{"path", target}, {"timeoutMs", 5000}});
		QVERIFY(!result.value("ok").toBool()); QVERIFY(result.value("error").toString().contains(reportedDsp ? "DSP" : "restoration"));
		QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("Prior export")); file.close();
		QCOMPARE(engine->hasStoredAudioDevice(), !reportedDsp); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior));
		QVERIFY(!Engine::getSong()->isExporting()); QCOMPARE(prior->isRunning(), reportedDsp);
		QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 1);
		fail = false; QVERIFY(StudioCapabilities::execute("audio.device.restore", {}).value("ok").toBool());
		if (reportedDsp) { QVERIFY(engine->resetDspFailure()); }
		QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(prior->isRunning());
	}
};
} // namespace lmms
QTEST_GUILESS_MAIN(lmms::AudioDeviceTransitionTest)
#include "AudioDeviceTransitionTest.moc"

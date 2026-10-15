/* Native renderer/manager callback lifetime. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AudioDeviceTransitionTestUtils.h"
#include "AudioFileDevice.h"
#include "AudioFileWave.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "RemotePlugin.h"
#include "RenderManager.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAudio.h"
#include "StudioCapabilities.h"
#include "TrackRenderScope.h"
using namespace lmms;
namespace lmms
{
class RenderManagerLifecycleTestAccess
{
public:
	static QPointer<ProjectRenderer> renderer(RenderManager* manager) { return manager->m_activeRenderer.get(); }
	static QString staging(RenderManager* manager) { return manager->m_activeOutputPath; }
	static bool finishing(RenderManager* manager) { return manager->m_finishing; }
};
}
namespace
{
OutputSettings settings() { return OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit); }
void arrange()
{
	Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100); auto track = new SampleTrack(Engine::getSong()); track->setName("Source"); auto clip = static_cast<SampleClip*>(track->createClip(TimePos())); clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(35280, SampleFrame(.125f)), 44100)); clip->changeLength(TimePos(192));
}
double decode(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return -1.; } SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close); if (!audio || info.channels != 2 || info.samplerate != 44100 || info.frames < 10000 || sf_seek(audio.get(), 1000, SEEK_SET) != 1000) { return -1.; } std::vector<float> pcm(8192); if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; } double sum = 0; for (auto value : pcm) { if (!std::isfinite(value)) { return -1.; } sum += value; } return sum / pcm.size();
}
bool run(const QPointer<RenderManager>& manager, bool tracks = false)
{
	QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool returned = false; QObject::connect(manager.data(), &RenderManager::finished, &loop, [&] { returned = true; loop.quit(); }); QObject::connect(manager.data(), &QObject::destroyed, &loop, [&] { returned = true; loop.quit(); }); QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(30000); if (tracks) { manager->renderTracks(); } else { manager->renderProject(); } if (!returned) { loop.exec(); } if (!returned && manager) { manager->abortProcessing(); } return returned;
}
}
class RenderManagerLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released()
	{
		QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!RemotePlugin::hasRunningRenderExecution()); QVERIFY(!TrackRenderScope::hasActiveSelection()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); std::atomic_bool acquired{false}; std::thread observer([&] { const auto ownership = Engine::audioEngine()->requestChangesGuard(); acquired = true; }); observer.join(); QVERIFY(acquired);
	}
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); arrange(); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); released(); }
	void rendererSurvivesManagerRetirementInsideDeviceSetup_data()
	{ QTest::addColumn<bool>("quality"); QTest::addColumn<bool>("tracks"); QTest::newRow("quality-project") << true << false; QTest::newRow("rate-project") << false << false; QTest::newRow("quality-stem") << true << true; QTest::newRow("rate-stem") << false << true; }
	void rendererSurvivesManagerRetirementInsideDeviceSetup()
	{
		QFETCH(bool, quality); QFETCH(bool, tracks); QTemporaryDir output; const auto path = output.filePath("cancelled.wav"); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, tracks ? output.path() : path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QPointer<ProjectRenderer> observed; bool fired = false, survived = false; int destroyed = 0; auto engine = Engine::audioEngine();
		const auto retire = [&] { if (!fired && dynamic_cast<AudioFileDevice*>(engine->audioDev())) { fired = true; QVERIFY(manager->isStarting()); observed = RenderManagerLifecycleTestAccess::renderer(manager); QVERIFY(observed); connect(observed.data(), &QObject::destroyed, this, [&] { ++destroyed; }); delete manager.data(); survived = bool(observed); } }; const auto connection = quality ? connect(engine, &AudioEngine::qualitySettingsChanged, this, retire, Qt::DirectConnection) : connect(engine, &AudioEngine::sampleRateChanged, this, retire, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager, tracks)); QVERIFY(fired && survived); QVERIFY(!manager); QVERIFY(!observed); QCOMPARE(destroyed, 1); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void setupCanThrowAfterManagerRetirement_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void setupCanThrowAfterManagerRetirement()
	{
		QFETCH(bool, nonstandard); QTemporaryDir output; const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.filePath("failed.wav"))); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QPointer<ProjectRenderer> observed; bool fired = false, survived = false; AudioDeviceTransitionProbe transitionProbe(Engine::audioEngine(), [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; } if (!fired && dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { fired = true; observed = RenderManagerLifecycleTestAccess::renderer(manager); delete manager.data(); survived = bool(observed); if (nonstandard) { throw 173; } throw std::runtime_error("Retired native manager setup"); } }); QVERIFY(run(manager)); QVERIFY(fired && survived); QVERIFY(!manager); QVERIFY(!observed); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void restorationCanRetireManagerBeforeAtomicPublication_data()
	{ QTest::addColumn<bool>("quality"); QTest::newRow("quality") << true; QTest::newRow("rate") << false; }
	void restorationCanRetireManagerBeforeAtomicPublication()
	{
		QFETCH(bool, quality); QTemporaryDir output; const auto path = output.filePath("completed.wav"); auto engine = Engine::audioEngine(); auto original = engine->audioDev(); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); QPointer<ProjectRenderer> observed; bool fired = false, survived = false, decoded = false;
		const auto retire = [&] { if (!fired && engine->audioDev() == original) { fired = true; QVERIFY(RenderManagerLifecycleTestAccess::finishing(manager)); observed = RenderManagerLifecycleTestAccess::renderer(manager); decoded = std::abs(decode(RenderManagerLifecycleTestAccess::staging(manager)) - .125) < .01; delete manager.data(); survived = bool(observed); } }; const auto connection = quality ? connect(engine, &AudioEngine::qualitySettingsChanged, this, retire, Qt::DirectConnection) : connect(engine, &AudioEngine::sampleRateChanged, this, retire, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager)); QVERIFY(fired && survived); QVERIFY(!manager); QVERIFY(!observed); QCOMPARE(finished.count(), 0); QVERIFY(decoded); QVERIFY(!QFile::exists(path)); released();
	}
	void abortDuringRestorationStopsCompletionWithoutReentry_data()
	{ QTest::addColumn<bool>("quality"); QTest::newRow("quality") << true; QTest::newRow("rate") << false; }
	void abortDuringRestorationStopsCompletionWithoutReentry()
	{
		QFETCH(bool, quality); QTemporaryDir output; const auto path = output.filePath("cancelled.wav"); auto engine = Engine::audioEngine(); auto original = engine->audioDev(); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); bool fired = false; const auto abort = [&] { if (!fired && engine->audioDev() == original) { fired = true; manager->abortProcessing(); manager->renderProject(); manager->renderTracks(); QCOMPARE(finished.count(), 0); } }; const auto connection = quality ? connect(engine, &AudioEngine::qualitySettingsChanged, this, abort, Qt::DirectConnection) : connect(engine, &AudioEngine::sampleRateChanged, this, abort, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager)); QVERIFY(fired); QCOMPARE(finished.count(), 1); QCOMPARE(manager->errorString(), QString("Audio export was cancelled.")); QVERIFY(!QFile::exists(path)); released();
	}
	void rendererDestructionMayRetireItsManagerAfterFinalization()
	{
		QTemporaryDir output; const auto path = output.filePath("completed.wav"); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); bool armed = false, retired = false; const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] { if (!armed && dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { armed = true; auto renderer = RenderManagerLifecycleTestAccess::renderer(manager); connect(renderer.data(), &QObject::destroyed, this, [&] { retired = true; delete manager.data(); }); } }, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager)); QVERIFY(armed && retired); QVERIFY(!manager); QCOMPARE(finished.count(), 0); QVERIFY(std::abs(decode(path) - .125) < .01); released();
	}
	void rendererDestructionCanStillCancelCompletion()
	{
		QTemporaryDir output; const auto path = output.filePath("cancelled.wav"); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); bool armed = false, cancelled = false; const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] { if (!armed && dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { armed = true; auto renderer = RenderManagerLifecycleTestAccess::renderer(manager); connect(renderer.data(), &QObject::destroyed, this, [&] { cancelled = true; manager->abortProcessing(); }); } }, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager)); QVERIFY(armed && cancelled); QCOMPARE(finished.count(), 1); QCOMPARE(manager->errorString(), QString("Audio export was cancelled.")); QVERIFY(std::abs(decode(path) - .125) < .01); released();
	}
	void transientRestorationExceptionAfterRetirementCanRecover_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void transientRestorationExceptionAfterRetirementCanRecover()
	{
		QFETCH(bool, nonstandard); QTemporaryDir output; const auto path = output.filePath("completed.wav"); auto engine = Engine::audioEngine(); auto original = engine->audioDev(); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); bool fired = false, decoded = false; AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::RestoreAfterRate) { return; } if (!fired && engine->audioDev() == original) { fired = true; decoded = std::abs(decode(RenderManagerLifecycleTestAccess::staging(manager)) - .125) < .01; delete manager.data(); if (nonstandard) { throw 181; } throw std::runtime_error("Retired manager restoration"); } }); QVERIFY(run(manager)); QVERIFY(fired); QVERIFY(!manager); QVERIFY(decoded); QVERIFY(!QFile::exists(path)); released();
	}
	void persistentRestorationFailureKeepsTheReservationForExplicitRetry()
	{
		QTemporaryDir output; const auto path = output.filePath("completed.wav"); auto engine = Engine::audioEngine(); auto original = engine->audioDev(); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); bool retired = false; int failures = 0; AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::RestoreAfterRate) { return; } if (engine->audioDev() == original) { ++failures; if (!retired) { retired = true; delete manager.data(); } throw std::runtime_error("Persistent restore fixture"); } }); QVERIFY(run(manager)); QVERIFY(retired); QVERIFY(!manager); QVERIFY(failures >= 2); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::NotificationException); QVERIFY(!RemotePlugin::hasActiveRenderContext()); transitionProbe.reset(); QVERIFY(StudioCapabilities::execute("audio.device.restore", {}).value("ok").toBool()); const auto retry = output.filePath("retry.wav"); QVERIFY(StudioAudio::render({{"path", retry}}).contains("wallTimeMs")); QVERIFY(std::abs(decode(retry) - .125) < .01); released();
	}
	void completionCanSynchronouslyReuseTheSameManager()
	{
		QTemporaryDir output; const auto path = output.filePath("reused.wav"); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QEventLoop loop; QTimer timer; timer.setSingleShot(true); int completed = 0; bool firstValid = false; const auto connection = connect(manager.data(), &RenderManager::finished, &loop, [&] { if (++completed == 1) { firstValid = !RenderManagerLifecycleTestAccess::finishing(manager) && manager->errorString().isEmpty() && std::abs(decode(path) - .125) < .01; manager->renderProject(); } else { loop.quit(); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(30000); manager->renderProject(); if (completed < 2) { loop.exec(); } if (completed < 2) { manager->abortProcessing(); } QCOMPARE(completed, 2); QVERIFY(firstValid); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(path) - .125) < .01); released();
	}
	void restorationCannotStartASecondBatchBeforeCompletion()
	{
		QTemporaryDir output; const auto path = output.filePath("completed.wav"); auto engine = Engine::audioEngine(); auto original = engine->audioDev(); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); bool fired = false; const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] { if (!fired && engine->audioDev() == original) { fired = true; auto before = RenderManagerLifecycleTestAccess::renderer(manager); manager->renderProject(); manager->renderTracks(); QCOMPARE(RenderManagerLifecycleTestAccess::renderer(manager), before); QCOMPARE(finished.count(), 0); } }, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager)); QVERIFY(fired); QCOMPARE(finished.count(), 1); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(path) - .125) < .01); released();
	}
	void aNewerReservationAndOutputCannotBeTakenByOrphanCleanup_data()
	{ QTest::addColumn<bool>("reuseStage"); QTest::newRow("destination") << false; QTest::newRow("old-staging-path") << true; }
	void aNewerReservationAndOutputCannotBeTakenByOrphanCleanup()
	{
		QFETCH(bool, reuseStage); QTemporaryDir output; const auto path = output.filePath("replaced.wav"); auto engine = Engine::audioEngine(); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QString replacementPath; bool armed = false, replaced = false; AudioFileDevice* replacement = nullptr; std::uint64_t newer = 0; const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] { if (!armed && dynamic_cast<AudioFileDevice*>(engine->audioDev())) { armed = true; replacementPath = reuseStage ? RenderManagerLifecycleTestAccess::staging(manager) : path; auto renderer = RenderManagerLifecycleTestAccess::renderer(manager); connect(renderer.data(), &QObject::destroyed, this, [&] { const auto older = engine->audioDeviceReservationGeneration(); QVERIFY(engine->restoreAudioDevice()); QVERIFY(engine->storeAudioDevice()); newer = engine->audioDeviceReservationGeneration(); QVERIFY(newer > older); bool good = false; std::unique_ptr<AudioFileDevice> next(AudioFileWave::getInst(replacementPath, settings(), DEFAULT_CHANNELS, engine, good)); QVERIFY(good); replacement = next.get(); engine->setAudioDevice(replacement, false); next.release(); std::vector<SampleFrame> pcm(35280, SampleFrame(.375f)); replacement->writeBuffer(pcm.data(), pcm.size()); QVERIFY(replacement->finalizeOutput()); replaced = true; }); delete manager.data(); } }, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager)); QVERIFY(armed && replaced); QVERIFY(!manager); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(replacement)); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(engine->audioDeviceReservationGeneration(), newer); QVERIFY(QFile::exists(replacementPath)); QVERIFY(engine->restoreAudioDevice()); QVERIFY(std::abs(decode(replacementPath) - .375) < .01); released();
	}
	void reservationGenerationsAdvanceOnlyForAcceptedStores()
	{
		auto engine = Engine::audioEngine(); const auto before = engine->audioDeviceReservationGeneration(); QVERIFY(engine->storeAudioDevice()); const auto first = engine->audioDeviceReservationGeneration(); QVERIFY(first > before); QVERIFY(!engine->storeAudioDevice()); QCOMPARE(engine->audioDeviceReservationGeneration(), first); QVERIFY(engine->restoreAudioDevice()); QCOMPARE(engine->audioDeviceReservationGeneration(), first); QVERIFY(engine->storeAudioDevice()); QVERIFY(engine->audioDeviceReservationGeneration() > first); QVERIFY(engine->restoreAudioDevice()); released();
	}
	void progressDeliveryCanRetireTheManagerAndRemoveUnfinalizedOutput()
	{
		QTemporaryDir output; const auto path = output.filePath("cancelled.wav"); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); bool fired = false; const auto connection = connect(manager.data(), &RenderManager::progressChanged, this, [&](int progress) { if (!fired && progress > 0) { fired = true; delete manager.data(); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager)); QVERIFY(fired); QVERIFY(!manager); QVERIFY(!QFile::exists(path)); released();
	}
	void failedAdmissionClosesAnUninstalledFileBeforeRemoval()
	{
		QTemporaryDir output; const auto path = output.filePath("failed.wav"); auto engine = Engine::audioEngine(); const auto reset = qScopeGuard([&] { engine->resetDspFailure(); }); engine->reportDspFailure(AudioEngine::DspFailure::NonfiniteOutput); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); QVERIFY(run(manager)); QCOMPARE(finished.count(), 1); QVERIFY(manager->errorString().contains("DSP")); QVERIFY(!RenderManagerLifecycleTestAccess::renderer(manager)); QVERIFY(!QFile::exists(path)); released(); QVERIFY(engine->resetDspFailure());
	}
	void aQueuedCompletionFromAnOlderRendererCannotFinishItsReplacement()
	{
		QTemporaryDir output; const auto path = output.filePath("replacement.wav"); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QEventLoop loop; QTimer timer; timer.setSingleShot(true); int completed = 0; bool failedSetup = false, staleProgress = false;
		const auto progress = connect(manager.data(), &RenderManager::progressChanged, &loop, [&](int value) { if (value == 1000) { staleProgress = true; } }); const auto disconnectProgress = qScopeGuard([&] { QObject::disconnect(progress); });
		AudioDeviceTransitionProbe transitionProbe(Engine::audioEngine(), [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; } if (!failedSetup && dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { failedSetup = true; auto renderer = RenderManagerLifecycleTestAccess::renderer(manager); QVERIFY(QMetaObject::invokeMethod(renderer.data(), "progressChanged", Qt::DirectConnection, Q_ARG(int, 1000))); QVERIFY(QMetaObject::invokeMethod(renderer.data(), "finished", Qt::DirectConnection)); throw std::runtime_error("Queued old renderer fixture"); } });
		const auto completion = connect(manager.data(), &RenderManager::finished, &loop, [&] { if (++completed == 1) { QVERIFY(!manager->errorString().isEmpty()); manager->renderProject(); } else { loop.quit(); } }); const auto disconnectCompletion = qScopeGuard([&] { QObject::disconnect(completion); }); connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(30000); manager->renderProject(); if (completed < 2) { loop.exec(); } if (completed < 2) { manager->abortProcessing(); } QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QCOMPARE(completed, 2); QVERIFY(failedSetup); QVERIFY(!staleProgress); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(path) - .125) < .01); released();
	}
};
QTEST_GUILESS_MAIN(RenderManagerLifecycleTest)
#include "RenderManagerLifecycleTest.moc"

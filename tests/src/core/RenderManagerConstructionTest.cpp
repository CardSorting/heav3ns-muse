/* Native renderer creation boundary failures. SPDX-License-Identifier: GPL-2.0-or-later */
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
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "RemotePlugin.h"
#include "RenderManager.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "TrackRenderScope.h"
using namespace lmms;
namespace lmms
{
class RenderManagerConstructionTestAccess
{
public:
	static bool clean(RenderManager* manager) { return !manager->m_starting && !manager->m_finishing && !manager->m_deviceStored && !manager->m_activeRenderer && !manager->m_outputTransaction && !manager->m_trackSelection && manager->m_tracksToRender.empty() && manager->m_exportTracks.empty(); }
	static QString failureMessage(RenderManager* manager) { return manager->m_rendererFailureMessage; }
};
}
namespace
{
enum class Mode { BadAllocation, Standard, Nonstandard, Null, Healthy, AfterStandard, AfterNonstandard };
struct FactoryState { Mode mode = Mode::Healthy; int calls = 0, created = 0, destroyed = 0; QPointer<ProjectRenderer> observed; std::function<void()> afterCreate; };
class FaultManager final : public RenderManager
{
public:
	FaultManager(const QString& path, const std::shared_ptr<FactoryState>& state) : RenderManager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, path), m_state(state) { }
protected:
	std::shared_ptr<ProjectRenderer> createRenderer(const OutputSettings& settings, ProjectRenderer::ExportFileFormat format, const QString& path) override
	{
		const auto state = m_state; ++state->calls; const auto mode = state->mode;
		if (mode == Mode::BadAllocation) { throw std::bad_alloc(); }
		if (mode == Mode::Standard) { throw std::runtime_error("Native creation boundary fixture"); }
		if (mode == Mode::Nonstandard) { throw 191; }
		if (mode == Mode::Null) { return {}; }
		auto renderer = RenderManager::createRenderer(settings, format, path); state->observed = renderer.get(); ++state->created; QObject::connect(renderer.get(), &QObject::destroyed, [state] { ++state->destroyed; }); const auto callback = state->afterCreate; if (callback) { callback(); }
		if (mode == Mode::AfterStandard) { throw std::runtime_error("Failure after native file creation"); }
		if (mode == Mode::AfterNonstandard) { throw 193; }
		return renderer;
	}
private:
	std::shared_ptr<FactoryState> m_state;
};
void arrange(bool second = false)
{
	Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100); for (int index = 0; index < (second ? 2 : 1); ++index) { auto track = new SampleTrack(Engine::getSong()); track->setName(index ? "Other" : "Source"); auto clip = static_cast<SampleClip*>(track->createClip(TimePos())); clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(35280, SampleFrame(.125f * (index + 1))), 44100)); clip->changeLength(TimePos(192)); }
}
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
double decode(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return -1.; } SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close); if (!audio || info.channels != 2 || info.samplerate != 44100 || info.frames < 10000 || sf_seek(audio.get(), 1000, SEEK_SET) != 1000) { return -1.; } std::vector<float> pcm(8192); if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; } double sum = 0; for (auto value : pcm) { if (!std::isfinite(value)) { return -1.; } sum += value; } return sum / pcm.size();
}
bool run(const QPointer<FaultManager>& manager, bool tracks = false)
{
	QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool done = false; QObject::connect(manager.data(), &RenderManager::finished, &loop, [&] { done = true; loop.quit(); }); QObject::connect(manager.data(), &QObject::destroyed, &loop, [&] { done = true; loop.quit(); }); QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(30000); if (tracks) { manager->renderTracks(); } else { manager->renderProject(); } if (!done) { loop.exec(); } if (!done && manager) { manager->abortProcessing(); } return done;
}
}
class RenderManagerConstructionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released() { QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!RemotePlugin::hasRunningRenderExecution()); QVERIFY(!TrackRenderScope::hasActiveSelection()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); std::atomic_bool acquired{false}; std::thread observer([&] { const auto ownership = Engine::audioEngine()->requestChangesGuard(); acquired = true; }); observer.join(); QVERIFY(acquired); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); released(); }
	void precreationFailuresPreserveExistingOutputAndFinishOnce_data()
	{ QTest::addColumn<int>("failure"); QTest::addColumn<bool>("tracks"); QTest::newRow("allocation-project") << 0 << false; QTest::newRow("standard-project") << 1 << false; QTest::newRow("nonstandard-project") << 2 << false; QTest::newRow("null-project") << 3 << false; QTest::newRow("allocation-stem") << 0 << true; QTest::newRow("standard-stem") << 1 << true; QTest::newRow("nonstandard-stem") << 2 << true; QTest::newRow("null-stem") << 3 << true; }
	void precreationFailuresPreserveExistingOutputAndFinishOnce()
	{
		QFETCH(int, failure); QFETCH(bool, tracks); arrange(); QTemporaryDir output; const auto path = output.filePath(tracks ? "1_Source.wav" : "preserved.wav"); QVERIFY(write(path, "previous audio")); const auto state = std::make_shared<FactoryState>(); state->mode = static_cast<Mode>(failure); const QPointer<FaultManager> manager(new FaultManager(tracks ? output.path() : path, state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); const auto original = Engine::audioEngine()->audioDev(); QVERIFY(run(manager, tracks)); QCOMPARE(finished.count(), 1); QCOMPARE(state->calls, 1); QCOMPARE(state->created, 0); QCOMPARE(manager->errorString(), RenderManagerConstructionTestAccess::failureMessage(manager)); QCOMPARE(read(path), QByteArray("previous audio")); QCOMPARE(Engine::audioEngine()->audioDev(), original); QVERIFY(RenderManagerConstructionTestAccess::clean(manager)); released();
	}
	void failuresAfterNativeCreationReleaseTheActualRendererAndAllowRetry_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void failuresAfterNativeCreationReleaseTheActualRendererAndAllowRetry()
	{
		QFETCH(bool, nonstandard); arrange(); QTemporaryDir output; const auto path = output.filePath("retry.wav"); const auto state = std::make_shared<FactoryState>(); state->mode = nonstandard ? Mode::AfterNonstandard : Mode::AfterStandard; const QPointer<FaultManager> manager(new FaultManager(path, state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); QVERIFY(run(manager)); QCOMPARE(finished.count(), 1); QCOMPARE(state->created, 1); QCOMPARE(state->destroyed, 1); QVERIFY(!state->observed); QCOMPARE(manager->errorString(), RenderManagerConstructionTestAccess::failureMessage(manager)); QVERIFY(RenderManagerConstructionTestAccess::clean(manager)); released(); state->mode = Mode::Healthy; QVERIFY(run(manager)); QCOMPARE(finished.count(), 2); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(path) - .125) < .01); QVERIFY(!state->observed); QCOMPARE(state->created, state->destroyed); released();
	}
	void sameManagerCanRetryEveryPrecreationFailure_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("allocation") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; QTest::newRow("null") << 3; }
	void sameManagerCanRetryEveryPrecreationFailure()
	{
		QFETCH(int, failure); arrange(); QTemporaryDir output; const auto path = output.filePath("retry.wav"); const auto state = std::make_shared<FactoryState>(); state->mode = static_cast<Mode>(failure); const QPointer<FaultManager> manager(new FaultManager(path, state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); QVERIFY(run(manager)); QCOMPARE(finished.count(), 1); QVERIFY(RenderManagerConstructionTestAccess::clean(manager)); released(); state->mode = Mode::Healthy; QVERIFY(run(manager)); QCOMPARE(state->calls, 2); QCOMPARE(finished.count(), 2); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(path) - .125) < .01); released();
	}
	void aNativeFactoryCanReturnOrThrowAfterManagerRetirement_data()
	{ QTest::addColumn<bool>("throwing"); QTest::newRow("return-native") << false; QTest::newRow("throw-after-native") << true; }
	void aNativeFactoryCanReturnOrThrowAfterManagerRetirement()
	{
		QFETCH(bool, throwing); arrange(); QTemporaryDir output; const auto path = output.filePath("retired.wav"); const auto state = std::make_shared<FactoryState>(); state->mode = throwing ? Mode::AfterStandard : Mode::Healthy; const QPointer<FaultManager> manager(new FaultManager(path, state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); bool survived = false; state->afterCreate = [&] { delete manager.data(); survived = bool(state->observed); }; const auto disarm = qScopeGuard([&] { state->afterCreate = {}; }); QVERIFY(run(manager)); QVERIFY(survived); QVERIFY(!manager); QCOMPARE(finished.count(), 0); QVERIFY(!state->observed); QCOMPARE(state->created, 1); QCOMPARE(state->destroyed, 1); if (!throwing) { QVERIFY(!QFile::exists(path)); } released();
	}
	void cancellationBeforeFactoryReturnDoesNotStartTheRenderer()
	{
		arrange(); QTemporaryDir output; const auto path = output.filePath("cancelled.wav"); const auto state = std::make_shared<FactoryState>(); const QPointer<FaultManager> manager(new FaultManager(path, state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); bool cancelled = false; state->afterCreate = [&] { cancelled = true; QVERIFY(!state->observed->isRunning()); manager->abortProcessing(); }; const auto disarm = qScopeGuard([&] { state->afterCreate = {}; }); QVERIFY(run(manager)); QVERIFY(cancelled); QCOMPARE(finished.count(), 1); QCOMPARE(manager->errorString(), QString("Audio export was cancelled.")); QVERIFY(!state->observed); QVERIFY(!QFile::exists(path)); QVERIFY(RenderManagerConstructionTestAccess::clean(manager)); released();
	}
	void nestedFactoryStartsCannotReplaceTheCurrentJob()
	{
		arrange(); QTemporaryDir output; const auto path = output.filePath("completed.wav"); const auto state = std::make_shared<FactoryState>(); const QPointer<FaultManager> manager(new FaultManager(path, state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); bool nested = false; state->afterCreate = [&] { if (!nested) { nested = true; QVERIFY(manager->isStarting()); manager->renderProject(); manager->renderTracks(); QCOMPARE(finished.count(), 0); } }; const auto disarm = qScopeGuard([&] { state->afterCreate = {}; }); QVERIFY(run(manager)); QVERIFY(nested); QCOMPARE(state->calls, 1); QCOMPARE(finished.count(), 1); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(path) - .125) < .01); released();
	}
	void aFactoryCannotTransferANewerAudioReservationToItsOldManager()
	{
		arrange(); QTemporaryDir output; const auto state = std::make_shared<FactoryState>(); const QPointer<FaultManager> manager(new FaultManager(output.filePath("superseded.wav"), state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); auto engine = Engine::audioEngine(); std::uint64_t newer = 0; state->afterCreate = [&] { const auto old = engine->audioDeviceReservationGeneration(); QVERIFY(engine->restoreAudioDevice()); QVERIFY(engine->storeAudioDevice()); newer = engine->audioDeviceReservationGeneration(); QVERIFY(newer > old); }; const auto disarm = qScopeGuard([&] { state->afterCreate = {}; }); QVERIFY(run(manager)); QCOMPARE(finished.count(), 1); QVERIFY(!manager->errorString().isEmpty()); QVERIFY(!state->observed); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(engine->audioDeviceReservationGeneration(), newer); delete manager.data(); QVERIFY(engine->hasStoredAudioDevice()); QVERIFY(engine->restoreAudioDevice()); released();
	}
	void aBulkCreationFailureStopsTheQueueBeforeExplicitRetry()
	{
		arrange(true); QTemporaryDir output; const auto state = std::make_shared<FactoryState>(); state->mode = Mode::Standard; const QPointer<FaultManager> manager(new FaultManager(output.path(), state)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QSignalSpy finished(manager.data(), &RenderManager::finished); QVERIFY(run(manager, true)); QCOMPARE(state->calls, 1); QCOMPARE(finished.count(), 1); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); QVERIFY(RenderManagerConstructionTestAccess::clean(manager)); released(); state->mode = Mode::Healthy; QVERIFY(run(manager, true)); QCOMPARE(state->calls, 3); QCOMPARE(finished.count(), 2); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(output.filePath("1_Source.wav")) - .125) < .01); QVERIFY(std::abs(decode(output.filePath("2_Other.wav")) - .25) < .01); released();
	}
};
QTEST_GUILESS_MAIN(RenderManagerConstructionTest)
#include "RenderManagerConstructionTest.moc"

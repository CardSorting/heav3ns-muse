/* Native bulk export preparation. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
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
class RenderManagerPreparationTestAccess
{
public:
	static bool clean(RenderManager* manager) { return !manager->m_deviceStored && !manager->m_starting && !manager->m_activeRenderer && !manager->m_outputTransaction && !manager->m_trackSelection && manager->m_exportTracks.empty() && manager->m_tracksToRender.empty(); }
	static bool contains(RenderManager* manager, Track* track) { return std::find(manager->m_exportTracks.begin(), manager->m_exportTracks.end(), track) != manager->m_exportTracks.end(); }
	static std::size_t count(RenderManager* manager) { return manager->m_exportTracks.size(); }
	static bool flagsClear() { for (auto owner : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())}) { for (auto track : owner->tracks()) { if (track->m_renderSelected.load() || track->m_renderExcluded.load()) { return false; } } } return !TrackRenderScope::hasActiveSelection(); }
};
}
namespace
{
struct NameState { QString text; std::function<void()> callback; };
class NamedSampleTrack final : public SampleTrack
{
public:
	NamedSampleTrack(TrackContainer* owner, const std::shared_ptr<NameState>& state) : SampleTrack(owner), m_state(state) { }
	const QString& name() const override { const auto state = m_state; const auto callback = state->callback; if (callback) { callback(); } return state->text; }
private:
	std::shared_ptr<NameState> m_state;
};
class ReadController final : public Controller
{
public:
	ReadController() : Controller(ControllerType::Dummy, nullptr, "Bulk eligibility fixture") { }
	std::function<void()> callback;
	float currentValue(int) override { const auto work = callback; if (work) { work(); } return 0.; }
};
struct Scene { std::shared_ptr<NameState> leftName = std::make_shared<NameState>(NameState{"Left:/", {}}), rightName = std::make_shared<NameState>(NameState{"Right?*", {}}); QPointer<NamedSampleTrack> left, right; };
Scene arrange()
{
	Scene scene; Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100); scene.left = new NamedSampleTrack(Engine::getSong(), scene.leftName); scene.right = new NamedSampleTrack(Engine::getSong(), scene.rightName); int index = 0;
	for (const auto track : {scene.left.data(), scene.right.data()}) { auto clip = static_cast<SampleClip*>(track->createClip(TimePos())); clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(35280, SampleFrame(.125f * (++index))), 44100)); clip->changeLength(TimePos(192)); }
	return scene;
}
bool render(RenderManager* manager)
{
	const QPointer<RenderManager> owner(manager); QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool finished = false; QObject::connect(manager, &RenderManager::finished, &loop, [&] { finished = true; loop.quit(); }); QObject::connect(manager, &QObject::destroyed, &loop, &QEventLoop::quit); QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(30000); manager->renderTracks(); if (owner && !finished) { loop.exec(); } if (owner && !finished) { owner->abortProcessing(); } return finished;
}
double decode(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return -1.; } SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close); if (!audio || info.channels != 2 || info.samplerate != 44100 || info.frames < 10000 || sf_seek(audio.get(), 1000, SEEK_SET) != 1000) { return -1.; } std::vector<float> pcm(8192); if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; } double sum = 0; for (auto value : pcm) { if (!std::isfinite(value)) { return -1.; } sum += value; } return sum / pcm.size();
}
OutputSettings settings() { return OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit); }
}
class RenderManagerPreparationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released() { QVERIFY(RenderManagerPreparationTestAccess::flagsClear()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QVERIFY(!RemotePlugin::hasActiveRenderContext()); std::atomic_bool acquired{false}; std::thread observer([&] { const auto ownership = Engine::audioEngine()->requestChangesGuard(); acquired = true; }); observer.join(); QVERIFY(acquired); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setProjectFileName(""); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); released(); }
	void nativeBulkOrderAndSanitizedNamesStillRenderSampleStems()
	{
		const auto scene = arrange(); auto muted = new SampleTrack(Engine::getSong()); muted->setMuted(true); new AutomationTrack(Engine::getSong()); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); QVERIFY(render(&manager)); QCOMPARE(finished.count(), 1); QVERIFY2(manager.errorString().isEmpty(), qPrintable(manager.errorString())); QCOMPARE(QDir(output.path()).entryList({"*.wav"}, QDir::Files, QDir::Name), (QStringList{"1_Left.wav", "2_Right.wav"})); QVERIFY(std::abs(decode(output.filePath("1_Left.wav")) - .125) < .01); QVERIFY(std::abs(decode(output.filePath("2_Right.wav")) - .25) < .01); QVERIFY(muted->isMutedByModel()); QVERIFY(!scene.left->isMutedByModel()); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); released();
	}
	void effectiveEligibilityCannotResumeAnInvalidatedRoster_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("delete-unvisited") << 0; QTest::newRow("delete-visited") << 1; QTest::newRow("add-track") << 2; QTest::newRow("reorder") << 3; QTest::newRow("project-filename") << 4; QTest::newRow("peer-stored-eligibility") << 5; }
	void effectiveEligibilityCannotResumeAnInvalidatedRoster()
	{
		QFETCH(int, change); const auto scene = arrange(); ReadController controller; auto actor = change == 1 ? scene.right.data() : scene.left.data(); actor->getMutedModel()->setControllerConnection(new ControllerConnection(&controller)); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false, starting = false; controller.callback = [&] { if (!fired) { fired = true; starting = manager.isStarting(); if (change == 0) { delete scene.right.data(); } if (change == 1) { delete scene.left.data(); } if (change == 2) { new SampleTrack(Engine::getSong()); } if (change == 3) { Engine::getSong()->moveTrack(scene.right.data(), 0); } if (change == 4) { Engine::getSong()->setProjectFileName("changed-bulk-project.mmp"); } if (change == 5) { scene.right->getMutedModel()->setValue(true); } } }; const auto disarm = qScopeGuard([&] { controller.callback = {}; }); manager.renderTracks(); QVERIFY(fired && starting); QCOMPARE(finished.count(), 1); QVERIFY(!manager.errorString().isEmpty()); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); released();
	}
	void eligibilityExceptionsDoNotEscapeTheManager_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void eligibilityExceptionsDoNotEscapeTheManager()
	{
		QFETCH(bool, nonstandard); const auto scene = arrange(); ReadController controller; scene.left->getMutedModel()->setControllerConnection(new ControllerConnection(&controller)); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false; controller.callback = [&] { fired = true; if (nonstandard) { throw 137; } throw std::runtime_error("Native bulk eligibility fixture"); }; const auto disarm = qScopeGuard([&] { controller.callback = {}; }); manager.renderTracks(); QVERIFY(fired); QCOMPARE(finished.count(), 1); QVERIFY(!manager.errorString().isEmpty()); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); released();
	}
	void eligibilityCanReturnAfterManagerRetirement()
	{
		const auto scene = arrange(); ReadController controller; scene.left->getMutedModel()->setControllerConnection(new ControllerConnection(&controller)); QTemporaryDir output; const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path())); auto raw = manager.data(); bool fired = false; controller.callback = [&] { if (!fired) { fired = true; delete manager.data(); } }; const auto disarm = qScopeGuard([&] { controller.callback = {}; }); raw->renderTracks(); QVERIFY(fired); QVERIFY(!manager); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void cancellationAndNestedStartsCannotPublishAnEligibilityQueue_data()
	{ QTest::addColumn<bool>("nested"); QTest::newRow("cancel") << false; QTest::newRow("nested-then-cancel") << true; }
	void cancellationAndNestedStartsCannotPublishAnEligibilityQueue()
	{
		QFETCH(bool, nested); const auto scene = arrange(); ReadController controller; scene.left->getMutedModel()->setControllerConnection(new ControllerConnection(&controller)); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false; controller.callback = [&] { if (!fired) { fired = true; QVERIFY(manager.isStarting()); if (nested) { manager.renderProject(); manager.renderTracks(); QCOMPARE(finished.count(), 0); } manager.abortProcessing(); } }; const auto disarm = qScopeGuard([&] { controller.callback = {}; }); manager.renderTracks(); QVERIFY(fired); QCOMPARE(finished.count(), 1); QCOMPARE(manager.errorString(), QString("Audio export was cancelled.")); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void filePollingCanRetireNativeRosterMembers_data()
	{ QTest::addColumn<bool>("project"); QTest::addColumn<int>("stage"); QTest::newRow("peer-first") << false << 1; QTest::newRow("project-first") << true << 1; QTest::newRow("peer-final") << false << 3; QTest::newRow("project-final") << true << 3; }
	void filePollingCanRetireNativeRosterMembers()
	{
		QFETCH(bool, project); QFETCH(int, stage); const auto scene = arrange(); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false; int polls = 0; { FileResourceIO::Scope work([&] { if (++polls == stage) { fired = true; if (project) { Engine::getSong()->clearProject(); } else { delete scene.right.data(); } } return false; }); manager.renderTracks(); } QVERIFY(fired); QCOMPARE(finished.count(), 1); QVERIFY(!manager.errorString().isEmpty()); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void fileCancellationAndExceptionsReleaseTheUntouchedReservation_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void fileCancellationAndExceptionsReleaseTheUntouchedReservation()
	{
		QFETCH(int, failure); const auto scene = arrange(); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false; { FileResourceIO::Scope work([&] { fired = true; if (failure == 1) { throw std::runtime_error("Bulk file callback"); } if (failure == 2) { throw 157; } return true; }); manager.renderTracks(); } QVERIFY(fired); QCOMPARE(finished.count(), 1); QVERIFY(!manager.errorString().isEmpty()); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void filenameGetterChecksReturnedNativeOwnershipAndFailures_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("selected-retired") << 0; QTest::newRow("peer-retired") << 1; QTest::newRow("manager-retired") << 2; QTest::newRow("project-filename") << 3; QTest::newRow("standard") << 4; QTest::newRow("nonstandard") << 5; }
	void filenameGetterChecksReturnedNativeOwnershipAndFailures()
	{
		QFETCH(int, change); const auto scene = arrange(); QTemporaryDir output; const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path())); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); auto raw = manager.data(); QSignalSpy finished(raw, &RenderManager::finished); bool fired = false, starting = false; scene.rightName->callback = [&] { if (!fired) { fired = true; starting = manager->isStarting(); if (change == 0) { delete scene.right.data(); } if (change == 1) { delete scene.left.data(); } if (change == 2) { delete manager.data(); } if (change == 3) { Engine::getSong()->setProjectFileName("changed-filename-project.mmp"); } if (change == 4) { throw std::runtime_error("Native filename getter"); } if (change == 5) { throw 149; } } }; const auto disarm = qScopeGuard([&] { scene.rightName->callback = {}; }); raw->renderTracks(); QVERIFY(fired && starting); if (change == 2) { QVERIFY(!manager); QCOMPARE(finished.count(), 0); } else { QVERIFY(manager); QCOMPARE(finished.count(), 1); QVERIFY(!manager->errorString().isEmpty()); QVERIFY(RenderManagerPreparationTestAccess::clean(manager)); } QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void filenameCancellationReleasesTheInstalledSelection()
	{
		const auto scene = arrange(); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false; scene.rightName->callback = [&] { if (!fired) { fired = true; QVERIFY(TrackRenderScope::hasActiveSelection()); manager.abortProcessing(); } }; const auto disarm = qScopeGuard([&] { scene.rightName->callback = {}; }); manager.renderTracks(); QVERIFY(fired); QCOMPARE(finished.count(), 1); QCOMPARE(manager.errorString(), QString("Audio export was cancelled.")); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void aLocalSelectionCannotPublishIntoARetiredManager()
	{
		const auto scene = arrange(); QTemporaryDir output; const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path())); auto raw = manager.data(); bool fired = false; { FileResourceIO::Scope work([&] { if (!fired && TrackRenderScope::hasActiveSelection()) { fired = true; delete manager.data(); } return false; }); raw->renderTracks(); } QVERIFY(fired); QVERIFY(!manager); QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty()); released();
	}
	void sameManagerCanRenderAfterExplicitPreparationRepair_data()
	{ QTest::addColumn<bool>("filename"); QTest::newRow("eligibility") << false; QTest::newRow("filename") << true; }
	void sameManagerCanRenderAfterExplicitPreparationRepair()
	{
		QFETCH(bool, filename); const auto scene = arrange(); ReadController controller; if (!filename) { scene.left->getMutedModel()->setControllerConnection(new ControllerConnection(&controller)); } QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false; const auto failure = [&] { fired = true; throw std::runtime_error("Preparation repair fixture"); }; if (filename) { scene.rightName->callback = failure; } else { controller.callback = failure; } const auto disarm = qScopeGuard([&] { scene.rightName->callback = {}; controller.callback = {}; }); manager.renderTracks(); QVERIFY(fired); QCOMPARE(finished.count(), 1); QVERIFY(!manager.errorString().isEmpty()); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); released(); scene.rightName->callback = {}; controller.callback = {}; QVERIFY(render(&manager)); QCOMPARE(finished.count(), 2); QVERIFY2(manager.errorString().isEmpty(), qPrintable(manager.errorString())); QVERIFY(std::abs(decode(output.filePath("1_Left.wav")) - .125) < .01); QVERIFY(std::abs(decode(output.filePath("2_Right.wav")) - .25) < .01); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); released();
	}
	void bulkEligibilityIncludesPatternTracksAccordingToStoredMute_data()
	{ QTest::addColumn<bool>("muted"); QTest::newRow("pattern-eligible") << false; QTest::newRow("pattern-muted") << true; }
	void bulkEligibilityIncludesPatternTracksAccordingToStoredMute()
	{
		QFETCH(bool, muted); const auto scene = arrange(); auto name = std::make_shared<NameState>(NameState{"Pattern sample", {}}); const QPointer<NamedSampleTrack> pattern(new NamedSampleTrack(Engine::patternStore(), name)); pattern->setMuted(muted); QTemporaryDir output; RenderManager manager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path()); QSignalSpy finished(&manager, &RenderManager::finished); bool fired = false; const auto inspect = [&] { if (!fired) { fired = true; QCOMPARE(RenderManagerPreparationTestAccess::contains(&manager, pattern), !muted); QCOMPARE(RenderManagerPreparationTestAccess::count(&manager), std::size_t(muted ? 2 : 3)); manager.abortProcessing(); } }; name->callback = inspect; scene.rightName->callback = inspect; const auto disarm = qScopeGuard([&] { name->callback = {}; scene.rightName->callback = {}; }); manager.renderTracks(); QVERIFY(fired); QCOMPARE(finished.count(), 1); QCOMPARE(manager.errorString(), QString("Audio export was cancelled.")); QVERIFY(RenderManagerPreparationTestAccess::clean(&manager)); QCOMPARE(pattern->getMutedModel()->storedValue(), muted ? 1.f : 0.f); released();
	}
};
QTEST_GUILESS_MAIN(RenderManagerPreparationTest)
#include "RenderManagerPreparationTest.moc"

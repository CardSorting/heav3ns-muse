/* Native stem admission callback ownership. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "RemotePlugin.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "SongExportTransitionTestUtils.h"
#include "StudioCapabilities.h"
#include "Timeline.h"
#include "TrackRenderScope.h"
using namespace lmms;
namespace lmms
{
class TrackRenderScopeLifecycleTestAccess
{
public:
	static bool selected(Track* track) { return track->m_renderSelected.load(std::memory_order_acquire); }
	static bool excluded(Track* track) { return track->m_renderExcluded.load(std::memory_order_acquire); }
};
}
namespace
{
std::vector<QPointer<Track>> tracks()
{
	std::vector<QPointer<Track>> result; for (const auto owner : {static_cast<TrackContainer*>(Engine::getSong()), static_cast<TrackContainer*>(Engine::patternStore())}) { for (auto track : owner->tracks()) { result.emplace_back(track); } } return result;
}
bool clearFlags() { for (const auto& track : tracks()) { if (track && (TrackRenderScopeLifecycleTestAccess::selected(track) || TrackRenderScopeLifecycleTestAccess::excluded(track))) { return false; } } return !TrackRenderScope::hasActiveSelection(); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
bool native(const QString& op, const QJsonObject& args) { return StudioCapabilities::execute(op, args).value("ok").toBool(); }
bool arrange()
{
	Engine::getSong()->clearProject(); if (!native("song.set", {{"tempo", 240}, {"masterVolume", 100}})) { return false; }
	for (int index = 0; index < 2; ++index)
	{
		if (!native("track.add", {{"type", "sample"}}) || !native("clip.add", {{"track", index}, {"position", 0}})) { return false; }
		auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks()[index]->getClip(0)); clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(35280, SampleFrame(.125f * (index + 1))), 44100));
		if (!native("clip.edit", {{"track", index}, {"clip", 0}, {"length", 192}})) { return false; }
	}
	return true;
}
double decodedMean(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return -1.; } SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close); if (!audio || info.channels != 2 || info.samplerate != 44100 || info.frames < 10000 || sf_seek(audio.get(), 1000, SEEK_SET) != 1000) { return -1.; }
	std::vector<float> pcm(8192); if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; } double sum = 0; for (const auto value : pcm) { if (!std::isfinite(value)) { return -1.; } sum += value; } return sum / pcm.size();
}
class ReadController final : public Controller
{
public:
	ReadController() : Controller(ControllerType::Dummy, nullptr, "Stem eligibility fixture") { }
	std::function<void()> callback;
	float currentValue(int) override { if (callback) { callback(); } return 0.; }
};
}
class TrackRenderScopeLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void modelOwnershipReleased() { std::atomic_bool acquired{false}; std::thread observer([&] { const auto ownership = Engine::audioEngine()->requestChangesGuard(); acquired = true; }); observer.join(); QVERIFY(acquired); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setProjectFileName(""); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); QVERIFY(clearFlags()); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); QVERIFY(clearFlags()); }
	void nativeSelectionRestoresFlagsWithoutWritingMuteOrSolo_data()
	{ QTest::addColumn<bool>("pattern"); QTest::addColumn<bool>("instrument"); QTest::newRow("song-sample") << false << false; QTest::newRow("song-instrument") << false << true; QTest::newRow("pattern-sample") << true << false; QTest::newRow("pattern-instrument") << true << true; }
	void nativeSelectionRestoresFlagsWithoutWritingMuteOrSolo()
	{
		QFETCH(bool, pattern); QFETCH(bool, instrument); auto selectedOwner = pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : static_cast<TrackContainer*>(Engine::getSong()); Track* chosen = instrument ? static_cast<Track*>(new InstrumentTrack(selectedOwner)) : static_cast<Track*>(new SampleTrack(selectedOwner)); new SampleTrack(Engine::getSong()); new InstrumentTrack(Engine::patternStore()); new AutomationTrack(Engine::getSong());
		const auto roster = tracks(); std::vector<float> controls; for (const auto& track : roster) { controls.push_back(track->getMutedModel()->storedValue()); controls.push_back(track->getSoloModel()->storedValue()); }
		{ TrackRenderScope scope(chosen); QVERIFY(TrackRenderScope::hasActiveSelection()); for (const auto& track : roster) { const bool playable = track->type() == Track::Type::Instrument || track->type() == Track::Type::Sample; QCOMPARE(TrackRenderScopeLifecycleTestAccess::selected(track), track.data() == chosen); QCOMPARE(TrackRenderScopeLifecycleTestAccess::excluded(track), playable && track.data() != chosen); } }
		QVERIFY(clearFlags()); std::size_t index = 0; for (const auto& track : roster) { QCOMPARE(track->getMutedModel()->storedValue(), controls[index++]); QCOMPARE(track->getSoloModel()->storedValue(), controls[index++]); }
	}
	void returnedCallbacksCanRetireSelectedExcludedOrProjectTracks_data()
	{ QTest::addColumn<int>("target"); QTest::addColumn<int>("stage"); QTest::newRow("selected-first") << 0 << 1; QTest::newRow("excluded-first") << 1 << 1; QTest::newRow("project-first") << 2 << 1; QTest::newRow("selected-final") << 0 << 3; QTest::newRow("excluded-final") << 1 << 3; QTest::newRow("project-final") << 2 << 3; }
	void returnedCallbacksCanRetireSelectedExcludedOrProjectTracks()
	{
		QFETCH(int, target); QFETCH(int, stage); const QPointer<SampleTrack> chosen(new SampleTrack(Engine::getSong())), other(new SampleTrack(Engine::getSong())); auto raw = chosen.data(); int polls = 0; bool fired = false;
		{ FileResourceIO::Scope work([&] { if (++polls == stage) { fired = true; if (target == 0) { delete chosen.data(); } else if (target == 1) { delete other.data(); } else { Engine::getSong()->clearProject(); } } return false; }); QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(raw), std::runtime_error); }
		QVERIFY(fired); QVERIFY(clearFlags()); modelOwnershipReleased(); auto next = new SampleTrack(Engine::getSong()); { TrackRenderScope fresh(next); QVERIFY(TrackRenderScopeLifecycleTestAccess::selected(next)); } QVERIFY(clearFlags());
	}
	void returnedCallbacksCannotPublishChangedNativeMembership_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("reorder") << 0; QTest::newRow("addition") << 1; QTest::newRow("detach") << 2; QTest::newRow("project-filename") << 3; QTest::newRow("render-context") << 4; }
	void returnedCallbacksCannotPublishChangedNativeMembership()
	{
		QFETCH(int, change); auto song = Engine::getSong(); auto chosen = new SampleTrack(song); const QPointer<SampleTrack> other(new SampleTrack(song)); std::uint64_t context = 0; const auto release = qScopeGuard([&] { if (context) { RemotePlugin::endRenderContext(context); } if (other && std::find(song->tracks().begin(), song->tracks().end(), other.data()) == song->tracks().end()) { delete other.data(); } }); bool fired = false;
		{ FileResourceIO::Scope work([&] { if (!fired) { fired = true; if (change == 0) { song->moveTrack(other.data(), 0); } if (change == 1) { new SampleTrack(song); } if (change == 2) { song->removeTrack(other.data()); } if (change == 3) { song->setProjectFileName("changed-during-stem-admission.mmp"); } if (change == 4) { context = RemotePlugin::beginRenderContext(); } } return false; }); QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), std::runtime_error); }
		QVERIFY(fired); QVERIFY(clearFlags()); modelOwnershipReleased();
	}
	void periodicPollingNeverResumesABorrowedNativeTrackIterator_data()
	{ QTest::addColumn<bool>("visited"); QTest::newRow("already-visited-excluded") << true; QTest::newRow("not-yet-visited-excluded") << false; }
	void periodicPollingNeverResumesABorrowedNativeTrackIterator()
	{
		QFETCH(bool, visited); auto chosen = new SampleTrack(Engine::getSong()); std::vector<QPointer<SampleTrack>> peers; for (int index = 0; index < 130; ++index) { peers.emplace_back(new SampleTrack(Engine::getSong())); } int polls = 0; bool fired = false;
		// FileResourceIO checks cancellation both before and after event polling;
		// callback three is the first read at the second native checkpoint.
		{ FileResourceIO::Scope work([&] { if (++polls == 3) { fired = true; delete peers[visited ? 0 : 129].data(); } return false; }); QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), std::runtime_error); }
		QVERIFY(fired); QVERIFY(polls >= 3); QVERIFY(clearFlags()); modelOwnershipReleased();
	}
	void changedStoredEligibilityRefusesWithoutRollingBackTheExternalEdit_data()
	{ QTest::addColumn<bool>("solo"); QTest::newRow("mute") << false; QTest::newRow("solo") << true; }
	void changedStoredEligibilityRefusesWithoutRollingBackTheExternalEdit()
	{
		QFETCH(bool, solo); auto chosen = new SampleTrack(Engine::getSong()); auto model = solo ? chosen->getSoloModel() : chosen->getMutedModel(); bool fired = false; { FileResourceIO::Scope work([&] { if (!fired) { fired = true; model->setValue(true); } return false; }); QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), std::runtime_error); } QVERIFY(fired); QCOMPARE(model->storedValue(), 1.f); QVERIFY(clearFlags()); modelOwnershipReleased();
	}
	void cancellationAndExceptionsReleaseBothAdmissionAndNativeModelOwnership_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void cancellationAndExceptionsReleaseBothAdmissionAndNativeModelOwnership()
	{
		QFETCH(int, failure); auto chosen = new SampleTrack(Engine::getSong()); int polls = 0; { FileResourceIO::Scope work([&] { ++polls; if (failure == 1) { throw std::runtime_error("Stem admission callback"); } if (failure == 2) { throw 53; } return true; }); if (failure == 2) { QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), int); } else { QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), std::runtime_error); } } QVERIFY(polls > 0); QVERIFY(clearFlags()); modelOwnershipReleased(); { TrackRenderScope fresh(chosen); QVERIFY(TrackRenderScopeLifecycleTestAccess::selected(chosen)); } QVERIFY(clearFlags());
	}
	void nestedAdmissionRefusesWithoutReleasingTheOuterReservation()
	{
		auto chosen = new SampleTrack(Engine::getSong()); bool fired = false, refused = false, unpublished = false; { FileResourceIO::Scope work([&] { if (!fired) { fired = true; try { TrackRenderScope nested(chosen); } catch (const std::runtime_error&) { refused = true; } unpublished = TrackRenderScope::hasActiveSelection() && !TrackRenderScopeLifecycleTestAccess::selected(chosen); } return false; }); TrackRenderScope outer(chosen); QVERIFY(fired && refused && unpublished); QVERIFY(TrackRenderScopeLifecycleTestAccess::selected(chosen)); QVERIFY(TrackRenderScope::hasActiveSelection()); } QVERIFY(clearFlags());
	}
	void installedScopeCleanupToleratesTrackRetirement_data()
	{ QTest::addColumn<int>("target"); QTest::newRow("selected") << 0; QTest::newRow("excluded") << 1; QTest::newRow("project") << 2; }
	void installedScopeCleanupToleratesTrackRetirement()
	{
		QFETCH(int, target); const QPointer<SampleTrack> chosen(new SampleTrack(Engine::getSong())), other(new SampleTrack(Engine::getSong())); auto scope = std::make_unique<TrackRenderScope>(chosen.data()); if (target == 0) { delete chosen.data(); } else if (target == 1) { delete other.data(); } else { Engine::getSong()->clearProject(); } scope.reset(); QVERIFY(clearFlags());
	}
	void foreignContainerCannotBecomeAStemSelection()
	{
		TrackContainer foreign; auto chosen = new SampleTrack(&foreign); new SampleTrack(Engine::getSong()); QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), std::invalid_argument); QVERIFY(clearFlags());
	}
	void foreignThreadRefusesBeforeNativeTrackAdmission()
	{
		auto chosen = new SampleTrack(Engine::getSong()); bool refused = false; std::thread observer([&] { try { TrackRenderScope scope(chosen); } catch (const std::invalid_argument&) { refused = true; } }); observer.join(); QVERIFY(refused); QVERIFY(clearFlags());
	}
	void nativeExportAndPendingCleanupRefuseSelection_data()
	{ QTest::addColumn<int>("state"); QTest::newRow("song-export") << 0; QTest::newRow("remote-context") << 1; QTest::newRow("pending-cleanup") << 2; }
	void nativeExportAndPendingCleanupRefuseSelection()
	{
		QFETCH(int, state); auto song = Engine::getSong(); auto chosen = new SampleTrack(song); std::uint64_t context = 0; std::unique_ptr<SongExportTransitionProbe> fault; const auto release = qScopeGuard([&] { fault.reset(); if (context) { RemotePlugin::endRenderContext(context); } song->stopExport(); });
		if (state == 1) { context = RemotePlugin::beginRenderContext(); QVERIFY(context); }
		else if (state == 0) { song->startExport(); }
		else
		{
			fault = std::make_unique<SongExportTransitionProbe>(song, [](SongExportPoint point) { if (point == SongExportPoint::StartAfterPlayback) { throw std::runtime_error("Stem export initialization"); } if (point == SongExportPoint::StopAfterTimeline) { throw std::runtime_error("Pending stem cleanup"); } });
			QVERIFY_EXCEPTION_THROWN(song->startExport(), std::runtime_error); QVERIFY(!song->isExporting()); QVERIFY(song->hasPendingExportCleanup());
			auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48); QVERIFY_EXCEPTION_THROWN(song->stopExport(), std::runtime_error); QVERIFY(song->hasPendingExportCleanup()); QVERIFY(!song->isExporting());
		}
		QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), std::runtime_error); QVERIFY(clearFlags());
	}
	void effectiveMuteGetterChecksReturnedNativeContext_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("filename") << 0; QTest::newRow("standard-exception") << 1; QTest::newRow("nonstandard-exception") << 2; }
	void effectiveMuteGetterChecksReturnedNativeContext()
	{
		QFETCH(int, change); auto chosen = new SampleTrack(Engine::getSong()); ReadController controller; chosen->getMutedModel()->setControllerConnection(new ControllerConnection(&controller)); bool fired = false; controller.callback = [&] { fired = true; if (change == 0) { Engine::getSong()->setProjectFileName("effective-getter-changed-project.mmp"); } if (change == 1) { throw std::runtime_error("Stem eligibility getter"); } if (change == 2) { throw 79; } }; const auto disarm = qScopeGuard([&] { controller.callback = {}; }); if (change == 2) { QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), int); } else { QVERIFY_EXCEPTION_THROWN(TrackRenderScope scope(chosen), std::runtime_error); } QVERIFY(fired); QVERIFY(clearFlags()); modelOwnershipReleased();
	}
	void nativeRenderRefusesStaleStemAndThenRendersAfterExplicitRepair_data()
	{ QTest::addColumn<bool>("selected"); QTest::newRow("selected-retired") << true; QTest::newRow("excluded-retired") << false; }
	void nativeRenderRefusesStaleStemAndThenRendersAfterExplicitRepair()
	{
		QFETCH(bool, selected); QVERIFY(arrange()); QTemporaryDir output; const auto path = output.filePath("preserved.wav"); QVERIFY(write(path, "prior user audio")); const QPointer<Track> victim(Engine::getSong()->tracks()[selected ? 0 : 1]); bool fired = false; QJsonObject response;
		{ FileResourceIO::Scope work([&] { if (!fired && TrackRenderScope::hasActiveSelection()) { fired = true; delete victim.data(); } return false; }); response = StudioCapabilities::execute("audio.render", {{"path", path}, {"track", 0}}); }
		QVERIFY(fired); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QCOMPARE(read(path), QByteArray("prior user audio")); QVERIFY(clearFlags()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QVERIFY(!RemotePlugin::hasActiveRenderContext()); modelOwnershipReleased(); QVERIFY(arrange()); const auto fresh = output.filePath("fresh.wav"); response = StudioCapabilities::execute("audio.render", {{"path", fresh}, {"track", 0}, {"timeoutMs", 30000}}); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(std::abs(decodedMean(fresh) - .125) < .01); QCOMPARE(read(path), QByteArray("prior user audio")); QVERIFY(clearFlags());
	}
};
QTEST_GUILESS_MAIN(TrackRenderScopeLifecycleTest)
#include "TrackRenderScopeLifecycleTest.moc"

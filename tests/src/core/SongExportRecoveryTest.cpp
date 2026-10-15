/* Returned native stop exceptions, export cleanup and retained-device retry.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "RemotePlugin.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "Song.h"
#include "SongExportTransitionTestUtils.h"
#include "StudioCapabilities.h"
#include "Timeline.h"

using namespace lmms;
namespace
{
[[noreturn]] void raise(bool nonstandard)
{ if (nonstandard) { throw 17; } throw std::runtime_error("Native export cleanup fixture"); }
void exceptionRows()
{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
QJsonObject restoration()
{
	return StudioCapabilities::execute("transport.inspect", {}).value("result").toObject().value("backend").toObject().value("restoration").toObject();
}
float settled(MidiController& controller)
{ Controller::triggerFrameCounter(); controller.currentValue(0); Controller::triggerFrameCounter(); return controller.currentValue(0); }
void cc(MidiController& controller, int value)
{ controller.processInEvent(MidiEvent(MidiControlChange, 0, 74, value), {}); }
QByteArray read(const QString& path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& data)
{ QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size(); }
bool arrange()
{
	if (!StudioCapabilities::execute("song.set", {{"tempo", 600}, {"masterVolume", 100}}).value("ok").toBool()
		|| !StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool()
		|| !StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool()) { return false; }
	auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks()[0]->getClip(0));
	clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(48000, SampleFrame(.25f)), 48000));
	return StudioCapabilities::execute("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 192}, {"autoResize", false}}).value("ok").toBool();
}
}

class SongExportRecoveryTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice());
		Engine::getSong()->clearProject(); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1);
		Engine::getSong()->getTimeline(Song::PlayMode::Song).setStopBehaviour(Timeline::StopBehaviour::BackToZero);
	}
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); }

	void returnedStopExceptionReleasesNativeModelOwnership_data() { exceptionRows(); }
	void returnedStopExceptionReleasesNativeModelOwnership()
	{
		QFETCH(bool, nonstandard); auto song = Engine::getSong(); song->playSong(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48);
		SongExportTransitionProbe transitionProbe(song, [&](SongExportPoint point) { if (point == SongExportPoint::StopAfterTimeline) { raise(nonstandard); } });
		bool rejected = false; try { song->stop(); } catch (...) { rejected = true; } QVERIFY(rejected);
		std::atomic_bool acquired{false}; std::thread observer([&] { const auto changes = Engine::audioEngine()->requestChangesGuard(); acquired = true; });
		// The process timeout bounds a leaked recursive model lock, which a same-
		// thread retry could conceal. This worker must acquire actual ownership.
		observer.join(); QVERIFY(acquired); transitionProbe.reset(); song->stop(); QVERIFY(song->isStopped());
	}

	void failedExportCleanupRetriesThroughExistingRestore_data()
	{
		QTest::addColumn<bool>("reserved"); QTest::addColumn<bool>("nonstandard");
		QTest::newRow("direct-standard") << false << false; QTest::newRow("direct-nonstandard") << false << true;
		QTest::newRow("reserved-standard") << true << false; QTest::newRow("reserved-nonstandard") << true << true;
	}
	void failedExportCleanupRetriesThroughExistingRestore()
	{
		QFETCH(bool, reserved); QFETCH(bool, nonstandard); auto song = Engine::getSong(); auto engine = Engine::audioEngine(); MidiController controller(nullptr);
		controller.midiPort()->setInputController(74); cc(controller, 63); QCOMPARE(settled(controller), 63.f / 127.f);
		if (reserved) { QVERIFY(engine->storeAudioDevice()); } song->startExport(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48);
		SongExportTransitionProbe transitionProbe(song, [&](SongExportPoint point) { if (point == SongExportPoint::StopAfterTimeline) { raise(nonstandard); } });
		bool rejected = false; try { song->stopExport(); } catch (...) { rejected = true; } QVERIFY(rejected); QVERIFY(song->hasPendingExportCleanup()); QVERIFY(song->isExporting());
		const auto info = restoration(); QVERIFY(info.value("pending").toBool()); QVERIFY(info.value("exportCleanupPending").toBool()); QVERIFY(info.value("failed").toBool()); QVERIFY(info.value("retryEligible").toBool());
		cc(controller, 0); QCOMPARE(settled(controller), 63.f / 127.f);
		const auto failed = StudioCapabilities::execute("audio.device.restore", {}); QVERIFY(!failed.value("ok").toBool()); QVERIFY(failed.value("error").toString().contains("export-stop-exception"));
		QVERIFY(song->hasPendingExportCleanup()); QCOMPARE(engine->hasStoredAudioDevice(), reserved); QVERIFY(!engine->isChangingAudioDevice());
		transitionProbe.reset(); const auto recovered = StudioCapabilities::execute("audio.device.restore", {});
		QVERIFY2(recovered.value("ok").toBool(), qPrintable(recovered.value("error").toString())); QVERIFY(!song->isExporting()); QVERIFY(!song->hasPendingExportCleanup()); QVERIFY(!engine->hasStoredAudioDevice());
		QVERIFY(song->isStopped()); QVERIFY(!restoration().value("pending").toBool()); cc(controller, 127); QCOMPARE(settled(controller), 1.f);
		song->startExport(); song->stopExport(); QVERIFY(!song->hasPendingExportCleanup());
	}

	void activeExecutionStillRefusesCleanupRecovery()
	{
		auto song = Engine::getSong(); auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice()); song->startExport();
		auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48);
		SongExportTransitionProbe transitionProbe(song, [](SongExportPoint point) { if (point == SongExportPoint::StopAfterTimeline) { raise(false); } });
		QVERIFY_EXCEPTION_THROWN(song->stopExport(), std::runtime_error); transitionProbe.reset();
		const auto context = RemotePlugin::beginRenderContext(); QVERIFY(context != 0); const auto end = qScopeGuard([&] { RemotePlugin::endRenderContext(context); });
		QVERIFY(!engine->restoreAudioDevice()); QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::Busy);
		QVERIFY(song->hasPendingExportCleanup()); QVERIFY(engine->hasStoredAudioDevice()); QVERIFY(!restoration().value("retryEligible").toBool());
		RemotePlugin::endRenderContext(context); QVERIFY(StudioCapabilities::execute("audio.device.restore", {}).value("ok").toBool()); QVERIFY(!song->hasPendingExportCleanup());
	}

	void nestedRestoreCannotReenterTransportCleanup()
	{
		auto song = Engine::getSong(); auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice()); song->startExport();
		auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48);
		SongExportTransitionProbe transitionProbe(song, [](SongExportPoint point) { if (point == SongExportPoint::StopAfterTimeline) { raise(false); } });
		QVERIFY_EXCEPTION_THROWN(song->stopExport(), std::runtime_error); transitionProbe.reset();
		bool observed = false, rejected = false;
		const auto reentry = connect(song, &Song::stopped, this, [&] { observed = true; rejected = !engine->restoreAudioDevice(); }, Qt::DirectConnection);
		const auto removeReentry = qScopeGuard([&] { QObject::disconnect(reentry); });
		QVERIFY(engine->restoreAudioDevice()); QVERIFY(observed); QVERIFY(rejected); QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!song->hasPendingExportCleanup());
		QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::None);
	}

	void ordinaryStopFailureDoesNotBecomeExportRecovery()
	{
		auto song = Engine::getSong(); song->playSong(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48);
		SongExportTransitionProbe transitionProbe(song, [](SongExportPoint point) { if (point == SongExportPoint::StopAfterTimeline) { raise(false); } });
		QVERIFY_EXCEPTION_THROWN(song->stop(), std::runtime_error);
		QVERIFY(!song->hasPendingExportCleanup()); QVERIFY(!restoration().value("pending").toBool()); transitionProbe.reset();
		QVERIFY(StudioCapabilities::execute("transport.stop", {}).value("ok").toBool());
	}

	void failedInitializationCanRetainCleanupWithNoExportFlag()
	{
		auto song = Engine::getSong(); MidiController controller(nullptr); controller.midiPort()->setInputController(74); cc(controller, 63); settled(controller);
		SongExportTransitionProbe startup(song, [](SongExportPoint point) { if (point == SongExportPoint::StartAfterPlayback) { raise(false); } });
		QVERIFY_EXCEPTION_THROWN(song->startExport(), std::runtime_error); startup.reset();
		QVERIFY(!song->isExporting()); QVERIFY(song->hasPendingExportCleanup());
		SongExportTransitionProbe cleanupFault(song, [](SongExportPoint point) { if (point == SongExportPoint::StopAfterTimeline) { raise(false); } });
		QVERIFY_EXCEPTION_THROWN(song->stopExport(), std::runtime_error);
		QVERIFY(!song->isExporting()); QVERIFY(song->hasPendingExportCleanup()); cc(controller, 0); QCOMPARE(settled(controller), 63.f / 127.f);
		QVERIFY_EXCEPTION_THROWN(song->startExport(), std::logic_error); cleanupFault.reset();
		QVERIFY(StudioCapabilities::execute("audio.device.restore", {}).value("ok").toBool()); QVERIFY(!song->hasPendingExportCleanup()); cc(controller, 127); QCOMPARE(settled(controller), 1.f);
	}

	void failedNativeRenderPreservesDestinationAndCanContinue_data() { exceptionRows(); }
	void failedNativeRenderPreservesDestinationAndCanContinue()
	{
		QFETCH(bool, nonstandard); QVERIFY(arrange()); QTemporaryDir directory; const auto path = directory.filePath("preserved.wav"); const QByteArray original("Prior user audio"); QVERIFY(write(path, original));
		auto song = Engine::getSong(); std::atomic_bool fired{false};
		SongExportTransitionProbe transitionProbe(song, [&](SongExportPoint point) { if (point == SongExportPoint::StopAfterPlayback && song->isExporting() && !fired.exchange(true)) { raise(nonstandard); } });
		QJsonObject args{{"path", path}, {"sampleRate", 48000}, {"bitDepth", 24}, {"timeoutMs", 30000}};
		const auto failed = StudioCapabilities::execute("audio.render", args); QVERIFY(fired); QVERIFY(!failed.value("ok").toBool()); QVERIFY(!failed.contains("result")); QCOMPARE(read(path), original);
		QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!song->isExporting()); QVERIFY(!song->hasPendingExportCleanup()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		transitionProbe.reset(); args.insert("path", directory.filePath("next.wav")); const auto next = StudioCapabilities::execute("audio.render", args);
		QVERIFY2(next.value("ok").toBool(), qPrintable(next.value("error").toString())); const auto audio = read(directory.filePath("next.wav")); QVERIFY(audio.size() > 44); QVERIFY(audio.startsWith("RIFF"));
		QCOMPARE(read(path), original);
	}
};
QTEST_GUILESS_MAIN(SongExportRecoveryTest)
#include "SongExportRecoveryTest.moc"

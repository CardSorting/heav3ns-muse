/* Native export transition isolation and pending cleanup. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
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
#include "SongExportTransitionTestUtils.h"
#include "StudioCapabilities.h"
#include "TrackRenderScope.h"
using namespace lmms;
namespace
{
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool prior(const QString& path) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write("prior export") == 12; }
void arrange()
{
	Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100);
	std::vector<SampleFrame> pcm(44100);
	for (std::size_t frame = 0; frame < pcm.size(); ++frame) { pcm[frame][0] = .4f * std::sin(6.283185307179586 * 440 * frame / 44100); pcm[frame][1] = .2f * std::sin(6.283185307179586 * 660 * frame / 44100); }
	auto track = new SampleTrack(Engine::getSong()); auto clip = static_cast<SampleClip*>(track->createClip(TimePos()));
	clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100)); clip->changeLength(TimePos(192));
}
double rms(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return -1.; } SF_INFO info{};
	std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
	if (!audio || info.samplerate != 44100 || info.channels != 2 || info.frames < 20000 || sf_seek(audio.get(), 10000, SEEK_SET) != 10000) { return -1.; }
	std::vector<float> pcm(8192); if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; }
	double square = 0.; for (std::size_t index = 0; index < pcm.size(); ++index) { if (!std::isfinite(pcm[index])) { return -1.; } if (index % 2 == 0) { square += double(pcm[index]) * pcm[index]; } }
	return std::sqrt(square / 4096);
}
bool run(RenderManager& manager)
{
	QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool done = false;
	QObject::connect(&manager, &RenderManager::finished, &loop, [&] { done = true; loop.quit(); });
	QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
	timer.start(30000); manager.renderProject(); if (!done) { loop.exec(); } if (!done) { manager.abortProcessing(); } return done;
}
QJsonObject render(const QString& path) { return StudioCapabilities::execute("audio.render", {{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}}); }
}
class SongExportTransitionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released()
	{
		auto song = Engine::getSong(); QVERIFY(!song->isExporting()); QVERIFY(!song->hasPendingExportCleanup()); QVERIFY(song->isStopped());
		QVERIFY(!SongExportTransitionTestAccess::active(song)); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!RemotePlugin::hasRunningRenderExecution()); QVERIFY(!TrackRenderScope::hasActiveSelection());
	}
	void decoded(const QString& path) { QVERIFY(std::abs(rms(path) - .4 * std::sqrt(.5)) < .04); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); arrange();
	}
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); released(); }
	void failedNativeTransitionsRequireExplicitRecovery_data()
	{
		QTest::addColumn<int>("point"); QTest::addColumn<bool>("nonstandard");
		for (int point = 0; point < 4; ++point) for (bool nonstandard : {false, true}) { QTest::newRow(qPrintable(QString::number(point) + QString::number(nonstandard))) << point << nonstandard; }
	}
	void failedNativeTransitionsRequireExplicitRecovery()
	{
		QFETCH(int, point); QFETCH(bool, nonstandard); auto song = Engine::getSong(); auto engine = Engine::audioEngine(); auto original = engine->audioDev(); QVERIFY(engine->storeAudioDevice());
		const auto repair = qScopeGuard([&] { engine->restoreAudioDevice(); }); int calls = 0; bool wasActive = false;
		SongExportTransitionProbe fault(song, [&](SongExportPoint current) { if (current == static_cast<SongExportPoint>(point)) { ++calls; wasActive = SongExportTransitionTestAccess::active(song); if (nonstandard) { throw 251; } throw std::runtime_error("Native export transition fixture"); } });
		if (point >= 2) { song->startExport(); song->getTimeline(Song::PlayMode::Song).setTicks(48); }
		if (nonstandard) { if (point < 2) { QVERIFY_EXCEPTION_THROWN(song->startExport(), int); } else { QVERIFY_EXCEPTION_THROWN(song->stopExport(), int); } }
		else { if (point < 2) { QVERIFY_EXCEPTION_THROWN(song->startExport(), std::runtime_error); } else { QVERIFY_EXCEPTION_THROWN(song->stopExport(), std::runtime_error); } }
		QCOMPARE(calls, 1); QVERIFY(wasActive); QVERIFY(!SongExportTransitionTestAccess::active(song)); QVERIFY(song->hasPendingExportCleanup());
		if (point < 2) { QVERIFY(!song->isExporting()); }
		QVERIFY_EXCEPTION_THROWN(song->startExport(), std::logic_error); QCOMPARE(calls, 1); QVERIFY(song->hasPendingExportCleanup());
		fault.reset(); QVERIFY(engine->restoreAudioDevice()); QCOMPARE(engine->audioDev(), original); released();
	}
	void reentryAtEachNativeCheckpointCannotChangeTheOuterTransition_data()
	{ QTest::addColumn<int>("point"); for (int point = 0; point < 4; ++point) { QTest::newRow(qPrintable(QString::number(point))) << point; } }
	void reentryAtEachNativeCheckpointCannotChangeTheOuterTransition()
	{
		QFETCH(int, point); auto song = Engine::getSong(); int refused = 0; bool fired = false;
		SongExportTransitionProbe fault(song, [&](SongExportPoint current) {
			if (!fired && current == static_cast<SongExportPoint>(point)) { fired = true; try { song->startExport(); } catch (const std::logic_error&) { ++refused; } try { song->stopExport(); } catch (const std::logic_error&) { ++refused; } }
		});
		song->startExport(); QVERIFY(song->isExporting()); QVERIFY(!song->hasPendingExportCleanup()); song->stopExport();
		QVERIFY(fired); QCOMPARE(refused, 2); released();
	}
	void playbackObserversCannotReenterExportTransitions()
	{
		auto song = Engine::getSong(); int refused = 0, delivered = 0;
		const auto connection = connect(song, &Song::playbackStateChanged, this, [&] {
			if (!SongExportTransitionTestAccess::active(song)) { return; } ++delivered;
			try { song->startExport(); } catch (const std::logic_error&) { ++refused; }
			try { song->stopExport(); } catch (const std::logic_error&) { ++refused; }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); song->startExport(); song->stopExport();
		QCOMPARE(delivered, 2); QCOMPARE(refused, 4); released();
	}
	void anAlreadyActiveExportCannotBeRestarted()
	{
		auto song = Engine::getSong(); song->startExport(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48);
		QVERIFY_EXCEPTION_THROWN(song->startExport(), std::logic_error); QCOMPARE(timeline.ticks(), tick_t(48)); QVERIFY(song->isExporting()); QVERIFY(!song->hasPendingExportCleanup());
		song->stopExport(); released();
	}
	void failedRendererInitializationPreservesOutputAndCanRetry_data()
	{
		QTest::addColumn<int>("point"); QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("studio");
		for (int point = 0; point < 2; ++point) for (bool nonstandard : {false, true}) for (bool studio : {false, true})
		{ QTest::newRow(qPrintable(QString::number(point) + QString::number(nonstandard) + QString::number(studio))) << point << nonstandard << studio; }
	}
	void failedRendererInitializationPreservesOutputAndCanRetry()
	{
		QFETCH(int, point); QFETCH(bool, nonstandard); QFETCH(bool, studio); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(prior(path));
		std::atomic_bool armed{true}; std::atomic_int calls{0};
		SongExportTransitionProbe fault(Engine::getSong(), [&](SongExportPoint current) { if (armed && current == static_cast<SongExportPoint>(point)) { ++calls; if (nonstandard) { throw 253; } throw std::runtime_error("Native worker export initialization fixture"); } });
		RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, path);
		if (studio) { const auto response = render(path); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); }
		else { QVERIFY(run(manager)); QVERIFY(!manager.errorString().isEmpty()); }
		QCOMPARE(calls.load(), 1); QCOMPARE(read(path), QByteArray("prior export")); released();
		QVERIFY(QDir(output.path()).entryList({"*.render-*", ".lmms-export-*"}, QDir::Files | QDir::Hidden).isEmpty()); armed = false;
		if (studio) { QVERIFY(render(path).value("ok").toBool()); } else { QVERIFY(run(manager)); QVERIFY(manager.errorString().isEmpty()); }
		decoded(path); released();
	}
	void failedRendererCleanupRetainsOnlyPersistentRecoveryWork_data()
	{
		QTest::addColumn<int>("point"); QTest::addColumn<bool>("persistent"); QTest::addColumn<bool>("studio");
		for (int point = 2; point < 4; ++point) for (bool persistent : {false, true}) for (bool studio : {false, true})
		{ QTest::newRow(qPrintable(QString::number(point) + QString::number(persistent) + QString::number(studio))) << point << persistent << studio; }
	}
	void failedRendererCleanupRetainsOnlyPersistentRecoveryWork()
	{
		QFETCH(int, point); QFETCH(bool, persistent); QFETCH(bool, studio); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(prior(path));
		auto engine = Engine::audioEngine(); std::atomic_bool armed{true}; std::atomic_int calls{0};
		SongExportTransitionProbe fault(Engine::getSong(), [&](SongExportPoint current) {
			if (armed && current == static_cast<SongExportPoint>(point)) { const auto attempt = ++calls; if (persistent || attempt == 1) { throw std::runtime_error("Native worker export cleanup fixture"); } }
		});
		const auto repair = qScopeGuard([&] { fault.reset(); engine->restoreAudioDevice(); });
		RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, path);
		if (studio) { QVERIFY(!render(path).value("ok").toBool()); } else { QVERIFY(run(manager)); QVERIFY(!manager.errorString().isEmpty()); }
		QVERIFY(calls.load() >= 1); QCOMPARE(read(path), QByteArray("prior export"));
		QCOMPARE(engine->hasStoredAudioDevice(), persistent); QCOMPARE(Engine::getSong()->hasPendingExportCleanup(), persistent);
		QVERIFY(!SongExportTransitionTestAccess::active(Engine::getSong())); QVERIFY(!RemotePlugin::hasActiveRenderContext());
		if (persistent) { QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::ExportStopException); }
		armed = false; QVERIFY(engine->restoreAudioDevice()); released();
		if (studio) { QVERIFY(render(path).value("ok").toBool()); } else { QVERIFY(run(manager)); QVERIFY(manager.errorString().isEmpty()); }
		decoded(path); released();
	}
};
QTEST_GUILESS_MAIN(SongExportTransitionTest)
#include "SongExportTransitionTest.moc"

/* Cancellation failures stay outside Qt event dispatch. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AudioFileDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAudio.h"
#include "StudioCapabilities.h"
#include "TrackRenderScope.h"
using namespace lmms;
namespace
{
// Test instrumentation pauses the actual renderer between native periods. It
// neither replaces the engine/encoder nor changes samples or installs a plugin.
struct Gate
{
	std::mutex mutex;
	std::condition_variable condition;
	std::atomic_bool entered{false}, waiting{false};
	bool released = false;
	void release() { { const std::lock_guard lock(mutex); released = true; } condition.notify_all(); }
	void pause()
	{
		if (entered.exchange(true)) { return; }
		std::unique_lock lock(mutex); waiting = true;
		condition.wait_for(lock, std::chrono::seconds(3), [&] { return released; }); waiting = false;
	}
};
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write("prior export") == 12; }
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
	if (!audio || info.channels != 2 || info.samplerate != 44100 || info.frames < 20000 || sf_seek(audio.get(), 10000, SEEK_SET) != 10000) { return -1.; }
	std::vector<float> pcm(8192); if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; }
	double square = 0.; for (std::size_t index = 0; index < pcm.size(); ++index) { if (!std::isfinite(pcm[index])) { return -1.; } if (index % 2 == 0) { square += double(pcm[index]) * pcm[index]; } }
	return std::sqrt(square / 4096);
}
}
class StudioRenderCancellationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released()
	{
		QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!RemotePlugin::hasRunningRenderExecution());
		QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QVERIFY(!TrackRenderScope::hasActiveSelection()); QVERIFY(!Engine::getSong()->isExporting());
		QVERIFY(Engine::getSong()->exportLoop()); QVERIFY(Engine::getSong()->renderBetweenMarkers()); QCOMPARE(Engine::getSong()->getLoopRenderCount(), 7);
	}
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
		Engine::getSong()->setExportLoop(true); Engine::getSong()->setRenderBetweenMarkers(true); Engine::getSong()->setLoopRenderCount(7); arrange();
	}
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); }
	void cancellationTimerFailuresReturnNormallyAndPermitNativeRetry_data()
	{
		QTest::addColumn<int>("failure"); QTest::addColumn<bool>("stem"); QTest::addColumn<bool>("capability");
		for (int failure = 0; failure < 3; ++failure) for (bool stem : {false, true}) for (bool capability : {false, true})
		{ QTest::newRow(qPrintable(QString::number(failure) + QString::number(stem) + QString::number(capability))) << failure << stem << capability; }
	}
	void cancellationTimerFailuresReturnNormallyAndPermitNativeRetry()
	{
		QFETCH(int, failure); QFETCH(bool, stem); QFETCH(bool, capability); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path));
		const auto gate = std::make_shared<Gate>(); const auto release = qScopeGuard([&] { gate->release(); });
		bool armed = false, duringExecution = false; int callbacks = 0;
		const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] {
			if (!armed && dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev()))
			{
				armed = true; const auto renderers = Engine::audioEngine()->findChildren<ProjectRenderer*>(); QVERIFY(!renderers.isEmpty());
				connect(renderers.back(), &ProjectRenderer::progressChanged, renderers.back(), [gate] { gate->pause(); }, Qt::DirectConnection);
			}
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto cancelled = [&]() -> bool {
			if (gate->waiting.load())
			{
				++callbacks; duringExecution = RemotePlugin::hasRunningRenderExecution(); gate->release();
				if (failure == 1) { throw std::runtime_error("Render cancellation callback fixture"); }
				if (failure == 2) { throw 227; }
				return true;
			}
			return false;
		};
		QJsonObject arguments{{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}}; if (stem) { arguments.insert("track", 0); }
		if (capability)
		{
			const auto response = StudioCapabilities::execute("audio.render", arguments, cancelled); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
			const auto error = response.value("error").toString();
			if (failure == 0) { QVERIFY(error.contains("cancelled")); }
			if (failure == 1) { QCOMPARE(error, QString("Render cancellation callback fixture")); }
			if (failure == 2) { QVERIFY(error.contains("nonstandard exception")); }
		}
		else
		{
			bool caught = false;
			try { StudioAudio::render(arguments, cancelled); }
			catch (const std::runtime_error& error) { QVERIFY(failure != 2); if (failure == 1) { QCOMPARE(QString::fromUtf8(error.what()), QString("Render cancellation callback fixture")); } else { QVERIFY(QString::fromUtf8(error.what()).contains("cancelled")); } caught = true; }
			catch (int value) { QCOMPARE(failure, 2); QCOMPARE(value, 227); caught = true; }
			QVERIFY(caught);
		}
		QVERIFY(armed); QVERIFY(duringExecution); QCOMPARE(callbacks, 1); QCOMPARE(read(path), QByteArray("prior export")); released();
		QVERIFY(QDir(output.path()).entryList({"*.render-*"}, QDir::Files | QDir::Hidden).isEmpty());
		QObject::disconnect(connection);
		const auto retried = StudioCapabilities::execute("audio.render", arguments); QVERIFY2(retried.value("ok").toBool(), qPrintable(retried.value("error").toString()));
		QVERIFY(std::abs(rms(path) - .4 * std::sqrt(.5)) < .04); released();
	}
};
QTEST_GUILESS_MAIN(StudioRenderCancellationTest)
#include "StudioRenderCancellationTest.moc"

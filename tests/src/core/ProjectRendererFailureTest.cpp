/* Native renderer format admission and encoder failure propagation. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AudioFileDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "RenderManager.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "TrackRenderScope.h"

using namespace lmms;
namespace lmms
{
class ProjectRendererFailureTestAccess
{
public:
	static AudioFileDevice* file(ProjectRenderer* renderer) { return renderer->m_fileDev; }
	static bool started(ProjectRenderer* renderer) { return renderer->m_started; }
};
}
namespace
{
using Format = ProjectRenderer::ExportFileFormat;
constexpr std::array<Format, 4> formats{Format::Wave, Format::Flac, Format::Ogg, Format::MP3};
OutputSettings settings() { return OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit); }
bool enabled(Format format) { return ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(format)).isAvailable(); }
QString suffix(Format format) { return ProjectRenderer::getFileExtensionFromFormat(format); }
void codecRows()
{
	QTest::addColumn<int>("format");
	for (const auto format : formats) { if (enabled(format)) { QTest::newRow(qPrintable(suffix(format))) << int(format); } }
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
void arrange()
{
	Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100);
	auto track = new SampleTrack(Engine::getSong());
	auto clip = static_cast<SampleClip*>(track->createClip(TimePos()));
	std::vector<SampleFrame> pcm(44100);
	for (std::size_t frame = 0; frame < pcm.size(); ++frame)
	{
		pcm[frame][0] = .4f * std::sin(6.283185307179586 * 440 * frame / 44100);
		pcm[frame][1] = .2f * std::sin(6.283185307179586 * 660 * frame / 44100);
	}
	clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100));
	clip->changeLength(TimePos(192));
}
double decode(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return -1.; }
	SF_INFO info{};
	std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
	if (!audio || info.channels != 2 || info.samplerate != 44100 || info.frames < 20000 || sf_seek(audio.get(), 10000, SEEK_SET) != 10000) { return -1.; }
	std::vector<float> pcm(8192);
	if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; }
	double square = 0.;
	for (std::size_t index = 0; index < pcm.size(); ++index)
	{
		const auto value = pcm[index]; if (!std::isfinite(value)) { return -1.; }
		if (index % 2 == 0) { square += double(value) * value; }
	}
	return std::sqrt(square / 4096);
}
void refuseInput(ProjectRenderer* renderer) { ProjectRendererFailureTestAccess::file(renderer)->writeBuffer(nullptr, 1); }
bool run(RenderManager& manager)
{
	QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool done = false;
	QObject::connect(&manager, &RenderManager::finished, &loop, [&] { done = true; loop.quit(); });
	QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
	timer.start(30000); manager.renderProject(); if (!done) { loop.exec(); }
	if (!done) { manager.abortProcessing(); }
	return done;
}
class FaultManager final : public RenderManager
{
public:
	FaultManager(Format format, const QString& path, int stage) : RenderManager(settings(), format, path), phase(stage) { }
	bool armed = true;
	const int phase;
	std::shared_ptr<std::atomic_bool> fired = std::make_shared<std::atomic_bool>(false);
protected:
	std::shared_ptr<ProjectRenderer> createRenderer(const OutputSettings& config, Format format, const QString& path) override
	{
		auto renderer = RenderManager::createRenderer(config, format, path);
		if (armed && renderer && renderer->isReady())
		{
			if (phase == 0) { refuseInput(renderer.get()); fired->store(true); }
			if (phase == 2)
			{
				const QPointer<ProjectRenderer> expected(renderer.get()); const auto observed = fired;
				connect(renderer.get(), &ProjectRenderer::progressChanged, renderer.get(), [expected, observed] {
					if (!observed->exchange(true)) { refuseInput(expected.data()); }
				}, Qt::DirectConnection);
			}
		}
		return renderer;
	}
};
}

class ProjectRendererFailureTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released()
	{
		QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		QVERIFY(!TrackRenderScope::hasActiveSelection()); QVERIFY(!Engine::getSong()->isExporting());
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::getSong()->setLoadOnLaunch(false); QVERIFY(enabled(Format::Wave)); QVERIFY(enabled(Format::Flac));
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice());
		Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1);
	}
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); released(); }

	void invalidFormatsPreserveExistingOutput_data()
	{
		QTest::addColumn<int>("format");
		for (const auto value : {-1, int(Format::Count), int(Format::Count) + 1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()})
		{ QTest::newRow(qPrintable(QString::number(value))) << value; }
	}
	void invalidFormatsPreserveExistingOutput()
	{
		QFETCH(int, format); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path, "prior export"));
		const auto kind = static_cast<Format>(format); QVERIFY(suffix(kind).isEmpty());
		{ ProjectRenderer renderer(settings(), kind, path); QVERIFY(!renderer.isReady()); QVERIFY(!renderer.startProcessing()); QVERIFY(!renderer.finalizeOutput()); }
		QCOMPARE(read(path), QByteArray("prior export")); arrange();
		{ RenderManager manager(settings(), kind, path); QVERIFY(run(manager)); QVERIFY(!manager.errorString().isEmpty()); }
		QCOMPARE(read(path), QByteArray("prior export"));
		QVERIFY(QDir(output.path()).entryList({".lmms-export-*"}, QDir::Files | QDir::Hidden).isEmpty()); released();
	}
	void enabledFormatConstructionStillUsesNativeDevices_data() { codecRows(); }
	void enabledFormatConstructionStillUsesNativeDevices()
	{
		QFETCH(int, format); const auto kind = static_cast<Format>(format); QTemporaryDir output;
		ProjectRenderer renderer(settings(), kind, output.filePath("native" + suffix(kind)));
		QVERIFY(renderer.isReady()); QVERIFY(ProjectRendererFailureTestAccess::file(&renderer)); QVERIFY(!renderer.hasEncodingFailure());
		QVERIFY(!ProjectRendererFailureTestAccess::file(&renderer)->hasWriteError()); QVERIFY(!renderer.finalizeOutput());
	}
	void disabledOptionalFormatsRefuseWithoutOpeningOutput()
	{
		QTemporaryDir output; const auto path = output.filePath("prior.audio"); QVERIFY(write(path, "prior export"));
		for (const auto kind : {Format::Ogg, Format::MP3})
		{
			if (enabled(kind)) { continue; }
			ProjectRenderer renderer(settings(), kind, path); QVERIFY(!renderer.isReady()); QVERIFY(!renderer.startProcessing());
			QCOMPARE(read(path), QByteArray("prior export"));
		}
	}
	void failedEncoderRefusesBeforeDeviceInstallation_data() { codecRows(); }
	void failedEncoderRefusesBeforeDeviceInstallation()
	{
		QFETCH(int, format); arrange(); QTemporaryDir output; const auto original = Engine::audioEngine()->audioDev();
		{
			ProjectRenderer renderer(settings(), static_cast<Format>(format), output.filePath("failed" + suffix(static_cast<Format>(format))));
			QVERIFY(renderer.isReady()); QSignalSpy started(&renderer, &QThread::started); refuseInput(&renderer);
			QVERIFY(!renderer.startProcessing()); QVERIFY(!renderer.startProcessing()); QVERIFY(renderer.hasEncodingFailure());
			QVERIFY(!ProjectRendererFailureTestAccess::started(&renderer)); QCOMPARE(started.size(), 0);
			QCOMPARE(Engine::audioEngine()->audioDev(), original); QVERIFY(!renderer.finalizeOutput());
		}
		released();
	}
	void setupEncoderFailureRefusesWorkerStartup_data() { codecRows(); }
	void setupEncoderFailureRefusesWorkerStartup()
	{
		QFETCH(int, format); arrange(); QTemporaryDir output; auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice());
		const auto restore = qScopeGuard([&] { engine->restoreAudioDevice(); });
		{
			ProjectRenderer renderer(settings(), static_cast<Format>(format), output.filePath("setup" + suffix(static_cast<Format>(format))));
			QVERIFY(renderer.isReady()); QSignalSpy started(&renderer, &QThread::started); bool fired = false;
			const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] {
				if (!fired && engine->audioDev() == ProjectRendererFailureTestAccess::file(&renderer)) { fired = true; refuseInput(&renderer); }
			}, Qt::DirectConnection);
			const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			QVERIFY(!renderer.startProcessing()); QVERIFY(fired); QVERIFY(renderer.hasEncodingFailure());
			QVERIFY(!ProjectRendererFailureTestAccess::started(&renderer)); QCOMPARE(started.size(), 0); QVERIFY(!Engine::getSong()->isExporting());
			QVERIFY(!renderer.finalizeOutput()); QVERIFY(engine->restoreAudioDevice()); QVERIFY(renderer.hasEncodingFailure());
		}
		released();
	}
	void workerEncoderFailureStopsWithoutFurtherProgress_data() { codecRows(); }
	void workerEncoderFailureStopsWithoutFurtherProgress()
	{
		QFETCH(int, format); arrange(); QTemporaryDir output; auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice());
		const auto restore = qScopeGuard([&] { engine->restoreAudioDevice(); });
		{
			ProjectRenderer renderer(settings(), static_cast<Format>(format), output.filePath("worker" + suffix(static_cast<Format>(format))));
			QVERIFY(renderer.isReady()); std::atomic_int progress{0};
			connect(&renderer, &ProjectRenderer::progressChanged, &renderer, [&] { ++progress; refuseInput(&renderer); }, Qt::DirectConnection);
			renderer.setExecutionTimeout(30000); QVERIFY(renderer.startProcessing()); QVERIFY(renderer.wait(30000));
			QCOMPARE(progress.load(), 1); QVERIFY(renderer.hasEncodingFailure()); QVERIFY(!renderer.hasExecutionFailure());
			QVERIFY(!renderer.wasAborted()); QVERIFY(!renderer.finalizeOutput()); QVERIFY(!renderer.finalizeOutput());
			QVERIFY(engine->restoreAudioDevice()); QVERIFY(renderer.hasEncodingFailure());
		}
		released();
	}
	void refusalBeforeFinalizationIsRetainedAfterDeviceRecovery_data() { codecRows(); }
	void refusalBeforeFinalizationIsRetainedAfterDeviceRecovery()
	{
		QFETCH(int, format); arrange(); QTemporaryDir output; auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice());
		const auto restore = qScopeGuard([&] { engine->restoreAudioDevice(); });
		{
			ProjectRenderer renderer(settings(), static_cast<Format>(format), output.filePath("finalize" + suffix(static_cast<Format>(format))));
			QVERIFY(renderer.isReady()); renderer.setExecutionTimeout(30000); QVERIFY(renderer.startProcessing()); QVERIFY(renderer.wait(30000));
			QVERIFY(!renderer.hasEncodingFailure()); refuseInput(&renderer); QVERIFY(!renderer.finalizeOutput());
			QVERIFY(renderer.hasEncodingFailure()); QVERIFY(!renderer.hasExecutionFailure()); QVERIFY(engine->restoreAudioDevice());
			QVERIFY(renderer.hasEncodingFailure());
		}
		released();
	}
	void managerFailurePreservesDestinationAndCanRetry_data()
	{
		QTest::addColumn<int>("format"); QTest::addColumn<int>("phase");
		for (const auto kind : formats) { if (enabled(kind)) { for (int phase = 0; phase < 3; ++phase) { QTest::newRow(qPrintable(suffix(kind) + QString::number(phase))) << int(kind) << phase; } } }
	}
	void managerFailurePreservesDestinationAndCanRetry()
	{
		QFETCH(int, format); QFETCH(int, phase); arrange(); QTemporaryDir output; const auto kind = static_cast<Format>(format);
		const auto path = output.filePath("prior" + suffix(kind)); QVERIFY(write(path, "prior export")); FaultManager manager(kind, path, phase);
		const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] {
			if (phase == 1 && manager.armed) { if (auto device = dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { device->writeBuffer(nullptr, 1); manager.fired->store(true); } }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(run(manager)); QVERIFY(manager.fired->load()); QVERIFY(manager.errorString().contains("encoding"));
		QCOMPARE(read(path), QByteArray("prior export")); released();
		QVERIFY(QDir(output.path()).entryList({".lmms-export-*"}, QDir::Files | QDir::Hidden).isEmpty());
		manager.armed = false; QVERIFY(run(manager)); QVERIFY2(manager.errorString().isEmpty(), qPrintable(manager.errorString()));
		QVERIFY2(std::abs(decode(path) - .4 * std::sqrt(.5)) < .04, "Enabled native codec retry lacks independent decoded PCM proof"); released();
	}
	void studioFailureReturnsNoArtifactAndCanRetry_data() { codecRows(); }
	void studioFailureReturnsNoArtifactAndCanRetry()
	{
		QFETCH(int, format); arrange(); QTemporaryDir output; const auto kind = static_cast<Format>(format);
		const auto path = output.filePath("prior" + suffix(kind)); QVERIFY(write(path, "prior export")); bool fired = false;
		const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] {
			if (!fired) { if (auto device = dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { fired = true; device->writeBuffer(nullptr, 1); } }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QJsonObject arguments{{"path", path}, {"sampleRate", 44100}, {"timeoutMs", 30000}};
		if (kind == Format::Wave || kind == Format::Flac) { arguments.insert("bitDepth", 24); }
		const auto response = StudioCapabilities::execute("audio.render", arguments);
		QVERIFY(fired); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		QVERIFY(response.value("error").toString().contains("encoding")); QCOMPARE(read(path), QByteArray("prior export")); released();
		QVERIFY(QDir(output.path()).entryList({"*.render-*"}, QDir::Files | QDir::Hidden).isEmpty());
		const auto retried = StudioCapabilities::execute("audio.render", arguments); QVERIFY2(retried.value("ok").toBool(), qPrintable(retried.value("error").toString()));
		QVERIFY2(std::abs(decode(path) - .4 * std::sqrt(.5)) < .04, "Enabled native codec studio retry lacks independent decoded PCM proof"); released();
	}
};
QTEST_GUILESS_MAIN(ProjectRendererFailureTest)
#include "ProjectRendererFailureTest.moc"

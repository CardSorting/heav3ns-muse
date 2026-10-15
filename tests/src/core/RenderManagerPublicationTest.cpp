/* Native per-file export publication. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AudioDeviceTransitionTestUtils.h"
#include "AudioFileDevice.h"
#include "AudioFileWave.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
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
class RenderManagerPublicationTestAccess
{
public:
	static bool publishing(RenderManager* manager) { return manager->m_finishing && manager->m_outputFinalized && !manager->m_deviceStored; }
};
}
namespace
{
OutputSettings settings(OutputSettings::BitDepth depth = OutputSettings::BitDepth::Depth24Bit) { return OutputSettings(44100, 160, depth); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
void arrange(bool second = false) { Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100); for (int index = 0; index < (second ? 2 : 1); ++index) { auto track = new SampleTrack(Engine::getSong()); track->setName(index ? "Other" : "Source"); auto clip = static_cast<SampleClip*>(track->createClip(TimePos())); clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(35280, SampleFrame(.125f * (index + 1))), 44100)); clip->changeLength(TimePos(192)); } }
double decode(const QString& path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return -1.; } SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close); if (!audio || info.channels != 2 || info.frames < 10000 || sf_seek(audio.get(), 1000, SEEK_SET) != 1000) { return -1.; } std::vector<float> pcm(8192); if (sf_readf_float(audio.get(), pcm.data(), 4096) != 4096 || sf_error(audio.get())) { return -1.; } double sum = 0; for (auto value : pcm) { if (!std::isfinite(value)) { return -1.; } sum += value; } return sum / pcm.size(); }
bool run(const QPointer<RenderManager>& manager, bool tracks = false) { QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool done = false; QObject::connect(manager.data(), &RenderManager::finished, &loop, [&] { done = true; loop.quit(); }); QObject::connect(manager.data(), &QObject::destroyed, &loop, [&] { done = true; loop.quit(); }); QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(30000); if (tracks) { manager->renderTracks(); } else { manager->renderProject(); } if (!done) { loop.exec(); } if (!done && manager) { manager->abortProcessing(); } return done; }
bool noStaging(const QTemporaryDir& output) { return QDir(output.path()).entryList({".lmms-export-*"}, QDir::Files | QDir::Hidden).isEmpty(); }
class RawFile final : public AudioFileDevice
{
public:
	RawFile(const QString& path) : AudioFileDevice(settings(), DEFAULT_CHANNELS, path, Engine::audioEngine()) { }
	int append() { return writeData("x", 1); }
	void writeBuffer(const SampleFrame*, f_cnt_t) override { }
};
}
class RenderManagerPublicationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released() { QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QVERIFY(!TrackRenderScope::hasActiveSelection()); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); released(); }
	void successfulNativeOutputReplacesTheDestination_data()
	{ QTest::addColumn<bool>("tracks"); QTest::newRow("project") << false; QTest::newRow("stem") << true; }
	void successfulNativeOutputReplacesTheDestination()
	{
		QFETCH(bool, tracks); arrange(); QTemporaryDir output; const auto path = output.filePath(tracks ? "1_Source.wav" : "existing.wav"); QVERIFY(write(path, "prior audio")); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, tracks ? output.path() : path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); bool untouchedDuringSetup = false; const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] { if (dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { untouchedDuringSetup = read(path) == "prior audio"; } }, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(run(manager, tracks)); QVERIFY(untouchedDuringSetup); QVERIFY2(manager->errorString().isEmpty(), qPrintable(manager->errorString())); QVERIFY(std::abs(decode(path) - .125) < .01); QVERIFY(noStaging(output)); released();
	}
	void setupExceptionsPreserveTheDestination_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void setupExceptionsPreserveTheDestination()
	{
		QFETCH(bool, nonstandard); arrange(); QTemporaryDir output; const auto path = output.filePath("existing.wav"); QVERIFY(write(path, "prior audio")); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); AudioDeviceTransitionProbe transitionProbe(Engine::audioEngine(), [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; } if (dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { if (nonstandard) { throw 197; } throw std::runtime_error("Publication setup fixture"); } }); QVERIFY(run(manager)); QVERIFY(!manager->errorString().isEmpty()); QCOMPARE(read(path), QByteArray("prior audio")); QVERIFY(noStaging(output)); released();
	}
	void dspRefusalDoesNotTruncateTheDestination()
	{
		arrange(); QTemporaryDir output; const auto path = output.filePath("existing.wav"); QVERIFY(write(path, "prior audio")); auto engine = Engine::audioEngine(); const auto reset = qScopeGuard([&] { engine->resetDspFailure(); }); engine->reportDspFailure(AudioEngine::DspFailure::NonfiniteOutput); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QVERIFY(run(manager)); QVERIFY(manager->errorString().contains("DSP")); QCOMPARE(read(path), QByteArray("prior audio")); QVERIFY(noStaging(output)); released(); QVERIFY(engine->resetDspFailure());
	}
	void publicationCancellationAndExceptionsPreserveTheDestination_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void publicationCancellationAndExceptionsPreserveTheDestination()
	{
		QFETCH(int, failure); arrange(); QTemporaryDir output; const auto path = output.filePath("existing.wav"); QVERIFY(write(path, "prior audio")); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); bool fired = false; { FileResourceIO::Scope work([&] { if (!fired && manager && RenderManagerPublicationTestAccess::publishing(manager)) { fired = true; if (failure == 0) { manager->abortProcessing(); } if (failure == 1) { throw std::runtime_error("Publication callback fixture"); } if (failure == 2) { throw 199; } } return false; }); QVERIFY(run(manager)); } QVERIFY(fired); QVERIFY(!manager->errorString().isEmpty()); QCOMPARE(read(path), QByteArray("prior audio")); QVERIFY(noStaging(output)); released();
	}
	void publicationCanReturnAfterManagerRetirementWithoutCommitting()
	{
		arrange(); QTemporaryDir output; const auto path = output.filePath("existing.wav"); QVERIFY(write(path, "prior audio")); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); bool fired = false; { FileResourceIO::Scope work([&] { if (!fired && manager && RenderManagerPublicationTestAccess::publishing(manager)) { fired = true; delete manager.data(); } return false; }); QVERIFY(run(manager)); } QVERIFY(fired); QVERIFY(!manager); QCOMPARE(read(path), QByteArray("prior audio")); QVERIFY(noStaging(output)); released();
	}
	void restorationFailurePreservesOutputUntilExplicitRecoveryAndRetry()
	{
		arrange(); QTemporaryDir output; const auto path = output.filePath("existing.wav"); QVERIFY(write(path, "prior audio")); auto engine = Engine::audioEngine(); auto original = engine->audioDev(); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::RestoreAfterRate) { return; } if (engine->audioDev() == original) { throw std::runtime_error("Publish restoration fixture"); } }); QVERIFY(run(manager)); QVERIFY(!manager->errorString().isEmpty()); QCOMPARE(read(path), QByteArray("prior audio")); QVERIFY(engine->hasStoredAudioDevice()); transitionProbe.reset(); QVERIFY(engine->restoreAudioDevice()); QVERIFY(run(manager)); QVERIFY(manager->errorString().isEmpty()); QVERIFY(std::abs(decode(path) - .125) < .01); QVERIFY(noStaging(output)); released();
	}
	void aLaterStemFailureDoesNotUndoAnEarlierCommittedStem()
	{
		arrange(true); QTemporaryDir output; const auto first = output.filePath("1_Source.wav"), second = output.filePath("2_Other.wav"); QVERIFY(write(first, "prior first")); QVERIFY(write(second, "prior second")); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, output.path())); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); int setups = 0; AudioDeviceTransitionProbe transitionProbe(Engine::audioEngine(), [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; } if (dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev()) && ++setups == 2) { throw std::runtime_error("Second stem setup fixture"); } }); QVERIFY(run(manager, true)); QCOMPARE(setups, 2); QVERIFY(!manager->errorString().isEmpty()); QCOMPARE(read(first), QByteArray("prior first")); QVERIFY(std::abs(decode(second) - .25) < .01); QVERIFY(noStaging(output)); released();
	}
	void waveFinalizationClosesTheFileAndIsIdempotent_data()
	{ QTest::addColumn<int>("depth"); QTest::newRow("16-bit") << 0; QTest::newRow("24-bit") << 1; QTest::newRow("32-bit") << 2; }
	void waveFinalizationClosesTheFileAndIsIdempotent()
	{
		QFETCH(int, depth); QTemporaryDir output; const auto path = output.filePath("native.wav"), moved = output.filePath("closed.wav"); bool good = false; std::unique_ptr<AudioFileDevice> device(AudioFileWave::getInst(path, settings(static_cast<OutputSettings::BitDepth>(depth)), DEFAULT_CHANNELS, Engine::audioEngine(), good)); QVERIFY(good); std::vector<SampleFrame> pcm(35280, SampleFrame(.375f)); device->writeBuffer(pcm.data(), pcm.size()); QVERIFY(device->finalizeOutput()); QVERIFY(device->finalizeOutput()); QVERIFY(QFile::rename(path, moved)); QVERIFY(std::abs(decode(moved) - .375) < .01); QVERIFY(device->finalizeOutput());
	}
	void publicationCannotReplaceADirectoryWithUserContent()
	{
		arrange(); QTemporaryDir output; const auto path = output.filePath("existing.wav"), marker = path + "/user.txt"; QVERIFY(QDir().mkpath(path)); QVERIFY(write(marker, "user data")); const QPointer<RenderManager> manager(new RenderManager(settings(), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); QVERIFY(run(manager)); QVERIFY(!manager->errorString().isEmpty()); QVERIFY(QDir(path).exists()); QCOMPARE(read(marker), QByteArray("user data")); QVERIFY(noStaging(output)); released();
	}
	void writesAfterBaseFinalizationRemainErrors()
	{
		QTemporaryDir output; RawFile file(output.filePath("raw.bin")); QCOMPARE(file.append(), 1); QVERIFY(file.finalizeOutput()); QVERIFY(file.finalizeOutput()); QCOMPARE(file.append(), -1); QVERIFY(file.hasWriteError()); QVERIFY(!file.finalizeOutput());
	}
};
QTEST_GUILESS_MAIN(RenderManagerPublicationTest)
#include "RenderManagerPublicationTest.moc"

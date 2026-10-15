/* Native studio render ownership across returned callbacks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <cmath>
#include <cstdint>
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
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "TrackRenderScope.h"
using namespace lmms;
namespace
{
using Format = ProjectRenderer::ExportFileFormat;
OutputSettings settings() { return OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QString digest(const QString& path) { return QString::fromLatin1(QCryptographicHash::hash(read(path), QCryptographicHash::Sha256).toHex()); }
void preferences(bool loop, bool markers, int count) { auto song = Engine::getSong(); song->setExportLoop(loop); song->setRenderBetweenMarkers(markers); song->setLoopRenderCount(count); }
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
QJsonObject arguments(const QString& path, bool stem = false)
{
	QJsonObject result{{"path", path}, {"sampleRate", 44100}, {"timeoutMs", 30000}};
	const auto extension = QFileInfo(path).suffix(); if (extension == "wav" || extension == "flac") { result.insert("bitDepth", 24); }
	if (stem) { result.insert("track", 0); } return result;
}
AudioFileDevice* newerOutput(const QString& path)
{
	auto engine = Engine::audioEngine(); if (!engine->storeAudioDevice()) { return nullptr; }
	bool good = false; std::unique_ptr<AudioFileDevice> device(AudioFileWave::getInst(path, settings(), DEFAULT_CHANNELS, engine, good));
	if (!good) { return nullptr; } auto installed = device.get(); engine->setAudioDevice(installed, false); device.release();
	const std::vector<SampleFrame> pcm(44100, SampleFrame(.375f)); installed->writeBuffer(pcm.data(), pcm.size());
	if (!installed->finalizeOutput()) { return nullptr; } preferences(false, false, 23); return installed;
}
bool noStaging(const QTemporaryDir& output) { return QDir(output.path()).entryList({"*.render-*"}, QDir::Files | QDir::Hidden).isEmpty(); }
}
class StudioRenderOwnershipTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void released() { QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!RemotePlugin::hasRunningRenderExecution()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); QVERIFY(!TrackRenderScope::hasActiveSelection()); QVERIFY(!Engine::getSong()->isExporting()); }
	void originalPreferences() { QVERIFY(Engine::getSong()->exportLoop()); QVERIFY(Engine::getSong()->renderBetweenMarkers()); QCOMPARE(Engine::getSong()->getLoopRenderCount(), 7); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
		QVERIFY(ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(Format::Wave)).isAvailable());
		QVERIFY(ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(Format::Flac)).isAvailable());
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setProjectFileName(""); preferences(true, true, 7); arrange(); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); released(); }
	void successfulNativeResultsRestoreSettings_data()
	{
		QTest::addColumn<int>("format"); QTest::addColumn<bool>("stem");
		for (const auto kind : {Format::Wave, Format::Flac, Format::Ogg, Format::MP3})
		{
			if (!ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(kind)).isAvailable()) { continue; }
			for (const bool stem : {false, true}) { QTest::newRow(qPrintable(ProjectRenderer::getFileExtensionFromFormat(kind) + (stem ? "-stem" : "-project"))) << int(kind) << stem; }
		}
	}
	void successfulNativeResultsRestoreSettings()
	{
		QFETCH(int, format); QFETCH(bool, stem); QTemporaryDir output;
		const auto path = output.filePath("accepted" + ProjectRenderer::getFileExtensionFromFormat(static_cast<Format>(format)));
		const auto response = StudioCapabilities::execute("audio.render", arguments(path, stem)); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		const auto result = response.value("result").toObject(); QCOMPARE(result.value("sha256").toString(), digest(path));
		QVERIFY(stem ? result.value("stem").isObject() : result.value("stem").isNull());
		QVERIFY2(std::abs(rms(path) - .4 * std::sqrt(.5)) < .04, "Enabled native codec output lacks independent PCM proof");
		QVERIFY(noStaging(output)); originalPreferences(); released();
	}
	void changedProjectRefusesPublicationAndKeepsNewPreferences_data()
	{
		QTest::addColumn<bool>("clear"); QTest::addColumn<bool>("publication");
		for (bool clear : {false, true}) for (bool publication : {false, true}) { QTest::newRow(qPrintable(QString::number(clear) + QString::number(publication))) << clear << publication; }
	}
	void changedProjectRefusesPublicationAndKeepsNewPreferences()
	{
		QFETCH(bool, clear); QFETCH(bool, publication); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path, "prior export"));
		auto engine = Engine::audioEngine(); auto original = engine->audioDev(); bool sawFile = false, restored = false, fired = false;
		const auto mutate = [&] { if (fired) { return; } fired = true; FileResourceIO::Suspension work; if (clear) { Engine::getSong()->clearProject(); } else { Engine::getSong()->setProjectFileName("replacement.mmp"); } preferences(false, false, 19); };
		const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] {
			if (dynamic_cast<AudioFileDevice*>(engine->audioDev())) { sawFile = true; }
			if (sawFile && engine->audioDev() == original) { restored = true; if (!publication) { mutate(); } }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("audio.render", arguments(path), [&] { if (publication && restored) { mutate(); } return false; });
		QVERIFY(fired); QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("superseded"));
		QCOMPARE(read(path), QByteArray("prior export")); QVERIFY(!Engine::getSong()->exportLoop()); QVERIFY(!Engine::getSong()->renderBetweenMarkers()); QCOMPARE(Engine::getSong()->getLoopRenderCount(), 19);
		QVERIFY(noStaging(output)); released();
	}
	void publicationCannotRestoreOrDeleteANewerReservation_data()
	{
		QTest::addColumn<bool>("reuseStage"); QTest::addColumn<bool>("outerScope");
		for (bool reuse : {false, true}) for (bool outer : {false, true}) { QTest::newRow(qPrintable(QString::number(reuse) + QString::number(outer))) << reuse << outer; }
	}
	void publicationCannotRestoreOrDeleteANewerReservation()
	{
		QFETCH(bool, reuseStage); QFETCH(bool, outerScope); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path, "prior export"));
		auto engine = Engine::audioEngine(); QString staging, replacementPath; AudioFileDevice* replacement = nullptr; bool fired = false; std::uint64_t newer = 0;
		const auto recover = qScopeGuard([&] { FileResourceIO::Suspension work; engine->restoreAudioDevice(); });
		const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] { if (staging.isEmpty()) { if (auto device = dynamic_cast<AudioFileDevice*>(engine->audioDev())) { staging = device->outputFile(); } } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto mutate = [&] {
			if (!fired && !staging.isEmpty() && !engine->hasStoredAudioDevice()) { fired = true; replacementPath = reuseStage ? staging : output.filePath("newer.wav"); replacement = newerOutput(replacementPath); newer = engine->audioDeviceReservationGeneration(); }
			return false;
		};
		QJsonObject response;
		{ FileResourceIO::Scope work([&] { return outerScope ? mutate() : false; }); response = StudioCapabilities::execute("audio.render", arguments(path), [&] { return outerScope ? false : mutate(); }); }
		QVERIFY(fired); QVERIFY(replacement); QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("superseded"));
		QCOMPARE(read(path), QByteArray("prior export")); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(replacement)); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(engine->audioDeviceReservationGeneration(), newer);
		QCOMPARE(Engine::getSong()->getLoopRenderCount(), 23); QVERIFY(QFile::exists(staging)); QVERIFY(QFile::exists(replacementPath));
		QVERIFY(engine->restoreAudioDevice()); QVERIFY(std::abs(rms(replacementPath) - .375) < .01); released();
	}
	void rendererDestructionCannotStealANewerReservation_data()
	{ QTest::addColumn<bool>("reuseStage"); QTest::newRow("new-path") << false; QTest::newRow("old-stage") << true; }
	void rendererDestructionCannotStealANewerReservation()
	{
		QFETCH(bool, reuseStage); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path, "prior export"));
		auto engine = Engine::audioEngine(); bool armed = false, fired = false; QString replacementPath; AudioFileDevice* replacement = nullptr;
		const auto recover = qScopeGuard([&] { FileResourceIO::Suspension work; engine->restoreAudioDevice(); });
		AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::SetupAfterRate) { return; }
			if (!armed) { if (auto device = dynamic_cast<AudioFileDevice*>(engine->audioDev())) {
				armed = true; const auto staging = device->outputFile(); const auto renderers = engine->findChildren<ProjectRenderer*>(); QVERIFY(!renderers.isEmpty());
				connect(renderers.back(), &QObject::destroyed, this, [&, staging] { QVERIFY(engine->restoreAudioDevice()); replacementPath = reuseStage ? staging : output.filePath("newer.wav"); replacement = newerOutput(replacementPath); fired = true; }, Qt::DirectConnection);
				throw std::runtime_error("Studio ownership setup fixture");
			} }
		});
		const auto response = StudioCapabilities::execute("audio.render", arguments(path)); QVERIFY(armed && fired); QVERIFY(replacement); QVERIFY(!response.value("ok").toBool());
		QCOMPARE(read(path), QByteArray("prior export")); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(replacement)); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(Engine::getSong()->getLoopRenderCount(), 23);
		QVERIFY(engine->restoreAudioDevice()); QVERIFY(std::abs(rms(replacementPath) - .375) < .01); released();
	}
	void publicationInterruptionsPreserveDestinationAndRecover_data()
	{
		QTest::addColumn<int>("failure"); QTest::addColumn<bool>("stem");
		for (int failure = 0; failure < 3; ++failure) for (bool stem : {false, true}) { QTest::newRow(qPrintable(QString::number(failure) + QString::number(stem))) << failure << stem; }
	}
	void publicationInterruptionsPreserveDestinationAndRecover()
	{
		QFETCH(int, failure); QFETCH(bool, stem); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path, "prior export"));
		bool installed = false, fired = false; const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] { if (dynamic_cast<AudioFileDevice*>(Engine::audioEngine()->audioDev())) { installed = true; } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("audio.render", arguments(path, stem), [&] {
			if (installed && !Engine::audioEngine()->hasStoredAudioDevice()) { fired = true; if (failure == 0) { return true; } if (failure == 1) { throw std::runtime_error("Studio publication fixture"); } throw 211; } return false;
		});
		QVERIFY(fired); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QCOMPARE(read(path), QByteArray("prior export"));
		QVERIFY(noStaging(output)); originalPreferences(); released();
	}
	void restorationFailuresRetainTheOwnedRecoveryPath_data()
	{ QTest::addColumn<bool>("persistent"); QTest::newRow("transient") << false; QTest::newRow("persistent") << true; }
	void restorationFailuresRetainTheOwnedRecoveryPath()
	{
		QFETCH(bool, persistent); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path, "prior export"));
		auto engine = Engine::audioEngine(); auto original = engine->audioDev(); bool installed = false; int failures = 0;
		const auto recover = qScopeGuard([&] { FileResourceIO::Suspension work; engine->restoreAudioDevice(); });
		AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) {
			if (point == DeviceTransitionPoint::SetupAfterRate) { installed = true; }
			else if (point == DeviceTransitionPoint::RestoreAfterRate && installed && engine->audioDev() == original && (persistent || failures == 0)) { ++failures; throw std::runtime_error("Studio restore fixture"); }
		});
		const auto response = StudioCapabilities::execute("audio.render", arguments(path)); QVERIFY(!response.value("ok").toBool()); QVERIFY(failures > 0); QCOMPARE(read(path), QByteArray("prior export"));
		QCOMPARE(engine->hasStoredAudioDevice(), persistent); originalPreferences(); QVERIFY(noStaging(output)); transitionProbe.reset(); QVERIFY(engine->restoreAudioDevice()); released();
	}
	void rendererDestructionAfterCommitKeepsAcceptedAndNewOutput()
	{
		QTemporaryDir output; const auto path = output.filePath("accepted.wav"), next = output.filePath("newer.wav"); auto engine = Engine::audioEngine(); bool armed = false; AudioFileDevice* replacement = nullptr;
		const auto recover = qScopeGuard([&] { FileResourceIO::Suspension work; engine->restoreAudioDevice(); });
		const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] {
			if (!armed && dynamic_cast<AudioFileDevice*>(engine->audioDev())) { armed = true; const auto renderers = engine->findChildren<ProjectRenderer*>(); QVERIFY(!renderers.isEmpty()); connect(renderers.back(), &QObject::destroyed, this, [&] { replacement = newerOutput(next); }, Qt::DirectConnection); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("audio.render", arguments(path)); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(replacement);
		QCOMPARE(response.value("result").toObject().value("sha256").toString(), digest(path)); QVERIFY(std::abs(rms(path) - .4 * std::sqrt(.5)) < .04);
		QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(replacement)); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(Engine::getSong()->getLoopRenderCount(), 23);
		QVERIFY(engine->restoreAudioDevice()); QVERIFY(std::abs(rms(next) - .375) < .01); released();
	}
	void stemAdmissionChangesCannotBeAdoptedByTheOldScope_data()
	{ QTest::addColumn<bool>("reservation"); QTest::newRow("project") << false; QTest::newRow("reservation") << true; }
	void stemAdmissionChangesCannotBeAdoptedByTheOldScope()
	{
		QFETCH(bool, reservation); QTemporaryDir output; const auto path = output.filePath("prior.wav"); QVERIFY(write(path, "prior export")); bool fired = false;
		auto engine = Engine::audioEngine(); std::uint64_t newer = 0; bool accepted = false;
		const auto recover = qScopeGuard([&] { FileResourceIO::Suspension work; engine->restoreAudioDevice(); });
		QJsonObject response; { FileResourceIO::Scope work([&] { if (!fired) { fired = true; if (reservation) { accepted = engine->storeAudioDevice(); newer = engine->audioDeviceReservationGeneration(); } else { Engine::getSong()->setProjectFileName("changed-during-studio-admission.mmp"); } } return false; }); response = StudioCapabilities::execute("audio.render", arguments(path, true)); }
		QVERIFY(fired); QVERIFY(!response.value("ok").toBool()); QCOMPARE(read(path), QByteArray("prior export")); QVERIFY(noStaging(output));
		if (reservation) { QVERIFY(accepted); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(engine->audioDeviceReservationGeneration(), newer); QVERIFY(engine->restoreAudioDevice()); }
		originalPreferences(); released();
	}
};
QTEST_GUILESS_MAIN(StudioRenderOwnershipTest)
#include "StudioRenderOwnershipTest.moc"

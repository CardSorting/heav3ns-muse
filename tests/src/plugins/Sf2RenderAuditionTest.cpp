#include "../core/NativeSongTestAccess.h"
/* Native SoundFont audition exclusion during offline rendering. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sndfile.h>
#include <fluidsynth.h>
#include "AudioEngine.h"
#include "AudioDummy.h"
#include "Timeline.h"
#include "AudioFileWave.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "OutputSettings.h"
#include "PatternStore.h"
#include "PluginFactory.h"
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "RenderManager.h"
#include "Sf2Player.h"
#include "Song.h"
#include "../core/SongExportTransitionTestUtils.h"
#include "StudioAudio.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "TrackRenderScope.h"
using namespace lmms;
namespace lmms
{
class Sf2RenderAuditionTestAccess
{
public:
	static std::pair<int, int> selection(Sf2Instrument* native) { QMutexLocker lock(&native->m_synthMutex); int font = -1, bank = -1, program = -1; if (!native->m_synth || fluid_synth_get_program(native->m_synth, native->m_channel, &font, &bank, &program) != FLUID_OK || font < 0 || program < 0 || program > 127) { return {-1, -1}; } return {bank, program}; }
	static QString snapshotPath(Sf2Instrument* native) { return native->m_bankSnapshot ? native->m_bankSnapshot->path() : QString{}; }
};
}
namespace
{
class NativeTrack final : public InstrumentTrack
{
public:
	NativeTrack(TrackContainer* owner) : InstrumentTrack(owner) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new Sf2Instrument(this); }
};
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
bool native(const QString& op, const QJsonObject& args) { return StudioCapabilities::execute(op, args).value("ok").toBool(); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
std::array<double, 2> measure(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return {}; } SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close); if (!audio || info.channels != 2 || info.samplerate < 1 || info.frames < info.samplerate / 2) { return {}; }
	const auto begin = info.samplerate / 10, count = info.samplerate / 2; if (sf_seek(audio.get(), begin, SEEK_SET) != begin) { return {}; } std::vector<float> pcm(count * info.channels); if (sf_readf_float(audio.get(), pcm.data(), count) != count || sf_error(audio.get())) { return {}; }
	double square = 0; std::vector<int> crossings; float previous = pcm[0]; for (int frame = 0; frame < count; ++frame) { const float value = pcm[frame * info.channels]; if (!std::isfinite(value)) { throw std::runtime_error("Nonfinite native rendered SoundFont output"); } square += double(value) * value; if (frame && previous <= 0 && value > 0) { crossings.push_back(frame); } previous = value; }
	const double pitch = crossings.size() > 1 ? double(info.samplerate) * (crossings.size() - 1) / (crossings.back() - crossings.front()) : 0.; return {std::sqrt(square / count), pitch};
}
}
class Sf2RenderAuditionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QString bank() const { return QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2"; }
	Sf2Instrument* seed(TrackContainer* owner = nullptr) { auto track = new NativeTrack(owner ? owner : static_cast<TrackContainer*>(Engine::getSong())); auto object = dynamic_cast<Sf2Instrument*>(track->loadInstrument("sf2player")); QString error; if (!object || !object->setFileResource("soundfont", bank(), error)) { throw std::runtime_error("Native SoundFont render fixture could not load"); } return object; }
	bool arrange() { return native("song.set", {{"tempo", 240}, {"masterVolume", 100}}) && native("clip.add", {{"track", 0}, {"position", 0}}) && native("note.add", {{"track", 0}, {"clip", 0}, {"position", 0}, {"length", 192}, {"key", 69}}) && native("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 192}, {"autoResize", false}}); }
	void gate(Sf2Instrument* object, const Sf2Instrument::ProgramPreviewToken& token, bool restoreAllowed = false) { QString error; Sf2Instrument::ProgramPreviewToken rejected; QVERIFY(!object->previewProgram(1024, 7, error, rejected)); QVERIFY(!error.isEmpty()); QVERIFY(!rejected); QVERIFY(!object->previewProgram(1024, 7, error)); QCOMPARE(object->restoreProgramPreview(token, error), restoreAllowed); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setSF2File(""); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(QFile::exists(bank())); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!TrackRenderScope::hasActiveSelection()); Engine::getSong()->stopExport(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); NativeSongTestAccess::setProjectFileName(Engine::getSong(), ""); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); }
	void cleanup() { Engine::getSong()->stopExport(); Engine::audioEngine()->restoreAudioDevice(); Engine::getSong()->clearProject(); QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); }
	void allNativeRenderAdmissionWindowsRefuseAudition_data()
	{ QTest::addColumn<int>("state"); QTest::newRow("remote-executing") << 0; QTest::newRow("remote-finished-unreleased") << 1; QTest::newRow("remote-cancelled") << 2; QTest::newRow("device-reserved") << 3; QTest::newRow("stem-selection") << 4; QTest::newRow("song-export") << 5; QTest::newRow("pending-export-cleanup") << 6; }
	void allNativeRenderAdmissionWindowsRefuseAudition()
	{
		QFETCH(int, state); auto object = seed(); QVERIFY(arrange()); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token));
		if (state < 3) { const auto context = RemotePlugin::beginRenderContext(); QVERIFY(context); const auto end = qScopeGuard([&] { RemotePlugin::endRenderContext(context); }); if (state == 1) { RemotePlugin::finishRenderExecution(context); QVERIFY(!RemotePlugin::hasRunningRenderExecution()); } if (state == 2) { RemotePlugin::cancelRenderContext(context); } gate(object, token); }
		if (state == 3) { QVERIFY(Engine::audioEngine()->storeAudioDevice()); const auto restore = qScopeGuard([] { Engine::audioEngine()->restoreAudioDevice(); }); gate(object, token, true); }
		if (state == 4) { TrackRenderScope selected(object->instrumentTrack()); gate(object, token, true); }
		if (state == 5) { Engine::getSong()->startExport(); const auto stop = qScopeGuard([] { Engine::getSong()->stopExport(); }); gate(object, token); }
		if (state == 6) { auto song = Engine::getSong(); song->startExport(); auto& timeline = song->getTimeline(Song::PlayMode::Song); timeline.setTicks(48); SongExportTransitionProbe fault(song, [](SongExportPoint point) { if (point == SongExportPoint::StopAfterTimeline) { throw std::runtime_error("Export cleanup fixture"); } }); QVERIFY_EXCEPTION_THROWN(song->stopExport(), std::runtime_error); QVERIFY(song->hasPendingExportCleanup()); gate(object, token); fault.reset(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); }
		Sf2Instrument::ProgramPreviewToken next; QVERIFY(object->previewProgram(1024, 7, error, next)); QVERIFY(object->restoreProgramPreview(next, error)); QCOMPARE(object->getBank(), 0); QCOMPARE(object->getPatch(), 0);
	}
	void aStandaloneFileDeviceAlsoRefusesAudition()
	{
		auto object = seed(); QTemporaryDir output; QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); bool good = false; std::unique_ptr<AudioFileDevice> device(AudioFileWave::getInst(output.filePath("direct.wav"), OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit), DEFAULT_CHANNELS, Engine::audioEngine(), good)); QVERIFY(good); auto raw = device.get(); Engine::audioEngine()->setAudioDevice(raw, false); device.release(); const auto restore = qScopeGuard([] { bool ready = false; auto fallback = std::make_unique<AudioDummy>(ready, Engine::audioEngine()); if (ready) { auto raw = fallback.get(); Engine::audioEngine()->setAudioDevice(raw, false); fallback.release(); } }); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice()); gate(object, token);
	}
	void deviceSetupAndFinalizationKeepAuditionExcluded_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song-instrument") << false; QTest::newRow("pattern-instrument") << true; }
	void deviceSetupAndFinalizationKeepAuditionExcluded()
	{
		QFETCH(bool, pattern); auto songObject = seed(); QVERIFY(arrange()); auto object = pattern ? seed(Engine::patternStore()) : songObject; QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); QTemporaryDir output; auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice()); const auto restore = qScopeGuard([&] { engine->restoreAudioDevice(); }); bool before = false, after = false;
		const auto quality = connect(engine, &AudioEngine::qualitySettingsChanged, this, [&] { before = true; gate(object, token); }); const auto rate = connect(engine, &AudioEngine::sampleRateChanged, this, [&] { after = true; gate(object, token); QCOMPARE(Sf2RenderAuditionTestAccess::selection(object), (std::pair<int, int>{0, 0})); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(quality); QObject::disconnect(rate); });
		{ ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit), ProjectRenderer::ExportFileFormat::Wave, output.filePath("prepared.wav")); QVERIFY(renderer.isReady()); QVERIFY(renderer.startProcessing()); QVERIFY(before); QVERIFY(after); gate(object, token); QVERIFY(renderer.wait(10000)); QVERIFY(RemotePlugin::hasActiveRenderContext()); gate(object, token); QVERIFY(renderer.finalizeOutput()); gate(object, token); } QVERIFY(!RemotePlugin::hasActiveRenderContext()); gate(object, token); QVERIFY(engine->restoreAudioDevice()); Sf2Instrument::ProgramPreviewToken next; QVERIFY(object->previewProgram(1024, 7, error, next)); QVERIFY(object->restoreProgramPreview(next, error));
	}
	void retainedSynthFailureRestoresControlsAndAbortsExport_data()
	{ QTest::addColumn<bool>("exporting"); QTest::newRow("ordinary-rebuild") << false; QTest::newRow("export-device-setup") << true; }
	void retainedSynthFailureRestoresControlsAndAbortsExport()
	{
		QFETCH(bool, exporting); auto object = seed(); QVERIFY(arrange()); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); const auto path = Sf2RenderAuditionTestAccess::snapshotPath(object); const auto original = read(path); QVERIFY(!original.isEmpty()); const auto repair = qScopeGuard([&] { write(path, original); }); QVERIFY(write(path, "damaged accepted SoundFont"));
		if (!exporting) { object->reloadSynth(); QVERIFY(Engine::getSong()->hasFatalErrors()); QCOMPARE(Sf2RenderAuditionTestAccess::selection(object), (std::pair<int, int>{0, 0})); }
		else { QTemporaryDir output; auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice()); const auto restore = qScopeGuard([&] { write(path, original); engine->restoreAudioDevice(); }); ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit), ProjectRenderer::ExportFileFormat::Wave, output.filePath("failed.wav")); QVERIFY(renderer.isReady()); QVERIFY(!renderer.startProcessing()); QVERIFY(!renderer.isRunning()); QVERIFY(renderer.hasExecutionFailure()); QVERIFY(!renderer.finalizeOutput()); QVERIFY(!RemotePlugin::hasActiveRenderContext()); QCOMPARE(Sf2RenderAuditionTestAccess::selection(object), (std::pair<int, int>{0, 0})); }
	}
	void fileCallbackExceptionsLeaveQtBeforeFailingExport_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void fileCallbackExceptionsLeaveQtBeforeFailingExport()
	{
		QFETCH(bool, nonstandard); auto object = seed(); QVERIFY(arrange()); QString error;
		Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token));
		QTemporaryDir output; auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice());
		const auto restore = qScopeGuard([&] { FileResourceIO::Suspension work; engine->restoreAudioDevice(); });
		int delivered = 0, polls = 0;
		const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] { ++delivered; });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		{
			ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, output.filePath("failed.wav"));
			QVERIFY(renderer.isReady()); bool started = true;
			{
				FileResourceIO::Scope work([&]() -> bool { ++polls; if (nonstandard) { throw 239; } throw std::runtime_error("SoundFont notification file callback"); });
				started = renderer.startProcessing();
			}
			QVERIFY(!started); QVERIFY(!renderer.isRunning()); QVERIFY(renderer.hasExecutionFailure()); QVERIFY(!renderer.finalizeOutput());
			QVERIFY(polls > 0); QCOMPARE(delivered, 1); QVERIFY(!RemotePlugin::hasActiveRenderContext());
			QCOMPARE(Sf2RenderAuditionTestAccess::selection(object), (std::pair<int, int>{0, 0})); QVERIFY(engine->restoreAudioDevice());
		}
		QObject::disconnect(connection); const auto retry = output.filePath("retry.wav");
		QVERIFY(StudioAudio::render({{"path", retry}, {"bitDepth", 24}}).contains("sha256"));
		const auto audio = measure(retry); QVERIFY(audio[0] > .0001); QVERIFY(std::abs(audio[1] - 440) < 6);
	}
	void reportedReloadFailuresRetainRestorationUntilExplicitRetry_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("snapshot") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void reportedReloadFailuresRetainRestorationUntilExplicitRetry()
	{
		QFETCH(int, failure); auto object = seed(); QVERIFY(arrange()); QTemporaryDir output;
		const auto snapshot = Sf2RenderAuditionTestAccess::snapshotPath(object); const auto originalBank = read(snapshot); QVERIFY(!originalBank.isEmpty());
		auto engine = Engine::audioEngine(); auto originalDevice = engine->audioDev(); QVERIFY(engine->storeAudioDevice());
		const auto repair = qScopeGuard([&] { FileResourceIO::Suspension work; write(snapshot, originalBank); engine->restoreAudioDevice(); });
		const auto path = output.filePath("native.wav");
		{
			ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, path);
			QVERIFY(renderer.isReady()); QVERIFY(renderer.startProcessing()); QVERIFY(renderer.wait(10000)); QVERIFY(renderer.finalizeOutput());
			const auto before = measure(path); QVERIFY(before[0] > .0001); QVERIFY(std::abs(before[1] - 440) < 6);
			QSignalSpy quality(engine, &AudioEngine::qualitySettingsChanged), rate(engine, &AudioEngine::sampleRateChanged);
			if (failure == 0) { QVERIFY(write(snapshot, "damaged SoundFont snapshot")); }
			bool restored = true; int polls = 0;
			{
				FileResourceIO::Scope work([&] { ++polls; if (failure == 1) { throw std::runtime_error("SoundFont restoration file callback"); } if (failure == 2) { throw 241; } return false; });
				restored = engine->restoreAudioDevice();
			}
			QVERIFY(!restored); QVERIFY(polls > 0); QVERIFY(engine->hasStoredAudioDevice()); QCOMPARE(engine->audioDev(), originalDevice);
			QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::NotificationException);
			QCOMPARE(quality.count(), 1); QCOMPARE(rate.count(), 1); QVERIFY(!renderer.hasExecutionFailure());
			QVERIFY(write(snapshot, originalBank)); Engine::getSong()->clearErrors(); QVERIFY(engine->restoreAudioDevice());
			QCOMPARE(quality.count(), 1); QCOMPARE(rate.count(), 2); QVERIFY(!engine->hasStoredAudioDevice());
			QCOMPARE(Sf2RenderAuditionTestAccess::selection(object), (std::pair<int, int>{0, 0}));
		}
		QVERIFY(!RemotePlugin::hasActiveRenderContext()); const auto retry = output.filePath("retry.wav");
		QVERIFY(StudioAudio::render({{"path", retry}, {"bitDepth", 24}}).contains("sha256"));
		const auto after = measure(retry); QVERIFY(after[0] > .0001); QVERIFY(std::abs(after[1] - 440) < 6);
	}
	void decodedOfflineAudioUsesCommittedPrograms_data()
	{ QTest::addColumn<int>("rate"); QTest::newRow("44100") << 44100; QTest::newRow("48000") << 48000; }
	void decodedOfflineAudioUsesCommittedPrograms()
	{
		QFETCH(int, rate); auto object = seed(); QVERIFY(arrange()); QTemporaryDir output; const auto base = output.filePath("base.wav"), auditioned = output.filePath("auditioned.wav"), alternate = output.filePath("committed-alternate.wav"); QVERIFY(StudioAudio::render({{"path", base}, {"sampleRate", rate}, {"bitDepth", 24}}).contains("wallTimeMs")); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); QVERIFY(StudioAudio::render({{"path", auditioned}, {"sampleRate", rate}, {"bitDepth", 24}}).contains("wallTimeMs")); const auto first = measure(base), second = measure(auditioned); QVERIFY(first[0] > .0001); QVERIFY(second[0] > .0001); QVERIFY(std::abs(first[1] - 440) < 6); QVERIFY(std::abs(second[1] - first[1]) < 4); QCOMPARE(object->getBank(), 0); QCOMPARE(object->getPatch(), 0); QVERIFY(object->selectProgram(1024, 7, error)); QVERIFY(StudioAudio::render({{"path", alternate}, {"sampleRate", rate}, {"bitDepth", 24}}).contains("wallTimeMs")); const auto changed = measure(alternate); QVERIFY(changed[0] > .0001); QVERIFY(std::abs(changed[1] - 880) < 8);
	}
	void renderCallbacksCannotPublishANewAudition()
	{
		auto object = seed(); QVERIFY(arrange()); QString error; Sf2Instrument::ProgramPreviewToken old; QVERIFY(object->previewProgram(1024, 7, error, old)); QTemporaryDir output; int attempts = 0; const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] { ++attempts; gate(object, old); }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(StudioAudio::render({{"path", output.filePath("safe.wav")}}).contains("wallTimeMs")); QVERIFY(attempts >= 2); const auto audio = measure(output.filePath("safe.wav")); QVERIFY(std::abs(audio[1] - 440) < 6);
	}
	void renderAdmissionRefusalDoesNotWaitForModelChangeOwnership()
	{
		auto object = seed(); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); const auto context = RemotePlugin::beginRenderContext(); QVERIFY(context); const auto end = qScopeGuard([&] { RemotePlugin::endRenderContext(context); }); std::mutex mutex; std::condition_variable condition; bool acquired = false, released = false; const auto engine = Engine::audioEngine();
		std::thread observer([&] { const auto changes = engine->requestChangesGuard(); std::unique_lock<std::mutex> lock(mutex); acquired = true; condition.notify_all(); condition.wait_for(lock, std::chrono::seconds(3), [&] { return released; }); }); const auto join = qScopeGuard([&] { { std::lock_guard<std::mutex> lock(mutex); released = true; } condition.notify_all(); observer.join(); }); { std::unique_lock<std::mutex> lock(mutex); QVERIFY(condition.wait_for(lock, std::chrono::seconds(5), [&] { return acquired; })); } QElapsedTimer elapsed; elapsed.start(); gate(object, token); QVERIFY(elapsed.elapsed() < 1000);
	}
	void cancellationBeforeDeviceSetupPreservesTheExistingAudition()
	{
		auto object = seed(); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); QTemporaryDir output; { ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit), ProjectRenderer::ExportFileFormat::Wave, output.filePath("cancelled.wav")); QVERIFY(renderer.isReady()); renderer.abortProcessing(); QVERIFY(!renderer.startProcessing()); QVERIFY(!RemotePlugin::hasActiveRenderContext()); QCOMPARE(Sf2RenderAuditionTestAccess::selection(object), (std::pair<int, int>{1024, 7})); } QVERIFY(object->restoreProgramPreview(token, error)); QCOMPARE(Sf2RenderAuditionTestAccess::selection(object), (std::pair<int, int>{0, 0}));
	}
	void failedPreparationPreservesExistingOutputAndSupportsExplicitRecovery()
	{
		auto object = seed(); QVERIFY(arrange()); QTemporaryDir output; const auto destination = output.filePath("preserved.wav"), source = Sf2RenderAuditionTestAccess::snapshotPath(object); const auto original = read(source); QVERIFY(!original.isEmpty()); const auto repair = qScopeGuard([&] { write(source, original); Engine::audioEngine()->restoreAudioDevice(); }); QVERIFY(write(destination, "prior destination")); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); QVERIFY(write(source, "damaged accepted source")); QVERIFY_EXCEPTION_THROWN(StudioAudio::render({{"path", destination}}), std::runtime_error); QCOMPARE(read(destination), QByteArray("prior destination")); QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(write(source, original)); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearErrors(); QVERIFY(StudioAudio::render({{"path", destination}}).contains("wallTimeMs")); const auto audio = measure(destination); QVERIFY(audio[0] > .0001); QVERIFY(std::abs(audio[1] - 440) < 6);
	}
	void requiredModuleRuntimeRendersCommittedStateAndReplaysWithoutReexecuting()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("sf2player").isNull(), "The built native SoundFont module must be discoverable"); QTemporaryDir session, source; const auto original = source.filePath("bank.sf2"), path = source.filePath("render.wav"); QVERIFY(QFile::copy(bank(), original)); QString error; QJsonObject receipt;
		{ StudioRuntime runtime(session.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}, {"instrument", "sf2player"}})).value("ok").toBool()); QVERIFY(runtime.process(request("bank", "instrument.resource.set", {{"track", 0}, {"resource", "soundfont"}, {"path", original}})).value("ok").toBool()); QVERIFY(runtime.process(request("tempo", "song.set", {{"tempo", 240}, {"masterVolume", 100}})).value("ok").toBool()); QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool()); QVERIFY(runtime.process(request("note", "note.add", {{"track", 0}, {"clip", 0}, {"position", 0}, {"length", 192}, {"key", 69}})).value("ok").toBool()); QVERIFY(runtime.process(request("length", "clip.edit", {{"track", 0}, {"clip", 0}, {"length", 192}, {"autoResize", false}})).value("ok").toBool()); auto object = static_cast<Sf2Instrument*>(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()); Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); int setups = 0; const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] { ++setups; }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("render", "audio.render", {{"path", path}, {"bitDepth", 24}}); receipt = runtime.process(command); QVERIFY2(receipt.value("ok").toBool(), QJsonDocument(receipt).toJson().constData()); const auto count = setups; auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QCOMPARE(setups, count); QVERIFY(std::abs(measure(path)[1] - 440) < 6); }
		QVERIFY(QFile::remove(original)); StudioRuntime reopened(session.path()); QVERIFY2(reopened.open(error), qPrintable(error)); const auto response = reopened.process(request("again", "audio.render", {{"path", source.filePath("reopened.wav")}, {"bitDepth", 24}})); QVERIFY(response.value("ok").toBool()); QVERIFY(std::abs(measure(source.filePath("reopened.wav"))[1] - 440) < 6);
	}
};
QTEST_GUILESS_MAIN(Sf2RenderAuditionTest)
#include "Sf2RenderAuditionTest.moc"

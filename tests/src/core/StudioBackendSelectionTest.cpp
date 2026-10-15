/* Native runtime audio backend selection. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <sndfile.h>
#include "AudioBusHandle.h"
#include "AudioDummy.h"
#include "AudioEngine.h"
#include "AudioDeviceTransitionTestUtils.h"
#include "AudioSdl.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "RemotePlugin.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "TrackRenderScope.h"

#ifdef LMMS_HAVE_SDL
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#endif

namespace lmms
{
namespace
{
struct Counts { int starts = 0, stops = 0, destroyed = 0; QSet<AudioBusHandle*> ports; };
// Only the native driver lifecycle is injected. This is not physical hardware.
class ControlledDevice final : public AudioDevice
{
public:
	std::shared_ptr<Counts> counts;
	int stopFailures = 0;
	bool failStart = false, throwStart = false, throwRegister = false;
	std::function<void()> onStart, onStop;
	ControlledDevice(AudioEngine* engine, std::shared_ptr<Counts> state, sample_rate_t rate = 48000)
		: AudioDevice(DEFAULT_CHANNELS, engine), counts(std::move(state)) { setSampleRate(rate); }
	~ControlledDevice() override { clearFaults(); stopProcessing(); ++counts->destroyed; }
	void clearFaults() { stopFailures = 0; failStart = throwStart = throwRegister = false; onStart = onStop = {}; }
	void registerPort(AudioBusHandle* bus) override
	{ counts->ports.insert(bus); if (throwRegister) { throw std::runtime_error("Native port registration fixture"); } }
private:
	void startProcessingImpl() override
	{
		++counts->starts; if (onStart) { onStart(); }
		if (throwStart) { throw std::runtime_error("Native startup fixture"); }
		if (failStart) { reportProcessingFailure(ProcessingFailure::Start, -77); }
	}
	void stopProcessingImpl() override
	{
		++counts->stops; if (auto callback = std::exchange(onStop, {})) { callback(); }
		if (stopFailures > 0) { --stopFailures; throw std::runtime_error("Native stop fixture"); }
	}
};
}

class StudioBackendSelectionTestAccess
{
public:
	static void select(const QString& name, const std::function<std::unique_ptr<AudioDevice>()>& factory)
	{ Engine::audioEngine()->selectAudioBackend(name, factory); }
	static void clearFaults()
	{
		if (const auto engine = Engine::audioEngine())
		{
			for (const auto device : {engine->m_audioDev, engine->m_oldAudioDev})
			{ if (auto fixture = dynamic_cast<ControlledDevice*>(device)) { fixture->clearFaults(); } }
		}
	}
};
}

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject call(const QString& id, const QString& name, const QJsonObject& args = {}) { return {{"id", id}, {"name", name}, {"args", args}}; }
QJsonObject manifest(const QString& directory)
{ QFile file(QDir(directory).filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject job(const QString& directory) { return manifest(directory).value("agent").toObject().value("job").toObject(); }
QJsonObject select() { return StudioCapabilities::execute("audio.backend.select", {{"name", AudioDummy::name()}}); }
bool native(const QString& op, const QJsonObject& args = {}) { return StudioCapabilities::execute(op, args).value("ok").toBool(); }
void destroyEngine()
{
	if (!Engine::audioEngine()) { return; }
	StudioBackendSelectionTestAccess::clearFaults(); Engine::getSong()->stopExport(); Engine::destroy();
}
bool wave(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::WriteOnly)) { return false; }
	SF_INFO info{}; info.samplerate = 44100; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
	auto audio = sf_open_fd(file.handle(), SFM_WRITE, &info, false); if (!audio) { return false; }
	std::vector<float> samples(44100, 0.25f); const auto written = sf_writef_float(audio, samples.data(), 22050); const auto closed = sf_close(audio);
	return written == 22050 && closed == 0 && file.flush();
}
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
}

class StudioBackendSelectionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void initializeEngine()
	{
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->clearProject();
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init() { initializeEngine(); }
	void cleanup() { destroyEngine(); }

	void exactDummySelectionPreservesNativeProjectAndConfiguration_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("stopped") << false; QTest::newRow("running") << true; }
	void exactDummySelectionPreservesNativeProjectAndConfiguration()
	{
		QFETCH(bool, running); auto engine = Engine::audioEngine(); auto old = engine->audioDev(); auto midi = engine->midiClient();
		if (running) { old->startProcessing(); }
		QVERIFY(native("song.set", {{"tempo", 143}})); QVERIFY(native("track.add", {{"type", "sample"}}));
		const auto inspectProject = [] { auto result = StudioCapabilities::execute("project.inspect", {}).value("result").toObject(); result.remove("transport"); return result; };
		const auto project = inspectProject(); QVERIFY(!project.isEmpty()); const auto configured = ConfigManager::inst()->value("audioengine", "audiodev");
		const auto response = select(); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
		QVERIFY(engine->audioDev() != old); QVERIFY(dynamic_cast<AudioDummy*>(engine->audioDev())); QVERIFY(engine->audioDev()->isRunning()); QVERIFY(!engine->renderOnly());
		QCOMPARE(engine->midiClient(), midi); QCOMPARE(engine->audioDevName(), AudioDummy::name()); QCOMPARE(engine->requestedAudioBackend(), AudioDummy::name());
		QCOMPARE(ConfigManager::inst()->value("audioengine", "audiodev"), configured); QCOMPARE(inspectProject(), project);
		QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!engine->isChangingAudioDevice()); QVERIFY(Engine::getSong()->isStopped());
		QVERIFY(native("transport.play")); QTRY_VERIFY_WITH_TIMEOUT(Engine::getSong()->getPlayPos().getTicks() > 0, 2000); QVERIFY(native("transport.stop"));
	}

	void injectedFailuresRestorePriorOwner_data()
	{
		QTest::addColumn<QString>("failure"); QTest::addColumn<bool>("running");
		for (const auto failure : {"factory-null", "factory-throw", "factory-nonstandard", "old-stop", "ports", "quality", "rate", "start-error", "start-exception", "cancel-factory", "cancel-notify", "cancel-start"})
			for (const bool running : {false, true}) { QTest::newRow(qPrintable(QString(failure) + (running ? "-running" : "-stopped"))) << QString(failure) << running; }
	}
	void injectedFailuresRestorePriorOwner()
	{
		QFETCH(QString, failure); QFETCH(bool, running); auto engine = Engine::audioEngine(); auto counts = std::make_shared<Counts>(), next = std::make_shared<Counts>();
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong()));
		auto prior = new ControlledDevice(engine, counts, 32000); engine->setAudioDevice(prior, running); const auto oldName = engine->audioDevName(); bool cancelled = false, created = false;
		if (failure == "old-stop") { prior->stopFailures = 1; }
		const auto notify = [&](bool quality) {
			if (engine->audioDev() == prior) { return; }
			if ((quality && failure == "quality") || (!quality && failure == "rate")) { throw std::runtime_error("Native notification fixture"); }
			if (failure == "cancel-notify") { cancelled = true; }
		};
		AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point == DeviceTransitionPoint::SetupAfterQuality) { notify(true); } if (point == DeviceTransitionPoint::SetupAfterRate) { notify(false); } });
		QString error;
		{
			FileResourceIO::Scope work([&] { return cancelled; }, true);
			try { StudioBackendSelectionTestAccess::select(AudioDummy::name(), [&]() -> std::unique_ptr<AudioDevice> {
				if (failure == "factory-null") { return {}; } if (failure == "factory-throw") { throw std::runtime_error("Native factory fixture"); } if (failure == "factory-nonstandard") { throw 7; }
				auto device = std::make_unique<ControlledDevice>(engine, next); created = true;
				device->throwRegister = failure == "ports"; device->failStart = failure == "start-error"; device->throwStart = failure == "start-exception";
				if (failure == "cancel-factory") { cancelled = true; } if (failure == "cancel-start") { device->onStart = [&] { cancelled = true; }; }
				return device;
			}); } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
		}
		QVERIFY(!error.isEmpty()); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior)); QCOMPARE(prior->isRunning(), running); QCOMPARE(counts->destroyed, 0); QCOMPARE(next->destroyed, created ? 1 : 0);
		QCOMPARE(engine->audioDevName(), oldName); QVERIFY(engine->renderOnly()); QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!engine->isChangingAudioDevice());
		QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::None); QCOMPARE(engine->outputSampleRate(), sample_rate_t(32000)); QVERIFY(counts->ports.contains(track->audioBusHandle()));
		transitionProbe.reset(); QVERIFY(select().value("ok").toBool()); QCOMPARE(counts->destroyed, 1);
	}

	void failedRollbackRetainsOwnershipAndCanBeRetried_data()
	{ QTest::addColumn<QString>("phase"); for (const auto phase : {"stop", "notification", "restart"}) { QTest::newRow(phase) << QString(phase); } }
	void failedRollbackRetainsOwnershipAndCanBeRetried()
	{
		QFETCH(QString, phase); auto engine = Engine::audioEngine(); auto counts = std::make_shared<Counts>(), next = std::make_shared<Counts>();
		auto prior = new ControlledDevice(engine, counts); engine->setAudioDevice(prior, true); if (phase == "restart") { prior->failStart = true; }
		AudioDeviceTransitionProbe transitionProbe(engine, [&](DeviceTransitionPoint point) { if (point != DeviceTransitionPoint::RestoreAfterRate) { return; }
			if (phase == "notification" && engine->isRestoringAudioDevice()) { throw std::runtime_error("Native rollback notification fixture"); }
		});
		QString error;
		try { StudioBackendSelectionTestAccess::select(AudioDummy::name(), [&]() -> std::unique_ptr<AudioDevice> {
			auto device = std::make_unique<ControlledDevice>(engine, next); device->failStart = true; if (phase == "stop") { device->stopFailures = 1; } return device;
		}); } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
		QVERIFY(error.contains("audio.device.restore")); QVERIFY(engine->hasStoredAudioDevice()); QVERIFY(!select().value("ok").toBool()); QCOMPARE(counts->destroyed, 0);
		QCOMPARE(next->destroyed, phase == "stop" ? 0 : 1); transitionProbe.reset(); prior->failStart = false;
		QVERIFY(native("audio.device.restore")); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(prior)); QVERIFY(prior->isRunning()); QCOMPARE(next->destroyed, 1); QVERIFY(!engine->hasStoredAudioDevice());
	}

	void busyAndInvalidSelectionsDoNotInvokeFactory_data()
	{
		QTest::addColumn<QString>("busy");
		for (const auto busy : {"playing", "paused", "recording", "sample-armed", "automation-armed", "note-recording", "export-reserved", "stem", "remote-execution", "foreign", "invalid"}) { QTest::newRow(busy) << QString(busy); }
	}
	void busyAndInvalidSelectionsDoNotInvokeFactory()
	{
		QFETCH(QString, busy); auto engine = Engine::audioEngine(); auto prior = engine->audioDev(); auto song = Engine::getSong(); bool invoked = false, refused = false;
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, song)); auto sample = static_cast<SampleClip*>(track->createClip(0));
		std::unique_ptr<TrackRenderScope> selection; std::uint64_t context = 0;
		if (busy == "playing" || busy == "paused") { song->playSong(); if (busy == "paused") { song->togglePause(); } }
		if (busy == "recording") { song->record(); } if (busy == "sample-armed") { sample->setRecord(true); }
		if (busy == "automation-armed") { static_cast<AutomationClip*>(song->globalAutomationTrack()->createClip(0))->setRecording(true); }
		if (busy == "note-recording") { QVERIFY(track->beginNoteRecording(this)); }
		if (busy == "export-reserved") { QVERIFY(engine->storeAudioDevice()); }
		if (busy == "stem") { selection = std::make_unique<TrackRenderScope>(track); }
		if (busy == "remote-execution") { context = RemotePlugin::beginRenderContext(); QVERIFY(context); }
		const auto release = qScopeGuard([&] { if (context) { RemotePlugin::finishRenderExecution(context); RemotePlugin::endRenderContext(context); } selection.reset(); track->endNoteRecording(this); song->stop(); engine->restoreAudioDevice(); });
		const auto attempt = [&] { try { StudioBackendSelectionTestAccess::select(busy == "invalid" ? "unknown backend" : AudioDummy::name(), [&]() -> std::unique_ptr<AudioDevice> { invoked = true; return {}; }); } catch (const std::exception&) { refused = true; } };
		if (busy == "foreign") { std::thread worker(attempt); worker.join(); } else { attempt(); }
		QVERIFY(refused); QVERIFY(!invoked); QCOMPARE(engine->audioDev(), prior); QVERIFY(engine->renderOnly());
		if (busy == "invalid")
		{
			for (const auto args : {QJsonObject{}, QJsonObject{{"name", 2}}, QJsonObject{{"name", ""}}, QJsonObject{{"name", AudioDummy::name()}, {"start", true}}})
			{ QVERIFY(!StudioCapabilities::execute("audio.backend.select", args).value("ok").toBool()); QCOMPARE(engine->audioDev(), prior); }
		}
	}

	void reentrantSelectorsCannotStealPendingSwitch()
	{
		auto engine = Engine::audioEngine(); const auto before = engine->audioDev(); bool inspected = false, refused = false, reserved = true, restored = true;
		const auto connection = connect(engine, &AudioEngine::sampleRateChanged, this, [&] {
			if (engine->audioDev() == before) { return; } inspected = true; refused = !select().value("ok").toBool(); reserved = engine->storeAudioDevice(); restored = engine->restoreAudioDevice();
		}, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(select().value("ok").toBool()); QVERIFY(inspected); QVERIFY(refused); QVERIFY(!reserved); QVERIFY(!restored); QVERIFY(!engine->hasStoredAudioDevice()); QCOMPARE(engine->deviceRestoreFailure(), AudioEngine::DeviceRestoreFailure::None);
	}

	void nativeStopCanJoinTheRenderThreadDuringSelection()
	{
		auto engine = Engine::audioEngine(); auto counts = std::make_shared<Counts>(); auto prior = new ControlledDevice(engine, counts); engine->setAudioDevice(prior, true); bool rendered = false;
		prior->onStop = [&] { std::thread worker([&] { rendered = !engine->renderNextPeriod().empty(); }); worker.join(); };
		QVERIFY(select().value("ok").toBool()); QVERIFY(rendered); QCOMPARE(counts->destroyed, 1);
	}

	void transientReceiptsReplayAcrossEngineRestart_data()
	{ QTest::addColumn<bool>("restart"); QTest::newRow("same-engine") << false; QTest::newRow("engine-restart") << true; }
	void transientReceiptsReplayAcrossEngineRestart()
	{
		QFETCH(bool, restart); QTemporaryDir dir; const auto session = dir.filePath("session"), sample = dir.filePath("source.wav"), output = dir.filePath("output.wav"); QString error;
		auto studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error)); QVERIFY(wave(sample));
		QVERIFY(studio->process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool()); QVERIFY(studio->process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool());
		QVERIFY(studio->process(request("sample", "sample.load", {{"track", 0}, {"clip", 0}, {"path", sample}})).value("ok").toBool());
		const int tempo = Engine::getSong()->getTempo(); QVERIFY(studio->process(request("tempo", "song.set", {{"tempo", 141}})).value("ok").toBool()); const auto before = manifest(session);
		const auto selection = request("backend", "audio.backend.select", {{"name", AudioDummy::name()}}); const auto response = studio->process(selection); QVERIFY(response.value("ok").toBool());
		QCOMPARE(manifest(session).value("revision"), before.value("revision")); QCOMPARE(manifest(session).value("history"), before.value("history"));
		QVERIFY(studio->process(request("render", "audio.render", {{"path", output}, {"timeoutMs", 30000}})).value("ok").toBool());
		QFile audio(output); QVERIFY(audio.open(QIODevice::ReadOnly)); SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> decoded(sf_open_fd(audio.handle(), SFM_READ, &info, false), &sf_close);
		QVERIFY(decoded); QVERIFY(info.frames > 0); QCOMPARE(info.channels, 2); std::vector<float> pcm(4096 * 2); QCOMPARE(sf_readf_float(decoded.get(), pcm.data(), 4096), sf_count_t(4096)); QVERIFY(*std::max_element(pcm.begin(), pcm.end()) > 0.2f); decoded.reset(); audio.close();
		if (restart) { studio.reset(); destroyEngine(); initializeEngine(); studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error)); }
		auto device = Engine::audioEngine()->audioDev(); device->stopProcessing(); auto replay = studio->process(selection); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, response); QCOMPARE(Engine::audioEngine()->audioDev(), device); QVERIFY(!device->isRunning()); QCOMPARE(Engine::audioEngine()->renderOnly(), restart);
		QVERIFY(studio->process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), tempo);
		QVERIFY(studio->process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->getTempo(), 141);
	}

	void scriptedAgentSelectsAndObservesNativeBackend_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void scriptedAgentSelectsAndObservesNativeBackend()
	{
		QFETCH(QString, protocol); QTemporaryDir dir; QString error; int round = 0;
		StudioRuntime studio(dir.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++round == 1) { return decision(protocol, {call("select", "audio_backend_select", {{"name", AudioDummy::name()}}), call("observe", "transport_inspect")}); }
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", "Native dummy backend selected and observed"}, {"evidence", QJsonArray{"select", "observe"}}})});
		});
		QVERIFY2(studio.open(error), qPrintable(error));
		QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-backend-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Select the native dummy audio backend and report its state."}})).value("ok").toBool());
		for (int i = 0; i < 10 && StudioAgent::active(job(dir.path())); ++i) { studio.advanceAgent(); }
		QCOMPARE(job(dir.path()).value("status").toString(), QString("complete")); QCOMPARE(round, 2); QVERIFY(!Engine::audioEngine()->renderOnly()); QVERIFY(Engine::audioEngine()->audioDev()->isRunning());
	}

	void sdlCandidateRetirementPreservesExistingSubsystemAndPlayback()
	{
#ifdef LMMS_HAVE_SDL
		SDL_SetMainReady();
		const auto oldDriver = qgetenv("SDL_AUDIODRIVER"); const bool hadDriver = qEnvironmentVariableIsSet("SDL_AUDIODRIVER"); qputenv("SDL_AUDIODRIVER", "dummy");
		const auto restoreDriver = qScopeGuard([&] { if (hadDriver) { qputenv("SDL_AUDIODRIVER", oldDriver); } else { qunsetenv("SDL_AUDIODRIVER"); } });
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) { QSKIP("SDL dummy audio driver unavailable"); }
		const auto release = qScopeGuard([] { SDL_QuitSubSystem(SDL_INIT_AUDIO); }); const auto destroy = qScopeGuard([] { destroyEngine(); });
		const auto result = StudioCapabilities::execute("audio.backend.select", {{"name", AudioSdl::name()}}); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		bool accepted = false; { auto candidate = std::make_unique<AudioSdl>(accepted, Engine::audioEngine()); }
		// Some SDL drivers permit one device per direction; either constructor
		// outcome must release only the candidate's own subsystem reference.
		QVERIFY(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO); QVERIFY(SDL_GetNumAudioDevices(0) > 0);
		QVERIFY(native("transport.play")); QTRY_VERIFY_WITH_TIMEOUT(Engine::getSong()->getPlayPos().getTicks() > 0, 2000); QVERIFY(native("transport.stop"));
		QVERIFY(select().value("ok").toBool()); QVERIFY(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO); destroyEngine(); QVERIFY(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO);
#else
		QSKIP("Native SDL backend not compiled");
#endif
	}
};

QTEST_GUILESS_MAIN(StudioBackendSelectionTest)
#include "StudioBackendSelectionTest.moc"

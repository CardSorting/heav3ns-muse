/* Native JACK admission/callback tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngine.h"
#include "AudioJack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleRecordHandle.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../../../src/core/audio/JackApi.h"
#include "../../../src/core/audio/JackCallbackGate.h"

namespace lmms
{
class AudioJackTestAccess
{
public:
	static std::unique_ptr<AudioJack> create(bool& successful, const detail::JackApi& api)
	{ return std::unique_ptr<AudioJack>(new AudioJack(successful, Engine::audioEngine(), api)); }
	static std::unique_ptr<AudioJack> configured(const detail::JackApi& api)
	{
		bool successful = false; auto result = create(successful, api);
		if (!successful) { throw std::runtime_error("JACK fixture configuration failed"); }
		return result;
	}
	static bool paused(const AudioJack& device) { return device.m_callbackGate->paused(); }
};
class StudioBackendSelectionTestAccess
{
public:
	static void select(const std::function<std::unique_ptr<AudioDevice>()>& factory)
	{ Engine::audioEngine()->selectAudioBackend(AudioJack::name(), factory); }
};
}

using namespace lmms;
namespace
{
struct Port { int index = 0; std::vector<float> samples = std::vector<float>(4096, 123.f); };
struct Transport
{
	static inline Transport* active = nullptr;
	detail::JackApi api;
	QString fault;
	std::array<Port, 4> ports;
	JackProcessCallback process = nullptr;
	JackBufferSizeCallback resize = nullptr;
	void* processArg = nullptr;
	void* resizeArg = nullptr;
	std::atomic<int> opened{0}, closed{0}, activations{0}, deactivations{0}, inputs{0}, outputs{0};
	int registrations = 0, failPort = -1, nullBuffer = -1, throwBuffer = -1;
	bool throwOnce = false, earlyCallback = false;
	std::thread worker;
	Transport()
	{
		active = this; for (int i = 0; i < 4; ++i) { ports[i].index = i; }
		api.open = [](const char*, jack_options_t, jack_status_t* status) -> jack_client_t*
		{
			auto& t = *active; *status = JackFailure;
			if (t.fault == "throw-open") { throw std::runtime_error("JACK open fixture"); }
			if (t.fault == "open") { return nullptr; }
			++t.opened; t.registrations = 0; *status = static_cast<jack_status_t>(0); return reinterpret_cast<jack_client_t*>(&t);
		};
		api.close = [](jack_client_t*) { active->join(); ++active->closed; return 0; };
		api.bufferSize = [](jack_client_t*) -> jack_nframes_t
		{
			if (active->fault == "throw-buffer") { throw std::runtime_error("JACK buffer fixture"); }
			return active->fault == "buffer-zero" ? 0 : 1024;
		};
		api.sampleRate = [](jack_client_t*) -> jack_nframes_t
		{ return active->fault == "rate-zero" ? 0 : Engine::audioEngine()->outputSampleRate(); };
		api.setBufferSizeCallback = [](jack_client_t*, JackBufferSizeCallback f, void* arg)
		{ active->resize = f; active->resizeArg = arg; return active->fault == "buffer-callback" ? -1 : 0; };
		api.setProcessCallback = [](jack_client_t*, JackProcessCallback f, void* arg)
		{ active->process = f; active->processArg = arg; return active->fault == "process-callback" ? -1 : 0; };
		api.onShutdown = [](jack_client_t*, JackShutdownCallback, void*) { };
		api.registerPort = [](jack_client_t*, const char*, const char*, unsigned long, unsigned long) -> jack_port_t*
		{
			auto& t = *active; const auto index = t.registrations++;
			if (t.fault == "throw-port") { throw std::runtime_error("JACK register fixture"); }
			return index == t.failPort || index >= 4 ? nullptr : reinterpret_cast<jack_port_t*>(&t.ports[index]);
		};
		api.activate = [](jack_client_t*)
		{
			auto& t = *active; ++t.activations;
			if (t.earlyCallback) { t.tick(13); }
			if (t.fault == "throw-activate") { throw std::runtime_error("JACK activate fixture"); }
			return t.fault == "activate" ? -77 : 0;
		};
		api.deactivate = [](jack_client_t*) { active->join(); ++active->deactivations; return 0; };
		api.portName = [](const jack_port_t*) { return "lmms:fixture"; };
		api.connect = [](jack_client_t*, const char*, const char*) { return 0; };
		api.portBuffer = [](jack_port_t* handle, jack_nframes_t frames) -> void*
		{
			auto& t = *active; auto& p = *reinterpret_cast<Port*>(handle);
			if (p.index % 2) { ++t.inputs; } else { ++t.outputs; }
			if (p.index == t.throwBuffer)
			{ if (t.throwOnce) { t.throwBuffer = -1; } throw std::runtime_error("JACK buffer access fixture"); }
			if (p.index == t.nullBuffer || frames > p.samples.size()) { return nullptr; }
			return p.samples.data();
		};
	}
	~Transport() { join(); active = nullptr; }
	int tick(jack_nframes_t frames) { return process(frames, processArg); }
	void launch(jack_nframes_t frames) { worker = std::thread([&, frames] { tick(frames); }); }
	void join() { if (worker.joinable()) { worker.join(); } }
	void poisonOutput() { for (int i : {0, 2}) { std::fill(ports[i].samples.begin(), ports[i].samples.end(), 123.f); } }
	bool silent(std::size_t frames) const
	{
		for (int i : {0, 2})
		{ if (!std::all_of(ports[i].samples.begin(), ports[i].samples.begin() + frames, [](float v) { return v == 0.f; })) { return false; } }
		return true;
	}
};
}

class AudioJackTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); NotePlayHandleManager::init(); }
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->clearProject();
		for (const auto key : {"input1", "input2", "output1", "output2"}) { ConfigManager::inst()->setValue("audiojack", key, "-"); }
	}
	void cleanup() { if (Engine::audioEngine()) { Engine::destroy(); } }

	void constructionReleasesPartialNativeStateWithoutActivation_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto name : {"none", "open", "buffer-callback", "process-callback", "buffer-zero", "rate-zero",
			"port-0", "port-1", "port-2", "port-3", "throw-open", "throw-buffer", "throw-port"})
		{ QTest::newRow(name) << QString(name); }
	}
	void constructionReleasesPartialNativeStateWithoutActivation()
	{
		QFETCH(QString, fault); Transport transport; transport.fault = fault;
		if (fault.startsWith("port-")) { transport.failPort = fault.right(1).toInt(); }
		bool successful = true;
		if (fault.startsWith("throw-")) { QVERIFY_EXCEPTION_THROWN(AudioJackTestAccess::create(successful, transport.api), std::runtime_error); }
		else
		{
			auto device = AudioJackTestAccess::create(successful, transport.api);
			QCOMPARE(successful, fault == "none"); QVERIFY(!device->isRunning());
			QCOMPARE(transport.closed.load(), fault == "none" ? 0 : transport.opened.load());
		}
		QCOMPARE(transport.closed.load(), transport.opened.load()); QCOMPARE(transport.activations.load(), 0);
	}

	void startupFailuresAndEarlyCallbacksCannotRunNativeDsp_data()
	{ QTest::addColumn<QString>("fault"); for (const auto f : {"none", "activate", "throw-activate"}) { QTest::newRow(f) << QString(f); } }
	void startupFailuresAndEarlyCallbacksCannotRunNativeDsp()
	{
		QFETCH(QString, fault); Transport transport; transport.fault = fault; transport.earlyCallback = true;
		auto device = AudioJackTestAccess::configured(transport.api); int periods = 0;
		const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer, this, [&](const SampleFrame*) { ++periods; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); });
		if (fault == "throw-activate") { QVERIFY_EXCEPTION_THROWN(device->startProcessing(), std::runtime_error); }
		else { device->startProcessing(); }
		QCOMPARE(device->isRunning(), fault == "none"); QCOMPARE(periods, 0); QCOMPARE(transport.inputs.load(), 0); QVERIFY(transport.silent(13));
		if (fault == "activate") { QCOMPARE(device->processingStatus().nativeError, -77); }
		if (fault != "none") { transport.fault.clear(); device->startProcessing(); QVERIFY(device->isRunning()); }
		QCOMPARE(transport.tick(777), 0); QVERIFY(periods > 0); QVERIFY(transport.inputs.load() > 0);
	}

	void stoppedCallbacksSilenceWithoutAdvancingOrCapturing_data()
	{ QTest::addColumn<int>("frames"); for (const auto n : {13, 777, 1024}) { QTest::newRow(qPrintable(QString::number(n))) << n; } }
	void stoppedCallbacksSilenceWithoutAdvancingOrCapturing()
	{
		QFETCH(int, frames); Transport transport; auto device = AudioJackTestAccess::configured(transport.api); int periods = 0;
		const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer, this, [&](const SampleFrame*) { ++periods; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); });
		QCOMPARE(transport.tick(frames), 0); QVERIFY(transport.silent(frames)); QCOMPARE(periods, 0); QCOMPARE(transport.inputs.load(), 0);
		device->startProcessing(); QCOMPARE(transport.tick(frames), 0); QVERIFY(periods > 0);
		device->stopProcessing(); const auto before = periods, captured = transport.inputs.load();
		for (int i = 0; i < 5; ++i) { transport.poisonOutput(); QCOMPARE(transport.tick(frames), 0); QVERIFY(transport.silent(frames)); }
		QCOMPARE(periods, before); QCOMPARE(transport.inputs.load(), captured); QCOMPARE(transport.deactivations.load(), 0);
		device->startProcessing();
		// A short callback can consume the remainder of an already-rendered
		// native period. Cross a full period before requiring another render.
		for (int resumed = 0; resumed <= int(Engine::audioEngine()->framesPerPeriod()); resumed += frames) { QCOMPARE(transport.tick(frames), 0); }
		QVERIFY(periods > before); QCOMPARE(transport.activations.load(), 1);
	}

	void malformedBuffersAndExceptionsStayInsideTheCallback_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto f : {"oversized", "output-null", "input-null", "output-throw", "input-throw", "output-persistent-throw"})
		{ QTest::newRow(f) << QString(f); }
	}
	void malformedBuffersAndExceptionsStayInsideTheCallback()
	{
		QFETCH(QString, fault); Transport transport; auto device = AudioJackTestAccess::configured(transport.api); device->startProcessing();
		if (fault == "output-null") { transport.nullBuffer = 0; } if (fault == "input-null") { transport.nullBuffer = 1; }
		if (fault.contains("throw")) { transport.throwBuffer = fault.startsWith("input") ? 1 : 0; transport.throwOnce = !fault.contains("persistent"); }
		const auto epoch = Engine::audioEngine()->inputErrorEpoch();
		QCOMPARE(transport.tick(fault == "oversized" ? 2048 : 777), 0);
		QVERIFY(!device->isRunning()); QVERIFY(Engine::audioEngine()->inputErrorEpoch() > epoch);
		QCOMPARE(device->processingStatus().failure, fault.contains("throw") ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::InvalidCallback);
		transport.nullBuffer = transport.throwBuffer = -1; device->stopProcessing(); device->startProcessing(); QVERIFY(device->isRunning()); QCOMPARE(transport.tick(13), 0);
	}

	void stopAndDestructionWaitForAdmittedNativeDsp_data()
	{ QTest::addColumn<bool>("destroy"); QTest::newRow("stop-and-restart") << false; QTest::newRow("destroy") << true; }
	void stopAndDestructionWaitForAdmittedNativeDsp()
	{
		QFETCH(bool, destroy); Transport transport; auto device = AudioJackTestAccess::configured(transport.api);
		std::atomic_bool entered{false}, release{false}, returned{false}; std::atomic<int> periods{0};
		const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer, Engine::audioEngine(), [&](const SampleFrame*)
		{ ++periods; entered = true; while (!release.load()) { QThread::msleep(1); } }, Qt::DirectConnection);
		std::thread control;
		const auto join = qScopeGuard([&] { release = true; transport.join(); if (control.joinable()) { control.join(); } QObject::disconnect(observed); });
		device->startProcessing(); transport.launch(777); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000);
		const auto raw = device.get();
		if (destroy)
		{
			std::atomic_bool joinedBeforeClose{false};
			control = std::thread([&]
			{
				QElapsedTimer deadline; deadline.start();
				while (!AudioJackTestAccess::paused(*raw) && deadline.elapsed() < 2000) { QThread::msleep(1); }
				joinedBeforeClose = AudioJackTestAccess::paused(*raw) && transport.closed.load() == 0;
				release = true;
			});
			device.reset(); control.join(); QVERIFY(joinedBeforeClose.load());
			QCOMPARE(transport.closed.load(), 1); QCOMPARE(transport.deactivations.load(), 1);
		}
		else
		{
			control = std::thread([&] { raw->stopProcessing(); returned = true; });
			QTRY_VERIFY_WITH_TIMEOUT(AudioJackTestAccess::paused(*raw), 2000); QVERIFY(!returned.load()); QCOMPARE(transport.closed.load(), 0);
			release = true; control.join(); transport.join(); QVERIFY(returned.load()); QVERIFY(transport.silent(777));
			const auto before = periods.load(); device->startProcessing(); transport.tick(777); QVERIFY(periods.load() > before);
		}
	}

	void nativeSamplePlaybackAndInputUseTheRegisteredPlanarBuffers()
	{
		Transport transport; auto device = AudioJackTestAccess::configured(transport.api); const auto engine = Engine::audioEngine();
		auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong())); auto clip = static_cast<SampleClip*>(track->createClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(32768, SampleFrame(.25f, -.125f)), engine->outputSampleRate()));
		Engine::getSong()->setMasterVolume(100); Engine::getSong()->playSong(); device->startProcessing();
		float peakLeft = 0, peakRight = 0;
		for (int cycle = 0; cycle < 8; ++cycle)
		{
			QCOMPARE(transport.tick(777), 0);
			for (int frame = 0; frame < 777; ++frame)
			{ peakLeft = std::max(peakLeft, transport.ports[0].samples[frame]); peakRight = std::min(peakRight, transport.ports[2].samples[frame]); }
		}
		QCOMPARE(peakLeft, .25f); QCOMPARE(peakRight, -.125f); Engine::getSong()->stop(); engine->discardInputBuffers();
		auto recorded = static_cast<SampleClip*>(track->createClip(0));
		for (int frame = 0; frame < 777; ++frame)
		{ transport.ports[1].samples[frame] = .125f; transport.ports[3].samples[frame] = -.25f; }
		QVERIFY(engine->addPlayHandle(new SampleRecordHandle(recorded, 777)));
		// JACK captures after rendering. The next native period consumes the
		// queued input; the frame limit excludes a later callback's input.
		QCOMPARE(transport.tick(777), 0); QCOMPARE(transport.tick(777), 0);
		engine->finishSampleRecordings(); QString error; QVERIFY2(SampleRecordHandle::finalizePendingRecordings(error), qPrintable(error));
		QCOMPARE(recorded->sample().sampleSize(), std::size_t{777});
		for (std::size_t frame = 0; frame < 777; ++frame)
		{ QCOMPARE(recorded->sample().data()[frame].left(), .125f); QCOMPARE(recorded->sample().data()[frame].right(), -.25f); }
	}

	void failedActivationRestoresPriorEngineDevice_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("running") << true; QTest::newRow("stopped") << false; }
	void failedActivationRestoresPriorEngineDevice()
	{
		QFETCH(bool, running); Transport transport; const auto destroy = qScopeGuard([] { Engine::destroy(); });
		const auto engine = Engine::audioEngine(); const auto prior = engine->audioDev(); if (running) { prior->startProcessing(); }
		const auto midi = engine->midiClient(); transport.fault = "activate";
		QVERIFY_EXCEPTION_THROWN(StudioBackendSelectionTestAccess::select([&] { return AudioJackTestAccess::configured(transport.api); }), std::runtime_error);
		QCOMPARE(engine->audioDev(), prior); QCOMPARE(prior->isRunning(), running); QCOMPARE(engine->midiClient(), midi); QCOMPARE(transport.closed.load(), 1);
	}

	void optionalRealServerPreservesProcessingAcrossStopAndRestart()
	{
		if (qgetenv("LMMS_TEST_JACK") != "1") { QSKIP("Set LMMS_TEST_JACK=1 with a running JACK server to exercise native callbacks"); }
		const auto selected = StudioCapabilities::execute("audio.backend.select", {{"name", AudioJack::name()}});
		QVERIFY2(selected.value("ok").toBool(), qPrintable(selected.value("error").toString()));
		const auto engine = Engine::audioEngine(); auto device = dynamic_cast<AudioJack*>(engine->audioDev()); QVERIFY(device);
		for (int cycle = 0; cycle < 3; ++cycle)
		{
			Engine::getSong()->playSong(); QTRY_VERIFY_WITH_TIMEOUT(Engine::getSong()->getPlayPos().getTicks() > 0, 2000);
			device->stopProcessing(); const auto stopped = Engine::getSong()->getPlayPos().getTicks();
			QTest::qWait(100); QCOMPARE(Engine::getSong()->getPlayPos().getTicks(), stopped);
			device->startProcessing(); QVERIFY(device->isRunning());
			QTRY_VERIFY_WITH_TIMEOUT(Engine::getSong()->getPlayPos().getTicks() > stopped, 2000); Engine::getSong()->stop();
		}
		QVERIFY(StudioCapabilities::execute("audio.backend.select", {{"name", "Dummy (no sound output)"}}).value("ok").toBool());
	}
};

QTEST_GUILESS_MAIN(AudioJackTest)
#include "AudioJackTest.moc"

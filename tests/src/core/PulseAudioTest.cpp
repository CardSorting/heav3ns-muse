/* Native PulseAudio lifecycle and DSP tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioDummy.h"
#include "AudioEngine.h"
#include "AudioPulseAudio.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../../../src/core/audio/PulseAudioApi.h"

namespace lmms
{
class PulseAudioTestAccess
{
public:
	static std::unique_ptr<AudioPulseAudio> create(const detail::PulseAudioApi& api, int timeout = 1000)
	{
		bool successful = false;
		auto device = std::unique_ptr<AudioPulseAudio>(new AudioPulseAudio(successful, Engine::audioEngine(), api, timeout));
		if (!successful) { throw std::runtime_error("invalid native PulseAudio format"); }
		return device;
	}
};
class StudioBackendSelectionTestAccess
{
public:
	static void select(const std::function<std::unique_ptr<AudioDevice>()>& factory)
	{ Engine::audioEngine()->selectAudioBackend(AudioPulseAudio::name(), factory); }
};
}

using namespace lmms;
namespace
{
enum class Event { ContextReady, ContextFailed, ContextTerminated, StreamReady, StreamFailed, StreamTerminated, Write, PrepareError, PollError, DispatchError };
enum class Fault { None, LoopNew, ContextNew, ContextConnect, ContextFailed, ContextTerminated, StreamNew, StreamConnect,
	StreamFailed, StreamTerminated, Prepare, Poll, Dispatch, ThrowContextNew, ThrowContextCallback, ThrowStreamCallback,
	ContextTimeout, StreamTimeout, BeginWrite, NullBuffer, ZeroBuffer, OversizedBuffer, UnalignedBuffer, MisalignedPointer, Write, ThrowBeginWrite };

// Only libpulse transport entry points are replaced. AudioPulseAudio's worker,
// synchronization, state callbacks, native rendering and engine ownership run
// unchanged. Tests in this executable are serial and own one transport at a time.
struct Transport
{
	static inline Transport* active = nullptr;
	detail::PulseAudioApi api;
	std::atomic<Fault> fault{Fault::None};
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<Event> events;
	bool awakened = false;
	bool holdPrepare = false, releasePrepare = false;
	bool holdCleanup = false, releaseCleanup = false;
	std::atomic_bool preparing{false};
	std::atomic_bool cleaning{false};
	std::atomic<int> loops{0}, contexts{0}, streams{0}, loopFrees{0}, contextFrees{0}, streamFrees{0}, wakes{0};
	std::atomic<int> polls{0}, dispatches{0}, writes{0}, cancels{0}, writeRequests{0}, callbackReleases{0};
	std::atomic_bool blocking{false};
	pa_mainloop_api loopApi{};
	pa_context_state_t contextState = PA_CONTEXT_UNCONNECTED;
	pa_stream_state_t streamState = PA_STREAM_UNCONNECTED;
	pa_context_notify_cb_t contextCallback = nullptr;
	pa_stream_notify_cb_t stateCallback = nullptr;
	pa_stream_request_cb_t writeCallback = nullptr;
	void* contextData = nullptr;
	void* streamData = nullptr;
	void* writeData = nullptr;
	QByteArray sink;
	pa_buffer_attr attributes{};
	pa_sample_spec spec{};
	std::size_t requestBytes = 0, maxLoanBytes = 0, beginRequested = 0;
	std::vector<float> loan, pcm;
	bool borrowed = false, guardsIntact = true;
	Event current = Event::ContextReady;
	int nativeError = PA_ERR_CONNECTIONREFUSED;
	std::function<void()> onBegin;

	static Transport& from(pa_mainloop* p) { return *reinterpret_cast<Transport*>(p); }
	static Transport& from(const pa_context* p) { return *reinterpret_cast<Transport*>(const_cast<pa_context*>(p)); }
	static Transport& from(const pa_stream* p) { return *reinterpret_cast<Transport*>(const_cast<pa_stream*>(p)); }
	pa_mainloop* loop() { return reinterpret_cast<pa_mainloop*>(this); }
	pa_context* context() { return reinterpret_cast<pa_context*>(this); }
	pa_stream* stream() { return reinterpret_cast<pa_stream*>(this); }

	void post(Event event)
	{ const std::lock_guard lock{mutex}; events.push_back(event); condition.notify_all(); }
	void reset()
	{
		const std::lock_guard lock{mutex}; events.clear(); fault = Fault::None; awakened = false;
		contextState = PA_CONTEXT_UNCONNECTED; streamState = PA_STREAM_UNCONNECTED;
	}
	Transport()
	{
		active = this; loopApi.userdata = this;
		api.mainloopNew = []() -> pa_mainloop* { if (active->fault == Fault::LoopNew) { return nullptr; } ++active->loops; return active->loop(); };
		api.mainloopFree = [](pa_mainloop* p) { ++from(p).loopFrees; };
		api.mainloopGetApi = [](pa_mainloop* p) { return &from(p).loopApi; };
		api.mainloopWakeup = [](pa_mainloop* p)
		{
			auto& t = from(p); const std::lock_guard lock{t.mutex};
			t.awakened = true; ++t.wakes; t.condition.notify_all();
		};
		api.mainloopPrepare = [](pa_mainloop* p, int)
		{
			auto& t = from(p); std::unique_lock lock{t.mutex};
			if (t.holdPrepare)
			{
				t.preparing = true; t.condition.notify_all();
				t.condition.wait(lock, [&] { return t.releasePrepare; });
			}
			// Match libpulse: preparing a poll clears previously posted wakeups.
			t.awakened = false;
			return t.fault == Fault::Prepare ? -1 : 0;
		};
		api.mainloopPoll = [](pa_mainloop* p)
		{
			auto& t = from(p); ++t.polls;
			if (t.fault == Fault::Poll) { return -1; }
			std::unique_lock lock{t.mutex}; t.blocking = true;
			t.condition.wait(lock, [&] { return t.awakened || !t.events.empty(); }); t.blocking = false;
			return 1;
		};
		api.mainloopDispatch = [](pa_mainloop* p)
		{
			auto& t = from(p); ++t.dispatches;
			if (t.fault == Fault::Dispatch) { return -1; }
			{
				const std::lock_guard lock{t.mutex}; if (t.events.empty()) { return 0; }
				t.current = t.events.front(); t.events.pop_front();
			}
			switch (t.current)
			{
			case Event::ContextReady: t.contextState = PA_CONTEXT_READY; break;
			case Event::ContextFailed: t.contextState = PA_CONTEXT_FAILED; break;
			case Event::ContextTerminated: t.contextState = PA_CONTEXT_TERMINATED; break;
			case Event::StreamReady: t.streamState = PA_STREAM_READY; break;
			case Event::StreamFailed: t.streamState = PA_STREAM_FAILED; break;
			case Event::StreamTerminated: t.streamState = PA_STREAM_TERMINATED; break;
			case Event::PrepareError: t.fault = Fault::Prepare; return 0;
			case Event::PollError: t.fault = Fault::Poll; return 0;
			case Event::DispatchError: return -1;
			case Event::Write:
				if (t.writeCallback) { t.writeCallback(t.stream(), t.requestBytes, t.writeData); }
				return 0;
			}
			if (t.current == Event::ContextReady || t.current == Event::ContextFailed || t.current == Event::ContextTerminated)
			{ if (t.contextCallback) { t.contextCallback(t.context(), t.contextData); } }
			else if (t.stateCallback) { t.stateCallback(t.stream(), t.streamData); }
			return 0;
		};
		api.contextNew = [](pa_mainloop_api* a, const char*) -> pa_context*
		{
			auto& t = *static_cast<Transport*>(a->userdata);
			if (t.fault == Fault::ThrowContextNew) { throw std::runtime_error("context allocation fixture"); }
			if (t.fault == Fault::ContextNew) { return nullptr; }
			++t.contexts; return t.context();
		};
		api.contextUnref = [](pa_context* p)
		{
			auto& t = from(p); std::unique_lock lock{t.mutex};
			if (t.holdCleanup)
			{
				t.cleaning = true; t.condition.notify_all();
				t.condition.wait(lock, [&] { return t.releaseCleanup; }); t.holdCleanup = false;
			}
			++t.contextFrees;
		};
		api.contextConnect = [](pa_context* p, const char*, pa_context_flags_t, const pa_spawn_api*)
		{
			auto& t = from(p); if (t.fault == Fault::ContextConnect) { return -1; }
			if (t.fault != Fault::ContextTimeout)
			{ t.post(t.fault == Fault::ContextFailed ? Event::ContextFailed : t.fault == Fault::ContextTerminated ? Event::ContextTerminated : Event::ContextReady); }
			return 0;
		};
		api.contextDisconnect = [](pa_context*) { };
		api.contextError = [](const pa_context* p) { return from(p).nativeError; };
		api.contextState = [](const pa_context* p)
		{ if (from(p).fault == Fault::ThrowContextCallback) { throw std::runtime_error("context callback fixture"); } return from(p).contextState; };
		api.contextSetCallback = [](pa_context* p, pa_context_notify_cb_t callback, void* data)
		{ auto& t = from(p); t.contextCallback = callback; t.contextData = data; if (!callback) { ++t.callbackReleases; } };
		api.streamNew = [](pa_context* p, const char*, const pa_sample_spec* spec, const pa_channel_map*) -> pa_stream*
		{ auto& t = from(p); if (t.fault == Fault::StreamNew) { return nullptr; } ++t.streams; t.spec = *spec; return t.stream(); };
		api.streamUnref = [](pa_stream* p) { ++from(p).streamFrees; };
		api.streamConnect = [](pa_stream* p, const char* sink, const pa_buffer_attr* attributes, pa_stream_flags_t, const pa_cvolume*, pa_stream*)
		{
			auto& t = from(p); t.sink = sink ? QByteArray(sink) : QByteArray{}; t.attributes = *attributes;
			if (t.fault == Fault::StreamConnect) { return -1; }
			if (t.fault != Fault::StreamTimeout)
			{ t.post(t.fault == Fault::StreamFailed ? Event::StreamFailed : t.fault == Fault::StreamTerminated ? Event::StreamTerminated : Event::StreamReady); }
			return 0;
		};
		api.streamDisconnect = [](pa_stream*) { return 0; };
		api.streamContext = [](const pa_stream* p) { return from(p).context(); };
		api.streamState = [](const pa_stream* p)
		{ if (from(p).fault == Fault::ThrowStreamCallback) { throw std::runtime_error("stream callback fixture"); } return from(p).streamState; };
		api.streamSetStateCallback = [](pa_stream* p, pa_stream_notify_cb_t callback, void* data)
		{ auto& t = from(p); t.stateCallback = callback; t.streamData = data; if (!callback) { ++t.callbackReleases; } };
		api.streamSetWriteCallback = [](pa_stream* p, pa_stream_request_cb_t callback, void* data)
		{ auto& t = from(p); t.writeCallback = callback; t.writeData = data; if (!callback) { ++t.callbackReleases; } };
		api.streamBeginWrite = [](pa_stream* p, void** data, std::size_t* bytes)
		{
			auto& t = from(p); ++t.writeRequests;
			if (t.fault == Fault::ThrowBeginWrite) { throw std::runtime_error("write callback fixture"); }
			if (t.fault == Fault::BeginWrite) { return -1; }
			t.beginRequested = *bytes;
			if (t.maxLoanBytes) { *bytes = std::min(*bytes, t.maxLoanBytes); }
			t.loan.assign((*bytes + sizeof(float) - 1) / sizeof(float) + 2, 42.f); t.borrowed = true;
			*data = t.fault == Fault::NullBuffer ? nullptr : t.loan.data() + 1;
			if (t.fault == Fault::MisalignedPointer) { *data = reinterpret_cast<char*>(t.loan.data() + 1) + 1; }
			if (t.fault == Fault::ZeroBuffer) { *bytes = 0; }
			if (t.fault == Fault::OversizedBuffer) { *bytes = t.beginRequested + 8; }
			if (t.fault == Fault::UnalignedBuffer) { --*bytes; }
			if (t.onBegin) { t.onBegin(); }
			return 0;
		};
		api.streamCancelWrite = [](pa_stream* p)
		{
			auto& t = from(p); t.guardsIntact &= t.borrowed && t.loan.front() == 42.f && t.loan.back() == 42.f;
			t.borrowed = false; ++t.cancels; return 0;
		};
		api.streamWrite = [](pa_stream* p, const void* data, std::size_t bytes, pa_free_cb_t, std::int64_t, pa_seek_mode_t)
		{
			auto& t = from(p); t.guardsIntact &= t.borrowed && t.loan.front() == 42.f && t.loan.back() == 42.f;
			t.borrowed = false;
			if (t.fault == Fault::Write) { return -1; }
			const auto samples = static_cast<const float*>(data); t.pcm.insert(t.pcm.end(), samples, samples + bytes / sizeof(float));
			++t.writes; return 0;
		};
	}
	~Transport() { active = nullptr; }
};
}

class PulseAudioTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void verifyReleased(const Transport& t)
	{
		QCOMPARE(t.loopFrees.load(), t.loops.load()); QCOMPARE(t.contextFrees.load(), t.contexts.load()); QCOMPARE(t.streamFrees.load(), t.streams.load());
		QVERIFY(!t.borrowed); QVERIFY(t.guardsIntact);
		QVERIFY(!t.contextCallback); QVERIFY(!t.stateCallback); QVERIFY(!t.writeCallback);
		QCOMPARE(t.callbackReleases.load(), t.contexts.load() + 2 * t.streams.load());
	}
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); NotePlayHandleManager::init(); }
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->clearProject();
		ConfigManager::inst()->setValue("audiopa", "device", "default");
	}
	void cleanup() { if (Engine::audioEngine()) { Engine::destroy(); } }

	void nativeStartupFailuresReleasePartialResourcesAndCanRestart_data()
	{
		QTest::addColumn<int>("fault");
		for (const auto value : {Fault::LoopNew, Fault::ContextNew, Fault::ContextConnect, Fault::ContextFailed, Fault::ContextTerminated,
			Fault::StreamNew, Fault::StreamConnect, Fault::StreamFailed, Fault::StreamTerminated, Fault::Prepare, Fault::Poll, Fault::Dispatch,
			Fault::ThrowContextNew, Fault::ThrowContextCallback, Fault::ThrowStreamCallback, Fault::ContextTimeout, Fault::StreamTimeout})
		{ QTest::newRow(qPrintable(QString::number(static_cast<int>(value)))) << static_cast<int>(value); }
	}
	void nativeStartupFailuresReleasePartialResourcesAndCanRestart()
	{
		QFETCH(int, fault); Transport transport; transport.fault = static_cast<Fault>(fault);
		auto device = PulseAudioTestAccess::create(transport.api, 500);
		std::atomic<int> periods{0}; const auto observed = QObject::connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer,
			Engine::audioEngine(), [&](const SampleFrame*) { ++periods; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); });
		device->startProcessing(); QVERIFY(!device->AudioDevice::isRunning()); QVERIFY(!device->QThread::isRunning());
		const auto status = device->processingStatus();
		const bool exception = transport.fault == Fault::ThrowContextNew || transport.fault == Fault::ThrowContextCallback || transport.fault == Fault::ThrowStreamCallback;
		QCOMPARE(status.failure, exception ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Start);
		const bool timeout = transport.fault == Fault::ContextTimeout || transport.fault == Fault::StreamTimeout;
		QCOMPARE(status.nativeError, timeout ? PA_ERR_TIMEOUT : exception || transport.fault == Fault::LoopNew || transport.fault == Fault::ContextNew ? PA_ERR_INTERNAL : PA_ERR_CONNECTIONREFUSED);
		QCOMPARE(periods.load(), 0); QCOMPARE(transport.writeRequests.load(), 0); verifyReleased(transport);
		transport.reset(); device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
		device->stopProcessing(); QVERIFY(!device->QThread::isRunning()); verifyReleased(transport);
	}

	void contextReadinessCannotCommitAPlaybackStreamAndCancellationJoins_data()
	{ QTest::addColumn<bool>("contextReady"); QTest::newRow("context-pending") << false; QTest::newRow("stream-pending") << true; }
	void contextReadinessCannotCommitAPlaybackStreamAndCancellationJoins()
	{
		QFETCH(bool, contextReady); Transport transport; transport.fault = contextReady ? Fault::StreamTimeout : Fault::ContextTimeout;
		auto device = PulseAudioTestAccess::create(transport.api); bool observedPending = false;
		FileResourceIO::Scope cancellation([&]
		{
			observedPending = contextReady ? transport.streams.load() > 0 : transport.contexts.load() > 0;
			return observedPending;
		});
		QVERIFY_EXCEPTION_THROWN(device->startProcessing(), std::runtime_error); QVERIFY(observedPending);
		QVERIFY(!device->AudioDevice::isRunning()); QVERIFY(!device->QThread::isRunning());
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Start); QCOMPARE(device->processingStatus().nativeError, PA_ERR_KILLED);
		verifyReleased(transport);
	}

	void configuredSinkAndExactNativePeriodReachPulseAudio_data()
	{ QTest::addColumn<QString>("sink"); QTest::newRow("server-policy") << QString("default"); QTest::newRow("explicit-sink") << QString("studio.monitor"); }
	void configuredSinkAndExactNativePeriodReachPulseAudio()
	{
		QFETCH(QString, sink); ConfigManager::inst()->setValue("audiopa", "device", sink);
		Transport transport; auto device = PulseAudioTestAccess::create(transport.api); device->startProcessing();
		QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing();
		QCOMPARE(transport.sink, sink == "default" ? QByteArray{} : sink.toUtf8());
		QCOMPARE(transport.attributes.tlength, static_cast<std::uint32_t>(Engine::audioEngine()->framesPerAudioBuffer() * 2 * sizeof(float)));
		QCOMPARE(transport.attributes.prebuf, std::uint32_t{0}); QCOMPARE(transport.spec.rate, device->sampleRate()); QCOMPARE(transport.spec.channels, std::uint8_t{2});
		verifyReleased(transport);
	}

	void successfulStartWaitsForThePlaybackStream()
	{
		Transport transport; transport.fault = Fault::StreamTimeout; auto device = PulseAudioTestAccess::create(transport.api);
		int pendingChecks = 0; bool released = false;
		FileResourceIO::Scope observe([&]
		{
			if (transport.streams.load() > 0 && !released && ++pendingChecks == 3)
			{ released = true; transport.post(Event::StreamReady); }
			return false;
		});
		device->startProcessing(); QVERIFY(released); QCOMPARE(pendingChecks, 3); QVERIFY(device->AudioDevice::isRunning());
		device->stopProcessing(); verifyReleased(transport);
	}

	void nativeDisconnectionStopsRenderingAndSupportsImmediateRestart_data()
	{
		QTest::addColumn<int>("event");
		for (const auto value : {Event::ContextFailed, Event::ContextTerminated, Event::StreamFailed, Event::StreamTerminated,
			Event::PrepareError, Event::PollError, Event::DispatchError})
		{ QTest::newRow(qPrintable(QString::number(static_cast<int>(value)))) << static_cast<int>(value); }
	}
	void nativeDisconnectionStopsRenderingAndSupportsImmediateRestart()
	{
		QFETCH(int, event); Transport transport; transport.nativeError = PA_OK; auto device = PulseAudioTestAccess::create(transport.api);
		std::atomic<int> periods{0}; const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer,
			Engine::audioEngine(), [&](const SampleFrame*) { ++periods; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); });
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); transport.post(static_cast<Event>(event));
		QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000);
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Disconnected);
		QCOMPARE(device->processingStatus().nativeError, PA_ERR_CONNECTIONTERMINATED);
		// Do not stop first: start must join the failed worker itself.
		// Reset faults only after its native cleanup is observable.
		QVERIFY(device->wait(2000)); verifyReleased(transport); transport.reset();
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing();
		QCOMPARE(periods.load(), 0); verifyReleased(transport);
	}

	void restartJoinsTheFailedWorkerBeforeResettingReadiness()
	{
		Transport transport; auto device = PulseAudioTestAccess::create(transport.api);
		const auto release = qScopeGuard([&]
		{
			const std::lock_guard lock{transport.mutex}; transport.releaseCleanup = true; transport.condition.notify_all();
		});
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		{ const std::lock_guard lock{transport.mutex}; transport.holdCleanup = true; }
		transport.post(Event::StreamFailed);
		QTRY_VERIFY_WITH_TIMEOUT(transport.cleaning.load(), 2000); QVERIFY(!device->AudioDevice::isRunning());
		const auto wakes = transport.wakes.load();
		std::thread finishCleanup([&]
		{
			std::unique_lock lock{transport.mutex}; transport.condition.wait(lock, [&] { return transport.wakes.load() > wakes; });
			transport.releaseCleanup = true; transport.condition.notify_all();
		});
		device->startProcessing(); finishCleanup.join();
		QVERIFY(device->AudioDevice::isRunning()); QCOMPARE(transport.loops.load(), 2); QCOMPARE(transport.loopFrees.load(), 1);
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
		device->stopProcessing(); verifyReleased(transport);
	}

	void stopAndDestructionWakeABlockedPoll_data()
	{ QTest::addColumn<bool>("destroy"); QTest::newRow("stop-restart") << false; QTest::newRow("destructor") << true; }
	void stopAndDestructionWakeABlockedPoll()
	{
		QFETCH(bool, destroy); Transport transport; auto device = PulseAudioTestAccess::create(transport.api);
		for (int attempt = 0; attempt < (destroy ? 1 : 12); ++attempt)
		{
			device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
			QTRY_VERIFY_WITH_TIMEOUT(transport.blocking.load(), 2000);
			QElapsedTimer elapsed; elapsed.start();
			if (destroy) { device.reset(); }
			else { device->stopProcessing(); QVERIFY(!device->QThread::isRunning()); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None); }
			QVERIFY(elapsed.elapsed() < 1000); verifyReleased(transport);
		}
		QVERIFY(transport.wakes.load() > 0);
	}

	void stopBetweenPrepareAndPollCannotLoseItsWakeup()
	{
		Transport transport; auto device = PulseAudioTestAccess::create(transport.api); device->startProcessing();
		QVERIFY(device->AudioDevice::isRunning()); QTRY_VERIFY_WITH_TIMEOUT(transport.blocking.load(), 2000);
		{
			const std::lock_guard lock{transport.mutex}; transport.holdPrepare = true;
		}
		const auto release = qScopeGuard([&]
		{
			const std::lock_guard lock{transport.mutex}; transport.releasePrepare = true; transport.condition.notify_all();
		});
		transport.post(Event::Write); // Zero-length write, followed by a held prepare.
		QTRY_VERIFY_WITH_TIMEOUT(transport.preparing.load(), 2000); const auto polls = transport.polls.load();
		const auto wakes = transport.wakes.load();
		std::thread finishPrepare([&]
		{
			std::unique_lock lock{transport.mutex};
			transport.condition.wait(lock, [&] { return transport.wakes.load() > wakes; });
			transport.releasePrepare = true; transport.condition.notify_all();
		});
		device->stopProcessing(); finishPrepare.join();
		QVERIFY(!device->QThread::isRunning()); QCOMPARE(transport.polls.load(), polls); verifyReleased(transport);
	}

	void nativePcmFillsOnlyBorrowedFramesAcrossShortAndLongRequests_data()
	{
		QTest::addColumn<int>("loanFrames");
		QTest::newRow("native-period") << 0; QTest::newRow("short-server-loans") << 13;
	}
	void nativePcmFillsOnlyBorrowedFramesAcrossShortAndLongRequests()
	{
		QFETCH(int, loanFrames); Transport transport;
		const auto destroy = qScopeGuard([] { Engine::destroy(); });
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		auto clip = static_cast<SampleClip*>(song->tracks().at(0)->getClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(engine->outputSampleRate(), SampleFrame(.25f, -.125f)), engine->outputSampleRate()));
		song->setMasterVolume(100);
		const auto frames = static_cast<std::size_t>(engine->framesPerAudioBuffer()) * 8 + 13;
		transport.requestBytes = frames * 2 * sizeof(float); transport.maxLoanBytes = loanFrames * 2 * sizeof(float);
		StudioBackendSelectionTestAccess::select([&] { return PulseAudioTestAccess::create(transport.api); });
		QVERIFY(engine->audioDev()->isRunning()); song->playSong(); transport.post(Event::Write);
		const auto targetWrites = loanFrames ? (frames + loanFrames - 1) / loanFrames : 9;
		QTRY_COMPARE_WITH_TIMEOUT(transport.writes.load(), static_cast<int>(targetWrites), 3000);
		engine->audioDev()->stopProcessing(); song->stop(); verifyReleased(transport);
		QCOMPARE(transport.pcm.size(), frames * 2); QVERIFY(transport.cancels.load() == 0);
		float leftPeak = 0, rightMinimum = 0;
		for (std::size_t i = 0; i < transport.pcm.size(); i += 2)
		{
			QVERIFY(std::isfinite(transport.pcm[i])); QVERIFY(std::isfinite(transport.pcm[i + 1]));
			leftPeak = std::max(leftPeak, transport.pcm[i]); rightMinimum = std::min(rightMinimum, transport.pcm[i + 1]);
		}
		QVERIFY(std::abs(leftPeak - .25f) < .0001f); QVERIFY(std::abs(rightMinimum + .125f) < .0001f);
	}

	void malformedAndFailedWritesPublishNativeFailureWithoutOverrunningLoans_data()
	{
		QTest::addColumn<int>("fault");
		for (const auto value : {Fault::BeginWrite, Fault::NullBuffer, Fault::ZeroBuffer, Fault::OversizedBuffer, Fault::UnalignedBuffer, Fault::MisalignedPointer, Fault::Write, Fault::ThrowBeginWrite})
		{ QTest::newRow(qPrintable(QString::number(static_cast<int>(value)))) << static_cast<int>(value); }
	}
	void malformedAndFailedWritesPublishNativeFailureWithoutOverrunningLoans()
	{
		QFETCH(int, fault); Transport transport; transport.fault = static_cast<Fault>(fault); transport.requestBytes = 13 * 2 * sizeof(float);
		auto device = PulseAudioTestAccess::create(transport.api); device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		transport.post(Event::Write); QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000);
		device->stopProcessing(); verifyReleased(transport); QCOMPARE(transport.writes.load(), 0);
		const auto kind = static_cast<Fault>(fault);
		const bool shape = kind == Fault::NullBuffer || kind == Fault::ZeroBuffer || kind == Fault::OversizedBuffer || kind == Fault::UnalignedBuffer || kind == Fault::MisalignedPointer;
		QCOMPARE(device->processingStatus().failure, shape ? AudioDevice::ProcessingFailure::InvalidCallback
			: kind == Fault::ThrowBeginWrite ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Disconnected);
		QCOMPARE(transport.cancels.load(), shape ? 1 : 0);
		if (shape) { QVERIFY(std::all_of(transport.loan.begin(), transport.loan.end(), [](float value) { return value == 42.f; })); }
	}

	void emptyAndUnalignedRequestsNeverBorrowABuffer_data()
	{ QTest::addColumn<int>("bytes"); QTest::newRow("empty") << 0; QTest::newRow("unaligned") << 7; }
	void emptyAndUnalignedRequestsNeverBorrowABuffer()
	{
		QFETCH(int, bytes); Transport transport; transport.requestBytes = bytes; auto device = PulseAudioTestAccess::create(transport.api);
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); const auto before = transport.dispatches.load(); transport.post(Event::Write);
		QTRY_VERIFY_WITH_TIMEOUT(transport.dispatches.load() > before, 2000);
		if (bytes) { QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000); }
		device->stopProcessing(); verifyReleased(transport); QCOMPARE(transport.writeRequests.load(), 0);
		QCOMPARE(device->processingStatus().failure, bytes ? AudioDevice::ProcessingFailure::InvalidCallback : AudioDevice::ProcessingFailure::None);
	}

	void stopAfterBorrowingCancelsWithoutRenderingOrSubmitting()
	{
		Transport transport; transport.requestBytes = 13 * 2 * sizeof(float); std::atomic_bool borrowed{false}, releaseLoan{false};
		transport.onBegin = [&]
		{
			borrowed = true;
			std::unique_lock lock{transport.mutex}; transport.condition.wait(lock, [&] { return releaseLoan.load(); });
		};
		auto device = PulseAudioTestAccess::create(transport.api);
		const auto release = qScopeGuard([&]
		{ const std::lock_guard lock{transport.mutex}; releaseLoan = true; transport.condition.notify_all(); });
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); transport.post(Event::Write);
		QTRY_VERIFY_WITH_TIMEOUT(borrowed.load(), 2000); const auto wakes = transport.wakes.load();
		std::thread finishLoan([&]
		{
			std::unique_lock lock{transport.mutex}; transport.condition.wait(lock, [&] { return transport.wakes.load() > wakes; });
			releaseLoan = true; transport.condition.notify_all();
		});
		device->stopProcessing(); finishLoan.join();
		QCOMPARE(transport.cancels.load(), 1); QCOMPARE(transport.writes.load(), 0);
		QVERIFY(std::all_of(transport.loan.begin(), transport.loan.end(), [](float value) { return value == 42.f; }));
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None); verifyReleased(transport);
	}

	void failedSelectionRestoresPriorNativeBackend_data()
	{
		QTest::addColumn<int>("fault"); QTest::addColumn<bool>("running");
		for (const auto value : {Fault::StreamTimeout, Fault::StreamConnect, Fault::StreamFailed})
			for (bool running : {false, true})
			{ QTest::newRow(qPrintable(QString::number(static_cast<int>(value)) + (running ? "-running" : "-stopped"))) << static_cast<int>(value) << running; }
	}
	void failedSelectionRestoresPriorNativeBackend()
	{
		QFETCH(int, fault); QFETCH(bool, running); Transport transport; transport.fault = static_cast<Fault>(fault);
		const auto engine = Engine::audioEngine(); const auto prior = engine->audioDev(); const auto priorName = engine->audioDevName(); const auto midi = engine->midiClient();
		if (running) { prior->startProcessing(); }
		const auto configured = ConfigManager::inst()->value("audioengine", "audiodev");
		QVERIFY_EXCEPTION_THROWN(StudioBackendSelectionTestAccess::select([&] { return PulseAudioTestAccess::create(transport.api, 500); }), std::runtime_error);
		QCOMPARE(engine->audioDev(), prior); QCOMPARE(prior->isRunning(), running); QCOMPARE(engine->audioDevName(), priorName); QCOMPARE(engine->midiClient(), midi);
		QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!engine->isChangingAudioDevice()); QVERIFY(Engine::getSong()->isStopped());
		QCOMPARE(ConfigManager::inst()->value("audioengine", "audiodev"), configured); verifyReleased(transport);
	}

	void optionalRealServerStartupPlaybackAndRecovery()
	{
		const auto sink = qEnvironmentVariable("LMMS_TEST_PULSEAUDIO_SINK");
		if (sink.isEmpty()) { QSKIP("Set LMMS_TEST_PULSEAUDIO_SINK to an existing test sink; PULSE_SERVER may select an isolated server"); }
		ConfigManager::inst()->setValue("audiopa", "device", sink); const auto engine = Engine::audioEngine();
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			const auto response = StudioCapabilities::execute("audio.backend.select", {{"name", AudioPulseAudio::name()}});
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			QVERIFY(engine->audioDev()->isRunning()); Engine::getSong()->playSong();
			QTRY_VERIFY_WITH_TIMEOUT(Engine::getSong()->getPlayPos().getTicks() > 0, 3000); Engine::getSong()->stop();
			const auto prior = engine->audioDev();
			ConfigManager::inst()->setValue("audiopa", "device", "lmms-test-missing-sink-6b848763");
			const auto refused = StudioCapabilities::execute("audio.backend.select", {{"name", AudioPulseAudio::name()}});
			QVERIFY(!refused.value("ok").toBool()); QCOMPARE(engine->audioDev(), prior); QVERIFY(prior->isRunning());
			QVERIFY(!engine->hasStoredAudioDevice()); ConfigManager::inst()->setValue("audiopa", "device", sink);
			engine->selectAudioBackend(AudioDummy::name()); QVERIFY(engine->audioDev()->isRunning());
		}
	}
};

QTEST_GUILESS_MAIN(PulseAudioTest)
#include "PulseAudioTest.moc"

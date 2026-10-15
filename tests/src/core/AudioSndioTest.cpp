/* Native sndio lifecycle and PCM tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioDummy.h"
#include "AudioEngine.h"
#include "AudioSndio.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../../../src/core/audio/SndioApi.h"

namespace lmms
{
class AudioSndioTestAccess
{
public:
	static std::unique_ptr<AudioSndio> create(bool& successful, const detail::SndioApi& api, int timeout = 2000)
	{ return std::unique_ptr<AudioSndio>(new AudioSndio(successful, Engine::audioEngine(), api, timeout)); }
	static std::unique_ptr<AudioSndio> configured(const detail::SndioApi& api, int timeout = 2000)
	{
		bool successful = false; auto device = create(successful, api, timeout);
		if (!successful) { throw std::runtime_error("sndio fixture configuration failed"); }
		return device;
	}
	static bool stopping(AudioSndio& device) { return device.m_stopRequested.load(); }
};
class StudioBackendSelectionTestAccess
{
public:
	static void select(const std::function<std::unique_ptr<AudioDevice>()>& factory)
	{ Engine::audioEngine()->selectAudioBackend(AudioSndio::name(), factory); }
};
}

using namespace lmms;
namespace
{
constexpr auto All = std::numeric_limits<std::size_t>::max();
struct Transport
{
	static inline Transport* active = nullptr;
	detail::SndioApi api;
	QString setupFault;
	int rateDelta = 0, nativeDescriptors = 1, returnedDescriptors = 1;
	bool failStart = false, throwStart = false, failStop = false, throwStop = false;
	bool throwWrite = false, throwPoll = false, throwEvents = false, eofOnWrite = false;
	bool blockStartUntilStopped = false;
	std::function<bool()> stopping;
	std::function<int()> onStop;
	std::atomic_bool eof{false}, exhausted{false};
	std::atomic<int> opened{0}, closed{0}, starts{0}, stops{0}, writes{0}, polls{0}, decoded{0}, descriptorCalls{0};
	sio_par requested{};
	QByteArray deviceName;
	unsigned int mode = 0;
	int nonblocking = 0;
	std::deque<std::size_t> writeResults;
	std::deque<int> pollResults, eventResults;
	int defaultPoll = 0, defaultEvents = 0;
	std::vector<unsigned char> firstBuffer, accepted;
	std::vector<std::size_t> requests;
	static Transport& from(sio_hdl* handle) { return *reinterpret_cast<Transport*>(handle); }
	Transport()
	{
		active = this;
		api.open = [](const char* name, unsigned int mode, int nonblocking) -> sio_hdl*
		{
			auto& t = *active; t.deviceName = name ? name : ""; t.mode = mode; t.nonblocking = nonblocking;
			if (t.setupFault == "open") { return nullptr; }
			++t.opened; t.eof = false; return reinterpret_cast<sio_hdl*>(&t);
		};
		api.close = [](sio_hdl* handle) { ++from(handle).closed; };
		api.setParameters = [](sio_hdl* handle, sio_par* par)
		{
			auto& t = from(handle); t.requested = *par;
			if (t.setupFault == "throw-set") { throw std::runtime_error("sndio set fixture"); }
			return t.setupFault == "set" ? 0 : 1;
		};
		api.getParameters = [](sio_hdl* handle, sio_par* par)
		{
			auto& t = from(handle); if (t.setupFault == "throw-get") { throw std::runtime_error("sndio get fixture"); }
			if (t.setupFault == "get") { return 0; }
			*par = t.requested; par->rate = static_cast<unsigned int>(static_cast<std::int64_t>(par->rate) + t.rateDelta);
			if (t.setupFault == "channels") { par->pchan = 1; }
			if (t.setupFault == "bits") { par->bits = 24; }
			if (t.setupFault == "bytes") { par->bps = 4; }
			if (t.setupFault == "unsigned") { par->sig = 0; }
			if (t.setupFault == "endian") { par->le = !SIO_LE_NATIVE; }
			if (t.setupFault == "rate-zero") { par->rate = 0; }
			if (t.setupFault == "rate-too-far") { par->rate += par->rate / 10; }
			if (t.setupFault == "round-zero") { par->round = 0; }
			if (t.setupFault == "buffer-zero") { par->appbufsz = 0; }
			if (t.setupFault == "eof") { t.eof = true; }
			return 1;
		};
		api.start = [](sio_hdl* handle)
		{
			auto& t = from(handle); ++t.starts;
			if (t.blockStartUntilStopped) { while (!t.stopping()) { QThread::msleep(1); } }
			if (t.throwStart) { throw std::runtime_error("sndio start fixture"); }
			return t.failStart ? 0 : 1;
		};
		api.stop = [](sio_hdl* handle)
		{
			auto& t = from(handle); ++t.stops;
			if (t.onStop) { return t.onStop(); }
			if (t.throwStop) { throw std::runtime_error("sndio stop fixture"); }
			return t.failStop || t.eof.load() ? 0 : 1;
		};
		api.write = [](sio_hdl* handle, const void* data, std::size_t bytes)
		{
			auto& t = from(handle); if (t.throwWrite) { throw std::runtime_error("sndio write fixture"); }
			const auto source = static_cast<const unsigned char*>(data);
			if (t.firstBuffer.empty()) { t.firstBuffer.assign(source, source + bytes); }
			t.requests.push_back(bytes); std::size_t result = 0;
			if (!t.writeResults.empty()) { result = t.writeResults.front(); t.writeResults.pop_front(); }
			if (result == All) { result = bytes; }
			if (result <= bytes) { t.accepted.insert(t.accepted.end(), source, source + result); }
			if (t.eofOnWrite) { t.eof = true; }
			++t.writes; if (t.writeResults.empty()) { t.exhausted = true; }
			return result;
		};
		api.eof = [](sio_hdl* handle) { return from(handle).eof.load() ? 1 : 0; };
		api.descriptorCount = [](sio_hdl* handle) { return from(handle).nativeDescriptors; };
		api.descriptors = [](sio_hdl* handle, pollfd* fds, int events)
		{
			auto& t = from(handle); ++t.descriptorCalls;
			for (int i = 0; i < std::min(t.nativeDescriptors, t.returnedDescriptors); ++i)
			{ fds[i] = pollfd{10 + i, static_cast<short>(events), 0}; }
			return t.returnedDescriptors;
		};
		api.poll = [](pollfd*, nfds_t, int timeout)
		{
			auto& t = *active; ++t.polls; if (t.throwPoll) { throw std::runtime_error("poll fixture"); }
			auto result = t.defaultPoll;
			if (!t.pollResults.empty()) { result = t.pollResults.front(); t.pollResults.pop_front(); }
			if (result < 0) { errno = -result; return -1; }
			if (result == 0) { QThread::msleep(static_cast<unsigned long>(timeout)); }
			return result;
		};
		api.events = [](sio_hdl* handle, pollfd*)
		{
			auto& t = from(handle); ++t.decoded; if (t.throwEvents) { throw std::runtime_error("sndio events fixture"); }
			auto result = t.defaultEvents;
			if (!t.eventResults.empty()) { result = t.eventResults.front(); t.eventResults.pop_front(); }
			return result;
		};
	}
	~Transport() { active = nullptr; }
};
}

class AudioSndioTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); NotePlayHandleManager::init(); }
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->clearProject();
		ConfigManager::inst()->setValue("audiosndio", "device", "");
	}
	void cleanup() { if (Engine::audioEngine()) { Engine::destroy(); } }

	void constructionNegotiatesWithoutStartingAndReleasesEveryFailure_data()
	{
		QTest::addColumn<QString>("kind");
		for (const auto kind : {"default", "explicit", "rate-up", "rate-down", "rate-near-three", "rate-three", "open", "set", "get", "channels", "bits", "bytes",
			"unsigned", "endian", "rate-zero", "rate-too-far", "round-zero", "buffer-zero", "eof", "throw-set", "throw-get"})
		{ QTest::newRow(kind) << QString(kind); }
	}
	void constructionNegotiatesWithoutStartingAndReleasesEveryFailure()
	{
		QFETCH(QString, kind); Transport transport; transport.setupFault = kind;
		if (kind == "explicit") { ConfigManager::inst()->setValue("audiosndio", "device", "snd/studio"); }
		const auto requestedRate = Engine::audioEngine()->outputSampleRate();
		if (kind == "rate-up" || kind == "rate-down") { transport.rateDelta = static_cast<int>(requestedRate / 100) * (kind == "rate-up" ? 1 : -1); }
		if (kind == "rate-near-three") { transport.rateDelta = static_cast<int>(static_cast<std::uint64_t>(requestedRate) * 299 / 10000); }
		if (kind == "rate-three") { transport.rateDelta = static_cast<int>((static_cast<std::uint64_t>(requestedRate) * 3 + 99) / 100); }
		bool successful = false;
		if (kind.startsWith("throw-")) { QVERIFY_EXCEPTION_THROWN(AudioSndioTestAccess::create(successful, transport.api), std::runtime_error); }
		else
		{
			auto device = AudioSndioTestAccess::create(successful, transport.api);
			const bool valid = kind == "default" || kind == "explicit" || kind == "rate-up" || kind == "rate-down" || kind == "rate-near-three";
			QCOMPARE(successful, valid); QVERIFY(!device->AudioDevice::isRunning()); QCOMPARE(transport.starts.load(), 0);
			QCOMPARE(transport.closed.load(), valid ? 0 : transport.opened.load());
			if (valid)
			{
				QCOMPARE(device->sampleRate(), static_cast<sample_rate_t>(requestedRate + transport.rateDelta));
				QCOMPARE(transport.requested.bps, static_cast<unsigned int>(sizeof(int_sample_t))); QCOMPARE(transport.requested.sig, 1u);
				QCOMPARE(transport.deviceName, kind == "explicit" ? QByteArray("snd/studio") : QByteArray(SIO_DEVANY));
			}
		}
		QCOMPARE(transport.closed.load(), transport.opened.load()); QCOMPARE(transport.starts.load(), 0);
		QCOMPARE(transport.mode, static_cast<unsigned int>(SIO_PLAY)); QCOMPARE(transport.nonblocking, 1);
	}

	void cancellationAfterOpenClosesTheCandidate()
	{
		Transport transport; bool successful = false;
		FileResourceIO::Scope cancelled([&] { return transport.opened.load() > 0; });
		QVERIFY_EXCEPTION_THROWN(AudioSndioTestAccess::create(successful, transport.api), std::runtime_error);
		QVERIFY(!successful); QCOMPARE(transport.opened.load(), 1); QCOMPARE(transport.closed.load(), 1); QCOMPARE(transport.starts.load(), 0);
	}

	void startupFailureTimeoutAndCancellationJoinBeforeRetry_data()
	{
		QTest::addColumn<QString>("kind");
		for (const auto kind : {"start", "throw-start", "zero-fds", "negative-fds", "deadline", "cancel"})
		{ QTest::newRow(kind) << QString(kind); }
	}
	void startupFailureTimeoutAndCancellationJoinBeforeRetry()
	{
		QFETCH(QString, kind); Transport transport; auto device = AudioSndioTestAccess::configured(transport.api, kind == "deadline" ? 100 : 2000);
		transport.failStart = kind == "start"; transport.throwStart = kind == "throw-start";
		transport.nativeDescriptors = kind == "zero-fds" ? 0 : kind == "negative-fds" ? -1 : 1;
		transport.blockStartUntilStopped = kind == "deadline" || kind == "cancel"; transport.stopping = [&] { return AudioSndioTestAccess::stopping(*device); };
		FileResourceIO::Scope cancelled([&] { return kind == "cancel" && transport.starts.load() > 0; });
		if (kind == "cancel") { QVERIFY_EXCEPTION_THROWN(device->startProcessing(), std::runtime_error); }
		else { device->startProcessing(); }
		QVERIFY(!device->AudioDevice::isRunning()); QVERIFY(!device->QThread::isRunning()); QCOMPARE(transport.writes.load(), 0);
		QCOMPARE(device->processingStatus().failure, kind == "throw-start" ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Start);
		QCOMPARE(device->processingStatus().nativeError, kind == "deadline" ? -ETIMEDOUT : kind == "cancel" ? -ECANCELED : kind.endsWith("fds") ? -EINVAL : -EIO);
		FileResourceIO::Suspension recovery; transport.failStart = false; transport.throwStart = false; transport.nativeDescriptors = 1; transport.blockStartUntilStopped = false;
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
	}

	void unalignedByteWritesKeepTheNativeBufferAndDecodePollEvents()
	{
		Transport transport; transport.writeResults = {1, 3, 0, 7, 0, All};
		transport.pollResults = {-EINTR, 1, 1, 1}; transport.eventResults = {0, POLLOUT, POLLOUT};
		auto device = AudioSndioTestAccess::configured(transport.api); device->startProcessing();
		QTRY_VERIFY_WITH_TIMEOUT(transport.exhausted.load(), 2000); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
		QVERIFY(transport.accepted == transport.firstBuffer); QVERIFY(!transport.firstBuffer.empty());
		const auto bytes = static_cast<std::size_t>(Engine::audioEngine()->framesPerAudioBuffer()) * 2 * sizeof(int_sample_t);
		QCOMPARE(transport.requests.at(0), bytes); QCOMPARE(transport.requests.at(1), bytes - 1); QCOMPARE(transport.requests.at(2), bytes - 4);
		QCOMPARE(transport.requests.at(3), bytes - 4); QCOMPARE(transport.requests.at(4), bytes - 11); QCOMPARE(transport.requests.at(5), bytes - 11);
		QVERIFY(transport.polls.load() >= 4); QVERIFY(transport.decoded.load() >= 3); QCOMPARE(transport.stops.load(), 1);
	}

	void fatalTransferAndPollResultsPublishFailure_data()
	{
		QTest::addColumn<QString>("kind");
		for (const auto kind : {"eof", "oversized-write", "negative-fds", "oversized-fds", "oversized-poll", "poll-error", "hangup", "pollerr", "invalid-fd",
			"negative-events", "throw-write", "throw-poll", "throw-events"})
		{ QTest::newRow(kind) << QString(kind); }
	}
	void fatalTransferAndPollResultsPublishFailure()
	{
		QFETCH(QString, kind); Transport transport; transport.defaultPoll = 1;
		transport.eofOnWrite = kind == "eof"; transport.throwWrite = kind == "throw-write"; transport.throwPoll = kind == "throw-poll"; transport.throwEvents = kind == "throw-events";
		if (kind == "oversized-write") { transport.writeResults = {All - 1}; }
		if (kind == "negative-fds") { transport.returnedDescriptors = -1; }
		if (kind == "oversized-fds") { transport.returnedDescriptors = 2; }
		if (kind == "poll-error") { transport.defaultPoll = -EBADF; }
		if (kind == "oversized-poll") { transport.defaultPoll = 2; }
		transport.defaultEvents = kind == "hangup" ? POLLHUP : kind == "pollerr" ? POLLERR : kind == "invalid-fd" ? POLLNVAL : kind == "negative-events" ? -1 : 0;
		auto device = AudioSndioTestAccess::configured(transport.api); device->startProcessing();
		QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000); device->stopProcessing(); QVERIFY(!device->QThread::isRunning());
		const bool malformed = kind == "oversized-write" || kind == "negative-fds" || kind == "oversized-fds" || kind == "oversized-poll";
		QCOMPARE(device->processingStatus().failure, malformed ? AudioDevice::ProcessingFailure::InvalidCallback
			: kind.startsWith("throw-") ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Disconnected);
		QCOMPARE(device->processingStatus().nativeError, malformed ? -EOVERFLOW : kind == "poll-error" ? -EBADF : -EIO);
		QCOMPARE(transport.stops.load(), 1); QVERIFY(transport.accepted.empty());
	}

	void stalledOutputExpiresWithoutRenderingAhead_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("timeout") << 0; QTest::newRow("ready-but-zero") << 1;
		QTest::newRow("no-descriptors") << 2; QTest::newRow("control-events-only") << 3;
	}
	void stalledOutputExpiresWithoutRenderingAhead()
	{
		QFETCH(int, kind); Transport transport;
		transport.defaultPoll = kind == 0 ? 0 : 1; transport.defaultEvents = kind == 1 ? POLLOUT : 0; transport.returnedDescriptors = kind == 2 ? 0 : 1;
		std::atomic<int> periods{0}; const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer,
			Engine::audioEngine(), [&](const SampleFrame*) { ++periods; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); });
		auto device = AudioSndioTestAccess::configured(transport.api, 100); device->startProcessing();
		QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Disconnected); QCOMPARE(device->processingStatus().nativeError, -ETIMEDOUT);
		QVERIFY(periods.load() <= Engine::audioEngine()->framesPerAudioBuffer() / Engine::audioEngine()->framesPerPeriod() + 1);
		QVERIFY(transport.writes.load() < 1000); QVERIFY(transport.accepted.empty());
	}

	void stopAndDestructionJoinBeforeClosing_data()
	{ QTest::addColumn<bool>("destroy"); QTest::newRow("stop-restart") << false; QTest::newRow("destruction") << true; }
	void stopAndDestructionJoinBeforeClosing()
	{
		QFETCH(bool, destroy); Transport transport; auto device = AudioSndioTestAccess::configured(transport.api);
		for (int i = 0; i < (destroy ? 1 : 5); ++i)
		{
			const auto polls = transport.polls.load(); device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
			QTRY_VERIFY_WITH_TIMEOUT(transport.polls.load() > polls, 2000); QElapsedTimer elapsed; elapsed.start();
			if (destroy) { device.reset(); }
			else { device->stopProcessing(); QVERIFY(!device->QThread::isRunning()); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None); }
			QVERIFY(elapsed.elapsed() < 1000); QCOMPARE(transport.stops.load(), i + 1); QCOMPARE(transport.closed.load(), destroy ? 1 : 0);
		}
	}

	void eofReopensTheSameSinkAndRefusesASilentRateChange_data()
	{ QTest::addColumn<bool>("changedRate"); QTest::newRow("same-rate") << false; QTest::newRow("changed-rate") << true; }
	void eofReopensTheSameSinkAndRefusesASilentRateChange()
	{
		QFETCH(bool, changedRate); Transport transport; auto device = AudioSndioTestAccess::configured(transport.api); const auto rate = device->sampleRate();
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); transport.eof = true;
		QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Disconnected);
		transport.rateDelta = changedRate ? static_cast<int>(rate / 100) : 0; device->startProcessing();
		QCOMPARE(transport.opened.load(), 2); QCOMPARE(device->sampleRate(), rate);
		if (changedRate)
		{
			QVERIFY(!device->AudioDevice::isRunning()); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Start); QCOMPARE(transport.closed.load(), 2);
			transport.rateDelta = 0; device->startProcessing(); QCOMPARE(transport.opened.load(), 3);
		}
		QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing(); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
	}

	void stopFailuresAreVisibleAndDoNotPoisonTheNextStart_data()
	{ QTest::addColumn<bool>("exception"); QTest::newRow("failure") << false; QTest::newRow("exception") << true; }
	void stopFailuresAreVisibleAndDoNotPoisonTheNextStart()
	{
		QFETCH(bool, exception); Transport transport; transport.failStop = !exception; transport.throwStop = exception;
		auto device = AudioSndioTestAccess::configured(transport.api); device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, exception ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Stop);
		transport.failStop = transport.throwStop = false; device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing();
		QCOMPARE(transport.opened.load(), 2); QCOMPARE(transport.closed.load(), 1);
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
	}

	void failedReopenReleasesBothHandlesAndPermitsAnotherRetry_data()
	{
		QTest::addColumn<QString>("kind");
		for (const auto kind : {"open", "set", "throw-set", "cancel"}) { QTest::newRow(kind) << QString(kind); }
	}
	void failedReopenReleasesBothHandlesAndPermitsAnotherRetry()
	{
		QFETCH(QString, kind); Transport transport; auto device = AudioSndioTestAccess::configured(transport.api);
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); transport.eof = true;
		QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000); device->stopProcessing(); transport.setupFault = kind;
		FileResourceIO::Scope cancelled([&] { return kind == "cancel" && transport.opened.load() > 1; });
		if (kind == "throw-set" || kind == "cancel") { QVERIFY_EXCEPTION_THROWN(device->startProcessing(), std::runtime_error); }
		else { device->startProcessing(); }
		QVERIFY(!device->AudioDevice::isRunning()); QVERIFY(!device->QThread::isRunning()); QCOMPARE(transport.closed.load(), transport.opened.load());
		QCOMPARE(device->processingStatus().failure, kind == "throw-set" ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Start);
		QCOMPARE(device->processingStatus().nativeError, kind == "cancel" ? -ECANCELED : -EIO);
		FileResourceIO::Suspension recovery; transport.setupFault.clear(); device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		device->stopProcessing(); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
	}

	void lateFailedStopCannotOverwriteTheReplacementStart()
	{
		Transport transport; std::atomic_bool armed{false}, entered{false}, release{false}; auto device = AudioSndioTestAccess::configured(transport.api);
		const auto unblock = qScopeGuard([&] { release = true; });
		transport.onStop = [&]
		{
			if (!armed.load() || transport.stops.load() > 1) { return 1; }
			entered = true;
			while (!device->AudioDevice::isRunning() && !release.load()) { QThread::msleep(1); }
			return 0;
		};
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); armed = true;
		device->reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected, -EIO);
		QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000); device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None); QCOMPARE(transport.opened.load(), 2); QCOMPARE(transport.closed.load(), 1);
		device->stopProcessing(); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None); QCOMPARE(transport.stops.load(), 2);
	}

	void failedStartupRestoresPriorEngineOwnership_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("stopped") << false; QTest::newRow("running") << true; }
	void failedStartupRestoresPriorEngineOwnership()
	{
		QFETCH(bool, running); Transport transport; transport.failStart = true; const auto engine = Engine::audioEngine();
		const auto old = engine->audioDev(); const auto midi = engine->midiClient(); const auto name = engine->audioDevName(); if (running) { old->startProcessing(); }
		QVERIFY_EXCEPTION_THROWN(StudioBackendSelectionTestAccess::select([&] { return AudioSndioTestAccess::configured(transport.api); }), std::runtime_error);
		QCOMPARE(engine->audioDev(), old); QCOMPARE(engine->midiClient(), midi); QCOMPARE(engine->audioDevName(), name); QCOMPARE(old->isRunning(), running);
		QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!engine->isChangingAudioDevice()); QCOMPARE(transport.closed.load(), 1);
	}

	void nativeSamplePcmIsConvertedAndTransferredWithoutLosingOddBytes()
	{
		Transport transport; const auto destroy = qScopeGuard([] { Engine::destroy(); });
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		auto clip = static_cast<SampleClip*>(song->tracks().at(0)->getClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(engine->outputSampleRate(), SampleFrame(.25f, -.125f)), engine->outputSampleRate()));
		song->setMasterVolume(100); auto device = AudioSndioTestAccess::configured(transport.api); engine->setAudioDevice(device.release(), false);
		for (int i = 0; i < 8; ++i) { transport.writeResults.push_back(3); transport.writeResults.push_back(11); transport.writeResults.push_back(All); }
		song->playSong(); engine->audioDev()->startProcessing(); QTRY_VERIFY_WITH_TIMEOUT(transport.exhausted.load(), 2000);
		engine->audioDev()->stopProcessing(); song->stop(); QCOMPARE(engine->audioDev()->processingStatus().failure, AudioDevice::ProcessingFailure::None);
		QCOMPARE(transport.accepted.size(), static_cast<std::size_t>(engine->framesPerAudioBuffer()) * 8 * 2 * sizeof(int_sample_t));
		int_sample_t left = 0, right = 0;
		for (std::size_t offset = 0; offset < transport.accepted.size(); offset += 2 * sizeof(int_sample_t))
		{
			int_sample_t frame[2]; std::memcpy(frame, transport.accepted.data() + offset, sizeof(frame));
			left = std::max(left, frame[0]); right = std::min(right, frame[1]);
		}
		QCOMPARE(left, static_cast<int_sample_t>(.25f * OUTPUT_SAMPLE_MULTIPLIER)); QCOMPARE(right, static_cast<int_sample_t>(-.125f * OUTPUT_SAMPLE_MULTIPLIER));
	}

	void optionalRealDeviceCanPlayRestartAndRestoreAfterFailedSelection()
	{
		const auto name = qEnvironmentVariable("LMMS_TEST_SNDIO_DEVICE");
		if (name.isEmpty()) { QSKIP("Set LMMS_TEST_SNDIO_DEVICE to a test device, or default to use sndio's default device"); }
		ConfigManager::inst()->setValue("audiosndio", "device", name == "default" ? QString{} : name); const auto engine = Engine::audioEngine();
		const auto selected = StudioCapabilities::execute("audio.backend.select", {{"name", AudioSndio::name()}});
		QVERIFY2(selected.value("ok").toBool(), qPrintable(selected.value("error").toString()));
		for (int i = 0; i < 3; ++i)
		{
			Engine::getSong()->playSong(); QTRY_VERIFY_WITH_TIMEOUT(Engine::getSong()->getPlayPos().getTicks() > 0, 3000); Engine::getSong()->stop();
			engine->audioDev()->stopProcessing(); engine->audioDev()->startProcessing(); QVERIFY(engine->audioDev()->isRunning());
		}
		const auto old = engine->audioDev(); ConfigManager::inst()->setValue("audiosndio", "device", "lmms-missing-sndio-device-b8618a");
		QVERIFY(!StudioCapabilities::execute("audio.backend.select", {{"name", AudioSndio::name()}}).value("ok").toBool());
		QCOMPARE(engine->audioDev(), old); QVERIFY(old->isRunning()); QVERIFY(!engine->hasStoredAudioDevice());
		engine->selectAudioBackend(AudioDummy::name()); QVERIFY(engine->audioDev()->isRunning());
	}
};

QTEST_GUILESS_MAIN(AudioSndioTest)
#include "AudioSndioTest.moc"

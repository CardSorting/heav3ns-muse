/* Native ALSA configuration, transfer and recovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include "AudioAlsa.h"
#include "AudioDummy.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "../../../src/core/audio/AlsaPcmIo.h"

namespace lmms
{
class AudioAlsaTestAccess
{
public:
	static std::unique_ptr<AudioAlsa> create(const detail::AlsaPcmIo& io, int timeout = 1000, const QString& name = "null")
	{
		bool successful = false;
		auto device = std::unique_ptr<AudioAlsa>(new AudioAlsa(successful, Engine::audioEngine(), io, name, timeout));
		if (!successful) { throw std::runtime_error("Could not configure the real ALSA null PCM"); }
		return device;
	}
	static snd_pcm_t* handle(AudioAlsa& device) { return device.m_handle; }
	static bool stopping(AudioAlsa& device) { return device.m_stopRequested.load(); }
};
class StudioBackendSelectionTestAccess
{
public:
	static void select(const std::function<std::unique_ptr<AudioDevice>()>& factory)
	{ Engine::audioEngine()->selectAudioBackend(AudioAlsa::name(), factory); }
};
}

using namespace lmms;
namespace
{
constexpr auto All = std::numeric_limits<snd_pcm_sframes_t>::max();
struct Transfers
{
	static inline Transfers* active = nullptr;
	detail::AlsaPcmIo io;
	std::deque<snd_pcm_sframes_t> writes;
	std::deque<int> resumes;
	snd_pcm_sframes_t otherwise = -EAGAIN;
	int waitResult = 0, prepareError = 0, failPrepareAt = 1, dropError = 0;
	bool throwPrepare = false, throwWrite = false, throwWait = false, throwResume = false, throwDrop = false;
	bool blockPrepareUntilStopped = false;
	std::function<bool()> stopping;
	std::function<int()> onDrop;
	std::atomic<int> prepared{0}, submitted{0}, waited{0}, resumed{0}, dropped{0};
	std::atomic_bool enteredPrepare{false}, exhausted{false};
	std::vector<float> firstBuffer, accepted;
	std::vector<snd_pcm_uframes_t> requested;
	Transfers()
	{
		active = this;
		io.prepare = [](snd_pcm_t* handle)
		{
			auto& t = *active; const auto call = ++t.prepared; t.enteredPrepare = true;
			if (t.blockPrepareUntilStopped) { while (!t.stopping()) { QThread::msleep(1); } }
			if (t.throwPrepare) { throw std::runtime_error("prepare fixture"); }
			return t.prepareError && call >= t.failPrepareAt ? t.prepareError : snd_pcm_prepare(handle);
		};
		io.write = [](snd_pcm_t*, const void* data, snd_pcm_uframes_t frames) -> snd_pcm_sframes_t
		{
			auto& t = *active; if (t.throwWrite) { throw std::runtime_error("write fixture"); }
			const auto samples = static_cast<const float*>(data);
			if (t.firstBuffer.empty()) { t.firstBuffer.assign(samples, samples + frames * 2); }
			t.requested.push_back(frames);
			auto result = t.otherwise;
			if (!t.writes.empty()) { result = t.writes.front(); t.writes.pop_front(); }
			if (result == All) { result = static_cast<snd_pcm_sframes_t>(frames); }
			if (result > 0 && static_cast<snd_pcm_uframes_t>(result) <= frames)
			{ t.accepted.insert(t.accepted.end(), samples, samples + result * 2); }
			++t.submitted; if (t.writes.empty()) { t.exhausted = true; }
			return result;
		};
		io.wait = [](snd_pcm_t*, int timeout)
		{
			auto& t = *active; ++t.waited;
			if (t.throwWait) { throw std::runtime_error("wait fixture"); }
			if (t.waitResult == 0) { QThread::msleep(static_cast<unsigned long>(timeout)); }
			return t.waitResult;
		};
		io.resume = [](snd_pcm_t*)
		{
			auto& t = *active; ++t.resumed; if (t.throwResume) { throw std::runtime_error("resume fixture"); }
			if (t.resumes.empty()) { return -EAGAIN; }
			const auto result = t.resumes.front(); t.resumes.pop_front(); return result;
		};
		io.drop = [](snd_pcm_t* handle)
		{
			auto& t = *active; ++t.dropped; const auto result = snd_pcm_drop(handle);
			if (t.onDrop) { return t.onDrop(); }
			if (t.throwDrop) { throw std::runtime_error("drop fixture"); }
			return t.dropError ? t.dropError : result;
		};
	}
	~Transfers() { active = nullptr; }
};
}

class AudioAlsaTest : public QObject
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
	}
	void cleanup() { if (Engine::audioEngine()) { Engine::destroy(); } }

	void nativeNullConfigurationTouchesOnlyItsOwnDescriptors()
	{
		Transfers transfers;
		// Leave one hole before two owned canaries. The old fd+i loop modified
		// these unrelated descriptors after opening the PCM into that hole.
		const auto hole = open("/dev/null", O_WRONLY); QVERIFY(hole >= 0);
		const auto one = open("/dev/null", O_WRONLY), two = open("/dev/null", O_WRONLY);
		const auto closeCanaries = qScopeGuard([&] { if (one >= 0) { close(one); } if (two >= 0) { close(two); } });
		close(hole); QVERIFY(one >= 0); QVERIFY(two >= 0);
		const auto firstFlags = fcntl(one, F_GETFD), secondFlags = fcntl(two, F_GETFD);
		auto device = AudioAlsaTestAccess::create(transfers.io); const auto handle = AudioAlsaTestAccess::handle(*device);
		const auto count = snd_pcm_poll_descriptors_count(handle); QVERIFY(count >= 0);
		std::vector<pollfd> descriptors(static_cast<std::size_t>(count));
		if (count)
		{
			const auto filled = snd_pcm_poll_descriptors(handle, descriptors.data(), count); QVERIFY(filled >= 0); QVERIFY(filled <= count);
			for (int i = 0; i < filled; ++i) { QVERIFY(fcntl(descriptors[i].fd, F_GETFD) & FD_CLOEXEC); }
		}
		QCOMPARE(fcntl(one, F_GETFD), firstFlags); QCOMPARE(fcntl(two, F_GETFD), secondFlags);
		QCOMPARE(snd_pcm_state(handle), SND_PCM_STATE_PREPARED); device.reset();
		QCOMPARE(fcntl(one, F_GETFD), firstFlags); QCOMPARE(fcntl(two, F_GETFD), secondFlags);
	}

	void startupPreparationFailureAndCancellationAreJoined_data()
	{
		QTest::addColumn<int>("kind");
		QTest::newRow("native-error") << 0; QTest::newRow("exception") << 1;
		QTest::newRow("deadline") << 2; QTest::newRow("cancelled") << 3;
	}

	void cancelledNativeSetupClosesTheOpenedPcm()
	{
		Transfers transfers;
		// Warm the library configuration before observing the next PCM descriptor.
		{ auto warm = AudioAlsaTestAccess::create(transfers.io); }
		const auto nextDescriptor = open("/dev/null", O_WRONLY); QVERIFY(nextDescriptor >= 0); close(nextDescriptor);
		bool sawOpenDescriptor = false;
		{
			FileResourceIO::Scope cancelled([&]
			{ sawOpenDescriptor = fcntl(nextDescriptor, F_GETFD) >= 0; return sawOpenDescriptor; });
			QVERIFY_EXCEPTION_THROWN(AudioAlsaTestAccess::create(transfers.io), std::runtime_error);
		}
		QVERIFY(sawOpenDescriptor); QCOMPARE(fcntl(nextDescriptor, F_GETFD), -1); QCOMPARE(errno, EBADF);
		auto retried = AudioAlsaTestAccess::create(transfers.io); retried->startProcessing(); QVERIFY(retried->AudioDevice::isRunning()); retried->stopProcessing();
	}
	void startupPreparationFailureAndCancellationAreJoined()
	{
		QFETCH(int, kind); Transfers transfers; auto device = AudioAlsaTestAccess::create(transfers.io, kind == 2 ? 100 : 1000);
		transfers.prepareError = kind == 0 ? -ENODEV : 0; transfers.throwPrepare = kind == 1;
		transfers.blockPrepareUntilStopped = kind >= 2; transfers.stopping = [&] { return AudioAlsaTestAccess::stopping(*device); };
		FileResourceIO::Scope cancellation([&] { return kind == 3 && transfers.enteredPrepare.load(); });
		if (kind == 3) { QVERIFY_EXCEPTION_THROWN(device->startProcessing(), std::runtime_error); }
		else { device->startProcessing(); }
		QVERIFY(!device->AudioDevice::isRunning()); QVERIFY(!device->QThread::isRunning()); QCOMPARE(transfers.submitted.load(), 0);
		QCOMPARE(device->processingStatus().failure, kind == 1 ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Start);
		QCOMPARE(device->processingStatus().nativeError, kind == 0 ? -ENODEV : kind == 1 ? -EIO : kind == 2 ? -ETIMEDOUT : -ECANCELED);
		FileResourceIO::Suspension recovery; transfers.prepareError = 0; transfers.throwPrepare = false; transfers.blockPrepareUntilStopped = false;
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
	}

	void partialWritesRetainTheExactNativeBufferAcrossRecovery_data()
	{
		QTest::addColumn<int>("kind");
		QTest::newRow("short-and-interrupted") << 0; QTest::newRow("underrun") << 1;
#ifdef ESTRPIPE
		QTest::newRow("suspended-resume") << 2; QTest::newRow("suspended-prepare") << 3;
#endif
	}
	void partialWritesRetainTheExactNativeBufferAcrossRecovery()
	{
		QFETCH(int, kind); Transfers transfers; transfers.writes = {3, -EINTR, 7, 0, -EAGAIN, 2};
		if (kind == 1) { transfers.writes.push_back(-EPIPE); }
#ifdef ESTRPIPE
		if (kind >= 2) { transfers.writes.push_back(-ESTRPIPE); transfers.resumes = kind == 2 ? std::deque<int>{-EAGAIN, -EAGAIN, 0} : std::deque<int>{-ENOSYS}; }
#endif
		transfers.writes.push_back(All); auto device = AudioAlsaTestAccess::create(transfers.io);
		device->startProcessing(); QTRY_VERIFY_WITH_TIMEOUT(transfers.exhausted.load(), 2000); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
		QCOMPARE(transfers.accepted, transfers.firstBuffer); QVERIFY(!transfers.firstBuffer.empty());
		const auto frames = static_cast<snd_pcm_uframes_t>(Engine::audioEngine()->framesPerAudioBuffer());
		QCOMPARE(transfers.requested.at(0), frames); QCOMPARE(transfers.requested.at(1), frames - 3); QCOMPARE(transfers.requested.at(2), frames - 3);
		QCOMPARE(transfers.requested.at(3), frames - 10); QCOMPARE(transfers.requested.at(4), frames - 10); QCOMPARE(transfers.requested.at(5), frames - 10);
		QCOMPARE(transfers.prepared.load(), kind == 1 || kind == 3 ? 2 : 1); QCOMPARE(transfers.dropped.load(), 1);
	}

	void fatalIoFailuresStopTheWorkerAndPreserveNativeError_data()
	{
		QTest::addColumn<QString>("kind");
		for (const auto kind : {"write", "wait", "prepare", "oversized", "wide-error", "throw-write", "throw-wait"})
		{ QTest::newRow(kind) << QString(kind); }
#ifdef ESTRPIPE
		QTest::newRow("suspend-prepare") << QString("suspend-prepare"); QTest::newRow("throw-resume") << QString("throw-resume");
#endif
	}
	void fatalIoFailuresStopTheWorkerAndPreserveNativeError()
	{
		QFETCH(QString, kind); Transfers transfers;
		int expectedError = -ENODEV; auto expectedFailure = AudioDevice::ProcessingFailure::Disconnected;
		if (kind == "write") { transfers.otherwise = -ENODEV; }
		if (kind == "wait") { transfers.waitResult = -ENODEV; }
		if (kind == "prepare") { transfers.otherwise = -EPIPE; transfers.prepareError = -EIO; transfers.failPrepareAt = 2; expectedError = -EIO; }
		if (kind == "oversized" || kind == "wide-error")
		{
			if (kind == "wide-error" && sizeof(snd_pcm_sframes_t) <= sizeof(int)) { QSKIP("wide ALSA error requires long wider than int"); }
			transfers.otherwise = kind == "oversized" ? All - 1 : std::numeric_limits<snd_pcm_sframes_t>::min();
			expectedError = -EOVERFLOW; expectedFailure = AudioDevice::ProcessingFailure::InvalidCallback;
		}
		if (kind.startsWith("throw-"))
		{
			transfers.throwWrite = kind == "throw-write"; transfers.throwWait = kind == "throw-wait"; transfers.throwResume = kind == "throw-resume";
			expectedError = -EIO; expectedFailure = AudioDevice::ProcessingFailure::Exception;
		}
#ifdef ESTRPIPE
		if (kind == "suspend-prepare")
		{ transfers.otherwise = -ESTRPIPE; transfers.resumes = {-ENOSYS}; transfers.prepareError = -EIO; transfers.failPrepareAt = 2; expectedError = -EIO; }
		if (kind == "throw-resume") { transfers.otherwise = -ESTRPIPE; }
#endif
		auto device = AudioAlsaTestAccess::create(transfers.io); device->startProcessing();
		QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000); device->stopProcessing();
		QVERIFY(!device->QThread::isRunning()); QCOMPARE(device->processingStatus().failure, expectedFailure);
		QCOMPARE(device->processingStatus().nativeError, expectedError); QCOMPARE(transfers.dropped.load(), 1);
		QVERIFY(transfers.accepted.empty());
	}

	void repeatedNoProgressExpiresWithoutAdvancingNativeDsp_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("zero-and-ready") << 0; QTest::newRow("again-and-ready") << 1;
		QTest::newRow("repeated-underrun") << 2;
#ifdef ESTRPIPE
		QTest::newRow("suspended") << 3;
#endif
	}
	void repeatedNoProgressExpiresWithoutAdvancingNativeDsp()
	{
		QFETCH(int, kind); Transfers transfers; transfers.otherwise = kind == 0 ? 0 : kind == 2 ? -EPIPE : -EAGAIN; transfers.waitResult = 1;
#ifdef ESTRPIPE
		if (kind == 3) { transfers.otherwise = -ESTRPIPE; }
#endif
		std::atomic<int> periods{0}; const auto observed = connect(Engine::audioEngine(), &AudioEngine::nextAudioBuffer,
			Engine::audioEngine(), [&](const SampleFrame*) { ++periods; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(observed); });
		auto device = AudioAlsaTestAccess::create(transfers.io, 100); device->startProcessing();
		QTRY_VERIFY_WITH_TIMEOUT(!device->AudioDevice::isRunning(), 2000); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::Disconnected); QCOMPARE(device->processingStatus().nativeError, -ETIMEDOUT);
		const auto maximumPeriods = Engine::audioEngine()->framesPerAudioBuffer() / Engine::audioEngine()->framesPerPeriod() + 1;
		QVERIFY(periods.load() <= maximumPeriods); QVERIFY(transfers.submitted.load() < 1000); QVERIFY(transfers.accepted.empty());
	}

	void stopAndDestructionJoinPendingTransferOrSuspend_data()
	{
		QTest::addColumn<bool>("suspended"); QTest::addColumn<bool>("destroy");
		for (bool destroy : {false, true})
		{
			QTest::newRow(destroy ? "destruct-wait" : "stop-wait") << false << destroy;
#ifdef ESTRPIPE
			QTest::newRow(destroy ? "destruct-suspend" : "stop-suspend") << true << destroy;
#endif
		}
	}
	void stopAndDestructionJoinPendingTransferOrSuspend()
	{
		QFETCH(bool, suspended); QFETCH(bool, destroy); Transfers transfers;
#ifdef ESTRPIPE
		if (suspended) { transfers.otherwise = -ESTRPIPE; }
#endif
		auto device = AudioAlsaTestAccess::create(transfers.io);
		for (int attempt = 0; attempt < (destroy ? 1 : 5); ++attempt)
		{
			const auto before = suspended ? transfers.resumed.load() : transfers.waited.load();
			device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
			QTRY_VERIFY_WITH_TIMEOUT((suspended ? transfers.resumed.load() : transfers.waited.load()) > before, 2000);
			QElapsedTimer elapsed; elapsed.start();
			if (destroy) { device.reset(); }
			else { device->stopProcessing(); QVERIFY(!device->QThread::isRunning()); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None); }
			QVERIFY(elapsed.elapsed() < 1000); QCOMPARE(transfers.dropped.load(), attempt + 1);
		}
	}

	void failedDropIsVisibleAndAnExplicitRestartClearsIt_data()
	{ QTest::addColumn<bool>("exception"); QTest::newRow("error") << false; QTest::newRow("exception") << true; }
	void failedDropIsVisibleAndAnExplicitRestartClearsIt()
	{
		QFETCH(bool, exception); Transfers transfers; transfers.dropError = exception ? 0 : -ENODEV; transfers.throwDrop = exception;
		auto device = AudioAlsaTestAccess::create(transfers.io); device->startProcessing(); QVERIFY(device->AudioDevice::isRunning()); device->stopProcessing();
		QCOMPARE(device->processingStatus().failure, exception ? AudioDevice::ProcessingFailure::Exception : AudioDevice::ProcessingFailure::Stop);
		QCOMPARE(device->processingStatus().nativeError, exception ? -EIO : -ENODEV);
		transfers.dropError = 0; transfers.throwDrop = false; device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		device->stopProcessing(); QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
	}

	void failedPreparationRollsSelectionBackToThePriorOwner_data()
	{ QTest::addColumn<bool>("running"); QTest::newRow("stopped") << false; QTest::newRow("running") << true; }
	void failedPreparationRollsSelectionBackToThePriorOwner()
	{
		QFETCH(bool, running); Transfers transfers; transfers.prepareError = -ENODEV;
		const auto engine = Engine::audioEngine(); const auto old = engine->audioDev(); const auto midi = engine->midiClient(); const auto name = engine->audioDevName();
		if (running) { old->startProcessing(); }
		QVERIFY_EXCEPTION_THROWN(StudioBackendSelectionTestAccess::select([&] { return AudioAlsaTestAccess::create(transfers.io); }), std::runtime_error);
		QCOMPARE(engine->audioDev(), old); QCOMPARE(engine->midiClient(), midi); QCOMPARE(engine->audioDevName(), name); QCOMPARE(old->isRunning(), running);
		QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!engine->isChangingAudioDevice());
	}

	void oldCleanupCannotOverwriteTheNewStartStatus()
	{
		Transfers transfers; std::atomic_bool entered{false}, release{false}, armed{false};
		auto device = AudioAlsaTestAccess::create(transfers.io);
		const auto unblock = qScopeGuard([&] { release = true; });
		transfers.onDrop = [&]
		{
			if (!armed.load() || transfers.dropped.load() > 1) { return 0; }
			entered = true;
			while (!device->AudioDevice::isRunning() && !release.load()) { QThread::msleep(1); }
			return -EIO;
		};
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		armed = true;
		// Publishing failure is the same base status transition used by the
		// worker, and leaves its cleanup pending for the next start to join.
		device->reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected, -ENODEV);
		QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 2000);
		device->startProcessing(); QVERIFY(device->AudioDevice::isRunning());
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
		QCOMPARE(device->processingStatus().nativeError, 0);
		device->stopProcessing(); QCOMPARE(transfers.dropped.load(), 2);
		QCOMPARE(device->processingStatus().failure, AudioDevice::ProcessingFailure::None);
	}

	void nativeSamplePcmPassesThroughTheProductionWorker()
	{
		Transfers transfers; const auto destroy = qScopeGuard([] { Engine::destroy(); });
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong();
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		auto clip = static_cast<SampleClip*>(song->tracks().at(0)->getClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(engine->outputSampleRate(), SampleFrame(.25f, -.125f)), engine->outputSampleRate()));
		song->setMasterVolume(100);
		auto device = AudioAlsaTestAccess::create(transfers.io); engine->setAudioDevice(device.release(), false);
		for (int i = 0; i < 8; ++i) { transfers.writes.push_back(13); transfers.writes.push_back(All); }
		song->playSong(); engine->audioDev()->startProcessing(); QTRY_VERIFY_WITH_TIMEOUT(transfers.exhausted.load(), 2000);
		engine->audioDev()->stopProcessing(); song->stop(); QCOMPARE(engine->audioDev()->processingStatus().failure, AudioDevice::ProcessingFailure::None);
		QCOMPARE(transfers.accepted.size(), static_cast<std::size_t>(engine->framesPerAudioBuffer()) * 8 * 2);
		float left = 0, right = 0;
		for (std::size_t i = 0; i < transfers.accepted.size(); i += 2)
		{ QVERIFY(std::isfinite(transfers.accepted[i])); QVERIFY(std::isfinite(transfers.accepted[i + 1])); left = std::max(left, transfers.accepted[i]); right = std::min(right, transfers.accepted[i + 1]); }
		QVERIFY(std::abs(left - .25f) < .0001f); QVERIFY(std::abs(right + .125f) < .0001f);
	}

	void actualNullBackendCanStartStopRestartAndBeReplaced()
	{
		ConfigManager::inst()->setValue("audioalsa", "device", "null"); const auto engine = Engine::audioEngine();
		for (int i = 0; i < 3; ++i)
		{
			const auto selected = StudioCapabilities::execute("audio.backend.select", {{"name", AudioAlsa::name()}});
			QVERIFY2(selected.value("ok").toBool(), qPrintable(selected.value("error").toString()));
			QVERIFY(dynamic_cast<AudioAlsa*>(engine->audioDev())); Engine::getSong()->playSong();
			QTRY_VERIFY_WITH_TIMEOUT(Engine::getSong()->getPlayPos().getTicks() > 0, 2000); Engine::getSong()->stop();
			engine->audioDev()->stopProcessing(); engine->audioDev()->startProcessing(); QVERIFY(engine->audioDev()->isRunning());
			engine->selectAudioBackend(AudioDummy::name()); QVERIFY(engine->audioDev()->isRunning());
		}
	}
};

QTEST_GUILESS_MAIN(AudioAlsaTest)
#include "AudioAlsaTest.moc"

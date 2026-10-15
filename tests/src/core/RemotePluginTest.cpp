/* Native remote-host fault tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QPointer>
#include <QScopeGuard>
#include <QDir>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "OutputSettings.h"
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "RenderManager.h"
#include "SampleFrame.h"
#include "Song.h"
#include "SystemSemaphore.h"
#include "StudioAudio.h"
#include "StudioCapabilities.h"

using namespace lmms;
using namespace std::chrono_literals;

#ifndef SYNC_WITH_SHM_FIFO
#include <sys/un.h>
#endif

namespace
{
// The child uses the real LMMS IPC implementation. It is a fault fixture, not
// a substitute VST/Zyn host or evidence of third-party plugin compatibility.
class HostFixture final : public RemotePluginBase
{
public:
#ifdef SYNC_WITH_SHM_FIFO
	HostFixture(char** argv) : RemotePluginBase(new shmFifo(argv[1]), new shmFifo(argv[2])) { }
#else
	HostFixture(char** argv)
	{
		m_socket = socket(AF_LOCAL, SOCK_STREAM, 0);
		sockaddr_un address{};
		address.sun_family = AF_LOCAL;
		const auto length = std::strlen(argv[1]);
		if (length >= sizeof(address.sun_path)) { std::_Exit(20); }
		std::memcpy(address.sun_path, argv[1], length + 1);
		if (::connect(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { std::_Exit(21); }
	}
	~HostFixture() override { if (m_socket >= 0) { close(m_socket); } }
#endif
	bool processMessage(const message&) override { return true; }

	void malformed(const QString& mode)
	{
		const bool hasLength = mode != "malformed" && mode != "bad-count";
		const int32_t count = mode == "malformed" ? -1 : mode == "bad-count" ? std::numeric_limits<int32_t>::max() : 1;
		const int32_t length = mode == "fragment" ? 100 : mode == "negative-length" ? -1 : std::numeric_limits<int32_t>::max();
#ifdef SYNC_WITH_SHM_FIFO
		auto fifo = const_cast<shmFifo*>(out());
		fifo->writeInt(IdProcessingDone);
		fifo->writeInt(count);
		if (hasLength) { fifo->writeInt(length); }
		fifo->messageSent();
#else
		writeInt(IdProcessingDone);
		writeInt(count);
		if (hasLength) { writeInt(length); }
#endif
	}

	int run(const QString& mode)
	{
		std::fputs("fixture child stdout marker\n", stdout);
		std::fflush(stdout);
		sendMessage(IdHostInfoGotten);
		if (mode != "no-init") { sendMessage(IdInitDone); }
		SharedMemory<float[]> audio;
		std::string lastPayload;
		while (!isInvalid())
		{
			const auto request = receiveMessage();
			if (request.id == IdQuit || request.id == IdUndefined) { return 0; }
			if (request.id == IdChangeSharedMemoryKey) { audio.attach(request.getString()); }
			else if (request.id == IdInitDone && mode == "slow-reader") { std::this_thread::sleep_for(100ms); }
			else if (request.id == IdDebugMessage) { lastPayload = request.getString(); }
			else if (request.id == IdSampleRateInformation)
			{
				if (mode == "hang-rate")
				{
					sendMessage(message(IdHostInfoGotten).addString("pending-rate"));
					std::this_thread::sleep_for(20s);
				}
#ifdef SYNC_WITH_SHM_FIFO
				if (mode == "abandon-lock")
				{
					if (!const_cast<shmFifo*>(in())->lock()) { return 22; }
					sendMessage(IdInformationUpdated);
					std::this_thread::sleep_for(20s);
					std::_Exit(23);
				}
#endif
				sendMessage(IdInformationUpdated);
			}
			else if (request.id == IdIsUIVisible)
			{
				if (mode == "slow-reader") { sendMessage(message(IdIsUIVisible).addString(lastPayload)); }
				else if (!request.getString().empty()) { sendMessage(message(IdIsUIVisible).addString(request.getString())); }
				else { sendMessage(message(IdIsUIVisible).addInt(0)); }
			}
			else if (request.id == IdStartProcessing)
			{
				if (mode == "crash") { std::_Exit(7); }
				if (mode == "hang" || mode == "no-init")
				{
					sendMessage(message(IdHostInfoGotten).addString("pending-processing"));
					std::this_thread::sleep_for(20s);
				}
				if (mode == "malformed" || mode == "fragment" || mode == "bad-count"
					|| mode == "bad-length" || mode == "negative-length")
				{
					malformed(mode);
					std::this_thread::sleep_for(20s);
				}
				if (mode == "flood")
				{
					for (int i = 0; i < 300; ++i)
					{
						sendMessage(IdInformationUpdated);
						std::this_thread::sleep_for(10ms);
					}
				}
				if (mode == "bad-channels") { sendMessage(message(IdChangeOutputCount).addInt(-1)); }
				if (audio) { std::fill_n(audio.get(), audio.size(), .25f); }
				sendMessage(IdProcessingDone);
			}
		}
		return 0;
	}
};

void configureEngine(const QTemporaryDir& directory)
{
	ConfigManager::inst()->setWorkingDir(directory.filePath("workspace"));
	ConfigManager::inst()->loadConfigFile(directory.filePath("config.xml"));
	NotePlayHandleManager::init();
	Engine::init(true);
	Engine::getSong()->setLoadOnLaunch(false);
}

bool startFixture(RemotePlugin& plugin, const QString& mode, bool waitForInit = true)
{
	return !plugin.init(QCoreApplication::applicationFilePath(), waitForInit, {"--remote-fixture", mode});
}

struct RenderContextGuard
{
	explicit RenderContextGuard(int timeoutMs = 0) : id(RemotePlugin::beginRenderContext(timeoutMs)) { }
	~RenderContextGuard() { end(); }
	void end() { RemotePlugin::endRenderContext(id); id = 0; }
	std::uint64_t id;
};

struct LongHostTimeout
{
	QByteArray previous = qgetenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS");
	bool present = qEnvironmentVariableIsSet("LMMS_REMOTE_PLUGIN_TIMEOUT_MS");
	LongHostTimeout() { qputenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS", "600000"); }
	~LongHostTimeout()
	{
		if (present) { qputenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS", previous); }
		else { qunsetenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS"); }
	}
};

struct ConnectionGuard
{
	QMetaObject::Connection connection;
	~ConnectionGuard() { QObject::disconnect(connection); }
};

struct ProjectReset
{
	ProjectReset() { Engine::getSong()->clearProject(); }
	~ProjectReset() { Engine::getSong()->clearProject(); }
};

class PendingHost final : public RemotePlugin
{
public:
	std::atomic_bool pending{false};
	bool processMessage(const message& m) override
	{
		if (m.id == IdHostInfoGotten && m.getString().rfind("pending-", 0) == 0) { pending = true; }
		return RemotePlugin::processMessage(m);
	}
};
}

class RemotePluginTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	QByteArray m_oldTimeout;
	bool m_hadTimeout = false;

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid());
		m_hadTimeout = qEnvironmentVariableIsSet("LMMS_REMOTE_PLUGIN_TIMEOUT_MS");
		m_oldTimeout = qgetenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS");
		qputenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS", "1000");
		configureEngine(m_configuration);
	}
	void cleanupTestCase()
	{
		Engine::destroy();
		NotePlayHandleManager::free();
		if (m_hadTimeout) { qputenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS", m_oldTimeout); }
		else { qunsetenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS"); }
	}
	void cleanup()
	{
		QVERIFY(!RemotePlugin::hasActiveRenderContext());
		QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
	}
	void semaphoreTimeoutPreservesCounts()
	{
		const auto key = ("lmms-test-" + QString::number(QCoreApplication::applicationPid())
			+ "-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)).toStdString();
		SystemSemaphore semaphore(key, 0);
		QVERIFY(!semaphore.tryAcquire());
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(!semaphore.acquireFor(25));
		QVERIFY(elapsed.elapsed() >= 20 && elapsed.elapsed() < 2000);
		QVERIFY(semaphore.release());
		QVERIFY(semaphore.acquireFor(25));
		QVERIFY(!semaphore.tryAcquire());
		SystemSemaphore peer(key);
		std::thread producer([&] { std::this_thread::sleep_for(20ms); peer.release(); });
		const auto acquired = semaphore.acquireFor(1000);
		producer.join();
		QVERIFY(acquired);
		QVERIFY(!semaphore.tryAcquire());
	}
	void healthyProcessingAndRestart()
	{
		RemotePlugin plugin;
		QVERIFY(startFixture(plugin, "healthy"));
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod());
		QVERIFY(plugin.process(nullptr, output.data()));
		for (const auto& frame : output) { QCOMPARE(frame.left(), .25f); QCOMPARE(frame.right(), .25f); }
		const auto failures = RemotePlugin::failureCount();
		plugin.updateSampleRate(44100);
		QCOMPARE(plugin.isUIVisible(), 0);
		QVERIFY(startFixture(plugin, "healthy"));
		QVERIFY(plugin.process(nullptr, output.data()));
		QVERIFY(!plugin.failed());
		QCOMPARE(RemotePlugin::failureCount(), failures);
	}
	void stalledHandshakeAndRecovery()
	{
		RemotePlugin plugin;
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(!startFixture(plugin, "no-init"));
		QVERIFY(plugin.failed() && plugin.isInvalid());
		QVERIFY(elapsed.elapsed() < 5000);
		QVERIFY(startFixture(plugin, "healthy"));
		QVERIFY(!plugin.failed());
	}
	void binaryMessageRoundTrip()
	{
		RemotePlugin plugin;
		QVERIFY(startFixture(plugin, "healthy"));
		const std::string binary{"ab\0cd", 5};
		QVERIFY(plugin.sendMessage(RemotePluginBase::message(IdIsUIVisible).addString(binary)) > 0);
		const auto reply = plugin.waitForMessage(IdIsUIVisible);
		QCOMPARE(reply.id, static_cast<int>(IdIsUIVisible));
		QVERIFY(reply.getString() == binary);
		QVERIFY(!plugin.failed());
	}
	void fifoBackpressureAllowsReaderToDrain()
	{
#ifdef SYNC_WITH_SHM_FIFO
		RemotePlugin plugin;
		QVERIFY(startFixture(plugin, "slow-reader"));
		const std::string first(RemotePluginBase::maximumMessageBytes / 2, 'a');
		const std::string second(RemotePluginBase::maximumMessageBytes / 2, 'b');
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(plugin.sendMessage(RemotePluginBase::message(IdDebugMessage).addString(first)) > 0);
		QVERIFY(plugin.sendMessage(RemotePluginBase::message(IdDebugMessage).addString(second)) > 0);
		QVERIFY(plugin.sendMessage(IdIsUIVisible) > 0);
		QVERIFY(plugin.waitForMessage(IdIsUIVisible).getString() == second);
		QVERIFY(elapsed.elapsed() < 5000);
		QVERIFY(!plugin.failed());
#else
		QSKIP("FIFO capacity/compaction applies to the shared-memory transport; socket binary round-trip runs separately");
#endif
	}
	void processingFailure_data()
	{
		QTest::addColumn<QString>("mode");
		for (const auto* mode : {"crash", "hang", "malformed", "fragment", "flood", "bad-channels",
			"bad-count", "bad-length", "negative-length"})
		{
			QTest::newRow(mode) << QString::fromLatin1(mode);
		}
	}
	void processingFailure()
	{
		QFETCH(QString, mode);
		RemotePlugin plugin;
		QVERIFY(startFixture(plugin, mode));
		const auto failures = RemotePlugin::failureCount();
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(), SampleFrame(1.f));
		QElapsedTimer elapsed;
		elapsed.start();
		QVERIFY(!plugin.process(nullptr, output.data()));
		QVERIFY(elapsed.elapsed() < 5000);
		QVERIFY(plugin.failed() && plugin.isInvalid());
		QVERIFY(RemotePlugin::failureCount() > failures);
		for (const auto& frame : output) { QCOMPARE(frame.left(), 0.f); QCOMPARE(frame.right(), 0.f); }
		const auto previousFailure = RemotePlugin::failureCount();
		QVERIFY(!plugin.process(nullptr, output.data()));
		QVERIFY(RemotePlugin::failureCount() > previousFailure);
		QVERIFY(startFixture(plugin, "healthy"));
		QVERIFY(plugin.process(nullptr, output.data()));
	}
	void abandonedSharedLockUnwinds()
	{
#ifdef SYNC_WITH_SHM_FIFO
		RemotePlugin plugin;
		QVERIFY(startFixture(plugin, "abandon-lock"));
		plugin.updateSampleRate(44100);
		QVERIFY(!plugin.failed());
		QElapsedTimer elapsed;
		elapsed.start();
		QCOMPARE(plugin.sendMessage(IdIsUIVisible), 0);
		QVERIFY(elapsed.elapsed() < 5000);
		QVERIFY(plugin.failed() && plugin.isInvalid());
		QVERIFY(startFixture(plugin, "healthy"));
#else
		QSKIP("Abandoned semaphore fixture applies to the shared-memory transport; socket EOF/fragment tests run separately");
#endif
	}
	void oversizedOutboundMessageFailsBeforePublication()
	{
		RemotePlugin plugin;
		QVERIFY(startFixture(plugin, "healthy"));
		QCOMPARE(plugin.sendMessage(RemotePluginBase::message(IdDebugMessage)
			.addString(std::string(RemotePluginBase::maximumMessageBytes, 'x'))), 0);
		QVERIFY(plugin.failed() && plugin.isInvalid());
	}
	void renderRefusesHostFailure()
	{
		QTemporaryDir output;
		QVERIFY(output.isValid());
		Engine::getSong()->clearProject();
		Engine::audioEngine()->storeAudioDevice();
		struct DeviceRestore { ~DeviceRestore() { Engine::audioEngine()->restoreAudioDevice(); } } restore;
		ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
			ProjectRenderer::ExportFileFormat::Wave, output.filePath("failed.wav"));
		QVERIFY(renderer.isReady());
		RemotePlugin plugin;
		QVERIFY(plugin.init("lmms-remote-fixture-does-not-exist", false));
		QVERIFY(!renderer.startProcessing());
		QVERIFY(!renderer.isRunning());
		QVERIFY(!RemotePlugin::hasActiveRenderContext());
		QVERIFY(renderer.hasPluginFailure());
		QVERIFY(!renderer.finalizeOutput());
	}
	void childDiagnosticsStayOnStderr()
	{
		QProcess probe;
		probe.start(QCoreApplication::applicationFilePath(), {"--stdio-probe"});
		QVERIFY(probe.waitForFinished(10000));
		QCOMPARE(probe.exitCode(), 0);
		QCOMPARE(probe.readAllStandardOutput().trimmed(), QByteArray("{\"ok\":true}"));
		QVERIFY(probe.readAllStandardError().contains("fixture child stdout marker"));
	}
	void renderManagerReportsOutputFailure()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
			ProjectRenderer::ExportFileFormat::Wave, directory.filePath("missing-directory/output.wav"));
		QSignalSpy finished(&manager, &RenderManager::finished);
		manager.renderProject();
		QCOMPARE(finished.count(), 1);
		QVERIFY(!manager.errorString().isEmpty());
	}
	void renderManagerReportsHostFailureAndRemovesPartialOutput()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto path = directory.filePath("failed-export.wav");
		RemotePlugin plugin;
		bool injected = false;
		ConnectionGuard connection{QObject::connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, &plugin, [&]
		{
			if (!injected) { injected = true; plugin.init("lmms-remote-fixture-does-not-exist", false); }
		})};
		RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
			ProjectRenderer::ExportFileFormat::Wave, path);
		QSignalSpy finished(&manager, &RenderManager::finished);
		manager.renderProject();
		if (finished.isEmpty()) { QVERIFY(finished.wait(5000)); }
		QVERIFY(injected);
		QCOMPARE(finished.count(), 1);
		QVERIFY(manager.errorString().contains("remote plugin", Qt::CaseInsensitive));
		QVERIFY(!QFile::exists(path));
	}
	void renderCancellationInterruptsProcessingAndPreservesIdleHosts()
	{
		LongHostTimeout timeout;
		PendingHost plugin;
		RemotePlugin idle;
		QVERIFY(startFixture(plugin, "hang"));
		QVERIFY(startFixture(idle, "healthy"));
		RenderContextGuard context;
		QVERIFY(context.id);
		const auto token = context.id;
		std::thread cancel([&]
		{
			const auto limit = std::chrono::steady_clock::now() + 2s;
			while (!plugin.pending && std::chrono::steady_clock::now() < limit) { std::this_thread::sleep_for(1ms); }
			RemotePlugin::cancelRenderContext(token);
		});
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(), SampleFrame(1.f));
		QElapsedTimer elapsed;
		elapsed.start();
		const auto processed = plugin.process(nullptr, output.data());
		cancel.join();
		context.end();
		QVERIFY(plugin.pending);
		QVERIFY(!processed);
		QVERIFY(elapsed.elapsed() < 3000);
		QVERIFY(plugin.failed() && plugin.isInvalid());
		for (const auto& frame : output) { QCOMPARE(frame.left(), 0.f); QCOMPARE(frame.right(), 0.f); }
		QCOMPARE(idle.isUIVisible(), 0);
		QVERIFY(!idle.failed());
		RenderContextGuard later;
		QVERIFY(later.id > token);
		RemotePlugin::cancelRenderContext(token);
		QCOMPARE(idle.isUIVisible(), 0);
		QVERIFY(!idle.failed());
		later.end();
		QVERIFY(startFixture(plugin, "healthy"));
		QVERIFY(plugin.process(nullptr, output.data()));
	}
	void renderDeadlineInterruptsWithoutMainThreadEvents()
	{
		LongHostTimeout timeout;
		PendingHost plugin;
		QVERIFY(startFixture(plugin, "hang"));
		RenderContextGuard context(200);
		QVERIFY(context.id);
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(), SampleFrame(1.f));
		QElapsedTimer elapsed;
		elapsed.start();
		const auto processed = plugin.process(nullptr, output.data());
		context.end();
		QVERIFY(plugin.pending);
		QVERIFY(!processed);
		QVERIFY(elapsed.elapsed() >= 150 && elapsed.elapsed() < 3000);
		QVERIFY(plugin.failed() && plugin.isInvalid());
		for (const auto& frame : output) { QCOMPARE(frame.left(), 0.f); QCOMPARE(frame.right(), 0.f); }
	}
	void exportAdoptsProcessingAlreadyInFlight()
	{
		LongHostTimeout timeout;
		PendingHost plugin;
		QVERIFY(startFixture(plugin, "hang"));
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(), SampleFrame(1.f));
		bool processed = true;
		std::thread processing([&] { processed = plugin.process(nullptr, output.data()); });
		const auto limit = std::chrono::steady_clock::now() + 2s;
		while (!plugin.pending && std::chrono::steady_clock::now() < limit) { std::this_thread::sleep_for(1ms); }
		RenderContextGuard context;
		const auto token = context.id;
		QElapsedTimer elapsed;
		elapsed.start();
		RemotePlugin::cancelRenderContext(token);
		processing.join();
		context.end();
		QVERIFY(token);
		QVERIFY(plugin.pending);
		QVERIFY(!processed);
		QVERIFY(elapsed.elapsed() < 3000);
		QVERIFY(plugin.failed() && plugin.isInvalid());
	}
	void cancellationUnwindsDeviceSetupBeforeWorkerStarts()
	{
		LongHostTimeout timeout;
		PendingHost plugin;
		QVERIFY(startFixture(plugin, "hang-rate"));
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		Engine::audioEngine()->storeAudioDevice();
		struct Restore { ~Restore() { Engine::audioEngine()->restoreAudioDevice(); } } restore;
		ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
			ProjectRenderer::ExportFileFormat::Wave, directory.filePath("cancelled.wav"));
		QVERIFY(renderer.isReady());
		bool requested = false;
		ConnectionGuard connection{QObject::connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, &plugin, [&]
		{
			if (!requested) { requested = true; plugin.updateSampleRate(44100); }
		})};
		bool cancelled = false;
		QTimer::singleShot(100, &renderer, [&] { cancelled = true; renderer.abortProcessing(); });
		QElapsedTimer elapsed;
		elapsed.start();
		const auto started = renderer.startProcessing();
		QVERIFY(cancelled && requested && plugin.pending);
		QVERIFY(!started && !renderer.isRunning());
		QVERIFY(renderer.wasAborted());
		QVERIFY(elapsed.elapsed() < 3000);
		QVERIFY(!renderer.finalizeOutput());
		QVERIFY(!RemotePlugin::hasActiveRenderContext());
	}
	void cancellationBeforeSetupClosesUnattachedOutput()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto path = directory.filePath("never-started.wav");
		const auto originalDevice = Engine::audioEngine()->audioDev();
		{
			ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
				ProjectRenderer::ExportFileFormat::Wave, path);
			QVERIFY(renderer.isReady());
			renderer.abortProcessing();
			QVERIFY(!renderer.startProcessing());
			QVERIFY(!renderer.finalizeOutput());
		}
		QCOMPARE(Engine::audioEngine()->audioDev(), originalDevice);
		QVERIFY(!RemotePlugin::hasActiveRenderContext());
		QFile file(path);
		QVERIFY(file.open(QIODevice::ReadOnly));
		QCOMPARE(file.read(4), QByteArray("RIFF"));
		file.close();
		// Also check release on platforms that disallow renaming an open file.
		QVERIFY(QFile::rename(path, directory.filePath("closed.wav")));
	}
	void cancelledStartupDoesNotWaitForConnectionOrHandshake()
	{
		LongHostTimeout timeout;
		RemotePlugin plugin;
		RenderContextGuard context(150);
		QVERIFY(context.id);
		QElapsedTimer elapsed;
		elapsed.start();
		const auto started = startFixture(plugin, "no-connect");
		context.end();
		QVERIFY(!started);
		QVERIFY(elapsed.elapsed() < 3000);
		QVERIFY(plugin.failed() && plugin.isInvalid());
		QVERIFY(startFixture(plugin, "healthy"));
	}
	void renderManagerRetainsStartingRendererDuringCancellation()
	{
		LongHostTimeout timeout;
		PendingHost plugin;
		QVERIFY(startFixture(plugin, "hang-rate"));
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto path = directory.filePath("cancelled-export.wav");
		bool requested = false;
		bool observedStarting = false;
		ConnectionGuard connection{QObject::connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, &plugin, [&]
		{
			if (!requested) { requested = true; plugin.updateSampleRate(44100); }
		})};
		RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
			ProjectRenderer::ExportFileFormat::Wave, path);
		QSignalSpy finished(&manager, &RenderManager::finished);
		QTimer::singleShot(100, &manager, [&]
		{
			observedStarting = manager.isStarting();
			manager.abortProcessing();
		});
		QElapsedTimer elapsed;
		elapsed.start();
		manager.renderProject();
		QVERIFY(observedStarting && requested && plugin.pending);
		QVERIFY(!manager.isStarting());
		QCOMPARE(finished.count(), 1);
		QCOMPARE(manager.errorString(), QString("Audio export was cancelled."));
		QVERIFY(elapsed.elapsed() < 3000);
		QVERIFY(!QFile::exists(path));
		QVERIFY(!RemotePlugin::hasActiveRenderContext());
	}
	void retiredManagerKeepsItsExecutingSetupAliveUntilHostCancellationReturns()
	{
		LongHostTimeout timeout; PendingHost plugin; QVERIFY(startFixture(plugin, "hang-rate")); QTemporaryDir directory; const auto path = directory.filePath("retired-manager.wav");
		const QPointer<RenderManager> manager(new RenderManager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit), ProjectRenderer::ExportFileFormat::Wave, path)); const auto cleanup = qScopeGuard([&] { delete manager.data(); }); bool requested = false, retired = false;
		ConnectionGuard connection{QObject::connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, &plugin, [&] { if (!requested) { requested = true; plugin.updateSampleRate(44100); } })};
		QTimer::singleShot(100, &plugin, [&] { if (manager) { retired = manager->isStarting(); delete manager.data(); } }); QElapsedTimer elapsed; elapsed.start(); manager->renderProject();
		QVERIFY(requested && retired && plugin.pending); QVERIFY(!manager); QVERIFY(elapsed.elapsed() < 3000); QVERIFY(!QFile::exists(path)); QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
	}
	void completedWorkerRetainsExportOwnershipUntilCleanup()
	{
		RemotePlugin idle;
		QVERIFY(startFixture(idle, "healthy"));
		ProjectReset project;
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "tripleoscillator"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("note.add", {{"track", 0}, {"clip", 0},
			{"position", 0}, {"length", 48}, {"key", 69}}).value("ok").toBool());
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto preservedPath = directory.filePath("other-export.wav");
		QFile preserved(preservedPath);
		QVERIFY(preserved.open(QIODevice::WriteOnly));
		QCOMPARE(preserved.write("original destination"), qint64(20));
		preserved.close();
		Engine::audioEngine()->storeAudioDevice();
		struct Restore { ~Restore() { Engine::audioEngine()->restoreAudioDevice(); } } restore;
		{
			ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
				ProjectRenderer::ExportFileFormat::Wave, directory.filePath("first-export.wav"));
			QVERIFY(renderer.isReady());
			renderer.setExecutionTimeout(5000);
			QVERIFY(renderer.startProcessing());
			QVERIFY(renderer.wait(5000));
			QVERIFY(!renderer.wasAborted() && !renderer.hasPluginFailure());
			QVERIFY(RemotePlugin::hasActiveRenderContext());
			RenderManager another(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
				ProjectRenderer::ExportFileFormat::Wave, preservedPath);
			QSignalSpy finished(&another, &RenderManager::finished);
			another.renderProject();
			QCOMPARE(finished.count(), 1);
			QVERIFY(!another.errorString().isEmpty());
			QVERIFY(preserved.open(QIODevice::ReadOnly));
			QCOMPARE(preserved.readAll(), QByteArray("original destination"));
			renderer.abortProcessing();
			QVERIFY(!renderer.finalizeOutput());
			QVERIFY(RemotePlugin::hasActiveRenderContext());
			QCOMPARE(idle.isUIVisible(), 0);
			QVERIFY(!idle.failed());
		}
		QVERIFY(!RemotePlugin::hasActiveRenderContext());
	}
	void activeExportRejectsAnotherDestinationBeforeOpeningIt_data()
	{
		QTest::addColumn<bool>("activeContext");
		QTest::newRow("active-render-context") << true;
		QTest::newRow("device-restoration-pending") << false;
	}
	void activeExportRejectsAnotherDestinationBeforeOpeningIt()
	{
		QFETCH(bool, activeContext);
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto path = directory.filePath("existing.wav");
		QFile existing(path);
		QVERIFY(existing.open(QIODevice::WriteOnly));
		QCOMPARE(existing.write("original destination"), qint64(20));
		existing.close();
		struct Reservation
		{
			std::uint64_t id;
			~Reservation()
			{
				if (id) { RemotePlugin::endRenderContext(id); }
				else { Engine::audioEngine()->restoreAudioDevice(); }
			}
		} reservation{activeContext ? RemotePlugin::beginRenderContext() : 0};
		if (activeContext) { QVERIFY(reservation.id); }
		else { Engine::audioEngine()->storeAudioDevice(); }
		RenderManager manager(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth16Bit),
			ProjectRenderer::ExportFileFormat::Wave, path);
		QSignalSpy finished(&manager, &RenderManager::finished);
		manager.renderProject();
		QCOMPARE(finished.count(), 1);
		QVERIFY(!manager.errorString().isEmpty());
		QCOMPARE(RemotePlugin::hasActiveRenderContext(), activeContext);
		QVERIFY(existing.open(QIODevice::ReadOnly));
		QCOMPARE(existing.readAll(), QByteArray("original destination"));
		QString studioError;
		try { StudioAudio::render({{"path", path}}); }
		catch (const std::runtime_error& failure) { studioError = QString::fromUtf8(failure.what()); }
		QCOMPARE(studioError, QString("Another native export is active"));
		QVERIFY(Engine::audioEngine()->hasStoredAudioDevice() || activeContext);
		QVERIFY(QDir(directory.path()).entryList({"*.render-*"}, QDir::Files).isEmpty());
	}
	void studioCancellationPreservesDestinationAndCanRenderAgain_data()
	{
		QTest::addColumn<bool>("cancel");
		QTest::newRow("cancel") << true;
		QTest::newRow("timeout") << false;
	}
	void studioCancellationPreservesDestinationAndCanRenderAgain()
	{
		QFETCH(bool, cancel);
		LongHostTimeout timeout;
		PendingHost plugin;
		QVERIFY(startFixture(plugin, "hang-rate"));
		ProjectReset project;
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "tripleoscillator"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("note.add", {{"track", 0}, {"clip", 0},
			{"position", 0}, {"length", 48}, {"key", 69}}).value("ok").toBool());
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto path = directory.filePath("existing.wav");
		QFile existing(path);
		QVERIFY(existing.open(QIODevice::WriteOnly));
		QCOMPARE(existing.write("original destination"), qint64(20));
		existing.close();
		bool requested = false;
		ConnectionGuard connection{QObject::connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, &plugin, [&]
		{
			if (!requested) { requested = true; plugin.updateSampleRate(44100); }
		})};
		bool cancelled = false;
		QObject timerOwner;
		if (cancel) { QTimer::singleShot(100, &timerOwner, [&] { cancelled = true; }); }
		QElapsedTimer elapsed;
		elapsed.start();
		QString error;
		try { StudioAudio::render({{"path", path}, {"timeoutMs", cancel ? 5000 : 200}}, [&] { return cancelled; }); }
		catch (const std::runtime_error& failure) { error = QString::fromUtf8(failure.what()); }
		QVERIFY(requested && plugin.pending);
		QCOMPARE(error, cancel ? QString("Native render cancelled") : QString("Native render exceeded its cooperative timeout"));
		QVERIFY(elapsed.elapsed() < 3000);
		QVERIFY(!RemotePlugin::hasActiveRenderContext());
		QVERIFY(existing.open(QIODevice::ReadOnly));
		QCOMPARE(existing.readAll(), QByteArray("original destination"));
		existing.close();
		QVERIFY(QDir(directory.path()).entryList({"*.render-*"}, QDir::Files).isEmpty());
		QVERIFY(startFixture(plugin, "healthy"));
		QVERIFY(StudioAudio::render({{"path", path}, {"timeoutMs", 5000}}).contains("wallTimeMs"));
		QVERIFY(existing.open(QIODevice::ReadOnly));
		QVERIFY(existing.size() > 44);
		QCOMPARE(existing.read(4), QByteArray("RIFF"));
	}
};

int main(int argc, char** argv)
{
	QCoreApplication application(argc, argv);
	const auto arguments = application.arguments();
	const auto fixture = arguments.indexOf("--remote-fixture");
	if (fixture >= 0 && fixture + 1 < arguments.size())
	{
		if (arguments[fixture + 1] == "no-connect") { std::this_thread::sleep_for(20s); return 0; }
		return HostFixture(argv).run(arguments[fixture + 1]);
	}
	if (arguments.contains("--stdio-probe"))
	{
		QTemporaryDir configuration;
		configureEngine(configuration);
		bool ready = false;
		{ RemotePlugin plugin; ready = startFixture(plugin, "healthy"); }
		Engine::destroy();
		NotePlayHandleManager::free();
		if (ready) { std::puts("{\"ok\":true}"); }
		return ready ? 0 : 1;
	}
	RemotePluginTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "RemotePluginTest.moc"

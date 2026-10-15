/* Live MIDI admission through native export and device reservations.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <sndfile.h>
#include <cmath>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include "AudioDummy.h"
#include "AudioEngine.h"
#include "AudioDeviceTransitionTestUtils.h"
#include "AutoDetectMidiController.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "MidiController.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "Song.h"
#include "StudioCapabilities.h"

using namespace lmms;
namespace lmms
{
class StudioMidiLearnTestAccess
{
public:
	static MidiClient* exchange(MidiClient* client) { return std::exchange(Engine::audioEngine()->m_midiClient, client); }
};
}
namespace
{
class InputClient final : public MidiClient
{
public:
	bool hasQueuedInputDelivery() const override { return true; }
	InputBackendStatus inputBackendStatus() const override { return {true, true, 0}; }
	void processOutEvent(const MidiEvent&, const TimePos&, const MidiPort*) override {}
	void send(MidiPort* target, int value, MidiEvent::Source source = MidiEvent::Source::External)
	{ dispatchInputEvent(MidiEvent(MidiControlChange, 0, 74, value, nullptr, source), {}, target); }
	void pump() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
};
std::vector<float> decode(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return {}; }
	SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
	if (!audio || info.frames < 1 || info.frames > 2000000 || info.channels != 2 || info.samplerate != 48000) { return {}; }
	std::vector<float> samples(static_cast<std::size_t>(info.frames) * info.channels);
	if (sf_readf_float(audio.get(), samples.data(), info.frames) != info.frames || sf_error(audio.get())) { return {}; }
	return samples;
}
}

class MidiInputRenderTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	InputClient input;
	MidiClient* prior = nullptr;
	MidiController* bind()
	{
		auto track = new InstrumentTrack(Engine::getSong());
		const auto result = StudioCapabilities::execute("controller.midi.connect", {{"parameter", int(track->volumeModel()->id())}, {"inputController", 74}});
		return result.value("ok").toBool() ? dynamic_cast<MidiController*>(track->volumeModel()->controllerConnection()->getController()) : nullptr;
	}
	static float settled(MidiController* controller)
	{
		Controller::triggerFrameCounter(); controller->currentValue(0); Controller::triggerFrameCounter(); return controller->currentValue(0);
	}
	void seed(MidiController* controller)
	{ input.send(controller->midiPort(), 63); input.pump(); QCOMPARE(settled(controller), 63.f / 127.f); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stopExport(); Engine::getSong()->clearProject();
		Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1);
		prior = StudioMidiLearnTestAccess::exchange(&input);
	}
	void cleanup()
	{
		Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->setRenderBetweenMarkers(false);
		Engine::getSong()->clearProject(); input.pump(); StudioMidiLearnTestAccess::exchange(prior); prior = nullptr;
	}

	void queuedLiveInputCannotCrossNativeReservations_data()
	{
		QTest::addColumn<int>("stage"); QTest::newRow("device-reservation") << 0; QTest::newRow("song-export") << 1;
		QTest::newRow("combined") << 2; QTest::newRow("post-export-reservation") << 3;
	}
	void queuedLiveInputCannotCrossNativeReservations()
	{
		QFETCH(int, stage); auto controller = bind(); QVERIFY(controller); seed(controller); QSignalSpy changed(controller, &Controller::valueChanged);
		input.send(controller->midiPort(), 0); // Already queued when the reservation begins.
		if (stage != 1) { QVERIFY(Engine::audioEngine()->storeAudioDevice()); }
		if (stage != 0) { Engine::getSong()->startExport(); }
		input.send(controller->midiPort(), 0);
		if (stage != 0) { Engine::getSong()->stopExport(); }
		if (stage == 3) { input.send(controller->midiPort(), 0); }
		if (stage != 1) { QVERIFY(Engine::audioEngine()->restoreAudioDevice()); }
		input.pump(); QCOMPARE(changed.size(), 0); QCOMPARE(settled(controller), 63.f / 127.f);
		const auto state = input.inputDeliveryStatus(controller->midiPort()); QVERIFY(!state.failed); QCOMPARE(state.pendingEvents, 0u);
		QCOMPARE(state.droppedEvents, std::uint64_t(stage == 3 ? 3 : 2));
		changed.clear(); input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(changed.size(), 1); QCOMPARE(settled(controller), 1.f);
	}

	void staleFullQueueDoesNotOverflowFreshInput()
	{
		auto controller = bind(); QVERIFY(controller); seed(controller);
		for (unsigned int i = 0; i < MidiClient::InputQueueLimit; ++i) { input.send(controller->midiPort(), 0); }
		QCOMPARE(input.inputDeliveryStatus(controller->midiPort()).pendingEvents, MidiClient::InputQueueLimit);
		QVERIFY(Engine::audioEngine()->storeAudioDevice()); QVERIFY(Engine::audioEngine()->restoreAudioDevice());
		input.send(controller->midiPort(), 127); const auto state = input.inputDeliveryStatus(controller->midiPort());
		QVERIFY(!state.failed); QCOMPARE(state.pendingEvents, 1u); QCOMPARE(state.droppedEvents, std::uint64_t(MidiClient::InputQueueLimit));
		input.pump(); QCOMPARE(settled(controller), 1.f);
	}

	void poppedPacketCannotArriveAfterACompletedExport()
	{
		auto controller = bind(); QVERIFY(controller); seed(controller); input.send(controller->midiPort(), 0);
		std::atomic_bool locked{false}, completed{false}, failed{false};
		std::thread rendering([&] {
			const auto changes = Engine::audioEngine()->requestChangesGuard(); locked = true;
			QElapsedTimer elapsed; elapsed.start();
			const auto popped = [&] { const auto state = input.inputDeliveryStatus(controller->midiPort()); return state.processing && state.pendingEvents == 0; };
			while (!popped() && elapsed.elapsed() < 1000) { QThread::msleep(1); }
			if (!popped()) { failed = true; return; }
			try { Engine::getSong()->startExport(); Engine::getSong()->stopExport(); completed = true; }
			catch (...) { failed = true; }
		});
		const auto join = qScopeGuard([&] { rendering.join(); }); QElapsedTimer elapsed; elapsed.start();
		while (!locked && elapsed.elapsed() < 1000) { QThread::msleep(1); }
		QVERIFY(locked); input.pump(); QVERIFY(completed); QVERIFY(!failed); QVERIFY(!Engine::getSong()->isExporting());
		QCOMPARE(settled(controller), 63.f / 127.f); QCOMPARE(input.inputDeliveryStatus(controller->midiPort()).droppedEvents, std::uint64_t(1));
		input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(settled(controller), 1.f);
	}

	void blockedDriverBurstIsDiscardedWithoutQueueFailure()
	{
		auto controller = bind(); QVERIFY(controller); seed(controller); QVERIFY(Engine::audioEngine()->storeAudioDevice());
		constexpr unsigned int count = MidiClient::InputQueueLimit * 3;
		for (unsigned int i = 0; i < count; ++i) { input.send(controller->midiPort(), 0); }
		const auto state = input.inputDeliveryStatus(controller->midiPort()); QVERIFY(!state.failed); QCOMPARE(state.pendingEvents, 0u); QCOMPARE(state.droppedEvents, std::uint64_t(count));
		QVERIFY(Engine::audioEngine()->restoreAudioDevice()); input.pump(); QCOMPARE(settled(controller), 63.f / 127.f);
		input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(settled(controller), 1.f);
	}

	void queuedInternalInputRemainsUsable()
	{
		auto controller = bind(); QVERIFY(controller); seed(controller); QVERIFY(Engine::audioEngine()->storeAudioDevice());
		input.send(controller->midiPort(), 0); input.send(controller->midiPort(), 127, MidiEvent::Source::Internal); input.pump(); QCOMPARE(settled(controller), 1.f);
		QVERIFY(!input.inputDeliveryStatus(controller->midiPort()).failed); QCOMPARE(input.inputDeliveryStatus(controller->midiPort()).droppedEvents, std::uint64_t(1));
		QVERIFY(Engine::audioEngine()->restoreAudioDevice()); input.send(controller->midiPort(), 0); input.pump(); QCOMPARE(settled(controller), 0.f);
	}

	void directProcessorsRespectNativeReservations_data()
	{
		QTest::addColumn<int>("processor"); QTest::addColumn<bool>("exporting");
		for (int processor = 0; processor < 4; ++processor)
		{
			QTest::newRow(qPrintable(QString::number(processor) + "-reservation")) << processor << false;
			QTest::newRow(qPrintable(QString::number(processor) + "-export")) << processor << true;
		}
	}
	void directProcessorsRespectNativeReservations()
	{
		QFETCH(int, processor); QFETCH(bool, exporting); auto controller = bind(); QVERIFY(controller); seed(controller); AutoDetectMidiController detector(nullptr); detector.reset();
		auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0]); int onsets = 0; QObject lifetime;
		connect(track, &InstrumentTrack::midiNoteOn, &lifetime, [&](const Note&) { ++onsets; }, Qt::DirectConnection);
		if (exporting) { Engine::getSong()->startExport(); } else { QVERIFY(Engine::audioEngine()->storeAudioDevice()); }
		const auto send = [&](MidiEvent::Source source) {
			const MidiEvent cc(MidiControlChange, 0, 74, 127, nullptr, source);
			if (processor == 0) { controller->midiPort()->processInEvent(cc); }
			if (processor == 1) { controller->processInEvent(cc, {}); }
			if (processor == 2) { detector.processInEvent(cc, {}); }
			if (processor == 3) { track->processInEvent(MidiEvent(MidiNoteOn, 0, 60, 100, nullptr, source)); }
		};
		send(MidiEvent::Source::External); QCOMPARE(settled(controller), 63.f / 127.f); QVERIFY(!detector.detection().received); QCOMPARE(onsets, 0);
		send(MidiEvent::Source::Internal);
		if (processor < 2) { QCOMPARE(settled(controller), 1.f); }
		if (processor == 2) { QVERIFY(detector.detection().received); QCOMPARE(detector.detection().value, 127); }
		if (processor == 3) { QCOMPARE(onsets, 1); }
		if (exporting) { Engine::getSong()->stopExport(); } else { QVERIFY(Engine::audioEngine()->restoreAudioDevice()); }
	}

	void failedExportStartKeepsLiveInputBlockedUntilRecovery()
	{
		auto controller = bind(); QVERIFY(controller); seed(controller); input.send(controller->midiPort(), 0);
		Engine::getSong()->getTimeline(Song::PlayMode::Song).setLoopPoints(192, 192); Engine::getSong()->setRenderBetweenMarkers(true);
		QVERIFY_EXCEPTION_THROWN(Engine::getSong()->startExport(), std::invalid_argument); QVERIFY(!Engine::getSong()->isExporting());
		QVERIFY(Engine::getSong()->hasPendingExportCleanup()); input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(settled(controller), 63.f / 127.f);
		QCOMPARE(input.inputDeliveryStatus(controller->midiPort()).droppedEvents, std::uint64_t(2));
		QVERIFY(Engine::audioEngine()->restoreAudioDevice()); QVERIFY(!Engine::getSong()->hasPendingExportCleanup());
		input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(settled(controller), 1.f); QVERIFY(!input.inputDeliveryStatus(controller->midiPort()).failed);
	}

	void busyDeviceRestoreKeepsInputReserved()
	{
		auto controller = bind(); QVERIFY(controller); seed(controller); QVERIFY(Engine::audioEngine()->storeAudioDevice()); Engine::getSong()->startExport();
		QVERIFY(!Engine::audioEngine()->restoreAudioDevice()); QVERIFY(Engine::audioEngine()->hasStoredAudioDevice()); input.send(controller->midiPort(), 0);
		Engine::getSong()->stopExport(); input.send(controller->midiPort(), 0); input.pump(); QCOMPARE(settled(controller), 63.f / 127.f);
		QVERIFY(Engine::audioEngine()->restoreAudioDevice()); input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(settled(controller), 1.f);
	}

	void restoreNotificationFailureKeepsInputReservedUntilRetry()
	{
		auto controller = bind(); QVERIFY(controller); seed(controller); const auto engine = Engine::audioEngine(); QVERIFY(engine->storeAudioDevice());
		bool successful = false; engine->setAudioDevice(new AudioDummy(successful, engine), false); QVERIFY(successful);
		AudioDeviceTransitionProbe transitionProbe(engine, [](DeviceTransitionPoint point) { if (point == DeviceTransitionPoint::RestoreAfterRate) { throw std::runtime_error("Input reservation restore fixture"); } });
		QVERIFY(!engine->restoreAudioDevice()); QVERIFY(engine->hasStoredAudioDevice());
		input.send(controller->midiPort(), 0); input.pump(); QCOMPARE(settled(controller), 63.f / 127.f); QVERIFY(!input.inputDeliveryStatus(controller->midiPort()).failed);
		transitionProbe.reset(); QVERIFY(engine->restoreAudioDevice()); input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(settled(controller), 1.f);
	}

	void nativeRenderPcmPreservesControllerGainDuringLiveTraffic()
	{
		QVERIFY(StudioCapabilities::execute("song.set", {{"tempo", 600}, {"masterVolume", 100}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "sample"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("clip.add", {{"track", 0}, {"position", 0}}).value("ok").toBool());
		auto clip = static_cast<SampleClip*>(Engine::getSong()->tracks()[0]->getClip(0));
		clip->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(48000, SampleFrame(.25f)), 48000));
		QVERIFY(StudioCapabilities::execute("clip.edit", {{"track", 0}, {"clip", 0}, {"length", 192}, {"autoResize", false}}).value("ok").toBool());
		const auto volume = int(Engine::mixer()->mixerChannel(0)->m_volumeModel.id());
		QVERIFY(StudioCapabilities::execute("controller.midi.connect", {{"parameter", volume}, {"inputController", 74}}).value("ok").toBool());
		const auto parameter = dynamic_cast<AutomatableModel*>(Engine::projectJournal()->journallingObject(volume)); QVERIFY(parameter && parameter->controllerConnection());
		auto controller = dynamic_cast<MidiController*>(parameter->controllerConnection()->getController()); QVERIFY(controller); seed(controller);
		QTemporaryDir directory; QJsonObject args{{"path", directory.filePath("before.wav")}, {"sampleRate", 48000}, {"bitDepth", 24}, {"timeoutMs", 30000}};
		const auto baseline = StudioCapabilities::execute("audio.render", args); QVERIFY2(baseline.value("ok").toBool(), qPrintable(baseline.value("error").toString()));
		const auto before = decode(directory.filePath("before.wav")); QVERIFY(!before.empty());
		bool nonzero = false; for (const auto sample : before) { QVERIFY(std::isfinite(sample)); nonzero = nonzero || std::abs(sample) > .001f; } QVERIFY(nonzero);
		unsigned int injected = 0;
		const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] {
			if (!Engine::audioEngine()->hasStoredAudioDevice() || injected) { return; }
			for (unsigned int i = 0; i < MidiClient::InputQueueLimit * 2; ++i) { input.send(controller->midiPort(), 0); ++injected; }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); args.insert("path", directory.filePath("during.wav"));
		const auto rendered = StudioCapabilities::execute("audio.render", args); QVERIFY2(rendered.value("ok").toBool(), qPrintable(rendered.value("error").toString()));
		QVERIFY(injected > MidiClient::InputQueueLimit); input.pump(); QCOMPARE(settled(controller), 63.f / 127.f); QVERIFY(!input.inputDeliveryStatus(controller->midiPort()).failed);
		const auto after = decode(directory.filePath("during.wav")); QCOMPARE(after.size(), before.size());
		for (std::size_t i = 0; i < before.size(); ++i) { QVERIFY(std::isfinite(after[i])); QCOMPARE(after[i], before[i]); }
		input.send(controller->midiPort(), 127); input.pump(); QCOMPARE(settled(controller), 1.f);
	}
};
QTEST_GUILESS_MAIN(MidiInputRenderTest)
#include "MidiInputRenderTest.moc"

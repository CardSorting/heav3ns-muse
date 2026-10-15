/* Native MIDI client retirement and local voice recovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "MidiEventProcessor.h"
#include "MidiPort.h"
#include "NotePlayHandle.h"
#include "PluginFactory.h"
#include "SampleClip.h"
#include "SampleRecordHandle.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"

using namespace lmms;

namespace lmms
{
class MidiPortTestAccess
{
public:
	static void useClient(MidiPort* port, MidiClient* client)
	{
		port->m_midiClient->removePort(port); port->m_midiClient = client; client->addPort(port);
	}
};
class MidiClientTestAccess
{
public:
	static std::size_t registered(MidiClient* client)
	{
		const auto lock = std::lock_guard{client->m_portsMutex}; return client->m_inputDeliveries.size();
	}
};
}

namespace
{
class Client final : public MidiClient
{
public:
	bool observed = false, backendActive = true;
	bool isRaw() const override { return true; }
	bool hasQueuedInputDelivery() const override { return true; }
	InputBackendStatus inputBackendStatus() const override { return {observed, backendActive, 0}; }
	void processOutEvent(const MidiEvent&, const TimePos&, const MidiPort*) override { }
	void feed(MidiPort* port, const MidiEvent& event) { dispatchInputEvent(event, TimePos{}, port); }
	void fault(MidiPort* port, InputDeliveryFailure reason = InputDeliveryFailure::QueueOverflow) { failInputEvents(reason, port); }
};

class Processor final : public MidiEventProcessor
{
public:
	int events = 0, cleanupCalls = 0, depth = 0, maximumDepth = 0;
	bool held = false, pedal = false, throwCleanup = false;
	std::vector<QThread*> threads;
	std::function<void(const MidiEvent&)> onEvent;
	void processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t) override
	{
		++depth; maximumDepth = std::max(maximumDepth, depth);
		const auto unwind = qScopeGuard([&] { --depth; });
		const bool cleanup = event.type() == MidiControlChange && event.controllerNumber() == MidiControllerAllSoundOff;
		if (cleanup && throwCleanup) { throw std::runtime_error("retirement cleanup fixture failure"); }
		++events; threads.push_back(QThread::currentThread());
		if (onEvent) { onEvent(event); }
		// Apply after callbacks: retirement inside note-on must wait for this
		// invocation to finish before attempting to silence the activated note.
		if (event.type() == MidiNoteOn && event.velocity() > 0) { held = true; }
		if (event.type() == MidiControlChange && (event.controllerNumber() == MidiControllerSustain
			|| event.controllerNumber() == MidiControllerSostenuto)) { pedal = event.controllerValue() >= 64; }
		if (cleanup) { ++cleanupCalls; held = false; pedal = false; }
	}
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
};

class TrackedHandle final : public PlayHandle
{
public:
	TrackedHandle(InstrumentTrack* track, Type type, int& destroyed, std::function<void()> afterUnlink = {})
		: PlayHandle(type), m_track(track), m_destroyed(destroyed), m_afterUnlink(std::move(afterUnlink))
	{ setAudioBusHandle(track->audioBusHandle()); setUsesBuffer(false); }
	~TrackedHandle() override { ++m_destroyed; if (m_afterUnlink) { m_afterUnlink(); } }
	void play(SampleFrame*) override { }
	bool isFinished() const override { return false; }
	bool isFromTrack(const Track* track) const override { return track == m_track; }
private:
	InstrumentTrack* m_track;
	int& m_destroyed;
	std::function<void()> m_afterUnlink;
};

class TrackedRecording final : public SampleRecordHandle
{
public:
	TrackedRecording(SampleClip* clip, bool reserved, int& destroyed)
		: SampleRecordHandle(clip, 0, reserved), m_destroyed(destroyed) { }
	~TrackedRecording() override { ++m_destroyed; }
private:
	int& m_destroyed;
};

void pump() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
QByteArray settings(MidiPort& port)
{
	QDomDocument document; auto root = document.createElement("midi"); document.appendChild(root);
	port.saveSettings(document, root); return document.toByteArray();
}
bool silent(std::span<const SampleFrame> frames)
{
	return std::all_of(frames.begin(), frames.end(), [](const auto& frame) { return frame[0] == 0.f && frame[1] == 0.f; });
}
}

class MidiClientRetirementTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init();
	}
	void cleanupTestCase() { NotePlayHandleManager::free(); }
	void init()
	{
		Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->clearProject();
	}
	void cleanup() { Engine::getSong()->stopExport(); Engine::destroy(); }

	void deliveredActivationsAreCleanedEvenAfterInputIsDisabled_data()
	{
		QTest::addColumn<int>("activation"); QTest::addColumn<bool>("disabled");
		for (const bool disabled : {false, true})
		{
			for (int activation = 0; activation < 3; ++activation)
			{
				QTest::newRow(qPrintable(QString("activation-%1-disabled-%2").arg(activation).arg(disabled))) << activation << disabled;
			}
		}
	}
	void deliveredActivationsAreCleanedEvenAfterInputIsDisabled()
	{
		QFETCH(int, activation); QFETCH(bool, disabled);
		auto client = std::make_unique<Client>(); Processor processor; MidiPort port("survivor", client.get(), &processor);
		port.setReadable(true); port.setInputChannel(0);
		client->feed(&port, activation == 0 ? MidiEvent(MidiNoteOn, 7, 60, 100)
			: MidiEvent(MidiControlChange, 7, activation == 1 ? MidiControllerSustain : MidiControllerSostenuto, 127));
		pump(); QVERIFY(processor.held || processor.pedal); port.setInputChannel(2);
		if (disabled) { port.setReadable(false); }
		const auto before = settings(port); client.reset();
		const auto state = port.client()->inputDeliveryStatus(&port);
		QVERIFY(state.registered && state.failed); QCOMPARE(state.failure, MidiClient::InputDeliveryFailure::ClientRetired);
		QCOMPARE(state.lastFailure, MidiClient::InputDeliveryFailure::ClientRetired); QVERIFY(!state.retirementCleanupPending);
		QCOMPARE(processor.cleanupCalls, 16); QVERIFY(!processor.held && !processor.pedal); QCOMPARE(processor.maximumDepth, 1);
		for (const auto owner : processor.threads) { QCOMPARE(owner, port.thread()); }
		QCOMPARE(settings(port), before); pump(); QCOMPARE(processor.cleanupCalls, 16);
		if (disabled) { port.setReadable(true); }
		QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		QVERIFY(!port.client()->inputDeliveryStatus(&port).failed); QCOMPARE(port.client()->inputDeliveryStatus(&port).recoveryCount, std::uint64_t{1});
		QVERIFY(!port.client()->inputBackendStatus().observed);
	}

	void queuedPacketsAreDroppedWithoutInventingDeliveredVoices()
	{
		auto client = std::make_unique<Client>(); Processor processor; MidiPort port("pending", client.get(), &processor); port.setReadable(true);
		for (int key = 60; key < 63; ++key) { client->feed(&port, MidiEvent(MidiNoteOn, 0, key, 100)); }
		QCOMPARE(client->inputDeliveryStatus(&port).pendingEvents, 3u); client.reset();
		const auto state = port.client()->inputDeliveryStatus(&port); QVERIFY(state.registered && state.failed);
		QCOMPARE(state.failure, MidiClient::InputDeliveryFailure::ClientRetired); QCOMPARE(state.pendingEvents, 0u);
		QCOMPARE(state.droppedEvents, std::uint64_t{3}); QVERIFY(!state.retirementCleanupPending);
		pump(); QCOMPARE(processor.events, 0);
	}

	void cleanupFailureRemainsRetryableAfterTheBackendIsGone_data()
	{
		QTest::addColumn<bool>("backendFault"); QTest::newRow("live-client") << false; QTest::newRow("stopped-backend") << true;
	}
	void cleanupFailureRemainsRetryableAfterTheBackendIsGone()
	{
		QFETCH(bool, backendFault); auto client = std::make_unique<Client>(); Processor processor;
		MidiPort port("retry", client.get(), &processor); port.setReadable(true); client->feed(&port, MidiEvent(MidiNoteOn, 0, 60, 100)); pump();
		if (backendFault) { client->observed = true; client->backendActive = false; client->fault(&port, MidiClient::InputDeliveryFailure::BackendInput); }
		client->feed(&port, MidiEvent(MidiNoteOff, 0, 60, 0)); processor.throwCleanup = true; client.reset();
		const auto state = port.client()->inputDeliveryStatus(&port); QVERIFY(state.registered && state.failed && state.retirementCleanupPending);
		QCOMPARE(state.failure, MidiClient::InputDeliveryFailure::ClientRetired); QCOMPARE(state.lastFailure, MidiClient::InputDeliveryFailure::Processor);
		QVERIFY(!state.requiresBackendObservation); QCOMPARE(state.droppedEvents, std::uint64_t{1}); QVERIFY(processor.held);
		QString error; bool changed = true; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed && !error.isEmpty());
		QVERIFY(port.client()->inputDeliveryStatus(&port).retirementCleanupPending);
		processor.throwCleanup = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		const auto recovered = port.client()->inputDeliveryStatus(&port); QVERIFY(!recovered.failed && !recovered.retirementCleanupPending);
		QVERIFY(recovered.failureVersion > state.failureVersion); QCOMPARE(recovered.droppedEvents, state.droppedEvents);
		QVERIFY(!processor.held); QCOMPARE(processor.cleanupCalls, 16); pump(); QCOMPARE(processor.cleanupCalls, 16);
	}

	void workerRetirementPostsCleanupToTheModelOwner()
	{
		auto client = std::make_unique<Client>(); Processor processor; MidiPort port("owner", client.get(), &processor); port.setReadable(true);
		client->feed(&port, MidiEvent(MidiNoteOn, 0, 60, 100)); pump();
		const auto raw = client.release(); std::thread retire([raw] { delete raw; }); retire.join();
		QCOMPARE(processor.cleanupCalls, 0); QVERIFY(processor.held); QVERIFY(port.client()->inputDeliveryStatus(&port).retirementCleanupPending);
		pump(); QCOMPARE(processor.cleanupCalls, 16); QVERIFY(!processor.held); QVERIFY(!port.client()->inputDeliveryStatus(&port).retirementCleanupPending);
		for (const auto owner : processor.threads) { QCOMPARE(owner, port.thread()); }
	}

	void retirementInsideNoteDeliveryDoesNotReenterTheProcessor()
	{
		auto client = std::make_unique<Client>(); Processor processor; MidiPort port("reentrant", client.get(), &processor); port.setReadable(true);
		processor.onEvent = [&](const MidiEvent& event) { if (event.type() == MidiNoteOn) { client.reset(); } };
		client->feed(&port, MidiEvent(MidiNoteOn, 0, 60, 100)); pump();
		QVERIFY(!client); QCOMPARE(processor.maximumDepth, 1); QCOMPARE(processor.cleanupCalls, 16); QVERIFY(!processor.held);
		const auto state = port.client()->inputDeliveryStatus(&port); QVERIFY(state.failed && !state.processing && !state.retirementCleanupPending);
	}

	void retirementInsideRecoveryPreservesTheFailureUntilUnwind_data()
	{
		QTest::addColumn<bool>("disabled"); QTest::newRow("enabled") << false; QTest::newRow("disabled-during-recovery") << true;
	}
	void retirementInsideRecoveryPreservesTheFailureUntilUnwind()
	{
		QFETCH(bool, disabled); auto client = std::make_unique<Client>(); Processor processor;
		MidiPort port("recovery", client.get(), &processor); port.setReadable(true); client->feed(&port, MidiEvent(MidiNoteOn, 0, 60, 100)); pump();
		client->fault(&port); bool retired = false;
		processor.onEvent = [&](const MidiEvent& event) {
			if (!retired && event.type() == MidiControlChange && event.controllerNumber() == MidiControllerAllSoundOff)
			{ retired = true; if (disabled) { port.setReadable(false); } client.reset(); pump(); }
		};
		QString error; bool changed = true; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed && !error.isEmpty());
		QVERIFY(retired); QCOMPARE(processor.maximumDepth, 1); QVERIFY(port.client()->inputDeliveryStatus(&port).retirementCleanupPending);
		pump(); const auto state = port.client()->inputDeliveryStatus(&port);
		QVERIFY(state.failed && !state.recovering && !state.retirementCleanupPending); QCOMPARE(state.failure, MidiClient::InputDeliveryFailure::ClientRetired);
		QCOMPARE(processor.cleanupCalls, 32); QVERIFY(!processor.held);
		if (disabled) { port.setReadable(true); }
		QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
	}

	void allPortsDetachBeforeCleanupCanDeleteAnotherPort()
	{
		auto client = std::make_unique<Client>(); Processor firstProcessor, secondProcessor;
		MidiPort first("first", client.get(), &firstProcessor); first.setReadable(true);
		auto second = std::make_unique<MidiPort>("second", client.get(), &secondProcessor); second->setReadable(true);
		client->feed(&first, MidiEvent(MidiNoteOn, 0, 60, 100)); client->feed(second.get(), MidiEvent(MidiNoteOn, 0, 61, 100)); pump();
		bool detachedBeforeCallback = false;
		firstProcessor.onEvent = [&](const MidiEvent&) {
			if (second) { detachedBeforeCallback = second->client() == first.client(); second.reset(); }
		};
		client.reset(); QVERIFY(detachedBeforeCallback); QVERIFY(!second); QCOMPARE(firstProcessor.cleanupCalls, 16);
		QCOMPARE(secondProcessor.cleanupCalls, 0); QCOMPARE(MidiClientTestAccess::registered(first.client()), std::size_t{1});
		pump(); QCOMPARE(firstProcessor.cleanupCalls, 16);
	}

	void cleanupCanDeleteItsOwnPortWithoutLeavingARegistryEntry()
	{
		auto client = std::make_unique<Client>(); Processor processor;
		auto port = std::make_unique<MidiPort>("self", client.get(), &processor); port->setReadable(true);
		client->feed(port.get(), MidiEvent(MidiNoteOn, 0, 60, 100)); pump(); MidiClient* fallback = nullptr;
		processor.onEvent = [&](const MidiEvent&) { if (port) { fallback = port->client(); port.reset(); } };
		client.reset(); QVERIFY(!port && fallback); QCOMPARE(processor.cleanupCalls, 1);
		QCOMPARE(MidiClientTestAccess::registered(fallback), std::size_t{0}); pump(); QCOMPARE(processor.cleanupCalls, 1);
	}

	void discardedCandidateDoesNotRetireTheActualOwner()
	{
		auto owner = std::make_unique<Client>(); Processor processor; MidiPort port("candidate", owner.get(), &processor); port.setReadable(true);
		owner->feed(&port, MidiEvent(MidiNoteOn, 0, 60, 100)); pump();
		auto candidate = std::make_unique<Client>(); candidate->addPort(&port); candidate.reset();
		QCOMPARE(port.client(), owner.get()); QVERIFY(!owner->inputDeliveryStatus(&port).failed); QCOMPARE(processor.cleanupCalls, 0); QVERIFY(processor.held);
		owner.reset(); QCOMPARE(processor.cleanupCalls, 16); QVERIFY(!processor.held);
	}

	void completedFailureCleanupDoesNotRepeatDuringRetirement()
	{
		auto client = std::make_unique<Client>(); Processor processor; MidiPort port("already-clean", client.get(), &processor); port.setReadable(true);
		client->feed(&port, MidiEvent(MidiNoteOn, 0, 60, 100)); pump(); client->fault(&port); pump();
		QCOMPARE(processor.cleanupCalls, 16); client.reset(); pump(); QCOMPARE(processor.cleanupCalls, 16);
		const auto state = port.client()->inputDeliveryStatus(&port); QVERIFY(state.failed && !state.retirementCleanupPending);
		QCOMPARE(state.failure, MidiClient::InputDeliveryFailure::ClientRetired);
	}

	void exportDefersCleanupUntilAnExplicitRetry()
	{
		auto client = std::make_unique<Client>(); Processor processor; MidiPort port("export", client.get(), &processor); port.setReadable(true);
		client->feed(&port, MidiEvent(MidiNoteOn, 0, 60, 100)); pump(); Engine::getSong()->startExport();
		const auto stopExport = qScopeGuard([] { Engine::getSong()->stopExport(); }); client.reset();
		QCOMPARE(processor.cleanupCalls, 0); QVERIFY(processor.held); QVERIFY(port.client()->inputDeliveryStatus(&port).retirementCleanupPending);
		QString error; bool changed = true; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed); pump(); QCOMPARE(processor.cleanupCalls, 0);
		Engine::getSong()->stopExport(); QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		QCOMPARE(processor.cleanupCalls, 16); QVERIFY(!processor.held); QVERIFY(!port.client()->inputDeliveryStatus(&port).retirementCleanupPending);
	}

	void nativeInstrumentVoicesRetireBeforeOrAfterTheirFirstRender_data()
	{
		QTest::addColumn<bool>("rendered"); QTest::addColumn<bool>("plugin");
		QTest::newRow("native-pending") << false << false; QTest::newRow("native-active") << true << false;
		QTest::newRow("tripleoscillator-pending") << false << true; QTest::newRow("tripleoscillator-active") << true << true;
	}
	void nativeInstrumentVoicesRetireBeforeOrAfterTheirFirstRender()
	{
		QFETCH(bool, rendered); QFETCH(bool, plugin);
		if (plugin && getPluginFactory()->pluginInfo("tripleoscillator").isNull()) { QSKIP("TripleOscillator unavailable; native DSP retirement acceptance still requires this scenario"); }
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		if (plugin) { QVERIFY(StudioCapabilities::execute("instrument.load", {{"track", 0}, {"instrument", "tripleoscillator"}}).value("ok").toBool()); }
		const auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); const auto port = track->midiPort();
		auto client = std::make_unique<Client>(); MidiPortTestAccess::useClient(port, client.get()); port->setReadable(true); port->setInputChannel(0);
		int started = 0; const auto connection = connect(track, &InstrumentTrack::midiNoteOn, track, [&](const Note&) { ++started; });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		client->feed(port, MidiEvent(MidiNoteOn, 0, 60, 100)); client->feed(port, MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127)); pump();
		QCOMPARE(started, 1); QVERIFY(track->isSustainPedalPressed()); const auto engine = Engine::audioEngine();
		if (rendered)
		{
			engine->renderNextPeriod(); const bool sounded = !silent(engine->renderNextPeriod());
			QCOMPARE(NotePlayHandle::nphsOfInstrumentTrack(track, true).size(), 1); if (plugin) { QVERIFY(sounded); }
		}
		const auto before = settings(*port); client.reset(); QVERIFY(!track->isSustainPedalPressed());
		QVERIFY(NotePlayHandle::nphsOfInstrumentTrack(track, true).empty()); QCOMPARE(settings(*port), before);
		// Flush the previous mixed period, then verify no pending note appears.
		engine->renderNextPeriod(); QVERIFY(silent(engine->renderNextPeriod())); QVERIFY(NotePlayHandle::nphsOfInstrumentTrack(track, true).empty());
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
		const auto inspected = StudioCapabilities::execute("midi.inspect", {{"track", 0}}); QVERIFY(inspected.value("ok").toBool());
		const auto state = inspected.value("result").toObject().value("inputDelivery").toObject();
		QVERIFY(state.value("registered").toBool() && state.value("failed").toBool()); QCOMPARE(state.value("failureCode").toInt(), 7);
		QVERIFY(state.contains("retirementCleanupPending") && !state.value("retirementCleanupPending").toBool());
		QVERIFY(state.value("resetEligible").toBool());
		const auto backend = state.value("backend").toObject(); QVERIFY(backend.contains("observationAvailable") && !backend.value("observationAvailable").toBool());
		const auto reset = StudioCapabilities::execute("midi.input.reset", {{"track", 0}}); QVERIFY(reset.value("ok").toBool());
		QVERIFY(reset.value("result").toObject().value("recoveryChanged").toBool()); QCOMPARE(settings(*port), before);
		// Local reset does not reconnect hardware. Exercise the real processor
		// directly to prove its old per-key pointer no longer blocks this note.
		port->processInEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QCOMPARE(started, 2); engine->renderNextPeriod();
		QCOMPARE(NotePlayHandle::nphsOfInstrumentTrack(track, true).size(), 1); track->silenceAllNotes();
	}

	void trackCleanupPreservesUnrelatedHandlesAndSurvivesReentrantDestruction_data()
	{
		QTest::addColumn<int>("placement"); QTest::newRow("pending") << 0; QTest::newRow("active") << 1; QTest::newRow("mixed") << 2;
	}
	void trackCleanupPreservesUnrelatedHandlesAndSurvivesReentrantDestruction()
	{
		QFETCH(int, placement); const auto engine = Engine::audioEngine();
		auto track = new InstrumentTrack(Engine::getSong()); auto other = new InstrumentTrack(Engine::getSong());
		int removed = 0, otherTrack = 0, otherType = 0;
		const auto cleanup = qScopeGuard([&] {
			engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle | PlayHandle::Type::PresetPreviewHandle);
			engine->removePlayHandlesOfTypes(other, PlayHandle::Type::SamplePlayHandle);
		});
		const auto reenter = [&] { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); };
		QVERIFY(engine->addPlayHandle(new TrackedHandle(track, PlayHandle::Type::SamplePlayHandle, removed, reenter)));
		QVERIFY(engine->addPlayHandle(new TrackedHandle(other, PlayHandle::Type::SamplePlayHandle, otherTrack)));
		QVERIFY(engine->addPlayHandle(new TrackedHandle(track, PlayHandle::Type::PresetPreviewHandle, otherType)));
		if (placement == 2) { engine->renderNextPeriod(); }
		QVERIFY(engine->addPlayHandle(new TrackedHandle(track, PlayHandle::Type::SamplePlayHandle, removed, reenter)));
		if (placement == 1) { engine->renderNextPeriod(); }
		engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle);
		QCOMPARE(removed, 2); QCOMPARE(otherTrack, 0); QCOMPARE(otherType, 0);
		engine->renderNextPeriod(); QCOMPARE(removed, 2); QCOMPARE(otherTrack, 0); QCOMPARE(otherType, 0);
		QCOMPARE(engine->playHandles().size(), 2);
	}

	void pendingTrackCleanupPreservesReservedRecordingCompletion()
	{
		const auto engine = Engine::audioEngine(); auto track = new SampleTrack(Engine::getSong());
		auto reservedClip = static_cast<SampleClip*>(track->createClip(0)); auto ordinaryClip = static_cast<SampleClip*>(track->createClip(0));
		int reserved = 0, ordinary = 0;
		const auto cleanup = qScopeGuard([&] { engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); });
		QVERIFY(engine->addPlayHandle(new TrackedRecording(reservedClip, true, reserved)));
		QVERIFY(engine->addPlayHandle(new TrackedRecording(ordinaryClip, false, ordinary)));
		engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle, true); QCOMPARE(reserved, 0); QCOMPARE(ordinary, 1);
		engine->removePlayHandlesOfTypes(track, PlayHandle::Type::SamplePlayHandle); QCOMPARE(reserved, 1); QCOMPARE(ordinary, 1);
	}
};
QTEST_GUILESS_MAIN(MidiClientRetirementTest)
#include "MidiClientRetirementTest.moc"

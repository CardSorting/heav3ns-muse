/* Native MIDI model, routing and byte-stream tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QThread>
#include <QEvent>
#include <QDomDocument>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <new>
#include <stdexcept>
#include <functional>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "MidiEventProcessor.h"
#include "MidiPort.h"
#include "MidiWinMM.h"
#include "MidiEventToByteSeq.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

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
	static void fault(MidiClient* client, MidiPort* port, MidiClient::InputDeliveryFailure reason)
	{
		client->failInputEvents(reason, port);
	}
};
}

#ifdef LMMS_HAVE_WINMM
namespace lmms
{
class MidiWinMMTestAccess
{
public:
	std::unique_ptr<MidiWinMM> client{new MidiWinMM(false)};
	const HMIDIIN input = reinterpret_cast<HMIDIIN>(std::uintptr_t{1});
	DWORD_PTR token;
	MidiWinMMTestAccess()
	{
		token = client->registerInput(input);
		const auto lock = std::lock_guard{client->m_devicesMutex}; client->m_inputDevices.insert(input, "Fixture Input"); client->m_startedInputs.insert(input);
	}
	~MidiWinMMTestAccess()
	{
		// Fixture handles never reach WinMM reset/close APIs.
		if (client) { const auto lock = std::lock_guard{client->m_devicesMutex}; client->m_inputDevices.clear(); client->m_startedInputs.clear(); }
	}
	void deliver(DWORD packed, UINT message = MIM_DATA)
	{
		late(input, token, message, packed);
	}
	static void late(HMIDIIN input, DWORD_PTR token, UINT message, DWORD packed) { MidiWinMM::inputCallback(input, message, token, packed, 0); }
	void retireToken() { client->removeInputToken(token); }
	void replaceToken() { client->removeInputToken(token); token = client->registerInput(input); }
	void clearFixtureHandles() { const auto lock = std::lock_guard{client->m_devicesMutex}; client->m_inputDevices.clear(); client->m_startedInputs.clear(); }
};
}
#endif

class NativeMidiProcessor final : public MidiEventProcessor
{
public:
	std::vector<MidiEvent> input;
	std::vector<TimePos> times;
	std::vector<QThread*> threads;
	bool throwNoteOn = false;
	bool throwAllSoundOff = false;
	std::function<void(const MidiEvent&)> onEvent;
	void processInEvent(const MidiEvent& event, const TimePos& time, f_cnt_t) override
	{
		if (throwNoteOn && event.type() == MidiNoteOn) { throw std::runtime_error("diagnostic MIDI processor failure"); }
		if (throwAllSoundOff && event.type() == MidiControlChange && event.controllerNumber() == MidiControllerAllSoundOff) { throw std::runtime_error("diagnostic MIDI cleanup failure"); }
		if (onEvent) { onEvent(event); }
		input.push_back(event); times.push_back(time); threads.push_back(QThread::currentThread());
	}
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
};

class NativeRawFixture final : public MidiClientRaw
{
public:
	QByteArray output;
	bool backendObserved = false, backendActive = true;
	InputBackendStatus inputBackendStatus() const override { return {backendObserved, backendActive, 0}; }
	void feed(std::initializer_list<unsigned char> bytes, bool pump = true)
	{
		for (const auto byte : bytes) { parseData(byte); }
		if (pump) { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
	}
	void send(const MidiEvent& event) { static_cast<MidiClient&>(*this).processOutEvent(event, TimePos{}, nullptr); }
private:
	void sendByte(unsigned char byte) override { output.append(static_cast<char>(byte)); }
};

class NativeNamedFixture final : public QObject, public MidiClient
{
	Q_OBJECT
public:
	QStringList inputs{"A", "B", QString::fromUtf8("Keyboard, α")}, outputs{QString::fromUtf8("Output, β")};
	QMap<QString, bool> inputRequests, outputRequests;
	bool queued = true;
	void processOutEvent(const MidiEvent&, const TimePos&, const MidiPort*) override { }
	QStringList readablePorts() const override { return inputs; }
	bool hasQueuedInputDelivery() const override { return queued; }
	QStringList writablePorts() const override { return outputs; }
	void subscribeReadablePort(MidiPort*, const QString& name, bool enabled) override { inputRequests.insert(name, enabled); }
	void subscribeWritablePort(MidiPort*, const QString& name, bool enabled) override { outputRequests.insert(name, enabled); }
	void connectRPChanged(QObject* receiver, const char* slot) override { connect(this, SIGNAL(readablePortsChanged()), receiver, slot); }
	void connectWPChanged(QObject* receiver, const char* slot) override { connect(this, SIGNAL(writablePortsChanged()), receiver, slot); }
	void refresh() { emit readablePortsChanged(); emit writablePortsChanged(); }
	void deliver(MidiPort* port, const MidiEvent& event, const TimePos& time, const QString& name)
	{
		dispatchInputEvent(event, time, port, name);
	}
signals:
	void readablePortsChanged();
	void writablePortsChanged();
};

class NativeOutputFaultFixture final : public MidiClient
{
public:
	std::vector<MidiEvent> events;
	bool fail = false;
	bool isRaw() const override { return true; }
	void processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort*) override
	{
		events.push_back(event);
		if (fail) { throw std::runtime_error("native output failed after submission"); }
	}
};

class NativeScheduledOutputFixture final : public MidiClient
{
public:
	struct Pending { MidiEvent event; const MidiPort* port; };
	std::vector<Pending> pending;
	QStringList actions;
	bool failCancellation = false;
	bool isRaw() const override { return true; }
	void processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort* port) override
	{
		if (int(time) > 0) { pending.push_back({event, port}); }
		else { actions.append("send"); }
	}
	void cancelOutputEvents(const MidiPort* port) override
	{
		actions.append("cancel");
		if (failCancellation) { throw std::runtime_error("scheduled output cancellation failed"); }
		std::erase_if(pending, [port](const auto& event) { return event.port == port; });
	}
};

class MidiPortTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
	MidiPort* instrumentPort() { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0))->midiPort(); }

private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); Engine::getSong()->clearProject(); }

	void nativeMessageDecoderPreservesChannelData_data()
	{
		QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
		QTest::newRow("on") << QByteArray::fromHex("9f3c64") << int(MidiNoteOn) << 60 << 100;
		QTest::newRow("off") << QByteArray::fromHex("8f3c28") << int(MidiNoteOff) << 60 << 40;
		QTest::newRow("pressure") << QByteArray::fromHex("af3c7f") << int(MidiKeyPressure) << 60 << 127;
		QTest::newRow("control") << QByteArray::fromHex("bf4a7f") << int(MidiControlChange) << 74 << 127;
		QTest::newRow("program") << QByteArray::fromHex("cf7f") << int(MidiProgramChange) << 127 << 0;
		QTest::newRow("channel-pressure") << QByteArray::fromHex("df40") << int(MidiChannelPressure) << 64 << 0;
		QTest::newRow("pitch-min") << QByteArray::fromHex("ef0000") << int(MidiPitchBend) << 0 << 0;
		QTest::newRow("pitch-center") << QByteArray::fromHex("ef0040") << int(MidiPitchBend) << 8192 << 0;
		QTest::newRow("pitch-max") << QByteArray::fromHex("ef7f7f") << int(MidiPitchBend) << 16383 << 0;
		QTest::newRow("zero-on") << QByteArray::fromHex("9f3c00") << int(MidiNoteOn) << 60 << 0;
	}
	void nativeMessageDecoderPreservesChannelData()
	{
		QFETCH(QByteArray, bytes); QFETCH(int, type); QFETCH(int, first); QFETCH(int, second);
		const auto event = readFromByteSeq(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size()); QVERIFY(event);
		bytes.fill('X'); QCOMPARE(event->type(), static_cast<MidiEventTypes>(type)); QCOMPARE(event->channel(), int8_t{15});
		QCOMPARE(event->param(0), int16_t(first)); QCOMPARE(event->param(1), int16_t(second)); QVERIFY(event->source() == MidiEvent::Source::Internal);
	}
	void nativeMessageDecoderRejectsIncompleteOrUnrepresentablePackets_data()
	{
		QTest::addColumn<QByteArray>("bytes");
		for (const auto* hex : {"", "90", "903c", "903c4000", "c0", "c00100", "d00100", "003c40", "f001", "f8", "908040", "903c80"})
		{
			QTest::newRow(*hex ? hex : "empty") << QByteArray::fromHex(hex);
		}
	}
	void nativeMessageDecoderRejectsIncompleteOrUnrepresentablePackets()
	{
		QFETCH(QByteArray, bytes);
		QVERIFY(!readFromByteSeq(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size()));
		QVERIFY(!readFromByteSeq(nullptr, 3));
	}
	void outputSourceSelectionPersistsAndPreventsDryAndGeneratedDuplicates()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("source", &client, &processor);
		port.setWritable(true); port.setOutputChannel(0);
		const MidiEvent note(MidiNoteOn, 2, 60, 100);
		port.processOutEvent(note); port.processOutEvent(note, {}, MidiPort::OutputSource::Instrument);
		QCOMPARE(client.output, QByteArray::fromHex("923c64")); client.output.clear();
		port.setOutputSource(MidiPort::OutputSource::Instrument); QCOMPARE(client.output, QByteArray::fromHex("823c00")); client.output.clear();
		port.processOutEvent(note); port.processOutEvent(note, {}, MidiPort::OutputSource::Instrument);
		QCOMPARE(client.output, QByteArray::fromHex("923c64"));
		QDomDocument doc; auto saved = doc.createElement("midiport"); doc.appendChild(saved); port.saveSettings(doc, saved);
		MidiPort restored("restored", &client, &processor); restored.loadSettings(saved);
		QVERIFY(restored.outputSource() == MidiPort::OutputSource::Instrument);
		saved.removeAttribute("outputsource"); restored.loadSettings(saved); QVERIFY(restored.outputSource() == MidiPort::OutputSource::Track);
		saved.setAttribute("outputsource", "broken");
		QVERIFY_EXCEPTION_THROWN(restored.loadSettings(saved), std::invalid_argument); QVERIFY(restored.outputSource() == MidiPort::OutputSource::Track);
	}
	void outputRetirementReleasesTransformedOverlappingNotesAndPedals_data()
	{
		QTest::addColumn<int>("retirement");
		QTest::newRow("source-change") << 0; QTest::newRow("disable") << 1; QTest::newRow("explicit") << 2; QTest::newRow("destructor") << 3;
		QTest::newRow("channel-change") << 4; QTest::newRow("fixed-note-change") << 5;
	}
	void outputRetirementReleasesTransformedOverlappingNotesAndPedals()
	{
		QFETCH(int, retirement); NativeRawFixture client; NativeMidiProcessor processor;
		auto port = std::make_unique<MidiPort>("release", &client, &processor); port->setWritable(true); port->setOutputChannel(0);
		port->setFixedOutputNote(65); port->setFixedOutputVelocity(70);
		port->processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100)); port->processOutEvent(MidiEvent(MidiNoteOn, 2, 62, 110));
		port->processOutEvent(MidiEvent(MidiControlChange, 2, MidiControllerSustain, 127));
		port->processOutEvent(MidiEvent(MidiControlChange, 2, MidiControllerSostenuto, 127)); client.output.clear();
		if (retirement == 0) { port->setOutputSource(MidiPort::OutputSource::Instrument); }
		if (retirement == 1) { port->setWritable(false); }
		if (retirement == 2) { port->releaseOutput(); }
		if (retirement == 3) { port.reset(); }
		if (retirement == 4) { port->setOutputChannel(4); }
		if (retirement == 5) { port->setFixedOutputNote(66); }
		QCOMPARE(client.output, QByteArray::fromHex("b24000b24200824100824100"));
		client.output.clear(); if (port) { port->releaseOutput(); } QVERIFY(client.output.isEmpty());
	}
	void offlineRenderingDoesNotSendTrackOrGeneratedMidi()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("offline", &client, &processor);
		port.setWritable(true); port.setOutputChannel(0); Engine::getSong()->startExport();
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100));
		port.setOutputSource(MidiPort::OutputSource::Instrument);
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), {}, MidiPort::OutputSource::Instrument);
		QVERIFY(client.output.isEmpty()); Engine::getSong()->stopExport();
		port.releaseOutput(); QVERIFY(client.output.isEmpty());
	}
	void noteTrackingLimitsRejectBeforeSendingAndRemainReleasable()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("limit", &client, &processor); port.setWritable(true);
		for (unsigned int i = 0; i < MidiPort::OutputNoteLimit; ++i) { port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); }
		QCOMPARE(port.trackedOutputNotes(), MidiPort::OutputNoteLimit); const auto sent = client.output.size();
		QVERIFY_EXCEPTION_THROWN(port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)), std::runtime_error); QCOMPARE(client.output.size(), sent);
		port.releaseOutput(); QCOMPARE(port.trackedOutputNotes(), 0u); QCOMPARE(client.output.size(), sent * 2);
	}
	void failedOutputKeepsCleanupStateAndCannotCommitASourceChange_data()
	{
		QTest::addColumn<bool>("initialFailure"); QTest::newRow("note-on-failed") << true; QTest::newRow("release-failed") << false;
	}
	void failedOutputKeepsCleanupStateAndCannotCommitASourceChange()
	{
		QFETCH(bool, initialFailure); NativeOutputFaultFixture client; NativeMidiProcessor processor;
		MidiPort port("fault", &client, &processor); port.setWritable(true); client.fail = initialFailure;
		if (initialFailure) { QVERIFY_EXCEPTION_THROWN(port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)), std::runtime_error); }
		else { port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); }
		QCOMPARE(port.trackedOutputNotes(), 1u); client.fail = true;
		QVERIFY_EXCEPTION_THROWN(port.setOutputSource(MidiPort::OutputSource::Instrument), std::runtime_error);
		QVERIFY(port.outputSource() == MidiPort::OutputSource::Track); QCOMPARE(port.trackedOutputNotes(), 1u);
		client.fail = false; port.setOutputSource(MidiPort::OutputSource::Instrument);
		QCOMPARE(port.trackedOutputNotes(), 0u); QCOMPARE(client.events.back().type(), MidiNoteOff); QCOMPARE(client.events.back().key(), int16_t{60});
	}
	void invalidOrUnavailableInstrumentOutputIsPrevalidated_data()
	{
		QTest::addColumn<QJsonValue>("source"); QTest::newRow("unknown") << QJsonValue("other");
		QTest::newRow("type") << QJsonValue(1); QTest::newRow("unsupported") << QJsonValue("instrument");
	}
	void failedOutputDisablePreservesNativeModeAndModelAgreement()
	{
		NativeOutputFaultFixture client; NativeMidiProcessor processor; MidiPort port("disable", &client, &processor);
		port.setWritable(true); port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); client.fail = true;
		QVERIFY_EXCEPTION_THROWN(port.setWritable(false), std::runtime_error);
		QVERIFY(port.isWritable()); QVERIFY(port.isOutputEnabled()); QCOMPARE(port.trackedOutputNotes(), 1u);
		client.fail = false; port.setWritable(false);
		QVERIFY(!port.isWritable()); QVERIFY(!port.isOutputEnabled()); QCOMPARE(port.trackedOutputNotes(), 0u);
	}
	void invalidOrUnavailableInstrumentOutputIsPrevalidated()
	{
		QFETCH(QJsonValue, source); QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		const auto before = StudioCapabilities::execute("midi.inspect", {{"track", 0}}).value("result").toObject();
		QVERIFY(!StudioCapabilities::execute("midi.configure", {{"track", 0}, {"writable", true}, {"outputChannel", 4}, {"outputSource", source}}).value("ok").toBool());
		QCOMPARE(StudioCapabilities::execute("midi.inspect", {{"track", 0}}).value("result").toObject(), before);
	}

	void scheduledEventsAreCancelledBeforeOutputRetirement_data()
	{
		QTest::addColumn<QString>("retirement");
		for (const auto value : {"source", "disable", "release", "destructor", "channel", "fixed-note", "unsubscribe"})
		{
			QTest::newRow(value) << QString::fromLatin1(value);
		}
	}
	void scheduledEventsAreCancelledBeforeOutputRetirement()
	{
		QFETCH(QString, retirement); NativeScheduledOutputFixture client; NativeMidiProcessor processor;
		auto port = std::make_unique<MidiPort>("scheduled", &client, &processor); port->subscribeWritablePort("external");
		port->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), TimePos(20));
		port->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0), TimePos(40));
		port->processOutEvent(MidiEvent(MidiProgramChange, 0, 7, 0), TimePos(30));
		QCOMPARE(port->trackedOutputNotes(), 0u); QCOMPARE(client.pending.size(), std::size_t(3)); client.actions.clear();
		if (retirement == "source") { port->setOutputSource(MidiPort::OutputSource::Instrument); }
		else if (retirement == "disable") { port->setWritable(false); }
		else if (retirement == "release") { port->releaseOutput(); }
		else if (retirement == "destructor") { port.reset(); }
		else if (retirement == "channel") { port->setOutputChannel(2); }
		else if (retirement == "fixed-note") { port->setFixedOutputNote(65); }
		else { port->subscribeWritablePort("external", false); }
		QVERIFY(client.pending.empty()); QVERIFY(!client.actions.empty()); QCOMPARE(client.actions.front(), QString("cancel"));
		QVERIFY(!client.actions.contains("send")); // The empty local note ledger must not suppress cancellation.
	}

	void cancellationPrecedesNoteCleanupAndLeavesOtherPortsQueued()
	{
		NativeScheduledOutputFixture client; NativeMidiProcessor processor;
		MidiPort first("first", &client, &processor), second("second", &client, &processor);
		first.setWritable(true); second.setWritable(true);
		first.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), TimePos(20));
		second.processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100), TimePos(20)); client.actions.clear();
		first.releaseOutput(); QCOMPARE(client.actions, QStringList({"cancel", "send"}));
		QCOMPARE(client.pending.size(), std::size_t(1)); QCOMPARE(client.pending.front().port, &second);
		QCOMPARE(first.trackedOutputNotes(), 0u); QCOMPARE(second.trackedOutputNotes(), 1u);
	}

	void failedCancellationPreservesRoutingUntilRetry_data()
	{
		QTest::addColumn<QString>("change");
		for (const auto value : {"source", "disable", "unsubscribe", "channel", "fixed-note"}) { QTest::newRow(value) << QString::fromLatin1(value); }
	}
	void failedCancellationPreservesRoutingUntilRetry()
	{
		QFETCH(QString, change); NativeScheduledOutputFixture client; NativeMidiProcessor processor;
		MidiPort port("retry", &client, &processor); port.subscribeWritablePort("external");
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), TimePos(20));
		port.processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0), TimePos(40)); client.failCancellation = true;
		const auto edit = [&] {
			if (change == "source") { port.setOutputSource(MidiPort::OutputSource::Instrument); }
			else if (change == "disable") { port.setWritable(false); }
			else if (change == "channel") { port.setOutputChannel(2); }
			else if (change == "fixed-note") { port.setFixedOutputNote(65); }
			else { port.subscribeWritablePort("external", false); }
		};
		QVERIFY_EXCEPTION_THROWN(edit(), std::runtime_error);
		QVERIFY(port.outputSource() == MidiPort::OutputSource::Track); QVERIFY(port.isWritable()); QVERIFY(port.isOutputEnabled());
		QCOMPARE(port.outputChannel(), 1); QCOMPARE(port.fixedOutputNote(), -1);
		QCOMPARE(port.requestedWritablePorts(), QStringList({"external"})); QCOMPARE(client.pending.size(), std::size_t(2));
		client.failCancellation = false; edit(); QVERIFY(client.pending.empty());
	}

	void failedCancellationDuringDestructionDoesNotReenterModelSignals()
	{
		NativeScheduledOutputFixture client; NativeMidiProcessor processor;
		auto port = std::make_unique<MidiPort>("retiring", &client, &processor); port->setWritable(true);
		port->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), TimePos(20)); client.actions.clear(); client.failCancellation = true;
		QTest::ignoreMessage(QtWarningMsg, "MIDI output cleanup failed while retiring a port"); port.reset();
		QCOMPARE(client.actions, QStringList({"cancel"}));
	}

	void transportStopAndPauseCancelSongAndPatternOutput_data()
	{
		QTest::addColumn<QString>("action");
		for (const auto value : {"stop", "pause", "already-stopped"}) { QTest::newRow(value) << QString::fromLatin1(value); }
	}
	void transportStopAndPauseCancelSongAndPatternOutput()
	{
		QFETCH(QString, action); NativeScheduledOutputFixture client; auto song = Engine::getSong();
		auto first = std::make_unique<InstrumentTrack>(song); auto second = std::make_unique<InstrumentTrack>(Engine::patternStore());
		for (const auto track : {first.get(), second.get()})
		{
			MidiPortTestAccess::useClient(track->midiPort(), &client); track->midiPort()->setWritable(true);
			track->midiPort()->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100), TimePos(20));
			track->midiPort()->processOutEvent(MidiEvent(MidiNoteOff, 0, 60, 0), TimePos(40));
		}
		QCOMPARE(client.pending.size(), std::size_t(4)); if (action != "already-stopped") { song->playSong(); }
		const auto stopped = qScopeGuard([&] { song->stop(); });
		const auto result = StudioCapabilities::execute(action == "pause" ? "transport.pause" : "transport.stop",
			action == "pause" ? QJsonObject{{"paused", true}} : QJsonObject{});
		QVERIFY2(result.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
		QVERIFY(client.pending.empty()); QVERIFY(song->midiOutputCleanupError().isEmpty());
		QVERIFY(action == "pause" ? song->isPaused() : song->isStopped());
	}

	void transportCleanupFailureDoesNotSkipOtherTracksAndCanRetry_data()
	{
		QTest::addColumn<bool>("pause"); QTest::newRow("stop") << false; QTest::newRow("pause") << true;
	}
	void transportCleanupFailureDoesNotSkipOtherTracksAndCanRetry()
	{
		QFETCH(bool, pause); NativeScheduledOutputFixture failed, healthy; auto song = Engine::getSong();
		auto first = std::make_unique<InstrumentTrack>(song); auto second = std::make_unique<InstrumentTrack>(Engine::patternStore());
		MidiPortTestAccess::useClient(first->midiPort(), &failed); MidiPortTestAccess::useClient(second->midiPort(), &healthy);
		for (const auto track : {first.get(), second.get()})
		{
			track->midiPort()->setWritable(true); track->midiPort()->processOutEvent(MidiEvent(MidiProgramChange, 0, 7, 0), TimePos(20));
		}
		song->playSong(); failed.failCancellation = true;
		const auto restored = qScopeGuard([&] { failed.failCancellation = false; song->stop(); });
		const auto execute = [&] { return StudioCapabilities::execute(pause ? "transport.pause" : "transport.stop", pause ? QJsonObject{{"paused", true}} : QJsonObject{}); };
		const auto result = execute(); QVERIFY(!result.value("ok").toBool()); QVERIFY(healthy.pending.empty());
		QCOMPARE(failed.pending.size(), std::size_t(1)); QVERIFY(pause ? song->isPaused() : song->isStopped());
		const auto state = StudioCapabilities::execute("transport.inspect", {}).value("result").toObject();
		QVERIFY(state.value("midiOutputCleanupError").toString().contains("scheduled output cancellation failed"));
		failed.failCancellation = false; QVERIFY(execute().value("ok").toBool()); QVERIFY(failed.pending.empty()); QVERIFY(song->midiOutputCleanupError().isEmpty());
	}

	void rawOutputPreservesAllNativeChannelVoiceMessages_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second"); QTest::addColumn<QByteArray>("expected");
		QTest::newRow("note-on") << int(MidiNoteOn) << 60 << 100 << QByteArray::fromHex("923c64");
		QTest::newRow("note-off") << int(MidiNoteOff) << 60 << 40 << QByteArray::fromHex("823c28");
		QTest::newRow("key-pressure") << int(MidiKeyPressure) << 60 << 70 << QByteArray::fromHex("a23c46");
		QTest::newRow("control") << int(MidiControlChange) << 74 << 99 << QByteArray::fromHex("b24a63");
		QTest::newRow("program") << int(MidiProgramChange) << 127 << 0 << QByteArray::fromHex("c27f");
		QTest::newRow("channel-pressure") << int(MidiChannelPressure) << 80 << 0 << QByteArray::fromHex("d250");
		QTest::newRow("pitch-bend") << int(MidiPitchBend) << 16383 << 0 << QByteArray::fromHex("e27f7f");
		QTest::newRow("zero-note-on") << int(MidiNoteOn) << 60 << 0 << QByteArray::fromHex("823c00");
	}
	void rawOutputPreservesAllNativeChannelVoiceMessages()
	{
		QFETCH(int, type); QFETCH(int, first); QFETCH(int, second); QFETCH(QByteArray, expected);
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("raw", &client, &processor); port.setWritable(true); port.setOutputChannel(0);
		port.processOutEvent(MidiEvent(static_cast<MidiEventTypes>(type), 2, first, second)); QCOMPARE(client.output, expected);
	}

	void nativeRawParserStartsEmptyAndKeepsRunningStatusAcrossRealtimeBytes()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("raw", &client, &processor); port.setReadable(true);
		client.feed({7, 8, 9}); QVERIFY(processor.input.empty());
		client.feed({0x90, 60, 0xf8, 100, 61, 101}); QCOMPARE(processor.input.size(), std::size_t(2));
		QCOMPARE(processor.input.at(0).key(), 60); QCOMPARE(processor.input.at(1).key(), 61); QCOMPARE(processor.input.at(1).velocity(), uint8_t(101));
		client.feed({0xf0, 1, 2, 3, 0xf7, 62, 102}); QCOMPARE(processor.input.size(), std::size_t(2));
		client.feed({0xc3, 127, 0xd3, 70}); QCOMPARE(processor.input.size(), std::size_t(4));
		QCOMPARE(processor.input.at(2).type(), MidiProgramChange); QCOMPARE(processor.input.at(2).program(), uint8_t(127));
		QCOMPARE(processor.input.at(2).param(1), int16_t(0)); QCOMPARE(processor.input.at(3).channelPressure(), uint8_t(70));
		client.feed({0xff, 63, 103}); QCOMPARE(processor.input.size(), std::size_t(5)); QCOMPARE(processor.input.back().type(), MidiSystemReset);
	}

	void nativePortModelsFilterChannelsAndPreserveZeroReleaseVelocity()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("raw", &client, &processor);
		port.setReadable(true); port.setInputChannel(4); port.setFixedInputVelocity(80);
		port.processInEvent(MidiEvent(MidiNoteOn, 2, 60, 100)); QVERIFY(processor.input.empty());
		port.processInEvent(MidiEvent(MidiNoteOn, 3, 60, 100)); QCOMPARE(processor.input.size(), std::size_t(1)); QCOMPARE(processor.input.back().velocity(), uint8_t(80));
		port.processInEvent(MidiEvent(MidiNoteOff, 3, 60, 0)); QCOMPARE(processor.input.back().velocity(), uint8_t(0));
		port.setWritable(true); port.setOutputChannel(4); port.setFixedOutputVelocity(70); port.setFixedOutputNote(65);
		port.processOutEvent(MidiEvent(MidiNoteOn, 2, 60, 100)); QVERIFY(client.output.isEmpty());
		port.processOutEvent(MidiEvent(MidiNoteOn, 3, 60, 100)); QCOMPARE(client.output, QByteArray::fromHex("934146")); client.output.clear();
		port.setOutputProgram(128); QCOMPARE(client.output, QByteArray::fromHex("c37f"));
	}

	void malformedRawVoiceEventsProduceNoTruncatedWireMessage_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<int>("channel"); QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
		QTest::newRow("channel") << int(MidiNoteOn) << 16 << 60 << 100;
		QTest::newRow("negative-key") << int(MidiNoteOn) << 0 << -1 << 100;
		QTest::newRow("velocity") << int(MidiNoteOn) << 0 << 60 << 128;
		QTest::newRow("controller") << int(MidiControlChange) << 0 << 128 << 7;
		QTest::newRow("program") << int(MidiProgramChange) << 0 << 128 << 0;
		QTest::newRow("negative-pressure") << int(MidiChannelPressure) << 0 << -1 << 0;
		QTest::newRow("bend") << int(MidiPitchBend) << 0 << 16384 << 0;
	}
	void malformedRawVoiceEventsProduceNoTruncatedWireMessage()
	{
		QFETCH(int, type); QFETCH(int, channel); QFETCH(int, first); QFETCH(int, second); NativeRawFixture client;
		client.send(MidiEvent(static_cast<MidiEventTypes>(type), channel, first, second)); QVERIFY(client.output.isEmpty());
		std::uint8_t buffer[3]{1, 2, 3};
		QCOMPARE(writeToByteSeq(MidiEvent(static_cast<MidiEventTypes>(type), channel, first, second), buffer, sizeof(buffer)), std::size_t(0));
		QCOMPARE(buffer[0], uint8_t(1));
	}

	void nativeRawConcurrentSendKeepsMessageBytesTogether()
	{
		NativeRawFixture client;
		std::thread first([&] { for (int n = 0; n < 100; ++n) { client.send(MidiEvent(MidiControlChange, 0, 74, 99)); } });
		std::thread second([&] { for (int n = 0; n < 100; ++n) { client.send(MidiEvent(MidiControlChange, 1, 7, 64)); } });
		first.join(); second.join(); QCOMPARE(client.output.size(), 600);
		for (int offset = 0; offset < client.output.size(); offset += 3)
		{
			const auto packet = client.output.mid(offset, 3); QVERIFY(packet == QByteArray::fromHex("b04a63") || packet == QByteArray::fromHex("b10740"));
		}
	}

	void incomingBackendEventsOwnTheirThreadSourceAndOrder()
	{
		NativeNamedFixture client; NativeMidiProcessor processor; MidiPort port("incoming", &client, &processor);
		port.subscribeReadablePort("A"); port.setInputChannel(0);
		const auto source = QString::fromUtf8("Borrowed callback, \xce\xb1");
		std::thread backend([&] {
			QString callbackName = source; int borrowedHandle = 7;
			for (int n = 0; n < 130; ++n)
			{
				client.deliver(&port, MidiEvent(MidiNoteOn, 2, n % 128, 100, &borrowedHandle), TimePos{n}, callbackName);
			}
			callbackName.clear();
		});
		backend.join(); QVERIFY(processor.input.empty()); QCOMPARE(client.inputDeliveryStatus(&port).pendingEvents, 130u);
		QTRY_COMPARE(processor.input.size(), std::size_t(130));
		for (int n = 0; n < 130; ++n)
		{
			const auto& event = processor.input.at(n); QCOMPARE(event.key(), n % 128); QVERIFY(event.sourcePort() == nullptr);
			QCOMPARE(client.sourcePortName(event), source); QCOMPARE(int(processor.times.at(n)), n);
			QCOMPARE(processor.threads.at(n), QThread::currentThread());
		}
		QCOMPARE(client.inputDeliveryStatus(&port).pendingEvents, 0u); QVERIFY(!client.inputDeliveryStatus(&port).failed);
	}

	void queuedInputIsCancelledBeforeRetirementOrRoutingChange_data()
	{
		QTest::addColumn<QString>("action");
		for (const auto action : {"port", "same-address", "client", "processor", "mode", "subscription"})
		{
			QTest::newRow(action) << QString::fromLatin1(action);
		}
	}
	void queuedInputIsCancelledBeforeRetirementOrRoutingChange()
	{
		QFETCH(QString, action); auto client = std::make_unique<NativeNamedFixture>(); NativeMidiProcessor processor, replacement;
		alignas(MidiPort) unsigned char storage[sizeof(MidiPort)];
		const auto destroy = [](MidiPort* target) { target->~MidiPort(); };
		std::unique_ptr<MidiPort, decltype(destroy)> owner{new (storage) MidiPort("retiring", client.get(), &processor), destroy};
		auto port = owner.get(); port->subscribeReadablePort("A");
		std::thread backend([&] { client->deliver(port, MidiEvent(MidiNoteOn, 0, 60, 100), TimePos{}, "A"); }); backend.join();
		QCOMPARE(client->inputDeliveryStatus(port).pendingEvents, 1u); QVERIFY(processor.input.empty());
		if (action == "port") { owner.reset(); port = nullptr; }
		else if (action == "same-address")
		{
			owner.reset(); owner.reset(new (storage) MidiPort("replacement", client.get(), &replacement)); port = owner.get(); port->subscribeReadablePort("A");
		}
		else if (action == "client") { client.reset(); }
		else if (action == "processor") { port->detachInputProcessor(); }
		else if (action == "mode") { port->setReadable(false); port->setReadable(true); }
		else { port->subscribeReadablePort("A", false); port->subscribeReadablePort("A", true); }
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.input.empty()); QVERIFY(replacement.input.empty());
		if (port && client && action != "processor")
		{
			client->deliver(port, MidiEvent(MidiNoteOn, 0, 61, 100), TimePos{}, "A");
			QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
			QCOMPARE((action == "same-address" ? replacement : processor).input.size(), std::size_t(1));
		}
	}

	void rawCallbackCanRacePortConstructionAndDeletion()
	{
		NativeRawFixture client; NativeMidiProcessor processor; std::atomic<bool> start{false};
		std::thread backend([&] {
			while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
			for (int n = 0; n < 4000; ++n) { client.feed({0x90, 60, 100}, false); }
		});
		start.store(true, std::memory_order_release);
		for (int n = 0; n < 200; ++n)
		{
			auto port = std::make_unique<MidiPort>("racing", &client, &processor); port->setReadable(true); port.reset();
		}
		backend.join(); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.input.empty());
		MidiPort survivor("survivor", &client, &processor); survivor.setReadable(true); client.feed({0x90, 62, 100});
		QCOMPARE(processor.input.size(), std::size_t(1)); QCOMPARE(processor.input.back().key(), 62);
	}

	void unsubscribingOneSourcePreservesOtherSourcesQueuedEvents()
	{
		NativeNamedFixture client; NativeMidiProcessor processor; MidiPort port("sources", &client, &processor);
		port.subscribeReadablePort("A"); port.subscribeReadablePort("B");
		client.deliver(&port, MidiEvent(MidiNoteOn, 0, 60, 100), TimePos{}, "A");
		client.deliver(&port, MidiEvent(MidiNoteOff, 0, 61, 0), TimePos{}, "B"); port.subscribeReadablePort("A", false);
		QTRY_COMPARE(processor.input.size(), std::size_t(1)); QCOMPARE(processor.input.back().key(), 61);
		QCOMPARE(processor.input.back().type(), MidiNoteOff); QCOMPARE(client.sourcePortName(processor.input.back()), QString("B"));
	}

	void boundedInputOverflowFailsAndReleasesVoices()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("overflow", &client, &processor); port.setReadable(true); port.setInputChannel(4);
		MidiPort disabled("disabled", &client, &processor);
		std::thread backend([&] {
			for (unsigned int n = 0; n < MidiClient::InputQueueLimit + 2; ++n) { client.feed({0x93, 60, 100}, false); }
		}); backend.join();
		const auto failed = client.inputDeliveryStatus(&port); QVERIFY(failed.failed); QCOMPARE(failed.pendingEvents, 0u);
		QVERIFY(failed.failure == MidiClient::InputDeliveryFailure::QueueOverflow);
		QCOMPARE(failed.droppedEvents, std::uint64_t(MidiClient::InputQueueLimit + 2)); QVERIFY(processor.input.empty());
		QVERIFY(!client.inputDeliveryStatus(&disabled).failed); QCOMPARE(client.inputDeliveryStatus(&disabled).pendingEvents, 0u);
		QTRY_COMPARE(processor.input.size(), std::size_t(MidiChannelCount));
		for (int channel = 0; channel < MidiChannelCount; ++channel)
		{
			const auto& event = processor.input.at(channel); QCOMPARE(event.type(), MidiControlChange);
			QCOMPARE(event.channel(), int8_t(channel)); QCOMPARE(event.controllerNumber(), uint8_t(MidiControllerAllSoundOff));
		}
		port.setReadable(false); port.setReadable(true); client.feed({0x93, 60, 100});
		QCOMPARE(processor.input.size(), std::size_t(MidiChannelCount)); QVERIFY(client.inputDeliveryStatus(&port).failed);
		disabled.setReadable(true); client.feed({0x93, 62, 100}); QCOMPARE(processor.input.size(), std::size_t(MidiChannelCount + 1));
		QCOMPARE(processor.input.back().key(), 62); QVERIFY(!client.inputDeliveryStatus(&disabled).failed);
	}

	void processorExceptionsBecomeInputFailures()
	{
		NativeRawFixture client; NativeMidiProcessor processor; processor.throwNoteOn = true;
		MidiPort port("throwing", &client, &processor); port.setReadable(true); client.feed({0x90, 60, 100});
		QTRY_VERIFY(client.inputDeliveryStatus(&port).failed); QCOMPARE(client.inputDeliveryStatus(&port).droppedEvents, std::uint64_t(1));
		QVERIFY(client.inputDeliveryStatus(&port).failure == MidiClient::InputDeliveryFailure::Processor);
		QCOMPARE(processor.input.size(), std::size_t(MidiChannelCount));
	}

	void incomingDispatchRejectsBorrowedPayloadAndMalformedVoice()
	{
		NativeNamedFixture client; NativeMidiProcessor processor; MidiPort port("scalar", &client, &processor); port.setReadable(true);
		const char borrowed[]{1, 2, 3}; client.deliver(&port, MidiEvent(MidiSysEx, borrowed, sizeof(borrowed)), TimePos{}, "A");
		client.deliver(&port, MidiEvent(MidiNoteOn, 0, 60, 128), TimePos{}, "A");
		MidiEvent meta(MidiMetaEvent); meta.setMetaEvent(MidiNotePanning); client.deliver(&port, meta, TimePos{}, "A");
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.input.empty()); QCOMPARE(client.inputDeliveryStatus(&port).pendingEvents, 0u);
		client.deliver(&port, MidiEvent(MidiPitchBend, 0, 16383, 0), TimePos{}, "A");
		QTRY_COMPARE(processor.input.size(), std::size_t(1)); QCOMPARE(processor.input.back().pitchBend(), int16_t(16383));
	}

	void localInputResetPreservesNativeModelsAndResumesTheParser()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("recover", &client, &processor);
		port.setReadable(true); port.setInputChannel(4); port.setFixedInputVelocity(80);
		QDomDocument before; auto beforeNode = before.createElement("midiport"); before.appendChild(beforeNode); port.saveSettings(before, beforeNode);
		for (unsigned int index = 0; index <= MidiClient::InputQueueLimit; ++index) { client.feed({0x93, 60, 100}, false); }
		QVERIFY(client.inputDeliveryStatus(&port).failed); bool injected = false;
		processor.onEvent = [&](const MidiEvent& event) {
			if (event.type() == MidiControlChange && !injected) { injected = true; client.feed({0x93, 61, 100}, false); }
		};
		QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		const auto recovered = client.inputDeliveryStatus(&port); QVERIFY(!recovered.failed); QVERIFY(!recovered.recovering); QCOMPARE(recovered.recoveryCount, std::uint64_t(1));
		QVERIFY(recovered.lastFailure == MidiClient::InputDeliveryFailure::QueueOverflow); QCOMPARE(recovered.failureVersion, std::uint64_t(1));
		QCOMPARE(recovered.droppedEvents, std::uint64_t(MidiClient::InputQueueLimit + 2)); QCOMPARE(processor.input.size(), std::size_t(MidiChannelCount));
		QDomDocument after; auto afterNode = after.createElement("midiport"); after.appendChild(afterNode); port.saveSettings(after, afterNode);
		QCOMPARE(after.toByteArray(), before.toByteArray()); client.feed({0x93, 65, 100});
		QTRY_COMPARE(processor.input.size(), std::size_t(MidiChannelCount + 1)); QCOMPARE(processor.input.back().key(), 65); QCOMPARE(processor.input.back().velocity(), uint8_t(80));
		changed = true; QVERIFY(port.resetInputDelivery(error, changed)); QVERIFY(!changed); QCOMPARE(client.inputDeliveryStatus(&port).recoveryCount, std::uint64_t(1));
	}

	void reentrantResetDuringDispatchAndCleanupIsRejected()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("nested", &client, &processor); port.setReadable(true);
		bool rejectedDispatch = false, rejectedRecovery = false;
		processor.onEvent = [&](const MidiEvent& event) {
			QString error; bool changed = false; const auto ok = port.resetInputDelivery(error, changed);
			if (event.type() == MidiNoteOn) { rejectedDispatch = !ok && !changed && !error.isEmpty(); }
			else if (event.type() == MidiControlChange)
			{
				rejectedRecovery = !ok && !changed && !error.isEmpty(); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
			}
		};
		client.feed({0x90, 60, 100}); QVERIFY(rejectedDispatch);
		MidiClientTestAccess::fault(&client, &port, MidiClient::InputDeliveryFailure::QueueOverflow);
		QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed); QVERIFY(rejectedRecovery);
		QVERIFY(!client.inputDeliveryStatus(&port).failed); QVERIFY(!client.inputDeliveryStatus(&port).processing); processor.onEvent = {};
		client.feed({0x90, 61, 100}); QTRY_COMPARE(processor.input.size(), std::size_t(MidiChannelCount + 2));
	}

	void aNewBackendFailureDuringCleanupKeepsInputSuspended()
	{
		NativeRawFixture client; client.backendObserved = true; NativeMidiProcessor processor; MidiPort port("refault", &client, &processor); port.setReadable(true);
		MidiClientTestAccess::fault(&client, &port, MidiClient::InputDeliveryFailure::BackendInput); bool injected = false;
		processor.onEvent = [&](const MidiEvent& event) {
			if (event.type() == MidiControlChange && !injected) { injected = true; MidiClientTestAccess::fault(&client, &port, MidiClient::InputDeliveryFailure::BackendInput); }
		};
		QString error; bool changed = false; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed); QVERIFY(!error.isEmpty());
		const auto failed = client.inputDeliveryStatus(&port); QVERIFY(failed.failed); QVERIFY(!failed.recovering); QCOMPARE(failed.failureVersion, std::uint64_t(2)); QCOMPARE(failed.recoveryCount, std::uint64_t(0));
		processor.onEvent = {}; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		client.feed({0x90, 62, 100}); QTRY_COMPARE(processor.input.size(), std::size_t(2 * MidiChannelCount + 1));
	}

	void inputResetPreflightLeavesFaultAndCountersUnchanged_data()
	{
		QTest::addColumn<QString>("condition");
		for (const auto condition : {"stopped-backend", "unknown-backend", "disabled", "processor-retired", "unregistered", "unsupported", "wrong-thread"})
		{
			QTest::newRow(condition) << QString::fromLatin1(condition);
		}
	}
	void inputResetPreflightLeavesFaultAndCountersUnchanged()
	{
		QFETCH(QString, condition); NativeRawFixture raw; NativeNamedFixture named; NativeMidiProcessor processor;
		MidiClient* client = condition == "unsupported" ? static_cast<MidiClient*>(&named) : static_cast<MidiClient*>(&raw);
		MidiPort port("preflight", client, &processor); port.setReadable(true);
		MidiClientTestAccess::fault(client, &port, condition == "unknown-backend" ? MidiClient::InputDeliveryFailure::BackendInput : MidiClient::InputDeliveryFailure::QueueOverflow);
		if (condition == "stopped-backend") { raw.backendObserved = true; raw.backendActive = false; }
		if (condition == "disabled") { port.setReadable(false); }
		if (condition == "processor-retired") { port.detachInputProcessor(); }
		if (condition == "unregistered") { client->removePort(&port); }
		if (condition == "unsupported") { named.queued = false; }
		QString error; bool changed = true, ok;
		if (condition == "wrong-thread") { std::thread other([&] { ok = client->resetInputDelivery(&port, error, changed); }); other.join(); }
		else { ok = port.resetInputDelivery(error, changed); }
		QVERIFY(!ok); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QVERIFY(processor.input.empty());
		QCOMPARE(client->inputDeliveryStatus(&port).recoveryCount, std::uint64_t(0));
		if (condition != "unregistered") { QVERIFY(client->inputDeliveryStatus(&port).failed); }
	}

	void cleanupExceptionsLeaveInputFailedUntilSuccessfulRecovery()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("cleanup", &client, &processor); port.setReadable(true);
		MidiClientTestAccess::fault(&client, &port, MidiClient::InputDeliveryFailure::QueueOverflow); processor.throwAllSoundOff = true;
		QString error; bool changed = false; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed);
		const auto failed = client.inputDeliveryStatus(&port); QVERIFY(failed.failed); QVERIFY(!failed.recovering); QCOMPARE(failed.failureVersion, std::uint64_t(2));
		QVERIFY(failed.lastFailure == MidiClient::InputDeliveryFailure::Processor); QCOMPARE(failed.recoveryCount, std::uint64_t(0));
		processor.throwAllSoundOff = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		client.feed({0x90, 63, 100}); QTRY_COMPARE(processor.input.size(), std::size_t(MidiChannelCount + 1));
	}

	void laterBackendFaultCannotBeHiddenByTheFirstLocalFailure()
	{
		NativeRawFixture client; NativeMidiProcessor processor; MidiPort port("mixed", &client, &processor); port.setReadable(true);
		MidiClientTestAccess::fault(&client, &port, MidiClient::InputDeliveryFailure::QueueOverflow);
		MidiClientTestAccess::fault(&client, &port, MidiClient::InputDeliveryFailure::BackendInput);
		const auto mixed = client.inputDeliveryStatus(&port); QVERIFY(mixed.failure == MidiClient::InputDeliveryFailure::QueueOverflow);
		QVERIFY(mixed.requiresBackendObservation); QString error; bool changed = false; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed);
		client.backendObserved = true; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		const auto reset = client.inputDeliveryStatus(&port); QVERIFY(!reset.requiresBackendObservation); QCOMPARE(reset.failureVersion, std::uint64_t(2));
	}

	void aBackendStoppingDuringCleanupCannotCommitRecovery()
	{
		NativeRawFixture client; client.backendObserved = true; NativeMidiProcessor processor; MidiPort port("stopping", &client, &processor); port.setReadable(true);
		MidiClientTestAccess::fault(&client, &port, MidiClient::InputDeliveryFailure::BackendInput);
		processor.onEvent = [&](const MidiEvent&) { client.backendActive = false; };
		QString error; bool changed = false; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed); QVERIFY(!error.isEmpty());
		const auto failed = client.inputDeliveryStatus(&port); QVERIFY(failed.failed); QVERIFY(!failed.recovering); QCOMPARE(failed.recoveryCount, std::uint64_t(0));
	}

	void studioInputResetPreservesRevisionHistoryAndReceiptReplay()
	{
		QTemporaryDir directory;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			const auto configured = runtime.process(request("configure", "midi.configure", {{"track", 0}, {"readable", true}, {"inputChannel", 3}})); QVERIFY(configured.value("ok").toBool());
			const auto before = runtime.process(request("before", "session.inspect")).value("result").toObject();
			QFile historyFile(directory.filePath("session.json")); QVERIFY(historyFile.open(QIODevice::ReadOnly));
			const auto historyBefore = QJsonDocument::fromJson(historyFile.readAll()).object().value("history"); historyFile.close(); QVERIFY(historyBefore.isArray());
			auto port = instrumentPort(); MidiClientTestAccess::fault(port->client(), port, MidiClient::InputDeliveryFailure::QueueOverflow);
			const auto reset = runtime.process(request("reset", "midi.input.reset", {{"track", 0}})); QVERIFY2(reset.value("ok").toBool(), qPrintable(reset.value("error").toString()));
			QCOMPARE(reset.value("revision"), configured.value("revision")); QVERIFY(reset.value("result").toObject().value("recoveryChanged").toBool());
			QCOMPARE(port->client()->inputDeliveryStatus(port).recoveryCount, std::uint64_t(1)); QCOMPARE(port->inputChannel(), 3);
			const auto after = runtime.process(request("after", "session.inspect")).value("result").toObject();
			QVERIFY(before.contains("historyCount")); QCOMPARE(after.value("historyCount"), before.value("historyCount")); QCOMPARE(after.value("cursor"), before.value("cursor"));
			QVERIFY(historyFile.open(QIODevice::ReadOnly)); QCOMPARE(QJsonDocument::fromJson(historyFile.readAll()).object().value("history"), historyBefore); historyFile.close();
			MidiClientTestAccess::fault(port->client(), port, MidiClient::InputDeliveryFailure::QueueOverflow);
			QVERIFY(runtime.process(request("reset", "midi.input.reset", {{"track", 0}})).value("replayed").toBool()); QVERIFY(port->client()->inputDeliveryStatus(port).failed);
			QCOMPARE(port->client()->inputDeliveryStatus(port).recoveryCount, std::uint64_t(1));
			QVERIFY(runtime.process(request("reset-again", "midi.input.reset", {{"track", 0}})).value("ok").toBool());
			QCOMPARE(port->client()->inputDeliveryStatus(port).recoveryCount, std::uint64_t(2));
		}
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); const auto port = instrumentPort();
			QCOMPARE(port->inputChannel(), 3); QCOMPARE(port->client()->inputDeliveryStatus(port).recoveryCount, std::uint64_t(0)); QVERIFY(!port->client()->inputDeliveryStatus(port).failed);
		}
	}

	void studioInputResetRequiresStoppedDisarmedTransportAndValidArguments()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("midi.configure", {{"track", 0}, {"readable", true}}).value("ok").toBool());
		const auto port = instrumentPort(); MidiClientTestAccess::fault(port->client(), port, MidiClient::InputDeliveryFailure::QueueOverflow);
		QVERIFY(StudioCapabilities::execute("transport.play", {}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("midi.input.reset", {{"track", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("transport.pause", {{"paused", true}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("midi.input.reset", {{"track", 0}}).value("ok").toBool());
		QVERIFY(StudioCapabilities::execute("transport.stop", {}).value("ok").toBool());
		Engine::getSong()->record(); QVERIFY(Engine::getSong()->isRecording());
		QVERIFY(!StudioCapabilities::execute("midi.input.reset", {{"track", 0}}).value("ok").toBool()); Engine::getSong()->stop();
		QVERIFY(!Engine::getSong()->isRecording());
		QVERIFY(!StudioCapabilities::execute("midi.input.reset", {{"track", 0}, {"readable", false}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::execute("midi.input.reset", {{"track", "zero"}}).value("ok").toBool());
		QVERIFY(port->client()->inputDeliveryStatus(port).failed); QCOMPARE(port->client()->inputDeliveryStatus(port).recoveryCount, std::uint64_t(0));
		const auto reset = StudioCapabilities::execute("midi.input.reset", {{"track", 0}}); QVERIFY(reset.value("ok").toBool());
		QVERIFY(reset.value("result").toObject().value("recoveryChanged").toBool()); QVERIFY(!StudioCapabilities::mutatesProject("midi.input.reset"));
	}

#ifdef LMMS_HAVE_WINMM
	void winMmCallbackPreservesVoiceDataAndOwnsItsSourceName()
	{
		MidiWinMMTestAccess backend; NativeMidiProcessor processor; MidiPort port("WinMM", backend.client.get(), &processor);
		port.subscribeReadablePort("Fixture Input"); port.setInputChannel(0);
		std::thread callback([&] {
			backend.deliver(0xe3 | (127u << 8) | (127u << 16)); backend.deliver(0xc3 | (127u << 8)); backend.deliver(0xd3 | (80u << 8));
			backend.deliver(0x93 | (128u << 8) | (100u << 16));
		}); callback.join(); QVERIFY(processor.input.empty()); QTRY_COMPARE(processor.input.size(), std::size_t(3));
		QCOMPARE(processor.input.at(0).pitchBend(), int16_t(16383)); QCOMPARE(processor.input.at(1).program(), uint8_t(127));
		QCOMPARE(processor.input.at(2).channelPressure(), uint8_t(80)); QCOMPARE(processor.input.at(2).param(1), int16_t(0));
		for (const auto& event : processor.input) { QVERIFY(event.sourcePort() == nullptr); QCOMPARE(backend.client->sourcePortName(event), QString("Fixture Input")); }
	}

	void winMmCallbackCanRaceUnsubscriptionAndPortDeletion()
	{
		MidiWinMMTestAccess backend; NativeMidiProcessor processor; auto port = std::make_unique<MidiPort>("WinMM", backend.client.get(), &processor);
		port->subscribeReadablePort("Fixture Input"); backend.deliver(0x90 | (60u << 8) | (100u << 16));
		port->subscribeReadablePort("Fixture Input", false); port->subscribeReadablePort("Fixture Input", true);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.input.empty());
		std::atomic<bool> started{false}; std::thread callback([&] {
			started.store(true, std::memory_order_release);
			for (int n = 0; n < 4000; ++n) { backend.deliver(0x90 | (60u << 8) | (100u << 16)); }
		});
		while (!started.load(std::memory_order_acquire)) { std::this_thread::yield(); }
		port.reset(); callback.join(); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.input.empty());
		MidiPort fresh("fresh", backend.client.get(), &processor); fresh.subscribeReadablePort("Fixture Input");
		backend.deliver(0x90 | (62u << 8) | (100u << 16)); QTRY_COMPARE(processor.input.size(), std::size_t(1)); QCOMPARE(processor.input.back().key(), 62);
	}

	void winMmRetiredTokensCannotTargetAReusedNativeHandle()
	{
		MidiWinMMTestAccess backend; NativeMidiProcessor processor; MidiPort port("WinMM", backend.client.get(), &processor); port.subscribeReadablePort("Fixture Input");
		const auto old = backend.token; backend.replaceToken(); QVERIFY(old != backend.token);
		MidiWinMMTestAccess::late(backend.input, old, MIM_DATA, 0x90 | (60u << 8) | (100u << 16));
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QVERIFY(processor.input.empty());
		backend.deliver(0x90 | (61u << 8) | (100u << 16)); QTRY_COMPARE(processor.input.size(), std::size_t(1)); QCOMPARE(processor.input.back().key(), 61);
	}

	void winMmCallbackTrafficCanRaceClientDestruction()
	{
		MidiWinMMTestAccess backend; NativeMidiProcessor processor; MidiPort survivor("WinMM", backend.client.get(), &processor); survivor.subscribeReadablePort("Fixture Input");
		const auto token = backend.token; const auto input = backend.input; std::atomic<bool> started{false};
		std::thread callback([&] {
			started.store(true, std::memory_order_release);
			for (int n = 0; n < 4000; ++n) { MidiWinMMTestAccess::late(input, token, MIM_DATA, 0x90 | (60u << 8) | (100u << 16)); }
		});
		while (!started.load(std::memory_order_acquire)) { std::this_thread::yield(); }
		backend.clearFixtureHandles(); backend.client.reset(); callback.join();
		MidiWinMMTestAccess::late(input, token, MIM_ERROR, 0); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QVERIFY(processor.input.empty()); QVERIFY(survivor.client()->isRaw());
	}

	void winMmInputErrorsCanRecoverWhileMoreDataPreservesItsEvent()
	{
		MidiWinMMTestAccess backend; NativeMidiProcessor processor; MidiPort port("WinMM", backend.client.get(), &processor); port.subscribeReadablePort("Fixture Input");
		QVERIFY(backend.client->inputBackendStatus().observed); QVERIFY(backend.client->inputBackendStatus().active);
		backend.deliver(0, MIM_ERROR); QVERIFY(backend.client->inputDeliveryStatus(&port).failed);
		QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		backend.deliver(0x90 | (62u << 8) | (100u << 16), MIM_MOREDATA);
		QTRY_COMPARE(processor.input.size(), std::size_t(MidiChannelCount + 1)); QCOMPARE(processor.input.back().key(), 62);
		QVERIFY(!backend.client->inputDeliveryStatus(&port).failed);
	}

	void winMmUnexpectedCloseStopsInputAndRetainsSubscriptionIntent()
	{
		MidiWinMMTestAccess backend; NativeMidiProcessor processor; MidiPort port("WinMM", backend.client.get(), &processor); port.subscribeReadablePort("Fixture Input");
		backend.deliver(0, MIM_CLOSE); QVERIFY(!backend.client->inputBackendStatus().active); QVERIFY(backend.client->readablePorts().isEmpty());
		QString error; bool changed = false; QVERIFY(!port.resetInputDelivery(error, changed)); QVERIFY(!changed);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QCOMPARE(port.requestedReadablePorts(), QStringList{"Fixture Input"});
		QCOMPARE(processor.input.size(), std::size_t(MidiChannelCount));
		backend.deliver(0x90 | (63u << 8) | (100u << 16)); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QCOMPARE(processor.input.size(), std::size_t(MidiChannelCount));
	}
#endif

	void structuredSubscriptionNamesSurviveDisappearanceAndNativeReload()
	{
		NativeNamedFixture client; NativeMidiProcessor processor; MidiPort port("named", &client, &processor);
		const auto input = QString::fromUtf8("Keyboard, α"); const auto output = QString::fromUtf8("Output, β");
		port.subscribeReadablePort(input); port.subscribeWritablePort(output);
		QDomDocument document; auto element = document.createElement("midiport"); document.appendChild(element); port.saveSettings(document, element);
		QDomDocument parsed; QVERIFY(parsed.setContent(document.toByteArray()));
		NativeNamedFixture restoredClient; MidiPort restored("restored", &restoredClient, &processor); restored.loadSettings(parsed.documentElement());
		QCOMPARE(restored.requestedReadablePorts(), QStringList{input}); QCOMPARE(restored.requestedWritablePorts(), QStringList{output});
		restoredClient.inputs.removeAll(input); restoredClient.refresh(); QVERIFY(!restored.readablePorts().contains(input)); QCOMPARE(restored.requestedReadablePorts(), QStringList{input});
		QDomDocument unavailable; auto node = unavailable.createElement("midiport"); unavailable.appendChild(node); restored.saveSettings(unavailable, node);
		MidiPort absent("absent", &restoredClient, &processor); absent.loadSettings(node); QCOMPARE(absent.requestedReadablePorts(), QStringList{input});
		restoredClient.inputs.append(input); restoredClient.refresh(); QVERIFY(restored.readablePorts().value(input)); QVERIFY(absent.readablePorts().value(input));
		QVERIFY(restoredClient.inputRequests.value(input)); restored.setReadable(false); QVERIFY(restored.requestedReadablePorts().isEmpty()); QVERIFY(!restoredClient.inputRequests.value(input));
	}

	void legacySubscriptionReloadUnsubscribesDeselectedPorts()
	{
		NativeNamedFixture client; NativeMidiProcessor processor; MidiPort port("legacy", &client, &processor); port.subscribeReadablePort("B");
		QDomDocument document; auto node = document.createElement("midiport"); node.setAttribute("readable", 1); node.setAttribute("writable", 0); node.setAttribute("inports", "A");
		port.loadSettings(node); QCOMPARE(port.requestedReadablePorts(), QStringList{"A"}); QVERIFY(client.inputRequests.value("A")); QVERIFY(!client.inputRequests.value("B"));
		QCOMPARE(port.baseVelocity(), MidiMaxVelocity);
	}

	void studioMidiConfigurationAndRecoveryUseNativeModels()
	{
		QTemporaryDir directory;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			const QJsonObject settings{{"track", 0}, {"readable", true}, {"writable", true}, {"inputChannel", 3}, {"outputChannel", 4},
				{"inputController", 74}, {"outputController", -1}, {"fixedInputVelocity", 80}, {"fixedOutputVelocity", 70},
				{"fixedOutputNote", 65}, {"outputProgram", 128}, {"baseVelocity", 100}};
			const auto configured = runtime.process(request("configure", "midi.configure", settings)); QVERIFY(configured.value("ok").toBool());
			QCOMPARE(instrumentPort()->inputChannel(), 3); QCOMPARE(instrumentPort()->outputProgram(), 128); QCOMPARE(instrumentPort()->baseVelocity(), 100);
			QVERIFY(runtime.process(request("configure", "midi.configure", settings)).value("replayed").toBool());
			QVERIFY(!runtime.process(request("unknown-port", "midi.subscribe", {{"track", 0}, {"direction", "input"}, {"port", "invented"}, {"enabled", true}})).value("ok").toBool());
			QCOMPARE(instrumentPort()->inputChannel(), 3);
		}
		{
			StudioRuntime recovered(directory.path()); QString error; QVERIFY2(recovered.open(error), qPrintable(error));
			const auto observed = recovered.process(request("observe", "midi.inspect", {{"track", 0}})); QVERIFY(observed.value("ok").toBool());
			QCOMPARE(observed.value("result").toObject().value("outputChannel").toInt(), 4); QVERIFY(!observed.value("result").toObject().value("connectionVerified").toBool());
			QCOMPARE(instrumentPort()->fixedOutputNote(), 65); QCOMPARE(instrumentPort()->outputController(), -1);
		}
	}

	void mixedInvalidMidiArgumentsArePrevalidatedBeforeAnyModelChange()
	{
		QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}}).value("ok").toBool());
		const auto before = StudioCapabilities::execute("midi.inspect", {{"track", 0}}).value("result").toObject();
		QVERIFY(!StudioCapabilities::execute("midi.configure", {{"track", 0}, {"inputChannel", 4}, {"outputProgram", 0}}).value("ok").toBool());
		QCOMPARE(StudioCapabilities::execute("midi.inspect", {{"track", 0}}).value("result").toObject(), before);
	}
};

QTEST_GUILESS_MAIN(MidiPortTest)
#include "MidiPortTest.moc"

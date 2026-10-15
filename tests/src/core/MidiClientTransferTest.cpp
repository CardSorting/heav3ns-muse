/* Native model-preserving MIDI client handoff. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "MidiDummy.h"
#include "InstrumentTrack.h"
#include "MidiClient.h"
#include "MidiController.h"
#include "MidiEventProcessor.h"
#include "MidiPort.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "TrackRenderScope.h"

namespace lmms
{
class StudioMidiBackendSelectionTestAccess
{
public:
	static auto select(const std::function<std::unique_ptr<MidiClient>()>& factory, const QString& name = MidiDummy::name(), bool abandonFailedOutput = false)
	{ return Engine::audioEngine()->selectMidiBackend(name, factory, abandonFailedOutput); }
};
class MidiClientTransferTestAccess
{
public:
	static auto transfer(MidiClient& source, MidiClient& target, const std::function<bool()>& cancelled = {})
	{ return source.transferPortsTo(target, cancelled); }
	static std::size_t ports(const MidiClient& client)
	{ const auto lock = std::lock_guard{client.m_portsMutex}; return client.m_inputDeliveries.size(); }
};
}
using namespace lmms;

class TransferClient final : public QObject, public MidiClient
{
	Q_OBJECT
public:
	bool raw = false, active = true, failDrain = false, failCancel = false, failRemoval = false;
	bool rejectOutput = false;
	OutputBackendStatus outputState;
	QString fault;
	int adds = 0, removes = 0, drains = 0, failPort = 1;
	QStringList inputs{"keyboard"}, outputs{"speaker"};
	std::function<void(const QString&, MidiPort*)> hook;
	std::function<void()> onDestroy;
	~TransferClient() override { if (onDestroy) { onDestroy(); } }
	std::vector<MidiEvent> output;
	QSet<MidiPort*> native;
	QMap<MidiPort*, QStringList> reads, writes;
	void stage(const QString& name, MidiPort* port = nullptr)
	{
		if (hook) { hook(name, port); }
		if (fault == name && adds == failPort) { throw std::runtime_error(("MIDI transfer fixture: " + name).toStdString()); }
	}
	bool isRaw() const override { return raw; }
	bool hasQueuedInputDelivery() const override { return true; }
	InputBackendStatus inputBackendStatus() const override { return {true, active, active ? 0 : -1}; }
	OutputBackendStatus outputBackendStatus() const override { return outputState; }
	void stopWithUnconfirmedOutput()
	{
		active = false; outputState = {true, false, 3, 1024, 2, -73, -73, true, true};
		failCancel = failDrain = rejectOutput = true; output.clear();
	}
	void addPort(MidiPort* port) override
	{
		++adds; stage("add-before", port); MidiClient::addPort(port); native.insert(port); stage("add-after", port);
	}
	void removePort(MidiPort* port) override
	{
		++removes; if (hook) { hook("remove", port); }
		if (failRemoval) { throw std::runtime_error("Native removal fixture"); }
		native.remove(port); reads.remove(port); writes.remove(port); MidiClient::removePort(port);
	}
	void applyPortMode(MidiPort* port) override { stage("mode", port); }
	void applyPortName(MidiPort* port) override { stage("name", port); }
	QStringList readablePorts() const override { const_cast<TransferClient*>(this)->stage("read-list"); return inputs; }
	QStringList writablePorts() const override { const_cast<TransferClient*>(this)->stage("write-list"); return outputs; }
	void subscribeReadablePort(MidiPort* port, const QString& name, bool enabled) override
	{ stage("subscribe-read", port); reads[port].removeAll(name); if (enabled) { reads[port].append(name); } }
	void subscribeWritablePort(MidiPort* port, const QString& name, bool enabled) override
	{ stage("subscribe-write", port); writes[port].removeAll(name); if (enabled) { writes[port].append(name); } }
	void connectRPChanged(QObject* receiver, const char* slot) override
	{ stage("connect-read"); connect(this, SIGNAL(inputListChanged()), receiver, slot); }
	void connectWPChanged(QObject* receiver, const char* slot) override
	{ stage("connect-write"); connect(this, SIGNAL(outputListChanged()), receiver, slot); }
	void refresh() { emit inputListChanged(); emit outputListChanged(); }
	void processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort*) override
	{ if (rejectOutput) { throw std::runtime_error("Terminal output fixture"); } output.push_back(event); }
	void cancelOutputEvents(const MidiPort* port) override
	{ stage("cancel", const_cast<MidiPort*>(port)); if (failCancel) { throw std::runtime_error("Native cancellation fixture"); } }
	void feed(MidiPort* port, int key = 60)
	{ dispatchInputEvent(MidiEvent(MidiNoteOn, 0, key, 100), {}, port, "keyboard"); }
protected:
	void validatePortForTransfer(const MidiPort* port) const override
	{ const_cast<TransferClient*>(this)->stage("validate", const_cast<MidiPort*>(port)); MidiClient::validatePortForTransfer(port); }
	void finishPortTransferOutput(const std::function<bool()>& cancelled) override
	{ ++drains; stage("drain"); MidiClient::finishPortTransferOutput(cancelled); if (failDrain) { throw std::runtime_error("Native drain fixture"); } }
signals:
	void inputListChanged();
	void outputListChanged();
};

class TransferProcessor final : public MidiEventProcessor
{
public:
	std::vector<MidiEvent> received;
	std::function<void()> cleanupHook;
	std::function<void()> noteHook;
	bool failCleanup = false;
	void processInEvent(const MidiEvent& event, const TimePos&, f_cnt_t) override
	{
		if (event.type() == MidiControlChange && event.controllerNumber() == MidiControllerAllSoundOff)
		{ if (cleanupHook) { cleanupHook(); } if (failCleanup) { throw std::runtime_error("Native processor cleanup fixture"); } }
		if (event.type() == MidiNoteOn && noteHook) { noteHook(); }
		received.push_back(event);
	}
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
	int notes() const { return static_cast<int>(std::count_if(received.begin(), received.end(), [](const auto& e) { return e.type() == MidiNoteOn; })); }
};

namespace
{
class TransferAudio final : public AudioDevice
{
public:
	explicit TransferAudio(AudioEngine* engine) : AudioDevice(DEFAULT_CHANNELS, engine) { setSampleRate(48000); }
	~TransferAudio() override { onStop = onStart = {}; failStops = 0; stopProcessing(); }
	int stops = 0, starts = 0, failStops = 0;
	bool failStart = false, throwStart = false;
	std::function<void()> onStop, onStart;
private:
	void startProcessingImpl() override
	{
		++starts; if (onStart) { onStart(); }
		if (throwStart) { throw std::runtime_error("MIDI selection audio restart fixture"); }
		if (failStart) { reportProcessingFailure(ProcessingFailure::Start, -73); }
	}
	void stopProcessingImpl() override
	{
		++stops; if (onStop) { onStop(); }
		if (failStops > 0) { --failStops; throw std::runtime_error("MIDI selection audio stop fixture"); }
	}
};
TransferAudio* installTransferAudio(bool running)
{
	auto engine = Engine::audioEngine(); auto audio = new TransferAudio(engine); engine->setAudioDevice(audio, running); return audio;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QString& directory)
{ QFile file(directory + "/session.json"); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto value : calls)
	{
		const auto item = value.toObject(); const auto arguments = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", arguments}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", arguments}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
void pump() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
QByteArray settings(MidiPort& port)
{
	QDomDocument document; auto element = document.createElement("midiport"); document.appendChild(element);
	port.saveSettings(document, element); return document.toByteArray();
}
void configure(MidiPort& port)
{
	port.setName(QString::fromUtf8("Keyboard \xce\xb1")); port.setInputChannel(0); port.setOutputChannel(0);
	port.setFixedInputVelocity(73); port.setFixedOutputVelocity(91); port.setFixedOutputNote(48);
	port.setInputController(74); port.setOutputController(75); port.setOutputProgram(9); port.setBaseVelocity(70);
	port.subscribeReadablePort("keyboard"); port.subscribeWritablePort("speaker");
	port.subscribeReadablePort("absent input"); port.subscribeWritablePort("absent output");
}
}

class MidiClientTransferTest : public QObject
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
	void cleanup()
	{
		if (auto engine = Engine::audioEngine())
		{
			if (auto midi = dynamic_cast<TransferClient*>(engine->midiClient())) { midi->hook = {}; midi->onDestroy = {}; }
			if (auto audio = dynamic_cast<TransferAudio*>(engine->audioDev())) { audio->onStop = audio->onStart = {}; audio->failStops = 0; audio->failStart = audio->throwStart = false; }
		}
		Engine::destroy();
	}

	void modelParametersAndRequestedEndpointsSurviveRawAndNamedTransfers_data()
	{
		QTest::addColumn<bool>("sourceRaw"); QTest::addColumn<bool>("targetRaw");
		for (const bool source : {false, true}) { for (const bool target : {false, true})
		{ QTest::newRow(qPrintable(QString("raw-%1-to-%2").arg(source).arg(target))) << source << target; } }
	}
	void modelParametersAndRequestedEndpointsSurviveRawAndNamedTransfers()
	{
		QFETCH(bool, sourceRaw); QFETCH(bool, targetRaw); auto source = std::make_unique<TransferClient>(); TransferClient target;
		source->raw = sourceRaw; target.raw = targetRaw; TransferProcessor processor; MidiPort port("native", source.get(), &processor, nullptr, MidiPort::Mode::Duplex);
		configure(port); port.setOutputSource(MidiPort::OutputSource::Instrument); const auto before = settings(port); const auto identity = &port;
		const auto result = MidiClientTransferTestAccess::transfer(*source, target);
		QCOMPARE(result.ports, 1u); QCOMPARE(result.inputResetsRequired, 1u); QCOMPARE(result.retirementExceptions, 0u); QCOMPARE(port.client(), static_cast<MidiClient*>(&target));
		QCOMPARE(&port, identity); QCOMPARE(settings(port), before); QCOMPARE(port.displayName(), QString::fromUtf8("Keyboard \xce\xb1"));
		QCOMPARE(MidiClientTransferTestAccess::ports(*source), std::size_t{0}); QVERIFY(target.inputDeliveryStatus(&port).failed);
		if (targetRaw) { QVERIFY(port.readablePorts().empty() && port.writablePorts().empty()); }
		else { QCOMPARE(target.reads.value(&port), QStringList{"keyboard"}); QCOMPARE(target.writes.value(&port), QStringList{"speaker"}); }
		source.reset(); QCOMPARE(port.client(), static_cast<MidiClient*>(&target)); QCOMPARE(settings(port), before);
		QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
		target.feed(&port, 64); pump(); QCOMPARE(processor.notes(), 1); QCOMPARE(processor.received.back().velocity(), 73);
		port.processOutEvent(MidiEvent(MidiNoteOn, 1, 65, 100), {}, MidiPort::OutputSource::Instrument); QCOMPARE(target.output.back().key(), 48); QCOMPARE(target.output.back().velocity(), 91);
	}

	void disabledAndSingleDirectionModesRemainUnchanged_data()
	{ QTest::addColumn<int>("mode"); for (int mode = 0; mode < 4; ++mode) { QTest::newRow(qPrintable(QString::number(mode))) << mode; } }
	void disabledAndSingleDirectionModesRemainUnchanged()
	{
		QFETCH(int, mode); TransferClient source, target; TransferProcessor processor; MidiPort port("mode", &source, &processor, nullptr, static_cast<MidiPort::Mode>(mode));
		const auto before = settings(port); const bool input = port.isInputEnabled(); const auto result = MidiClientTransferTestAccess::transfer(source, target);
		QCOMPARE(settings(port), before); QCOMPARE(static_cast<int>(port.mode()), mode); QCOMPARE(result.inputResetsRequired, input ? 1u : 0u);
	}

	void everyPortCommitsTogetherAfterAllPreparations()
	{
		TransferClient source, target; TransferProcessor processor; MidiPort a("a", &source, &processor), b("b", &source, &processor); configure(a); configure(b);
		bool preparedTogether = true, retiredTogether = true;
		target.hook = [&](const QString&, MidiPort*) { preparedTogether &= a.client() == &source && b.client() == &source; };
		source.hook = [&](const QString& stage, MidiPort*) { if (stage == "remove") { retiredTogether &= a.client() == &target && b.client() == &target; } };
		const auto result = MidiClientTransferTestAccess::transfer(source, target); target.hook = {}; source.hook = {};
		QCOMPARE(result.ports, 2u); QVERIFY(preparedTogether && retiredTogether); QCOMPARE(target.native.size(), 2); QVERIFY(source.native.empty());
	}

	void preparationFailuresPreserveAllOriginalBindings_data()
	{
		QTest::addColumn<QString>("fault"); QTest::addColumn<int>("at");
		for (const auto fault : {"add-before", "add-after", "mode", "name", "read-list", "write-list", "subscribe-read", "subscribe-write", "connect-read", "connect-write", "validate"})
		{ for (const int at : {1, 2}) { QTest::newRow(qPrintable(QString("%1-port-%2").arg(fault).arg(at))) << QString(fault) << at; } }
	}
	void preparationFailuresPreserveAllOriginalBindings()
	{
		QFETCH(QString, fault); QFETCH(int, at); TransferClient source; auto target = std::make_unique<TransferClient>(); TransferProcessor processor;
		MidiPort a("a", &source, &processor), b("b", &source, &processor); configure(a); configure(b); const auto beforeA = settings(a), beforeB = settings(b);
		source.feed(&a); source.feed(&b); target->fault = fault; target->failPort = at;
		QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(source, *target), std::runtime_error);
		QCOMPARE(a.client(), static_cast<MidiClient*>(&source)); QCOMPARE(b.client(), static_cast<MidiClient*>(&source));
		QCOMPARE(settings(a), beforeA); QCOMPARE(settings(b), beforeB); QCOMPARE(MidiClientTransferTestAccess::ports(*target), std::size_t{0}); QVERIFY(target->native.empty());
		target.reset(); QCOMPARE(a.client(), static_cast<MidiClient*>(&source)); QCOMPARE(source.native.size(), 2); pump(); QCOMPARE(processor.notes(), 0);
		QString error; bool changed = false; QVERIFY(a.resetInputDelivery(error, changed)); QVERIFY(changed); source.feed(&a, 67); pump(); QCOMPARE(processor.notes(), 1);
	}

	void cancellationAtAdmissionPreparationAndFinalValidationKeepsBindings_data()
	{
		QTest::addColumn<QString>("stage"); QTest::addColumn<bool>("throws");
		for (const auto stage : {"admission", "cancel", "first-add", "second-add", "subscribe-write", "validate"})
		{ for (const bool throws : {false, true}) { QTest::newRow(qPrintable(QString("%1-throw-%2").arg(stage).arg(throws))) << QString(stage) << throws; } }
	}
	void cancellationAtAdmissionPreparationAndFinalValidationKeepsBindings()
	{
		QFETCH(QString, stage); QFETCH(bool, throws); TransferClient source, target; TransferProcessor processor;
		MidiPort a("a", &source, &processor), b("b", &source, &processor); configure(a); configure(b); bool cancel = stage == "admission";
		const auto observe = [&](const QString& value, MidiPort*) {
			if (value == stage || (value == "add-after" && ((stage == "first-add" && target.adds == 1) || (stage == "second-add" && target.adds == 2)))) { cancel = true; }
		}; source.hook = target.hook = observe;
		QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(source, target, [&] {
			if (cancel && throws) { throw std::runtime_error("fixture cancellation"); } return cancel;
		}), std::runtime_error);
		source.hook = target.hook = {}; QCOMPARE(a.client(), static_cast<MidiClient*>(&source)); QCOMPARE(b.client(), static_cast<MidiClient*>(&source));
		QCOMPARE(MidiClientTransferTestAccess::ports(target), std::size_t{0}); QVERIFY(target.native.empty()); QCOMPARE(source.native.size(), 2);
		QCOMPARE(source.inputDeliveryStatus(&a).failed, stage != "admission");
	}

	void cleanupFailureRefusesPublicationAndKeepsRouting_data()
	{ QTest::addColumn<QString>("failure"); for (const auto f : {"cancel", "processor", "drain"}) { QTest::newRow(f) << QString(f); } }
	void cleanupFailureRefusesPublicationAndKeepsRouting()
	{
		QFETCH(QString, failure); TransferClient source, target; TransferProcessor processor; MidiPort port("cleanup", &source, &processor); configure(port);
		source.failCancel = failure == "cancel"; source.failDrain = failure == "drain"; processor.failCleanup = failure == "processor";
		QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(source, target), std::runtime_error);
		source.failCancel = source.failDrain = processor.failCleanup = false;
		QCOMPARE(port.client(), static_cast<MidiClient*>(&source)); QCOMPARE(target.adds, 0); QVERIFY(source.inputDeliveryStatus(&port).failed);
	}

	void queuedAndConcurrentInputCannotCrossTheHandoff()
	{
		TransferClient source, target; TransferProcessor processor; MidiPort port("input", &source, &processor); configure(port); source.feed(&port, 60);
		target.hook = [&](const QString& stage, MidiPort*) {
			if (stage != "validate") { return; }
			std::thread producer([&] { for (int i = 0; i < 80; ++i) { source.feed(&port, 61); target.feed(&port, 62); } }); producer.join();
			pump(); // Deliberate nested Qt dispatch must still see paused deliveries.
		};
		MidiClientTransferTestAccess::transfer(source, target); target.hook = {}; pump(); QCOMPARE(processor.notes(), 0);
		source.feed(&port, 63); target.feed(&port, 64); pump(); QCOMPARE(processor.notes(), 0); QVERIFY(target.inputDeliveryStatus(&port).droppedEvents >= 80);
		QString error; bool changed = false; QVERIFY(port.resetInputDelivery(error, changed)); target.feed(&port, 65); pump();
		QCOMPARE(processor.notes(), 1); QCOMPARE(processor.received.back().key(), 65);
	}

	void outputIsReleasedOnTheOldClientAndSuppressedDuringPreparation()
	{
		TransferClient source, target; TransferProcessor processor; MidiPort port("output", &source, &processor, nullptr, MidiPort::Mode::Duplex);
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); port.processOutEvent(MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127)); source.output.clear();
		bool refusedOutput = true; target.hook = [&](const QString&, MidiPort*) {
			refusedOutput &= !port.acceptsOutput(MidiPort::OutputSource::Track); port.processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100));
		};
		MidiClientTransferTestAccess::transfer(source, target); target.hook = {};
		QVERIFY(refusedOutput); QCOMPARE(port.trackedOutputNotes(), 0u); QVERIFY(target.output.empty()); QCOMPARE(source.drains, 1);
		QCOMPARE(source.output.size(), std::size_t{2}); QCOMPARE(source.output[0].controllerNumber(), static_cast<int>(MidiControllerSustain)); QCOMPARE(source.output[0].controllerValue(), 0);
		QCOMPARE(source.output[1].type(), MidiNoteOff); port.processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100)); QCOMPARE(target.output.back().key(), 62);
	}

	void reentrantMutationAndRegistrationAreRefusedDuringTransfer()
	{
		TransferClient source, target; TransferProcessor processor; MidiPort port("guard", &source, &processor); configure(port); int refused = 0;
		target.hook = [&](const QString& stage, MidiPort*) {
			if (stage != "add-after") { return; }
			const auto attempt = [&](const auto& action) { try { action(); } catch (...) { ++refused; } };
			attempt([&] { MidiClientTransferTestAccess::transfer(source, target); }); attempt([&] { port.setMode(MidiPort::Mode::Disabled); });
			attempt([&] { port.setName("changed"); }); attempt([&] { port.setOutputSource(MidiPort::OutputSource::Instrument); });
			attempt([&] { port.subscribeReadablePort("other"); }); attempt([&] { port.subscribeWritablePort("other"); });
			attempt([&] { MidiPort extra("extra", &source, &processor); }); attempt([&] { port.setOutputChannel(4); });
		};
		MidiClientTransferTestAccess::transfer(source, target); target.hook = {}; QCOMPARE(refused, 8); QCOMPARE(port.outputChannel(), 0); QCOMPARE(port.mode(), MidiPort::Mode::Duplex);
	}

	void activeInputCallbackCannotReplaceItsOwnRegistration()
	{
		TransferClient source, target; TransferProcessor processor; MidiPort port("active", &source, &processor, nullptr, MidiPort::Mode::Input); int refused = 0;
		processor.noteHook = [&] { try { MidiClientTransferTestAccess::transfer(source, target); } catch (...) { ++refused; } };
		source.feed(&port); pump(); QCOMPARE(refused, 1); QCOMPARE(port.client(), static_cast<MidiClient*>(&source)); QCOMPARE(target.adds, 0); QVERIFY(!source.inputDeliveryStatus(&port).failed);
	}

	void cancellationCanRetireAPreparedModelWithoutLeavingDanglingDeliveries()
	{
		TransferClient source, target; TransferProcessor processor; auto port = std::make_unique<MidiPort>("retiring", &source, &processor); configure(*port); bool retire = false;
		target.hook = [&](const QString& stage, MidiPort*) { if (stage == "add-after") { retire = true; } };
		QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(source, target, [&] { if (retire && port) { port.reset(); } return false; }), std::runtime_error);
		target.hook = {}; QVERIFY(!port); QVERIFY(source.native.empty() && target.native.empty()); QCOMPARE(MidiClientTransferTestAccess::ports(source), std::size_t{0});
		QCOMPARE(MidiClientTransferTestAccess::ports(target), std::size_t{0}); pump();
	}

	void retirementExceptionsDoNotRollBackCommittedModelPointers()
	{
		auto source = std::make_unique<TransferClient>(); TransferClient target; TransferProcessor processor; MidiPort port("retire", source.get(), &processor); configure(port); source->failRemoval = true;
		const auto result = MidiClientTransferTestAccess::transfer(*source, target); QCOMPARE(result.retirementExceptions, 1u); QCOMPARE(port.client(), static_cast<MidiClient*>(&target));
		QCOMPARE(MidiClientTransferTestAccess::ports(*source), std::size_t{0}); source->feed(&port); source.reset(); pump(); QCOMPARE(processor.notes(), 0); QCOMPARE(port.client(), static_cast<MidiClient*>(&target));
	}

	void failedReplacementCleanupCannotInvalidateTheOriginalClient()
	{
		TransferClient source; auto target = std::make_unique<TransferClient>(); TransferProcessor processor; MidiPort port("rollback", &source, &processor); configure(port);
		target->fault = "mode"; target->failRemoval = true; QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(source, *target), std::runtime_error);
		QCOMPARE(MidiClientTransferTestAccess::ports(*target), std::size_t{0}); target.reset(); QCOMPARE(port.client(), static_cast<MidiClient*>(&source));
		QString error; bool changed = false; QVERIFY(port.resetInputDelivery(error, changed)); source.feed(&port); pump(); QCOMPARE(processor.notes(), 1);
	}

	void portConstructionFailureUnregistersItsPartialDelivery_data()
	{ QTest::addColumn<QString>("fault"); for (const auto f : {"add-after", "read-list", "write-list", "connect-read", "connect-write", "mode"}) { QTest::newRow(f) << QString(f); } }
	void portConstructionFailureUnregistersItsPartialDelivery()
	{
		QFETCH(QString, fault); TransferClient client; client.fault = fault; TransferProcessor processor;
		QVERIFY_EXCEPTION_THROWN((std::make_unique<MidiPort>("partial", &client, &processor)), std::runtime_error);
		QCOMPARE(MidiClientTransferTestAccess::ports(client), std::size_t{0}); QVERIFY(client.native.empty()); client.fault.clear(); client.feed(nullptr); pump(); QCOMPARE(processor.notes(), 0);
	}

	void engineReservationsAndOwnerChecksRefuseTransfer_data()
	{
		QTest::addColumn<QString>("busy");
		for (const auto value : {"audio-running", "playing", "paused", "recording", "sample-armed", "export-reserved", "stem", "model-edit", "foreign-thread", "occupied-target", "same-client", "offline-target"})
		{ QTest::newRow(value) << QString(value); }
	}
	void engineReservationsAndOwnerChecksRefuseTransfer()
	{
		QFETCH(QString, busy); TransferClient source, target; TransferProcessor processor; MidiPort port("guard", &source, &processor); configure(port);
		const auto engine = Engine::audioEngine(); const auto song = Engine::getSong(); const auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, song));
		const auto sample = static_cast<SampleClip*>(track->createClip(0)); std::unique_ptr<TrackRenderScope> stem; std::unique_ptr<MidiPort> occupied;
		const auto release = qScopeGuard([&] { stem.reset(); sample->setRecord(false); song->stop(); if (engine->hasStoredAudioDevice()) { engine->restoreAudioDevice(); } engine->audioDev()->stopProcessing(); });
		if (busy == "audio-running") { engine->audioDev()->startProcessing(); } if (busy == "playing" || busy == "paused") { song->playSong(); if (busy == "paused") { song->togglePause(); } }
		if (busy == "recording") { song->record(); } if (busy == "sample-armed") { sample->setRecord(true); } if (busy == "export-reserved") { QVERIFY(engine->storeAudioDevice()); }
		if (busy == "stem") { stem = std::make_unique<TrackRenderScope>(track); } if (busy == "occupied-target") { occupied = std::make_unique<MidiPort>("occupied", &target, &processor); }
		if (busy == "offline-target") { target.active = false; }
		std::atomic_bool refused{false}; const auto attempt = [&] { try { MidiClientTransferTestAccess::transfer(source, busy == "same-client" ? source : target); } catch (...) { refused = true; } };
		if (busy == "foreign-thread") { std::thread worker(attempt); worker.join(); } else if (busy == "model-edit") { const auto guard = engine->requestChangesGuard(); attempt(); } else { attempt(); }
		QVERIFY(refused.load()); QCOMPARE(port.client(), static_cast<MidiClient*>(&source)); QVERIFY(!source.inputDeliveryStatus(&port).failed);
	}

	void nativeTrackAndControllerModelsKeepTheirIdentities()
	{
		TransferClient target;
		const auto songTrack = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		const auto patternTrack = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::patternStore()));
		auto controller = std::make_unique<MidiController>(nullptr);
		const auto a = songTrack->midiPort(), b = patternTrack->midiPort(), c = controller->midiPort(); configure(*a); configure(*b); c->setReadable(true); c->setInputController(74);
		const auto beforeA = settings(*a), beforeB = settings(*b), beforeC = settings(*c); QPointer<InstrumentTrack> trackIdentity(songTrack); QPointer<MidiController> controllerIdentity(controller.get());
		const auto result = MidiClientTransferTestAccess::transfer(*Engine::audioEngine()->midiClient(), target); QVERIFY(result.ports >= 3);
		QCOMPARE(songTrack->midiPort(), a); QCOMPARE(patternTrack->midiPort(), b); QCOMPARE(controller->midiPort(), c); QVERIFY(trackIdentity && controllerIdentity);
		QCOMPARE(a->client(), static_cast<MidiClient*>(&target)); QCOMPARE(b->client(), static_cast<MidiClient*>(&target)); QCOMPARE(c->client(), static_cast<MidiClient*>(&target));
		QCOMPARE(settings(*a), beforeA); QCOMPARE(settings(*b), beforeB); QCOMPARE(settings(*c), beforeC);
		controller.reset(); Engine::getSong()->clearProject();
	}

	void newlyAvailableNamedEndpointsUseTheReplacementSignals()
	{
		auto source = std::make_unique<TransferClient>(); TransferClient target; target.inputs.clear(); target.outputs.clear(); TransferProcessor processor;
		MidiPort port("refresh", source.get(), &processor); configure(port); MidiClientTransferTestAccess::transfer(*source, target); source.reset();
		QVERIFY(port.readablePorts().empty()); target.inputs = {"keyboard", "absent input"}; target.outputs = {"speaker", "absent output"}; target.refresh();
		QVERIFY(port.readablePorts().value("absent input")); QVERIFY(port.writablePorts().value("absent output")); QCOMPARE(target.reads.value(&port).size(), 2); QCOMPARE(target.writes.value(&port).size(), 2);
	}

	void emptyTransfersStillCheckBackendHealth()
	{
		TransferClient source, target; target.active = false; QVERIFY_EXCEPTION_THROWN(MidiClientTransferTestAccess::transfer(source, target), std::runtime_error);
		target.active = true; const auto result = MidiClientTransferTestAccess::transfer(source, target); QCOMPARE(result.ports, 0u); QCOMPARE(source.drains, 1);
	}

	void engineSelectionPreservesNativeModelsAndAudioState_data()
	{
		QTest::addColumn<bool>("running"); QTest::addColumn<bool>("raw");
		for (const bool running : {false, true}) { for (const bool raw : {false, true})
		{ QTest::newRow(qPrintable(QString("running-%1-raw-%2").arg(running).arg(raw))) << running << raw; } }
	}
	void engineSelectionPreservesNativeModelsAndAudioState()
	{
		QFETCH(bool, running); QFETCH(bool, raw); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(running);
		const auto songTrack = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		const auto patternTrack = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::patternStore()));
		auto controller = std::make_unique<MidiController>(nullptr); auto a = songTrack->midiPort(), b = patternTrack->midiPort(), c = controller->midiPort();
		configure(*a); configure(*b); c->setReadable(true); const auto beforeA = settings(*a), beforeB = settings(*b), beforeC = settings(*c);
		const auto configured = ConfigManager::inst()->value("audioengine", "mididev"); const auto source = engine->midiClient(); TransferClient* target = nullptr;
		const auto expectedName = AudioEngine::availableMidiClientNames().front();
		const auto result = StudioMidiBackendSelectionTestAccess::select([&] {
			auto client = std::make_unique<TransferClient>(); client->raw = raw; target = client.get(); return client;
		}, expectedName);
		QVERIFY(result.audioRestored); QVERIFY(result.ports >= 3); QVERIFY(result.inputResetsRequired >= 1); QCOMPARE(result.retirementExceptions, 0u);
		QVERIFY(engine->midiClient() != source); QCOMPARE(engine->midiClient(), static_cast<MidiClient*>(target)); QCOMPARE(engine->audioDev(), static_cast<AudioDevice*>(audio));
		QCOMPARE(audio->isRunning(), running); QCOMPARE(audio->starts, running ? 2 : 0); QCOMPARE(audio->stops, 1); QVERIFY(!engine->hasStoredAudioDevice());
		QCOMPARE(songTrack->midiPort(), a); QCOMPARE(patternTrack->midiPort(), b); QCOMPARE(controller->midiPort(), c);
		QCOMPARE(a->client(), engine->midiClient()); QCOMPARE(b->client(), engine->midiClient()); QCOMPARE(c->client(), engine->midiClient());
		QCOMPARE(settings(*a), beforeA); QCOMPARE(settings(*b), beforeB); QCOMPARE(settings(*c), beforeC);
		QCOMPARE(engine->midiClientName(), expectedName); QCOMPARE(engine->requestedMidiBackend(), expectedName);
		QCOMPARE(ConfigManager::inst()->value("audioengine", "mididev"), configured); QVERIFY(!engine->isChangingMidiBackend());
		pump(); QVERIFY(target->inputDeliveryStatus(c).failed); QString error; bool changed = false; QVERIFY2(c->resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
	}

	void enginePreparationFailureKeepsOwnershipAndRestartsAudio_data()
	{
		QTest::addColumn<bool>("running"); QTest::addColumn<QString>("fault");
		for (const bool running : {false, true}) { for (const auto fault : {"null", "throw", "offline", "mode"})
		{ QTest::newRow(qPrintable(QString("%1-%2").arg(fault).arg(running))) << running << QString(fault); } }
	}
	void enginePreparationFailureKeepsOwnershipAndRestartsAudio()
	{
		QFETCH(bool, running); QFETCH(QString, fault); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(running); const auto source = engine->midiClient();
		TransferProcessor processor; MidiPort port("owned", source, &processor, nullptr, MidiPort::Mode::Input); const auto original = settings(port);
		const auto name = engine->midiClientName(), requested = engine->requestedMidiBackend(); int destroyed = 0;
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select([&]() -> std::unique_ptr<MidiClient> {
			if (fault == "null") { return {}; } if (fault == "throw") { throw std::runtime_error("factory fixture"); }
			auto target = std::make_unique<TransferClient>(); target->onDestroy = [&] { ++destroyed; };
			if (fault == "offline") { target->active = false; } if (fault == "mode") { target->fault = "mode"; } return target;
		}), std::runtime_error);
		QCOMPARE(destroyed, fault == "null" || fault == "throw" ? 0 : 1); QCOMPARE(engine->midiClient(), source); QCOMPARE(port.client(), source);
		QCOMPARE(settings(port), original); QCOMPARE(engine->midiClientName(), name); QCOMPARE(engine->requestedMidiBackend(), requested);
		QCOMPARE(audio->isRunning(), running); QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!engine->isChangingMidiBackend());
	}

	void engineCancellationDoesNotPublishCandidate_data()
	{ QTest::addColumn<QString>("phase"); for (const auto phase : {"admission", "factory", "mode", "validate"}) { QTest::newRow(phase) << QString(phase); } }
	void engineCancellationDoesNotPublishCandidate()
	{
		QFETCH(QString, phase); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(true); const auto source = engine->midiClient();
		TransferProcessor processor; MidiPort port("cancel", source, &processor); bool cancelled = phase == "admission"; int factories = 0;
		FileResourceIO::Scope work([&] { return cancelled; }, false);
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select([&] {
			++factories; if (phase == "factory") { cancelled = true; } auto target = std::make_unique<TransferClient>();
			target->hook = [&](const QString& stage, MidiPort*) { if (stage == phase) { cancelled = true; } }; return target;
		}), std::runtime_error);
		QCOMPARE(factories, phase == "admission" ? 0 : 1); QCOMPARE(engine->midiClient(), source); QCOMPARE(port.client(), source);
		QVERIFY(audio->isRunning()); QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!engine->isChangingMidiBackend());
	}

	void engineSelectionRejectsReentryThroughoutTransition_data()
	{ QTest::addColumn<QString>("phase"); for (const auto phase : {"stop", "factory", "validate", "restart"}) { QTest::newRow(phase) << QString(phase); } }
	void engineSelectionRejectsReentryThroughoutTransition()
	{
		QFETCH(QString, phase); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(true); TransferProcessor processor;
		MidiPort port("reentry", engine->midiClient(), &processor); int checks = 0, refused = 0;
		const auto attempt = [&] {
			++checks; try { engine->selectMidiBackend(MidiDummy::name()); } catch (...) { ++refused; }
			QVERIFY(!engine->canChangeBackendGraph()); QVERIFY(!engine->storeAudioDevice());
			QVERIFY(!StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}}).value("ok").toBool());
		};
		if (phase == "stop") { audio->onStop = attempt; } if (phase == "restart") { audio->onStart = attempt; }
		const auto result = StudioMidiBackendSelectionTestAccess::select([&] {
			if (phase == "factory") { attempt(); } auto target = std::make_unique<TransferClient>();
			target->hook = [&](const QString& stage, MidiPort*) { if (phase == "validate" && stage == "validate") { attempt(); } }; return target;
		});
		QVERIFY(result.audioRestored); QVERIFY(checks > 0); QCOMPARE(refused, checks); QVERIFY(!engine->hasStoredAudioDevice());
		audio->onStop = audio->onStart = {}; static_cast<TransferClient*>(engine->midiClient())->hook = {};
	}

	void engineRestartFailureReportsWhetherMidiCommitted_data()
	{
		QTest::addColumn<bool>("committed"); QTest::addColumn<bool>("throwing");
		for (const bool committed : {false, true}) { for (const bool throwing : {false, true})
		{ QTest::newRow(qPrintable(QString("committed-%1-throw-%2").arg(committed).arg(throwing))) << committed << throwing; } }
	}
	void engineRestartFailureReportsWhetherMidiCommitted()
	{
		QFETCH(bool, committed); QFETCH(bool, throwing); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(true); const auto source = engine->midiClient();
		TransferProcessor processor; MidiPort port("restart", source, &processor); audio->throwStart = throwing; audio->failStart = !throwing;
		QString error; bool returned = false;
		try
		{
			const auto result = StudioMidiBackendSelectionTestAccess::select([&]() -> std::unique_ptr<MidiClient> {
				if (!committed) { throw std::runtime_error("prepare fixture"); } return std::make_unique<TransferClient>();
			}); returned = true; QVERIFY(!result.audioRestored);
		}
		catch (const std::exception& failure) { error = QString::fromUtf8(failure.what()); }
		QCOMPARE(returned, committed); QCOMPARE(engine->midiClient() != source, committed); QCOMPARE(port.client(), engine->midiClient());
		QVERIFY(engine->hasStoredAudioDevice()); QVERIFY(engine->isRestoringAudioDevice()); QVERIFY(!audio->isRunning());
		if (!committed) { QVERIFY(error.contains("audio.device.restore")); }
		QVERIFY_EXCEPTION_THROWN(engine->selectMidiBackend(MidiDummy::name()), std::runtime_error);
		const auto selected = engine->midiClient(); audio->throwStart = audio->failStart = false;
		QVERIFY(engine->restoreAudioDevice()); QCOMPARE(engine->midiClient(), selected); QCOMPARE(port.client(), selected); QVERIFY(audio->isRunning()); QVERIFY(!engine->hasStoredAudioDevice());
	}

	void engineFailedAudioJoinNeverStartsPreparingMidi_data()
	{
		QTest::addColumn<int>("failures"); QTest::addColumn<bool>("running");
		for (const int failures : {1, 2}) { for (const bool running : {false, true})
		{ QTest::newRow(qPrintable(QString("failures-%1-running-%2").arg(failures).arg(running))) << failures << running; } }
	}
	void engineFailedAudioJoinNeverStartsPreparingMidi()
	{
		QFETCH(int, failures); QFETCH(bool, running); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(running); const auto source = engine->midiClient();
		audio->failStops = failures; int factories = 0;
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select([&] { ++factories; return std::make_unique<TransferClient>(); }), std::runtime_error);
		QCOMPARE(factories, 0); QCOMPARE(engine->midiClient(), source); QCOMPARE(engine->hasStoredAudioDevice(), failures == 2);
		QVERIFY(engine->restoreAudioDevice()); QCOMPARE(audio->isRunning(), running); QVERIFY(!engine->hasStoredAudioDevice());
	}

	void engineAdmissionDoesNotTouchAudioOrFactory_data()
	{ QTest::addColumn<QString>("busy"); for (const auto busy : {"unknown", "model-edit", "playing", "paused", "reserved", "foreign-thread"}) { QTest::newRow(busy) << QString(busy); } }
	void engineAdmissionDoesNotTouchAudioOrFactory()
	{
		QFETCH(QString, busy); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(false); const auto source = engine->midiClient();
		if (busy == "playing" || busy == "paused") { Engine::getSong()->playSong(); } if (busy == "paused") { Engine::getSong()->togglePause(); }
		if (busy == "reserved") { QVERIFY(engine->storeAudioDevice()); } const auto clear = qScopeGuard([&] { Engine::getSong()->stop(); engine->restoreAudioDevice(); });
		int factories = 0; bool refused = false; const auto attempt = [&] {
			try { if (busy == "unknown") { engine->selectMidiBackend("not a compiled native MIDI backend"); }
				else { StudioMidiBackendSelectionTestAccess::select([&] { ++factories; return std::make_unique<TransferClient>(); }); } }
			catch (...) { refused = true; }
		};
		if (busy == "model-edit") { const auto guard = engine->requestChangesGuard(); attempt(); }
		else if (busy == "foreign-thread") { std::thread worker(attempt); worker.join(); } else { attempt(); }
		QVERIFY(refused); QCOMPARE(factories, 0); QCOMPARE(audio->stops, 0); QCOMPARE(engine->midiClient(), source);
	}

	void engineAbandonmentAdmissionRefusesLiveOrUnobservedOutput_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto& fault : {"unobserved", "active", "unsupported", "worker-running"}) { QTest::newRow(fault) << QString(fault); }
	}
	void engineAbandonmentAdmissionRefusesLiveOrUnobservedOutput()
	{
		QFETCH(QString, fault); auto engine = Engine::audioEngine(); auto source = new TransferClient;
		StudioMidiBackendSelectionTestAccess::select([&] { return std::unique_ptr<MidiClient>(source); });
		auto audio = installTransferAudio(true); TransferProcessor processor; MidiPort port("protected", source, &processor, nullptr, MidiPort::Mode::Output);
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); source->stopWithUnconfirmedOutput();
		if (fault == "unobserved") { source->outputState.observed = false; }
		if (fault == "active") { source->outputState.active = true; }
		if (fault == "unsupported") { source->outputState.abandonmentSupported = false; }
		if (fault == "worker-running") { source->outputState.abandonmentEligible = false; }
		const auto stops = audio->stops; int factories = 0;
		QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select([&] { ++factories; return std::make_unique<TransferClient>(); }, MidiDummy::name(), true), std::runtime_error);
		QCOMPARE(factories, 0); QCOMPARE(audio->stops, stops); QCOMPARE(engine->midiClient(), static_cast<MidiClient*>(source)); QCOMPARE(port.trackedOutputNotes(), 1u);
		QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(source->output.empty());
	}

	void engineFailedAbandonmentPreservesLedgersUntilCommit_data()
	{
		QTest::addColumn<QString>("fault");
		for (const auto& fault : {"factory", "registration", "cancel", "eligibility-revoked", "local-cleanup", "final-validation"}) { QTest::newRow(fault) << QString(fault); }
	}
	void engineFailedAbandonmentPreservesLedgersUntilCommit()
	{
		QFETCH(QString, fault); auto engine = Engine::audioEngine(); auto source = new TransferClient;
		StudioMidiBackendSelectionTestAccess::select([&] { return std::unique_ptr<MidiClient>(source); });
		TransferProcessor processor; MidiPort port("uncertain", source, &processor, nullptr, MidiPort::Mode::Duplex); configure(port); const auto before = settings(port);
		port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); port.processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100));
		port.processOutEvent(MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127)); source->stopWithUnconfirmedOutput();
		processor.failCleanup = fault == "local-cleanup"; bool cancelled = false; int validations = 0;
		{
			FileResourceIO::Scope work([&] { return cancelled; }, false);
			QVERIFY_EXCEPTION_THROWN(StudioMidiBackendSelectionTestAccess::select([&]() -> std::unique_ptr<MidiClient> {
				if (fault == "factory") { throw std::runtime_error("candidate factory fixture"); }
				auto target = std::make_unique<TransferClient>(); auto identity = target.get();
				if (fault == "registration") { target->fault = "add-after"; }
				target->hook = [&, identity](const QString& stage, MidiPort*) {
					if (stage == "validate" && fault == "cancel") { cancelled = true; }
					if (stage == "add-after" && fault == "eligibility-revoked") { source->outputState.abandonmentEligible = false; }
					if (stage == "validate" && ++validations == 2 && fault == "final-validation") { identity->active = false; }
				};
				return target;
			}, MidiDummy::name(), true), std::runtime_error);
		}
		QCOMPARE(engine->midiClient(), static_cast<MidiClient*>(source)); QCOMPARE(port.client(), static_cast<MidiClient*>(source));
		QCOMPARE(port.trackedOutputNotes(), 2u); QCOMPARE(settings(port), before); QVERIFY(source->output.empty()); QVERIFY(!engine->hasStoredAudioDevice());
		processor.failCleanup = false; source->outputState.abandonmentEligible = true;
		const auto retry = engine->selectMidiBackend(MidiDummy::name(), true); QVERIFY(retry.outputAbandonment.abandoned);
		QCOMPARE(retry.outputAbandonment.trackedNotes, std::uint64_t{2}); QCOMPARE(retry.outputAbandonment.pedalHolds, std::uint64_t{1}); QCOMPARE(port.trackedOutputNotes(), 0u);
	}

	void engineExplicitAbandonmentReportsLossWithoutChangingModels_data()
	{
		QTest::addColumn<bool>("targetRaw"); QTest::addColumn<bool>("restartFails");
		for (const bool raw : {false, true}) { for (const bool fail : {false, true}) { QTest::newRow(qPrintable(QString("raw-%1-restart-fail-%2").arg(raw).arg(fail))) << raw << fail; } }
	}
	void engineExplicitAbandonmentReportsLossWithoutChangingModels()
	{
		QFETCH(bool, targetRaw); QFETCH(bool, restartFails); auto engine = Engine::audioEngine(); auto source = new TransferClient;
		StudioMidiBackendSelectionTestAccess::select([&] { return std::unique_ptr<MidiClient>(source); }); auto audio = installTransferAudio(true);
		TransferProcessor processor; MidiPort a("first", source, &processor, nullptr, MidiPort::Mode::Duplex), b("second", source, &processor, nullptr, MidiPort::Mode::Output);
		configure(a); configure(b); const auto first = settings(a), second = settings(b);
		a.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); a.processOutEvent(MidiEvent(MidiNoteOn, 0, 62, 100)); b.processOutEvent(MidiEvent(MidiNoteOn, 0, 65, 100));
		a.processOutEvent(MidiEvent(MidiControlChange, 0, MidiControllerSustain, 127)); b.processOutEvent(MidiEvent(MidiControlChange, 0, MidiControllerSostenuto, 127));
		source->stopWithUnconfirmedOutput(); bool retiredWithoutSending = false; source->onDestroy = [&] { retiredWithoutSending = source->output.empty() && source->drains == 0; };
		audio->failStart = restartFails; auto target = new TransferClient; target->raw = targetRaw;
		const auto result = StudioMidiBackendSelectionTestAccess::select([&] { return std::unique_ptr<MidiClient>(target); }, MidiDummy::name(), true);
		QVERIFY(retiredWithoutSending); QCOMPARE(engine->midiClient(), static_cast<MidiClient*>(target)); QCOMPARE(a.client(), static_cast<MidiClient*>(target)); QCOMPARE(b.client(), static_cast<MidiClient*>(target));
		QVERIFY(result.outputAbandonment.abandoned); QCOMPARE(result.outputAbandonment.trackedNotes, std::uint64_t{3}); QCOMPARE(result.outputAbandonment.pedalHolds, std::uint64_t{2});
		QCOMPARE(result.outputAbandonment.backend.pendingEvents, 3u); QCOMPARE(result.outputAbandonment.backend.cleanupPendingChannels, 2u); QCOMPARE(result.outputAbandonment.backend.nativeErrorCode, -73);
		QCOMPARE(a.trackedOutputNotes(), 0u); QCOMPARE(b.trackedOutputNotes(), 0u); QCOMPARE(settings(a), first); QCOMPARE(settings(b), second);
		QVERIFY(target->inputDeliveryStatus(&a).failed); QCOMPARE(result.audioRestored, !restartFails); QCOMPARE(engine->hasStoredAudioDevice(), restartFails);
		audio->failStart = false; if (restartFails) { QVERIFY(engine->restoreAudioDevice()); }
		target->output.clear(); a.releaseOutput(); b.releaseOutput(); QVERIFY(target->output.empty());
		a.processOutEvent(MidiEvent(MidiNoteOn, 0, 67, 100)); QCOMPARE(target->output.size(), std::size_t{1}); QCOMPARE(a.trackedOutputNotes(), 1u);
	}

	void engineAbandonmentRequiresAnExplicitBooleanAndPreservesDefaultCleanup()
	{
		auto engine = Engine::audioEngine(); auto source = new TransferClient; StudioMidiBackendSelectionTestAccess::select([&] { return std::unique_ptr<MidiClient>(source); });
		TransferProcessor processor; MidiPort port("explicit", source, &processor, nullptr, MidiPort::Mode::Output); port.processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); source->stopWithUnconfirmedOutput();
		for (const auto bad : QJsonArray{1, "true", QJsonValue::Null})
		{ QVERIFY(!StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}, {"abandonFailedOutput", bad}}).value("ok").toBool()); }
		for (const auto& args : {QJsonObject{{"name", MidiDummy::name()}}, QJsonObject{{"name", MidiDummy::name()}, {"abandonFailedOutput", false}}})
		{ QVERIFY(!StudioCapabilities::execute("midi.backend.select", args).value("ok").toBool()); QCOMPARE(port.trackedOutputNotes(), 1u); QCOMPARE(engine->midiClient(), static_cast<MidiClient*>(source)); }
		const auto recovered = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}, {"abandonFailedOutput", true}});
		QVERIFY(recovered.value("ok").toBool()); const auto loss = recovered.value("result").toObject().value("outputAbandonment").toObject();
		QVERIFY(loss.value("abandoned").toBool()); QCOMPARE(loss.value("trackedNotes").toString(), QString("1")); QVERIFY(!loss.value("deviceCleanupVerified").toBool());
	}

	void engineDummySelectionCanRecoverAnIntentionalInputDiscontinuity()
	{
		auto engine = Engine::audioEngine(); TransferProcessor processor; MidiPort port("dummy", engine->midiClient(), &processor, nullptr, MidiPort::Mode::Input);
		const auto result = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}}); QVERIFY(result.value("ok").toBool());
		QVERIFY(result.value("result").toObject().value("committed").toBool()); QCOMPARE(port.client(), engine->midiClient());
		const auto status = engine->midiClient()->inputDeliveryStatus(&port); QVERIFY(status.failed); QCOMPARE(status.failure, MidiClient::InputDeliveryFailure::ClientTransfer); QVERIFY(!status.requiresBackendObservation);
		QString error; bool changed = false; QVERIFY2(port.resetInputDelivery(error, changed), qPrintable(error)); QVERIFY(changed);
	}

	void engineCommandReportsCommitDespiteLateCancellationAndRestartFailure_data()
	{ QTest::addColumn<bool>("restartFails"); QTest::newRow("restarted") << false; QTest::newRow("recovery-required") << true; }
	void engineCommandReportsCommitDespiteLateCancellationAndRestartFailure()
	{
		QFETCH(bool, restartFails); auto engine = Engine::audioEngine(); auto audio = installTransferAudio(true); const auto source = engine->midiClient(); bool cancelled = false;
		audio->onStart = [&] { cancelled = true; }; audio->failStart = restartFails;
		const auto response = StudioCapabilities::execute("midi.backend.select", {{"name", MidiDummy::name()}}, [&] { return cancelled; });
		QVERIFY(response.value("ok").toBool()); const auto result = response.value("result").toObject(); QVERIFY(result.value("committed").toBool());
		QVERIFY(cancelled); QVERIFY(engine->midiClient() != source); QCOMPARE(result.value("audioRestored").toBool(), !restartFails);
		QCOMPARE(result.value("recoveryRequired").toString(), restartFails ? QString("audio.device.restore") : QString{});
		QCOMPARE(engine->hasStoredAudioDevice(), restartFails); audio->onStart = {}; audio->failStart = false; QVERIFY(engine->restoreAudioDevice());
	}

	void engineSelectionReceiptsPreserveHistoryAndDoNotReapply_data()
	{ QTest::addColumn<bool>("restart"); QTest::newRow("same-engine") << false; QTest::newRow("restart") << true; }
	void engineSelectionReceiptsPreserveHistoryAndDoNotReapply()
	{
		QFETCH(bool, restart); QTemporaryDir directory; const auto session = directory.filePath("session"); QString error;
		auto studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error));
		QVERIFY(studio->process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
		QVERIFY(studio->process(request("tempo", "song.set", {{"tempo", 137}})).value("ok").toBool()); const auto before = manifest(session);
		const auto command = request("select", "midi.backend.select", {{"name", MidiDummy::name()}}); const auto response = studio->process(command); QVERIFY(response.value("ok").toBool());
		QCOMPARE(manifest(session).value("revision"), before.value("revision")); QCOMPARE(manifest(session).value("history"), before.value("history"));
		if (restart) { studio.reset(); Engine::destroy(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error)); }
		const auto selected = Engine::audioEngine()->midiClient(); auto replay = studio->process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, response); QCOMPARE(Engine::audioEngine()->midiClient(), selected);
		QCOMPARE(Engine::getSong()->getTempo(), 137); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
	}

	void engineScriptedAgentSelectsAndInspectsMidi_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("abandon");
		for (const auto& protocol : {"chat", "responses"}) { for (const bool abandon : {false, true}) { QTest::newRow(qPrintable(QString("%1-abandon-%2").arg(protocol).arg(abandon))) << QString(protocol) << abandon; } }
	}
	void engineScriptedAgentSelectsAndInspectsMidi()
	{
		QFETCH(QString, protocol); QFETCH(bool, abandon); QTemporaryDir directory; QString error; int round = 0;
		const auto call = [](const QString& id, const QString& name, const QJsonObject& args = QJsonObject{}) -> QJsonObject { return {{"id", id}, {"name", name}, {"args", args}}; };
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (++round == 1) { return decision(protocol, {call("select", "midi_backend_select", {{"name", MidiDummy::name()}, {"abandonFailedOutput", abandon}}), call("observe", "transport_inspect")}); }
			return decision(protocol, {call("finish", "agent_finish", {{"status", "complete"}, {"summary", abandon ? "Failed output abandoned; hardware cleanup remains unconfirmed" : "Native dummy MIDI selected and observed"}, {"evidence", QJsonArray{"select", "observe"}}})});
		});
		QVERIFY2(studio.open(error), qPrintable(error));
		TransferProcessor processor; std::unique_ptr<MidiPort> port;
		if (abandon)
		{
			auto source = new TransferClient; StudioMidiBackendSelectionTestAccess::select([&] { return std::unique_ptr<MidiClient>(source); });
			port = std::make_unique<MidiPort>("agent-output", source, &processor, nullptr, MidiPort::Mode::Output);
			port->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); source->stopWithUnconfirmedOutput();
		}
		QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-midi-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", abandon ? "Abandon the stopped failed MIDI output, select Dummy, and report the uncertain hardware cleanup." : "Select the native dummy MIDI backend and inspect its current state."}})).value("ok").toBool());
		for (int i = 0; i < 10 && StudioAgent::active(manifest(directory.path()).value("agent").toObject().value("job").toObject()); ++i) { studio.advanceAgent(); }
		QCOMPARE(manifest(directory.path()).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete"));
		QCOMPARE(round, 2); QCOMPARE(Engine::audioEngine()->midiClientName(), MidiDummy::name()); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
		if (port) { QCOMPARE(port->trackedOutputNotes(), 0u); QCOMPARE(port->client(), Engine::audioEngine()->midiClient()); }
	}

	void engineAbandonmentReceiptsRetainUncertaintyWithoutRepeatingTheSwitch_data()
	{ QTest::addColumn<bool>("restart"); QTest::newRow("same-engine") << false; QTest::newRow("restart") << true; }
	void engineAbandonmentReceiptsRetainUncertaintyWithoutRepeatingTheSwitch()
	{
		QFETCH(bool, restart); QTemporaryDir directory; const auto session = directory.filePath("session"); QString error;
		auto studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error));
		QVERIFY(studio->process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); const auto before = manifest(session);
		auto source = new TransferClient; StudioMidiBackendSelectionTestAccess::select([&] { return std::unique_ptr<MidiClient>(source); }); TransferProcessor processor;
		auto port = std::make_unique<MidiPort>("receipt-output", source, &processor, nullptr, MidiPort::Mode::Output);
		port->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); source->stopWithUnconfirmedOutput();
		const auto command = request("abandon", "midi.backend.select", {{"name", MidiDummy::name()}, {"abandonFailedOutput", true}});
		const auto response = studio->process(command); QVERIFY(response.value("ok").toBool()); const auto loss = response.value("result").toObject().value("outputAbandonment").toObject();
		QVERIFY(loss.value("abandoned").toBool()); QCOMPARE(loss.value("trackedNotes").toString(), QString("1")); QCOMPARE(loss.value("nativeErrorCode").toInt(), -73); QVERIFY(!loss.value("deviceCleanupVerified").toBool());
		QCOMPARE(manifest(session).value("revision"), before.value("revision")); QCOMPARE(manifest(session).value("history"), before.value("history")); port.reset();
		if (restart) { studio.reset(); Engine::destroy(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); studio = std::make_unique<StudioRuntime>(session); QVERIFY2(studio->open(error), qPrintable(error)); }
		const auto selected = Engine::audioEngine()->midiClient(); auto replay = studio->process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, response); QCOMPARE(Engine::audioEngine()->midiClient(), selected);
		QVERIFY(!selected->outputBackendStatus().abandonmentEligible); QVERIFY(!Engine::audioEngine()->hasStoredAudioDevice());
	}
};
QTEST_GUILESS_MAIN(MidiClientTransferTest)
#include "MidiClientTransferTest.moc"

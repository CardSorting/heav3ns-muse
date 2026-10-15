/* Native MIDI port callbacks and retained output locks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <functional>
#include <stdexcept>
#include <utility>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "MidiClient.h"
#include "MidiController.h"
#include "MidiEventProcessor.h"
#include "MidiPort.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace lmms
{
class MidiPortTestAccess
{
public:
	static AutomatableModel* field(MidiPort* port, int index)
	{ AutomatableModel* fields[] = {&port->m_inputChannelModel, &port->m_outputChannelModel, &port->m_inputControllerModel, &port->m_outputControllerModel, &port->m_fixedInputVelocityModel, &port->m_fixedOutputVelocityModel, &port->m_fixedOutputNoteModel, &port->m_outputProgramModel, &port->m_baseVelocityModel, &port->m_readableModel, &port->m_writableModel}; return fields[index]; }
	static auto lock(MidiPort* port) { return port->lockOutput(); }
};
}
namespace
{
class Processor final : public MidiEventProcessor
{
public:
	void processInEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
	void processOutEvent(const MidiEvent&, const TimePos&, f_cnt_t) override { }
};
class Client final : public QObject, public MidiClient
{
	Q_OBJECT
public:
	std::function<void()> onMode, onName, onInput, onOutput, onCancel, onSend;
	mutable std::function<void()> onInputList, onOutputList;
	QStringList inputNames{"A", "Keyboard, alpha"}, outputNames{"B", "Synth, beta"};
	QString lastInput, lastOutput;
	std::vector<MidiEvent> sent;
	std::uint16_t retained = 0;
	void applyPortMode(MidiPort*) override { const auto callback = onMode; if (callback) { callback(); } }
	void applyPortName(MidiPort*) override { const auto callback = onName; if (callback) { callback(); } }
	QStringList readablePorts() const override { const auto callback = onInputList; if (callback) { callback(); } return inputNames; }
	QStringList writablePorts() const override { const auto callback = onOutputList; if (callback) { callback(); } return outputNames; }
	void subscribeReadablePort(MidiPort*, const QString& name, bool) override { lastInput = name; const auto callback = onInput; if (callback) { callback(); } }
	void subscribeWritablePort(MidiPort*, const QString& name, bool) override { lastOutput = name; const auto callback = onOutput; if (callback) { callback(); } }
	void cancelOutputEvents(const MidiPort*) override { const auto callback = onCancel; if (callback) { callback(); } }
	void retainFailedOutputCleanup(const MidiPort*, std::uint16_t channels) noexcept override { retained |= channels; }
	void processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort*) override { sent.push_back(event); const auto callback = onSend; if (callback) { callback(); } }
	void connectRPChanged(QObject* receiver, const char* slot) override { connect(this, SIGNAL(inputsChanged()), receiver, slot); }
	void connectWPChanged(QObject* receiver, const char* slot) override { connect(this, SIGNAL(outputsChanged()), receiver, slot); }
	void refreshInput() { emit inputsChanged(); }
	void refreshOutput() { emit outputsChanged(); }
signals:
	void inputsChanged();
	void outputsChanged();
};
const char* keys[] = {"inputchannel", "outputchannel", "inputcontroller", "outputcontroller", "fixedinputvelocity", "fixedoutputvelocity", "fixedoutputnote", "outputprogram", "basevelocity", "readable", "writable"};
QDomElement settings(QDomDocument& document)
{
	auto node = document.createElement("midiport"); document.appendChild(node); const int values[] = {5, 2, 10, 11, 42, 43, 64, 2, 70, 1, 1};
	for (int index = 0; index < 11; ++index) { node.setAttribute(keys[index], values[index]); } return node;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
[[noreturn]] void raise(bool nonstandard) { if (nonstandard) { throw 17; } throw std::runtime_error("fixture native MIDI port failure"); }
}
class MidiPortLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	static void fields() { QTest::addColumn<int>("field"); for (int index = 0; index < 11; ++index) { QTest::newRow(keys[index]) << index; } }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void exactNamedAndUnavailableSubscriptionsRoundTrip()
	{
		Client client; Processor processor; MidiPort original("fixture", &client, &processor); original.subscribeReadablePort("Keyboard, alpha"); original.subscribeReadablePort("offline input"); original.subscribeWritablePort("Synth, beta"); original.subscribeWritablePort("offline output"); original.setOutputSource(MidiPort::OutputSource::Instrument);
		QDomDocument document; auto node = document.createElement("midiport"); document.appendChild(node); original.saveSettings(document, node); MidiPort restored("restored", &client, &processor); restored.loadSettings(node);
		QCOMPARE(restored.requestedReadablePorts(), original.requestedReadablePorts()); QCOMPARE(restored.requestedWritablePorts(), original.requestedWritablePorts()); QCOMPARE(restored.outputSource(), MidiPort::OutputSource::Instrument); QVERIFY(restored.readablePorts().value("Keyboard, alpha")); QVERIFY(restored.writablePorts().value("Synth, beta")); QVERIFY(!restored.readablePorts().contains("offline input")); QVERIFY(!restored.writablePorts().contains("offline output"));
	}
	void settingsLoadFieldNotificationsCanRetireThePort_data() { fields(); }
	void settingsLoadFieldNotificationsCanRetireThePort()
	{
		QFETCH(int, field); Client client; Processor processor; auto target = new MidiPort("fixture", &client, &processor); const QPointer<MidiPort> owner(target); bool invoked = false; connect(MidiPortTestAccess::field(target, field), &AutomatableModel::dataChanged, this, [&] { if (!invoked) { invoked = true; delete target; } }); QDomDocument document; const auto node = settings(document);
		QVERIFY_EXCEPTION_THROWN(target->loadSettings(node), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void settingsSaveFieldWorkCanRetireThePort_data() { fields(); }
	void settingsSaveFieldWorkCanRetireThePort()
	{
		QFETCH(int, field); Client client; Processor processor; auto target = new MidiPort("fixture", &client, &processor); const QPointer<MidiPort> owner(target); QDomDocument document; auto node = document.createElement("midiport"); document.appendChild(node); bool invoked = false;
		{ FileResourceIO::Scope work([&] { if (!invoked && node.hasAttribute(keys[field])) { invoked = true; delete target; } return false; }); QVERIFY_EXCEPTION_THROWN(target->saveSettings(document, node), std::runtime_error); }
		QVERIFY(invoked); QVERIFY(!owner);
	}
	void modeNotificationsStopAfterReceiverRetirement_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("readable") << 0; QTest::newRow("writable") << 1; QTest::newRow("mode") << 2; }
	void modeNotificationsStopAfterReceiverRetirement()
	{
		QFETCH(int, phase); Client client; Processor processor; auto target = new MidiPort("fixture", &client, &processor); const QPointer<MidiPort> owner(target); bool invoked = false; const auto retire = [&] { if (!invoked) { invoked = true; delete target; } };
		if (phase == 0) { connect(target, &MidiPort::readablePortsChanged, this, retire); } if (phase == 1) { connect(target, &MidiPort::writablePortsChanged, this, retire); } if (phase == 2) { connect(target, &MidiPort::modeChanged, this, retire); }
		QVERIFY_EXCEPTION_THROWN(target->setReadable(true), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void returnedBackendCallbacksCanRetireThePort_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("mode") << 0; QTest::newRow("name") << 1; QTest::newRow("input-subscription") << 2; QTest::newRow("output-subscription") << 3; QTest::newRow("input-discovery") << 4; QTest::newRow("output-discovery") << 5; QTest::newRow("note-submission") << 6; QTest::newRow("input-refresh-subscription") << 7; QTest::newRow("output-refresh-subscription") << 8; }
	void returnedBackendCallbacksCanRetireThePort()
	{
		QFETCH(int, phase); Client client; Processor processor; auto target = new MidiPort("fixture", &client, &processor); const QPointer<MidiPort> owner(target); if (phase == 6) { target->setWritable(true); } bool invoked = false; const auto retire = [&] { if (!invoked) { invoked = true; delete target; } };
		if (phase == 7) { target->subscribeReadablePort("A"); } if (phase == 8) { target->subscribeWritablePort("B"); }
		if (phase == 0) { client.onMode = retire; } if (phase == 1) { client.onName = retire; } if (phase == 2) { client.onInput = retire; } if (phase == 3) { client.onOutput = retire; } if (phase == 4) { client.onInputList = retire; } if (phase == 5) { client.onOutputList = retire; } if (phase == 6) { client.onSend = retire; }
		if (phase == 7) { client.onInput = retire; } if (phase == 8) { client.onOutput = retire; }
		bool caught = false; try { if (phase == 0) { target->setMode(MidiPort::Mode::Input); } if (phase == 1) { target->setName("changed"); } if (phase == 2) { target->subscribeReadablePort("A"); } if (phase == 3) { target->subscribeWritablePort("B"); } if (phase == 4 || phase == 7) { client.refreshInput(); } if (phase == 5 || phase == 8) { client.refreshOutput(); } if (phase == 6) { target->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); } } catch (const std::runtime_error&) { caught = true; }
		QVERIFY(caught); QVERIFY(invoked); QVERIFY(!owner);
	}
	void releaseRetirementRetainsUncertainCleanupWithoutUnlockingFreedStorage()
	{
		Client client; Processor processor; auto target = new MidiPort("fixture", &client, &processor); target->setWritable(true); target->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); QCOMPARE(target->trackedOutputNotes(), 1u); const QPointer<MidiPort> owner(target); bool invoked = false; client.onCancel = [&] { if (!invoked) { invoked = true; delete target; } };
		QVERIFY_EXCEPTION_THROWN(target->releaseOutput(), std::runtime_error); QVERIFY(!owner); QVERIFY(invoked); QVERIFY(client.retained & 1u);
	}
	void retainedOutputLocksMoveAndUnlockAfterBothPortsRetire()
	{
		Client client; Processor processor; auto first = new MidiPort("first", &client, &processor); auto second = new MidiPort("second", &client, &processor); auto one = MidiPortTestAccess::lock(first); auto two = MidiPortTestAccess::lock(second); delete first; delete second;
		one = std::move(two); QVERIFY(one.owns_lock()); QVERIFY(!two.owns_lock()); one.unlock(); QVERIFY(!one.owns_lock());
	}
	void nestedModeAndReleaseTransitionsAreRefused()
	{
		Client client; Processor processor; MidiPort target("fixture", &client, &processor); bool mode = false; connect(&target, &MidiPort::modeChanged, this, [&] { if (!mode) { mode = true; QVERIFY_EXCEPTION_THROWN(target.setMode(MidiPort::Mode::Disabled), std::logic_error); QVERIFY_EXCEPTION_THROWN(target.updateMidiPortMode(), std::logic_error); } }); target.setReadable(true); QVERIFY(mode); QVERIFY(target.isInputEnabled());
		client.onCancel = [&] { QVERIFY_EXCEPTION_THROWN(target.releaseOutput(), std::logic_error); target.processOutEvent(MidiEvent(MidiNoteOn, 0, 61, 100)); }; target.setWritable(true); target.releaseOutput(); QCOMPARE(target.trackedOutputNotes(), 0u); client.onCancel = {};
	}
	void backendExceptionsReleaseAdmissionAndKeepModeModelsConsistent_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void backendExceptionsReleaseAdmissionAndKeepModeModelsConsistent()
	{
		QFETCH(bool, nonstandard); Client client; Processor processor; MidiPort target("fixture", &client, &processor); client.onMode = [&] { raise(nonstandard); }; bool caught = false; try { target.setReadable(true); } catch (...) { caught = true; } QVERIFY(caught); QCOMPARE(target.isReadable(), target.isInputEnabled()); QCOMPARE(target.isWritable(), target.isOutputEnabled()); client.onMode = {}; target.setReadable(false); target.updateMidiPortMode(); QCOMPARE(target.mode(), MidiPort::Mode::Disabled);
	}
	void borrowedEndpointNameIsCopiedBeforeModeNotification()
	{
		Client client; Processor processor; MidiPort target("fixture", &client, &processor); QString name = "A"; connect(&target, &MidiPort::modeChanged, this, [&] { name = "changed by callback"; }); target.subscribeReadablePort(name);
		QCOMPARE(client.lastInput, QString("A")); QVERIFY(target.requestedReadablePorts().contains("A")); QVERIFY(name != "A");
	}
	void retargetRollbackDoesNotTouchADeletedModel()
	{
		Client client; Processor processor; auto target = new MidiPort("fixture", &client, &processor); target->setWritable(true); target->processOutEvent(MidiEvent(MidiNoteOn, 0, 60, 100)); const QPointer<MidiPort> owner(target); bool invoked = false; client.onCancel = [&] { if (!invoked) { invoked = true; delete target; } };
		QVERIFY_EXCEPTION_THROWN(target->setOutputChannel(2), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner); QVERIFY(client.retained & 1u);
	}
	void settingsReentryIsRefusedAcrossModeAndCooperativeCallbacks()
	{
		Client client; Processor processor; MidiPort target("fixture", &client, &processor); QDomDocument document; const auto node = settings(document); bool invoked = false;
		connect(&target, &MidiPort::modeChanged, this, [&] { if (!invoked) { invoked = true; QVERIFY_EXCEPTION_THROWN(target.loadSettings(node), std::logic_error); QDomDocument saved; auto element = saved.createElement("midiport"); QVERIFY_EXCEPTION_THROWN(target.saveSettings(saved, element), std::logic_error); } }); target.loadSettings(node); QVERIFY(invoked); QDomDocument saved; auto element = saved.createElement("midiport"); target.saveSettings(saved, element);
	}
	void legacyMissingBaseVelocityRetainsNativeMaximum()
	{
		Client client; Processor processor; MidiPort target("fixture", &client, &processor); QDomDocument document; auto node = settings(document); node.removeAttribute("basevelocity"); target.loadSettings(node); QCOMPARE(target.baseVelocity(), MidiMaxVelocity);
	}
	void actualMidiControllerRestorationStopsAtItsRetiredPort()
	{
		auto target = new MidiController(nullptr); const QPointer<MidiController> owner(target); bool invoked = false; connect(target->midiPort(), &MidiPort::modeChanged, this, [&] { if (!invoked) { invoked = true; delete target; } }); QDomDocument document; auto node = settings(document); node.setAttribute("type", static_cast<int>(Controller::ControllerType::Midi));
		QVERIFY_EXCEPTION_THROWN(target->loadSettings(node), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void cancelledSettingsReleaseAdmissionForNativeRetry_data()
	{ QTest::addColumn<bool>("late"); QTest::newRow("before-fields") << false; QTest::newRow("after-input") << true; }
	void cancelledSettingsReleaseAdmissionForNativeRetry()
	{
		QFETCH(bool, late); Client client; Processor processor; MidiPort target("fixture", &client, &processor); QDomDocument document; const auto node = settings(document); bool cancelled = false;
		{ FileResourceIO::Scope work([&] { cancelled = !late || target.inputChannel() == 5; return cancelled; }); QVERIFY_EXCEPTION_THROWN(target.loadSettings(node), std::runtime_error); }
		QVERIFY(cancelled); target.loadSettings(node); QCOMPARE(target.inputChannel(), 5);
	}
	void runtimeRecoversMidiPortLoadFaultAndReplaysIt_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void runtimeRecoversMidiPortLoadFaultAndReplaysIt()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; QTemporaryDir input; const auto path = input.filePath("incoming.mmp"); DataFile file(DataFile::Type::SongProject); auto rack = file.createElement("controllers"); auto node = file.createElement("Midicontroller"); node.setAttribute("type", static_cast<int>(Controller::ControllerType::Midi)); node.setAttribute("name", "Incoming MIDI"); rack.appendChild(node); file.content().appendChild(rack); QVERIFY(file.writeFile(path)); const auto command = request("load", "project.load", {{"path", path}}); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "controller.add", {{"type", "lfo"}, {"name", "Keep me"}})).value("ok").toBool()); bool attached = false, invoked = false;
			{ FileResourceIO::Scope work([&] { if (!attached && Engine::getSong()->isLoadingProject()) { const auto target = Engine::getSong()->findChild<MidiController*>(); if (target) { attached = true; connect(target->midiPort(), &MidiPort::modeChanged, this, [&] { if (!invoked) { invoked = true; raise(nonstandard); } }); } } return false; }); receipt = runtime.process(command); }
			QVERIFY(attached); QVERIFY(invoked); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->controllers().front()->name(), QString("Keep me")); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); }
		StudioRuntime reopened(directory.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QCOMPARE(Engine::getSong()->controllers().front()->name(), QString("Keep me"));
	}
};
QTEST_GUILESS_MAIN(MidiPortLifecycleTest)
#include "MidiPortLifecycleTest.moc"

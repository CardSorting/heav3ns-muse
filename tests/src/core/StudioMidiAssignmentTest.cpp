/* Checked native parameter-owned MIDI controller assignment.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "MidiClient.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
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
class AssignmentInput final : public MidiClient
{
public:
	std::vector<QPointer<MidiPort>> ports;
	std::function<void(MidiPort*)> onAdd, onMode, onRemove;
	bool isRaw() const override { return true; }
	void processOutEvent(const MidiEvent&, const TimePos&, const MidiPort*) override { }
	void addPort(MidiPort* port) override { MidiClient::addPort(port); ports.emplace_back(port); if (onAdd) { onAdd(port); } }
	void removePort(MidiPort* port) override
	{ MidiClient::removePort(port); std::erase_if(ports, [port](const auto& entry) { return !entry || entry == port; }); if (onRemove) { onRemove(port); } }
	void applyPortMode(MidiPort* port) override { if (onMode) { onMode(port); } }
	std::size_t count() const { return std::count_if(ports.begin(), ports.end(), [](const auto& port) { return !port.isNull(); }); }
	void reset() { onAdd = {}; onMode = {}; onRemove = {}; }
};
class AssignmentParameter final : public FloatModel
{
public:
	AssignmentParameter() : FloatModel(42, 1, 100, 0, nullptr) { }
	bool supported = true;
	bool supportsAutomation() const override { return supported; }
};
}
class StudioMidiAssignmentTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	AssignmentInput input;
	MidiClient* oldClient = nullptr;
	QPointer<InstrumentTrack> instrument;
	bool journalling = false;
	FloatModel* model() const { return instrument->volumeModel(); }
	MidiController* midi() const { return model()->controllerConnection() ? dynamic_cast<MidiController*>(model()->controllerConnection()->getController()) : nullptr; }
	void seed() { instrument = new InstrumentTrack(Engine::getSong()); model()->setValue(42); }
	Controller* bind(int kind)
	{
		if (kind == 0) { return nullptr; }
		Controller* native = kind == 1 ? static_cast<Controller*>(new LfoController(Engine::getSong())) : static_cast<Controller*>(new MidiController(nullptr));
		if (kind == 1) { Engine::getSong()->addController(native); }
		model()->setControllerConnection(new ControllerConnection(native)); return native;
	}
	QJsonObject args() const { return {{"parameter", int(model()->id())}, {"inputChannel", 16}, {"inputController", 74}, {"readable", true}}; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& a = {}) const { return {{"id", id}, {"op", op}, {"args", a}}; }
	void refresh() { instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); }
	void replay(StudioRuntime& runtime, const QJsonObject& command, const QJsonObject& receipt)
	{ auto response = runtime.process(command); QVERIFY(response.take("replayed").toBool()); QCOMPARE(response, receipt); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->getSaveOptions().setDefaultOptions();
		input.reset(); seed(); oldClient = StudioMidiLearnTestAccess::exchange(&input);
	}
	void cleanup()
	{
		input.reset(); Engine::getSong()->stop(); Engine::getSong()->clearProject(); StudioMidiLearnTestAccess::exchange(oldClient); oldClient = nullptr;
		QCOMPARE(input.count(), std::size_t(0)); Engine::getSong()->getSaveOptions().setDefaultOptions();
	}
	void oneFinalObserverSeesCompleteAssignmentAndRetiredPrevious_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("unbound") << 0; QTest::newRow("rack") << 1; QTest::newRow("owned-midi") << 2; }
	void oneFinalObserverSeesCompleteAssignmentAndRetiredPrevious()
	{
		QFETCH(int, kind); const QPointer<Controller> oldNative(bind(kind)); const QPointer<ControllerConnection> previous(model()->controllerConnection());
		model()->setUseControllerValue(false); int notifications = 0; const auto connection = connect(model(), &Model::dataChanged, this, [&] {
			++notifications; QVERIFY(midi()); QVERIFY(!previous); QVERIFY(model()->useControllerValue()); QCOMPARE(model()->storedValue(), 42.f);
			QCOMPARE(midi()->midiPort()->inputChannel(), 16); QCOMPARE(midi()->midiPort()->inputController(), 74); QVERIFY(midi()->midiPort()->isReadable());
			QCOMPARE(midi()->parent(), static_cast<QObject*>(Engine::getSong())); QCOMPARE(midi()->connectionCount(), 1); if (kind == 2) { QVERIFY(!oldNative); }
			if (kind == 1) { QVERIFY(oldNative); QCOMPARE(oldNative->connectionCount(), 0); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY2(model()->connectMidiController(16, 74, true, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications, 1); QCOMPARE(input.count(), std::size_t(1));
		QCOMPARE(Engine::getSong()->controllers().size(), kind == 1 ? std::size_t(1) : std::size_t(0));
	}
	void exactNativeChannelControllerAndReadableSettings_data()
	{
		QTest::addColumn<int>("channel"); QTest::addColumn<int>("control"); QTest::addColumn<bool>("readable");
		QTest::newRow("omni-cc0") << 0 << 0 << true; QTest::newRow("channel16-cc127") << 16 << 127 << true; QTest::newRow("disabled") << 3 << 74 << false;
	}
	void exactNativeChannelControllerAndReadableSettings()
	{
		QFETCH(int, channel); QFETCH(int, control); QFETCH(bool, readable); bool changed = false; QString error;
		QVERIFY(model()->connectMidiController(channel, control, readable, changed, error)); QVERIFY(changed); QVERIFY(midi());
		QCOMPARE(midi()->midiPort()->inputChannel(), channel); QCOMPARE(midi()->midiPort()->inputController(), control); QCOMPARE(midi()->midiPort()->isReadable(), readable);
		QCOMPARE(midi()->midiPort()->mode(), readable ? MidiPort::Mode::Input : MidiPort::Mode::Disabled); QCOMPARE(midi()->name(), QString("MIDI ch%1 ctrl%2").arg(channel).arg(control));
	}
	void repeatedIdenticalAssignmentRetiresTheOldOwnedPair()
	{
		bool changed = false; QString error; QVERIFY(model()->connectMidiController(0, 74, true, changed, error));
		const QPointer<ControllerConnection> previous(model()->controllerConnection()); const QPointer<MidiController> native(midi());
		QVERIFY(model()->connectMidiController(0, 74, true, changed, error)); QVERIFY(changed); QVERIFY(!previous); QVERIFY(!native); QCOMPARE(input.count(), std::size_t(1));
	}
	void invalidInputsRefuseBeforeCreatingAPort_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("negative-channel") << 0; QTest::newRow("past-channel") << 1; QTest::newRow("negative-cc") << 2; QTest::newRow("past-cc") << 3; QTest::newRow("unsupported") << 4; QTest::newRow("decibel") << 5; }
	void invalidInputsRefuseBeforeCreatingAPort()
	{
		QFETCH(int, failure); AssignmentParameter parameter; parameter.supported = failure != 4; if (failure == 5) { parameter.setScaleType(AutomatableModel::ScaleType::Decibel); }
		bool changed = true; QString error; QVERIFY(!parameter.connectMidiController(failure == 0 ? -1 : failure == 1 ? 17 : 0, failure == 2 ? -1 : failure == 3 ? 128 : 74, true, changed, error));
		QVERIFY(!changed); QVERIFY(!parameter.controllerConnection()); QCOMPARE(input.count(), std::size_t(0));
	}
	void preparationCallbacksCannotOverwriteChangedOrRetiredSource_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("value") << 0; QTest::newRow("dimension") << 1; QTest::newRow("retire") << 2; QTest::newRow("throw") << 3; QTest::newRow("backend") << 4; }
	void preparationCallbacksCannotOverwriteChangedOrRetiredSource()
	{
		QFETCH(int, failure); bind(2); const QPointer<AutomatableModel> receiver(model()); const QPointer<ControllerConnection> previous(model()->controllerConnection()); bool invoked = false;
		input.onAdd = [&](MidiPort*) { if (invoked) { return; } invoked = true;
			if (failure == 0) { receiver->setValue(60); } if (failure == 1) { receiver->setStep(0.5f); }
			if (failure == 2) { delete instrument.data(); } if (failure == 3) { throw std::runtime_error("fixture MIDI assignment construction"); }
			if (failure == 4) { StudioMidiLearnTestAccess::exchange(oldClient); }
		};
		const auto restoreClient = qScopeGuard([&] { if (Engine::audioEngine()->midiClient() != &input) { StudioMidiLearnTestAccess::exchange(&input); } });
		bool changed = true; QString error; QVERIFY(!receiver->connectMidiController(16, 74, true, changed, error)); QVERIFY(invoked); QVERIFY(!changed);
		if (receiver) { QCOMPARE(receiver->controllerConnection(), previous.data()); QCOMPARE(input.count(), std::size_t(1)); } else { QCOMPARE(input.count(), std::size_t(0)); }
	}
	void preparedPortInterferenceRefusesBeforePublication()
	{
		bind(2); const QPointer<ControllerConnection> previous(model()->controllerConnection()); bool invoked = false;
		input.onMode = [&](MidiPort* port) { if (!invoked && port->inputController() == 74) { invoked = true; port->setInputChannel(5); } };
		bool changed = true; QString error; QVERIFY(!model()->connectMidiController(16, 74, false, changed, error)); QVERIFY(invoked); QVERIFY(!changed);
		QCOMPARE(model()->controllerConnection(), previous.data()); QCOMPARE(input.count(), std::size_t(1));
	}
	void oldResourceRetirementSeesThePublishedActiveReplacement()
	{
		bind(2); bool invoked = false; const auto connection = connect(model()->controllerConnection(), &QObject::destroyed, this, [&] {
			invoked = true; QVERIFY(midi()); QCOMPARE(midi()->midiPort()->inputController(), 74); QVERIFY(model()->useControllerValue());
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY(model()->connectMidiController(16, 74, true, changed, error)); QVERIFY(invoked); QVERIFY(changed);
	}
	void retiringOldMidiResourcesMayRetireTheParameter()
	{
		bind(2); const QPointer<AutomatableModel> receiver(model()); const QPointer<MidiPort> previous(midi()->midiPort()); bool invoked = false;
		input.onRemove = [&](MidiPort* port) { if (!invoked && port == previous.data()) { invoked = true; delete instrument.data(); } };
		bool changed = true; QString error; QVERIFY(!receiver->connectMidiController(16, 74, true, changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!receiver); QCOMPARE(input.count(), std::size_t(0));
	}
	void finalCallbackFaultsAreDetectedWithoutDuplicateOwnership_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("value") << 1; QTest::newRow("port") << 2; QTest::newRow("retire-model") << 3; QTest::newRow("retire-midi") << 4; QTest::newRow("replace-binding") << 5; }
	void finalCallbackFaultsAreDetectedWithoutDuplicateOwnership()
	{
		QFETCH(int, failure); bind(2); const QPointer<AutomatableModel> receiver(model()); const QPointer<ControllerConnection> previous(model()->controllerConnection());
		ModelValueNotificationProbe fault(receiver.data(), [&] { if (failure == 0) { throw std::runtime_error("fixture MIDI assignment notification"); } });
		QPointer<ControllerConnection> candidate; bool invoked = false; const auto connection = connect(receiver.data(), &Model::dataChanged, this, [&] {
			if (invoked) { return; } invoked = true; candidate = receiver->controllerConnection();
			if (failure == 1) { receiver->setValue(60); }
			if (failure == 2) { midi()->midiPort()->setInputController(71); } if (failure == 3) { delete instrument.data(); }
			if (failure == 4) { delete midi(); } if (failure == 5) { receiver->setControllerConnection(new ControllerConnection(nullptr)); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!receiver->connectMidiController(16, 74, true, changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!previous); QVERIFY(!error.isEmpty());
		if (failure >= 3) { QVERIFY(!candidate); } if (receiver) { QVERIFY(!receiver->signalsBlocked()); }
	}
	void existingSignalBlocksSurviveAssignment_data()
	{ QTest::addColumn<bool>("blocked"); QTest::newRow("blocked") << true; QTest::newRow("unblocked") << false; }
	void existingSignalBlocksSurviveAssignment()
	{
		QFETCH(bool, blocked); model()->blockSignals(blocked); QSignalSpy notifications(model(), &Model::dataChanged); bool changed = false; QString error;
		QVERIFY(model()->connectMidiController(0, 74, true, changed, error)); QVERIFY(changed); QCOMPARE(model()->signalsBlocked(), blocked); QCOMPARE(notifications.count(), blocked ? 0 : 1);
	}
	void linkedPeerValuesFlagsAndBindingsRemainNative()
	{
		FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer)); auto rack = new LfoController(Engine::getSong()); Engine::getSong()->addController(rack);
		peer.setControllerConnection(new ControllerConnection(rack)); peer.setUseControllerValue(false); const QPointer<ControllerConnection> remaining(peer.controllerConnection());
		bool changed = false; QString error; QVERIFY(model()->connectMidiController(0, 74, true, changed, error)); QVERIFY(changed);
		QVERIFY(model()->isLinkedToModel(&peer)); QCOMPARE(peer.storedValue(), 42.f); QVERIFY(!peer.useControllerValue()); QCOMPARE(peer.controllerConnection(), remaining.data()); QCOMPARE(rack->connectionCount(), 1);
	}
	void cancellationKeepsTheOriginalBindingAndCleansPreparedPorts_data()
	{ QTest::addColumn<bool>("prepared"); QTest::newRow("before-construction") << false; QTest::newRow("after-preparation") << true; }
	void cancellationKeepsTheOriginalBindingAndCleansPreparedPorts()
	{
		QFETCH(bool, prepared); bind(2); const QPointer<ControllerConnection> previous(model()->controllerConnection()); const auto ports = input.count(); bool timer = false; QObject owner;
		QTimer::singleShot(0, &owner, [&] { timer = true; }); FileResourceIO::Scope work([&] { return !prepared || input.count() > ports; }, false);
		bool changed = true; QString error; QVERIFY(!model()->connectMidiController(16, 74, true, changed, error)); QVERIFY(!changed); QVERIFY(!timer); QCOMPARE(model()->controllerConnection(), previous.data()); QCOMPARE(input.count(), ports);
	}
	void finalCallbacksCannotChangeLinkedGroupState_data()
	{ QTest::addColumn<bool>("unlink"); QTest::newRow("peer-value") << false; QTest::newRow("peer-unlink") << true; }
	void finalCallbacksCannotChangeLinkedGroupState()
	{
		QFETCH(bool, unlink); FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer)); bool invoked = false;
		const auto connection = connect(model(), &Model::dataChanged, this, [&] { if (!invoked) { invoked = true; if (unlink) { peer.unlink(); } else { peer.setValue(60); } } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!model()->connectMidiController(16, 74, true, changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!model()->signalsBlocked());
	}
	void assignmentPreservesNativeLinearAndLogarithmicDimensions_data()
	{ QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true; }
	void assignmentPreservesNativeLinearAndLogarithmicDimensions()
	{
		QFETCH(bool, logarithmic); AssignmentParameter parameter; parameter.setScaleLogarithmic(logarithmic); bool changed = false; QString error;
		QVERIFY2(parameter.connectMidiController(0, 74, true, changed, error), qPrintable(error)); QVERIFY(changed);
		QCOMPARE(parameter.storedValue(), 42.f); QCOMPARE(parameter.isScaleLogarithmic(), logarithmic); QCOMPARE(parameter.minValue(), 1.f); QCOMPARE(parameter.maxValue(), 100.f);
	}
	void nativeInputStillFiltersThroughTheAssignedMidiController()
	{
		bool changed = false; QString error; QVERIFY(model()->connectMidiController(16, 74, true, changed, error));
		midi()->processInEvent(MidiEvent(MidiControlChange, 14, 74, 127), {}); Controller::triggerFrameCounter(); midi()->currentValue(0); Controller::triggerFrameCounter(); QCOMPARE(model()->value(), 0.f);
		midi()->processInEvent(MidiEvent(MidiControlChange, 15, 74, 127), {}); Controller::triggerFrameCounter(); midi()->currentValue(0); Controller::triggerFrameCounter(); QCOMPARE(model()->value(), model()->maxValue()); QCOMPARE(model()->storedValue(), 42.f);
	}
	void structuredAssignmentReportsPreparedNativeOwnership()
	{
		const auto receipt = StudioCapabilities::execute("controller.midi.connect", args()); QVERIFY(receipt.value("ok").toBool());
		QVERIFY(receipt.value("result").toObject().value("changed").toBool()); QCOMPARE(receipt.value("result").toObject().value("ownership").toString(), QString("parameter connection"));
		QVERIFY(midi()); QCOMPARE(midi()->midiPort()->inputController(), 74); QCOMPARE(model()->storedValue(), 42.f);
	}
	void nativeSongAncestryKeepsMidiPortControlsDiscoverable()
	{
		bool changed = false; QString error; QVERIFY(model()->connectMidiController(16, 74, true, changed, error));
		const auto controls = midi()->midiPort()->findChildren<AutomatableModel*>(); QVERIFY(!controls.empty());
		for (const auto control : controls)
		{
			const auto receipt = StudioCapabilities::execute("parameter.inspect", {{"parameter", int(control->id())}});
			QVERIFY2(receipt.value("ok").toBool(), qPrintable(receipt.value("error").toString()));
		}
	}
	void sessionUndoRedoReopenAndReplay()
	{
		QTemporaryDir directory; QString error; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); bind(2); midi()->midiPort()->setInputController(71); command = request("assign", "controller.midi.connect", args());
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); replay(runtime, command, receipt);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); refresh(); QVERIFY(midi()); QCOMPARE(midi()->midiPort()->inputController(), 71);
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt); refresh(); QVERIFY(midi()); QCOMPARE(midi()->midiPort()->inputController(), 74); QCOMPARE(model()->storedValue(), 42.f);
	}
	void sessionRecoversFinalFailureAndRetirement_data()
	{ QTest::addColumn<bool>("retire"); QTest::newRow("throw") << false; QTest::newRow("retire") << true; }
	void sessionRecoversFinalFailureAndRetirement()
	{
		QFETCH(bool, retire); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); bind(2); midi()->midiPort()->setInputController(71);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); const auto command = request("reject", "controller.midi.connect", args());
		ModelValueNotificationProbe fault(model(), [&] { if (!retire) { throw std::runtime_error("fixture session MIDI assignment"); } });
		const auto connection = connect(model(), &Model::dataChanged, this, [&] { if (retire) { delete instrument.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto receipt = runtime.process(command); QVERIFY(!receipt.value("ok").toBool()); replay(runtime, command, receipt);
		refresh(); QVERIFY(midi()); QCOMPARE(midi()->midiPort()->inputController(), 71); QCOMPARE(model()->storedValue(), 42.f); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioMidiAssignmentTest)
#include "StudioMidiAssignmentTest.moc"

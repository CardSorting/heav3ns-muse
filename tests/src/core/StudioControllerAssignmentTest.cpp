/* Checked shared-controller assignment and native in-place rewiring.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <limits>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class CountedController final : public LfoController
{
public:
	CountedController() : LfoController(Engine::getSong()) { }
	void setCount(int count) { m_connectionCount = count; }
};
class RackParameter final : public FloatModel
{
public:
	RackParameter() : FloatModel(42, 1, 100, 0, nullptr) { }
	bool supported = true;
	bool supportsAutomation() const override { return supported; }
};
}
class StudioControllerAssignmentTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QPointer<InstrumentTrack> instrument;
	bool journalling = false;
	FloatModel* model() const { return instrument->volumeModel(); }
	void seed() { instrument = new InstrumentTrack(Engine::getSong()); model()->setValue(42); }
	LfoController* rack(const QString& name = "Target")
	{ auto native = new LfoController(Engine::getSong()); native->setName(name); Engine::getSong()->addController(native); return native; }
	Controller* bind(int kind)
	{
		if (!kind) { return nullptr; } Controller* native = kind == 1 ? static_cast<Controller*>(rack("Previous"))
			: kind == 2 ? static_cast<Controller*>(new MidiController(Engine::getSong())) : nullptr;
		model()->setControllerConnection(new ControllerConnection(native)); return model()->controllerConnection()->getController();
	}
	QJsonObject args(int index = 0) const { return {{"parameter", int(model()->id())}, {"controller", index}}; }
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
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->getSaveOptions().setDefaultOptions(); seed(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void finalObservationPreservesConnectionIdentityAndNativeCounts_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("unbound") << 0; QTest::newRow("shared") << 1; QTest::newRow("owned-midi") << 2; QTest::newRow("dummy") << 3; }
	void finalObservationPreservesConnectionIdentityAndNativeCounts()
	{
		QFETCH(int, kind); const QPointer<Controller> previousNative(bind(kind)); const QPointer<ControllerConnection> previous(model()->controllerConnection()); auto target = rack();
		const auto identity = previous ? previous->id() : 0; int notifications = 0; const auto connection = connect(model(), &Model::dataChanged, this, [&] {
			++notifications; QVERIFY(model()->controllerConnection()); QCOMPARE(model()->controllerConnection()->getController(), static_cast<Controller*>(target));
			if (kind) { QCOMPARE(model()->controllerConnection(), previous.data()); QCOMPARE(model()->controllerConnection()->id(), identity); }
			QCOMPARE(target->connectionCount(), 1); if (kind == 1) { QVERIFY(previousNative); QCOMPARE(previousNative->connectionCount(), 0); } if (kind == 2) { QVERIFY(!previousNative); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY2(model()->connectController(target, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications, 1); QCOMPARE(model()->storedValue(), 42.f);
	}
	void identicalFinalizedBindingIsAnUnnotifiedNoop()
	{
		auto target = rack(); model()->setControllerConnection(new ControllerConnection(target)); const QPointer<ControllerConnection> original(model()->controllerConnection());
		QSignalSpy notifications(model(), &Model::dataChanged); bool changed = true; QString error; QVERIFY(model()->connectController(target, changed, error));
		QVERIFY(!changed); QCOMPARE(notifications.count(), 0); QCOMPARE(model()->controllerConnection(), original.data()); QCOMPARE(target->connectionCount(), 1);
	}
	void currentControllerUseFlagIsPreserved_data()
	{ QTest::addColumn<bool>("active"); QTest::newRow("active") << true; QTest::newRow("suppressed") << false; }
	void currentControllerUseFlagIsPreserved()
	{
		QFETCH(bool, active); bind(2); model()->setUseControllerValue(active); auto target = rack(); bool changed = false; QString error;
		QVERIFY(model()->connectController(target, changed, error)); QVERIFY(changed); QCOMPARE(model()->useControllerValue(), active); QCOMPARE(model()->storedValue(), 42.f); if (!active) { QCOMPARE(model()->value(), 42.f); }
	}
	void invalidTargetsAndParameterCapabilitiesRefuseBeforeMutation_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("null") << 0; QTest::newRow("unregistered") << 1; QTest::newRow("dummy") << 2; QTest::newRow("midi") << 3; QTest::newRow("unsupported") << 4; QTest::newRow("decibel") << 5; }
	void invalidTargetsAndParameterCapabilitiesRefuseBeforeMutation()
	{
		QFETCH(int, failure); RackParameter parameter; auto target = rack(); auto unregistered = std::make_unique<LfoController>(nullptr);
		Controller* selected = failure == 0 ? nullptr : failure == 1 ? unregistered.get() : target;
		if (failure == 2 || failure == 3) { selected = bind(failure == 2 ? 3 : 2); }
		parameter.supported = failure != 4; if (failure == 5) { parameter.setScaleType(AutomatableModel::ScaleType::Decibel); }
		bool changed = true; QString error; QVERIFY(!parameter.connectController(selected, changed, error)); QVERIFY(!changed); QVERIFY(!parameter.controllerConnection()); QCOMPARE(target->connectionCount(), 0);
	}
	void directAndLinkedControllerFeedbackRefuse_data()
	{ QTest::addColumn<bool>("linked"); QTest::newRow("direct-child") << false; QTest::newRow("linked-child") << true; }
	void directAndLinkedControllerFeedbackRefuse()
	{
		QFETCH(bool, linked); auto target = rack(); FloatModel child(42, 0, 200, 0, target); AutomatableModel* receiver = &child;
		if (linked) { QVERIFY(model()->linkToModel(&child)); receiver = model(); } bool changed = true; QString error;
		QVERIFY(!receiver->connectController(target, changed, error)); QVERIFY(!changed); QVERIFY(!receiver->controllerConnection()); QCOMPARE(target->connectionCount(), 0);
	}
	void replacementCountIsCompleteBeforeOwnedMidiRetirement()
	{
		const QPointer<Controller> previousNative(bind(2)); const QPointer<ControllerConnection> binding(model()->controllerConnection()); auto target = rack(); bool observed = false;
		const auto connection = connect(previousNative.data(), &QObject::destroyed, this, [&] {
			observed = true; QCOMPARE(model()->controllerConnection(), binding.data()); QCOMPARE(binding->getController(), static_cast<Controller*>(target)); QCOMPARE(target->connectionCount(), 1);
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); binding->setController(target); QVERIFY(observed); QVERIFY(binding); QCOMPARE(target->connectionCount(), 1);
	}
	void ownedMidiRetirementCanRetireTheModelDuringInPlaceRewire()
	{
		const QPointer<Controller> previousNative(bind(2)); const QPointer<ControllerConnection> binding(model()->controllerConnection()); const QPointer<AutomatableModel> receiver(model()); auto target = rack();
		connect(previousNative.data(), &QObject::destroyed, this, [&] { QCOMPARE(target->connectionCount(), 1); delete instrument.data(); });
		bool changed = true; QString error; QVERIFY(!receiver->connectController(target, changed, error)); QVERIFY(!changed); QVERIFY(!receiver); QVERIFY(!binding); QCOMPARE(target->connectionCount(), 0);
	}
	void ownedMidiRetirementCanRetireTheReplacementController()
	{
		const QPointer<Controller> previousNative(bind(2)); const QPointer<ControllerConnection> binding(model()->controllerConnection()); const QPointer<Controller> target(rack());
		connect(previousNative.data(), &QObject::destroyed, this, [&] { Engine::getSong()->removeController(target.data()); });
		bool changed = true; QString error; QVERIFY(!model()->connectController(target.data(), changed, error)); QVERIFY(!changed); QVERIFY(!binding); QVERIFY(!target); QVERIFY(!model()->controllerConnection());
	}
	void rawFinalConnectionObserversMayRetireTheConnection()
	{
		bind(3); const QPointer<ControllerConnection> binding(model()->controllerConnection()); auto target = rack();
		connect(binding.data(), &ControllerConnection::valueChanged, this, [&] { delete binding.data(); }); binding->setController(target);
		QVERIFY(!binding); QVERIFY(!model()->controllerConnection()); QCOMPARE(target->connectionCount(), 0);
	}
	void nestedRawRewireKeepsTheInnerControllerAndCounts()
	{
		bind(3); const QPointer<ControllerConnection> binding(model()->controllerConnection()); auto outer = rack("Outer"), inner = rack("Inner"); bool invoked = false;
		const auto connection = connect(binding.data(), &ControllerConnection::valueChanged, this, [&] { if (!invoked) { invoked = true; binding->setController(inner); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!model()->connectController(outer, changed, error)); QVERIFY(invoked); QVERIFY(!changed); QCOMPARE(binding->getController(), static_cast<Controller*>(inner)); QCOMPARE(outer->connectionCount(), 0); QCOMPARE(inner->connectionCount(), 1);
	}
	void exhaustedNativeCounterRefusesWithoutIncrementing()
	{
		auto target = new CountedController; Engine::getSong()->addController(target); target->setCount(std::numeric_limits<int>::max());
		const auto reset = qScopeGuard([&] { target->setCount(0); }); bool changed = true; QString error;
		QVERIFY(!model()->connectController(target, changed, error)); QVERIFY(!changed); QVERIFY(!model()->controllerConnection()); QCOMPARE(target->connectionCount(), std::numeric_limits<int>::max());
	}
	void connectionRetirementDetachesLinkedReadersBeforeResourceHooks_data()
	{ QTest::addColumn<bool>("blocked"); QTest::newRow("blocked-connection") << true; QTest::newRow("unblocked-connection") << false; }
	void connectionRetirementDetachesLinkedReadersBeforeResourceHooks()
	{
		QFETCH(bool, blocked); FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer));
		model()->valueBuffer(); peer.valueBuffer(); auto native = bind(2);
		const QPointer<ControllerConnection> binding(model()->controllerConnection()); binding->blockSignals(blocked); model()->valueBuffer(); peer.valueBuffer(); int retirements = 0;
		connect(binding.data(), &ControllerConnection::retiring, this, [&] {
			++retirements; QVERIFY(binding->isRetiring()); QCOMPARE(native->connectionCount(), 0); QVERIFY(!model()->controllerConnection());
			QCOMPARE(model()->value(), 42.f); QCOMPARE(peer.value(), 42.f);
			for (auto value : {model()->valueBuffer(), peer.valueBuffer()}) { if (value) { for (auto sample : *value) { QCOMPARE(sample, 42.f); } } }
		});
		delete binding.data(); QVERIFY(!binding); QCOMPARE(retirements, 1); QVERIFY(!model()->controllerConnection());
	}
	void connectionDestructorResourceCallbacksMayRetireTheModel()
	{
		const QPointer<Controller> native(bind(2)); const QPointer<ControllerConnection> binding(model()->controllerConnection()); const QPointer<AutomatableModel> receiver(model());
		connect(native.data(), &QObject::destroyed, this, [&] { QVERIFY(!receiver->controllerConnection()); QCOMPARE(receiver->value<float>(), 42.f); delete instrument.data(); });
		delete binding.data(); QVERIFY(!binding); QVERIFY(!receiver); QVERIFY(!native);
	}
	void retirementCallbacksCanInstallALaterBinding()
	{
		const QPointer<Controller> native(bind(2)); const QPointer<ControllerConnection> binding(model()->controllerConnection()); auto target = rack();
		connect(binding.data(), &ControllerConnection::retiring, this, [&] { model()->setControllerConnection(new ControllerConnection(target)); });
		delete binding.data(); QVERIFY(!binding); QVERIFY(!native); QVERIFY(model()->controllerConnection()); QCOMPARE(model()->controllerConnection()->getController(), static_cast<Controller*>(target)); QCOMPARE(target->connectionCount(), 1);
	}
	void finalParameterCallbackFaultsAreDetected_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("value") << 1; QTest::newRow("dimension") << 2; QTest::newRow("retire-model") << 3; QTest::newRow("retire-controller") << 4; QTest::newRow("replace-binding") << 5; QTest::newRow("control-flag") << 6; }
	void finalParameterCallbackFaultsAreDetected()
	{
		QFETCH(int, failure); bind(2); const QPointer<AutomatableModel> receiver(model()); const QPointer<ControllerConnection> binding(model()->controllerConnection()); const QPointer<Controller> target(rack()); auto other = rack("Other"); bool invoked = false;
		ModelValueNotificationProbe fault(receiver.data(), [&] { if (failure == 0 && !invoked) { invoked = true; throw std::runtime_error("fixture rack assignment"); } });
		const auto connection = connect(receiver.data(), &Model::dataChanged, this, [&] {
			if (invoked || failure == 0) { return; } invoked = true;
			if (failure == 1) { receiver->setValue(60); } if (failure == 2) { receiver->setStep(0.5f); } if (failure == 3) { delete instrument.data(); }
			if (failure == 4) { Engine::getSong()->removeController(target.data()); } if (failure == 5) { receiver->setControllerConnection(new ControllerConnection(other)); }
			if (failure == 6) { receiver->setUseControllerValue(false); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!receiver->connectController(target.data(), changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!error.isEmpty());
		if (receiver) { QVERIFY(!receiver->signalsBlocked()); } if (failure == 5) { QVERIFY(!binding); QCOMPARE(target->connectionCount(), 0); QCOMPARE(other->connectionCount(), 1); }
	}
	void signalBlocksAreRestored_data()
	{ QTest::addColumn<bool>("blocked"); QTest::newRow("blocked") << true; QTest::newRow("unblocked") << false; }
	void signalBlocksAreRestored()
	{
		QFETCH(bool, blocked); bind(2); auto target = rack(); model()->blockSignals(blocked); QSignalSpy notifications(model(), &Model::dataChanged); bool changed = false; QString error;
		QVERIFY(model()->connectController(target, changed, error)); QVERIFY(changed); QCOMPARE(model()->signalsBlocked(), blocked); QCOMPARE(notifications.count(), blocked ? 0 : 1);
	}
	void linkedPeerValuesBindingsAndFlagsRemainUnchanged()
	{
		FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer)); auto other = rack("Peer"), target = rack();
		peer.setControllerConnection(new ControllerConnection(other)); peer.setUseControllerValue(false); const QPointer<ControllerConnection> remaining(peer.controllerConnection()); bool changed = false; QString error;
		QVERIFY(model()->connectController(target, changed, error)); QVERIFY(changed); QVERIFY(model()->isLinkedToModel(&peer)); QCOMPARE(peer.controllerConnection(), remaining.data()); QVERIFY(!peer.useControllerValue()); QCOMPARE(peer.storedValue(), 42.f); QCOMPARE(other->connectionCount(), 1);
	}
	void finalCallbacksCannotChangeLinkedPeers_data()
	{ QTest::addColumn<bool>("unlink"); QTest::newRow("peer-value") << false; QTest::newRow("peer-unlink") << true; }
	void finalCallbacksCannotChangeLinkedPeers()
	{
		QFETCH(bool, unlink); FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer)); auto target = rack(); bool invoked = false;
		const auto connection = connect(model(), &Model::dataChanged, this, [&] { if (!invoked) { invoked = true; if (unlink) { peer.unlink(); } else { peer.setValue(60); } } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!model()->connectController(target, changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!model()->signalsBlocked());
	}
	void cooperativeCancellationAndInterferenceCannotPublish_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("cancel-entry") << 0; QTest::newRow("cancel-prepared") << 1; QTest::newRow("value") << 2; QTest::newRow("retire-target") << 3; }
	void cooperativeCancellationAndInterferenceCannotPublish()
	{
		QFETCH(int, failure); const QPointer<Controller> target(rack()); bool timer = false, invoked = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		FileResourceIO::Scope work([&] { if (!invoked) { invoked = true; if (failure == 2) { model()->setValue(60); } if (failure == 3) { Engine::getSong()->removeController(target.data()); } }
			return failure == 0 || (failure == 1 && target && target->connectionCount() > 0); }, false);
		bool changed = true; QString error; QVERIFY(!model()->connectController(target.data(), changed, error)); QVERIFY(invoked); QVERIFY(!timer); QVERIFY(!changed); QVERIFY(!model()->controllerConnection()); if (target) { QCOMPARE(target->connectionCount(), 0); }
	}
	void structuredReceiptUsesCurrentRackIndexAfterCallbacks()
	{
		auto earlier = rack("Earlier"), target = rack(); const QPointer<Controller> oldMidi(bind(2));
		connect(oldMidi.data(), &QObject::destroyed, this, [&] { Engine::getSong()->removeController(earlier); });
		const auto receipt = StudioCapabilities::execute("controller.connect", args(1)); QVERIFY2(receipt.value("ok").toBool(), qPrintable(receipt.value("error").toString()));
		const auto result = receipt.value("result").toObject(); QCOMPARE(result.value("controller").toInt(), 0); QVERIFY(result.value("changed").toBool()); QCOMPARE(model()->controllerConnection()->getController(), static_cast<Controller*>(target));
	}
	void sessionUndoRedoReopenAndReplay()
	{
		QTemporaryDir directory; QString error; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); bind(1); rack("Target"); command = request("assign", "controller.connect", args(1));
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); replay(runtime, command, receipt);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); refresh(); QCOMPARE(model()->controllerConnection()->getController()->name(), QString("Previous"));
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt); refresh(); QCOMPARE(model()->controllerConnection()->getController()->name(), QString("Target")); QCOMPARE(model()->storedValue(), 42.f);
	}
	void sessionRecoversReturnedFailureAndRetirement_data()
	{ QTest::addColumn<bool>("retire"); QTest::newRow("throw") << false; QTest::newRow("retire") << true; }
	void sessionRecoversReturnedFailureAndRetirement()
	{
		QFETCH(bool, retire); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); bind(2); rack();
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); const auto command = request("reject", "controller.connect", args());
		ModelValueNotificationProbe fault(model(), [&] { if (!retire) { throw std::runtime_error("fixture session rack assignment"); } });
		const auto connection = connect(model(), &Model::dataChanged, this, [&] { if (retire) { delete instrument.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto receipt = runtime.process(command); QVERIFY(!receipt.value("ok").toBool()); replay(runtime, command, receipt);
		refresh(); QVERIFY(model()->controllerConnection()); QCOMPARE(model()->controllerConnection()->getController()->type(), Controller::ControllerType::Midi); QCOMPARE(model()->storedValue(), 42.f); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioControllerAssignmentTest)
#include "StudioControllerAssignmentTest.moc"

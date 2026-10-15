/* Checked native controller disconnection and setter lifetime.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "Controller.h"
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
class StudioControllerDisconnectTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	QPointer<InstrumentTrack> instrument;
	FloatModel* model() const { return instrument->volumeModel(); }
	void seed() { instrument = new InstrumentTrack(Engine::getSong()); model()->setValue(42); }
	Controller* bind(int kind = 0)
	{
		Controller* native = kind == 0 ? static_cast<Controller*>(new LfoController(Engine::getSong()))
			: kind == 1 ? static_cast<Controller*>(new MidiController(Engine::getSong())) : nullptr;
		if (kind == 0) { Engine::getSong()->addController(native); }
		model()->setControllerConnection(new ControllerConnection(native)); return model()->controllerConnection()->getController();
	}
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& a = {}) const { return {{"id", id}, {"op", op}, {"args", a}}; }
	QJsonObject selector() const { return {{"parameter", int(model()->id())}}; }
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
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); seed(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void oneFinalNotificationSeesRetiredResources_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("rack") << 0; QTest::newRow("owned-midi") << 1; QTest::newRow("dummy") << 2; }
	void oneFinalNotificationSeesRetiredResources()
	{
		QFETCH(int, kind); const QPointer<Controller> native(bind(kind)); const QPointer<ControllerConnection> previous(model()->controllerConnection()); int notifications = 0;
		const auto connection = connect(model(), &Model::dataChanged, this, [&] {
			++notifications; QVERIFY(!model()->controllerConnection()); QVERIFY(!previous); QCOMPARE(model()->storedValue(), 42.f);
			if (kind == 1) { QVERIFY(!native); } else { QVERIFY(native); QCOMPARE(native->connectionCount(), 0); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY2(model()->disconnectController(changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications, 1); QCOMPARE(model()->value(), 42.f);
	}
	void absentConnectionIsAnUnnotifiedNoop()
	{
		QSignalSpy notifications(model(), &Model::dataChanged); bool changed = true; QString error;
		QVERIFY(model()->disconnectController(changed, error)); QVERIFY(!changed); QCOMPARE(notifications.count(), 0); QCOMPARE(model()->storedValue(), 42.f);
	}
	void anExistingSignalBlockIsPreserved_data()
	{ QTest::addColumn<bool>("blocked"); QTest::newRow("blocked") << true; QTest::newRow("unblocked") << false; }
	void anExistingSignalBlockIsPreserved()
	{
		QFETCH(bool, blocked); bind(); model()->blockSignals(blocked); QSignalSpy notifications(model(), &Model::dataChanged); bool changed = false; QString error;
		QVERIFY(model()->disconnectController(changed, error)); QVERIFY(changed); QCOMPARE(model()->signalsBlocked(), blocked); QCOMPARE(notifications.count(), blocked ? 0 : 1);
	}
	void linkedPeersKeepTheirManualValuesBindingsAndControlFlag()
	{
		FloatModel first(42, 0, 100, 0, nullptr), peer(42, 0, 100, 0, nullptr); QVERIFY(first.linkToModel(&peer));
		auto a = new LfoController(Engine::getSong()); auto b = new LfoController(Engine::getSong()); Engine::getSong()->addController(a); Engine::getSong()->addController(b);
		first.setControllerConnection(new ControllerConnection(a)); peer.setControllerConnection(new ControllerConnection(b)); first.setUseControllerValue(false);
		const QPointer<ControllerConnection> remaining(peer.controllerConnection()); bool changed = false; QString error;
		QVERIFY2(first.disconnectController(changed, error), qPrintable(error)); QVERIFY(changed); QVERIFY(first.isLinkedToModel(&peer)); QVERIFY(!first.useControllerValue());
		QCOMPARE(first.storedValue(), 42.f); QCOMPARE(peer.storedValue(), 42.f); QCOMPARE(peer.controllerConnection(), remaining.data()); QCOMPARE(a->connectionCount(), 0); QCOMPARE(b->connectionCount(), 1);
	}
	void oldConnectionDestructionObservesTheDetachedModel()
	{
		bind(1); bool observed = false; const auto connection = connect(model()->controllerConnection(), &QObject::destroyed, this, [&] {
			observed = true; QVERIFY(!model()->controllerConnection()); QCOMPARE(model()->storedValue(), 42.f);
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY(model()->disconnectController(changed, error)); QVERIFY(changed); QVERIFY(observed);
	}
	void destructionCallbacksMayRetireTheParameter()
	{
		bind(1); const QPointer<AutomatableModel> receiver(model()); const QPointer<ControllerConnection> previous(model()->controllerConnection());
		connect(previous.data(), &QObject::destroyed, this, [&] { delete instrument.data(); }); bool changed = true; QString error;
		QVERIFY(!receiver->disconnectController(changed, error)); QVERIFY(!changed); QVERIFY(!receiver); QVERIFY(!previous); QVERIFY(!error.isEmpty());
	}
	void finalCallbackInterferenceIsRefused_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("nonstandard-throw") << 1;
		QTest::newRow("value") << 2; QTest::newRow("range") << 3; QTest::newRow("rebind") << 4; QTest::newRow("retire") << 5;
	}
	void finalCallbackInterferenceIsRefused()
	{
		QFETCH(int, failure); auto native = bind(); const QPointer<AutomatableModel> receiver(model()); const QPointer<ControllerConnection> previous(model()->controllerConnection()); bool invoked = false;
		ModelValueNotificationProbe fault(receiver.data(), [&] { if (failure <= 1 && !invoked) { invoked = true; if (failure == 0) { throw std::runtime_error("fixture controller disconnect"); } throw 7; } });
		const auto connection = connect(receiver.data(), &Model::dataChanged, this, [&] {
			if (invoked || failure <= 1) { return; } invoked = true;
			if (failure == 2) { receiver->setValue(60); } if (failure == 3) { receiver->setStep(0.5f); }
			if (failure == 4) { receiver->setControllerConnection(new ControllerConnection(native)); } if (failure == 5) { delete instrument.data(); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!receiver->disconnectController(changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!previous); QVERIFY(!error.isEmpty()); if (receiver) { QVERIFY(!receiver->signalsBlocked()); }
	}
	void oldResourceCallbacksCannotSilentlyRebind()
	{
		auto native = bind(); bool invoked = false; const auto connection = connect(model()->controllerConnection(), &QObject::destroyed, this, [&] {
			invoked = true; model()->setControllerConnection(new ControllerConnection(native));
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!model()->disconnectController(changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(model()->controllerConnection()); QVERIFY(!model()->signalsBlocked());
	}
	void finalCallbacksCannotChangeLinkedPeerState_data()
	{ QTest::addColumn<bool>("unlink"); QTest::newRow("peer-value") << false; QTest::newRow("unlink-peer") << true; }
	void finalCallbacksCannotChangeLinkedPeerState()
	{
		QFETCH(bool, unlink); FloatModel peer(42, 0, 200, 0, nullptr); QVERIFY(model()->linkToModel(&peer)); bind(); bool invoked = false;
		const auto connection = connect(model(), &Model::dataChanged, this, [&] { if (!invoked) { invoked = true; if (unlink) { peer.unlink(); } else { peer.setValue(60); } } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!model()->disconnectController(changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!model()->signalsBlocked());
	}
	void cancellationAndAdmissionInterferenceRetainTheOldBinding_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("cancel") << 0; QTest::newRow("value") << 1; QTest::newRow("dimension") << 2; }
	void cancellationAndAdmissionInterferenceRetainTheOldBinding()
	{
		QFETCH(int, failure); bind(); const QPointer<ControllerConnection> previous(model()->controllerConnection()); bool invoked = false, timer = false; QObject owner;
		QTimer::singleShot(0, &owner, [&] { timer = true; }); FileResourceIO::Scope work([&] {
			if (!invoked) { invoked = true; if (failure == 1) { model()->setValue(60); } if (failure == 2) { model()->setStep(0.5f); } } return failure == 0;
		}, false); bool changed = true; QString error;
		QVERIFY(!model()->disconnectController(changed, error)); QVERIFY(invoked); QVERIFY(!timer); QVERIFY(!changed); QCOMPARE(model()->controllerConnection(), previous.data());
	}
	void theNativeSetterAllowsFinalObserverRetirement()
	{
		auto receiver = std::make_unique<FloatModel>(42, 0, 100, 0, nullptr); const QPointer<AutomatableModel> weak(receiver.get());
		auto replacement = new ControllerConnection(nullptr); const QPointer<ControllerConnection> binding(replacement);
		connect(receiver.get(), &Model::dataChanged, this, [&] { receiver.reset(); }); receiver->setControllerConnection(replacement); QVERIFY(!weak); QVERIFY(!binding);
	}
	void theNativeSetterKeepsTheInnermostReplacement()
	{
		FloatModel receiver(42, 0, 100, 0, nullptr); auto first = new ControllerConnection(nullptr); auto second = new ControllerConnection(nullptr);
		const QPointer<ControllerConnection> previous(first); bool invoked = false;
		const auto connection = connect(&receiver, &Model::dataChanged, this, [&] { if (!invoked) { invoked = true; receiver.setControllerConnection(second); delete previous.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); receiver.setControllerConnection(first); QVERIFY(invoked); QVERIFY(!previous); QCOMPARE(receiver.controllerConnection(), second);
	}
	void deferredOldDestructionCannotClearANewerBinding()
	{
		FloatModel receiver(42, 0, 100, 0, nullptr); auto first = new ControllerConnection(nullptr); receiver.setControllerConnection(first);
		QThread worker; worker.start(); const auto stop = qScopeGuard([&] { worker.quit(); worker.wait(5000); }); first->moveToThread(&worker);
		QVERIFY(QMetaObject::invokeMethod(first, [first] { delete first; }, Qt::BlockingQueuedConnection)); QVERIFY(!receiver.controllerConnection());
		auto replacement = new ControllerConnection(nullptr); receiver.setControllerConnection(replacement); QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
		QCOMPARE(receiver.controllerConnection(), replacement); QCOMPARE(receiver.storedValue(), 42.f);
	}
	void foreignThreadConnectionsRefuseCheckedDisconnection()
	{
		bind(); auto binding = model()->controllerConnection(); const QPointer<ControllerConnection> weak(binding); QThread worker; worker.start();
		const auto stop = qScopeGuard([&] { worker.quit(); worker.wait(5000); }); binding->moveToThread(&worker); bool changed = true; QString error;
		QVERIFY(!model()->disconnectController(changed, error)); QVERIFY(!changed); QCOMPARE(model()->controllerConnection(), binding);
		QVERIFY(QMetaObject::invokeMethod(binding, [binding] { delete binding; }, Qt::BlockingQueuedConnection)); QVERIFY(!weak); QCoreApplication::sendPostedEvents(model(), QEvent::MetaCall);
		QVERIFY(!model()->controllerConnection());
	}
	void structuredDisconnectionReturnsTheNativeManualValue_data()
	{ QTest::addColumn<bool>("present"); QTest::newRow("present") << true; QTest::newRow("absent") << false; }
	void structuredDisconnectionReturnsTheNativeManualValue()
	{
		QFETCH(bool, present); if (present) { bind(1); } const auto receipt = StudioCapabilities::execute("controller.disconnect", selector());
		QVERIFY(receipt.value("ok").toBool()); const auto result = receipt.value("result").toObject(); QCOMPARE(result.value("manualValue").toDouble(), 42.); QCOMPARE(result.value("changed").toBool(), present);
	}
	void sessionUndoRedoReopenAndReplay_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("rack") << 0; QTest::newRow("midi") << 1; }
	void sessionUndoRedoReopenAndReplay()
	{
		QFETCH(int, kind); QTemporaryDir directory; QString error; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); bind(kind); command = request("disconnect", "controller.disconnect", selector());
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); replay(runtime, command, receipt);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); refresh(); QVERIFY(model()->controllerConnection());
			QCOMPARE(model()->controllerConnection()->getController()->type(), kind == 0 ? Controller::ControllerType::Lfo : Controller::ControllerType::Midi);
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt); refresh(); QVERIFY(!model()->controllerConnection()); QCOMPARE(model()->storedValue(), 42.f);
	}
	void sessionRecoversReturnedFailureAndCachesIt_data()
	{ QTest::addColumn<bool>("retirement"); QTest::newRow("throw") << false; QTest::newRow("retirement") << true; }
	void sessionRecoversReturnedFailureAndCachesIt()
	{
		QFETCH(bool, retirement); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); bind(1);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); const auto command = request("reject", "controller.disconnect", selector());
		ModelValueNotificationProbe fault(model(), [&] { if (!retirement) { throw std::runtime_error("fixture session disconnect"); } });
		const auto connection = connect(model(), &Model::dataChanged, this, [&] { if (retirement) { delete instrument.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto receipt = runtime.process(command); QVERIFY(!receipt.value("ok").toBool()); replay(runtime, command, receipt);
		refresh(); QVERIFY(model()->controllerConnection()); QCOMPARE(model()->controllerConnection()->getController()->type(), Controller::ControllerType::Midi); QCOMPARE(model()->storedValue(), 42.f); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioControllerDisconnectTest)
#include "StudioControllerDisconnectTest.moc"

/* Native controller rack registration, removal and checked receipts.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace lmms
{
class StudioControllerLifecycleTestAccess
{
public:
	static Song* create() { return new Song; }
	static void destroy(Song* song) { delete song; }
};
}
namespace
{
using IsolatedSong = std::unique_ptr<Song, decltype(&StudioControllerLifecycleTestAccess::destroy)>;
IsolatedSong isolatedSong() { return IsolatedSong(StudioControllerLifecycleTestAccess::create(), &StudioControllerLifecycleTestAccess::destroy); }
class LifecycleController final : public LfoController
{
public:
	explicit LifecycleController(Model* parent) : LfoController(parent) { }
	std::function<void()> onRename;
	mutable std::function<void()> onRead;
	void setName(const QString& value) override { Controller::setName(value); const auto callback = onRename; if (callback) { callback(); } }
	const QString& name() const override { const auto callback = onRead; if (callback) { callback(); } return Controller::name(); }
};
}
class StudioControllerLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QPointer<InstrumentTrack> instrument;
	bool journalling = false;
	FloatModel* model() const { return instrument->volumeModel(); }
	void seed() { instrument = new InstrumentTrack(Engine::getSong()); model()->setValue(42); }
	LfoController* rack(const QString& name = "Original")
	{ auto native = new LfoController(Engine::getSong()); native->setName(name); Engine::getSong()->addController(native); return native; }
	QJsonObject create(bool summary = true) const { return {{"type", "lfo"}, {"name", "Created"}, {"summary", summary}}; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& a = {}) const { return {{"id", id}, {"op", op}, {"args", a}}; }
	void refresh() { instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); }
	void replay(StudioRuntime& runtime, const QJsonObject& command, const QJsonObject& receipt)
	{ auto response = runtime.process(command); QVERIFY(response.take("replayed").toBool()); QCOMPARE(response, receipt); }
private slots:
	void initTestCase()
	{
		qRegisterMetaType<Controller*>("lmms::Controller*"); QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->getSaveOptions().setDefaultOptions(); seed(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->getSaveOptions().setDefaultOptions(); }
	void removalObserversSeeDetachedBindingsAndCannotReaddTheTarget()
	{
		auto song = Engine::getSong(); const QPointer<Controller> target(rack()); model()->setControllerConnection(new ControllerConnection(target.data()));
		const QPointer<ControllerConnection> binding(model()->controllerConnection()); int removed = 0;
		const auto connection = connect(song, &Song::controllerRemoved, this, [&](Controller* native) {
			++removed; QCOMPARE(native, target.data()); QVERIFY(native->isRetiring()); QVERIFY(!song->containsController(native)); QVERIFY(!binding); QVERIFY(!model()->controllerConnection());
			QCOMPARE(model()->value(), 42.f); song->addController(native); QVERIFY(!song->containsController(native)); song->removeController(native);
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		song->removeController(target.data()); QVERIFY(!target); QCOMPARE(removed, 1); QVERIFY(song->controllers().empty());
	}
	void removalObserverMayDeleteTheTargetWithoutDoubleDeletion()
	{
		auto song = Engine::getSong(); const QPointer<Controller> target(rack()); int deaths = 0; connect(target.data(), &QObject::destroyed, this, [&] { ++deaths; });
		const auto connection = connect(song, &Song::controllerRemoved, this, [](Controller* native) { delete native; });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); song->removeController(target.data()); QVERIFY(!target); QCOMPARE(deaths, 1); QVERIFY(song->controllers().empty());
	}
	void earlyRetirementCanDeleteTheTargetBeforeTheRemovalPayload()
	{
		auto song = Engine::getSong(); const QPointer<Controller> target(rack()); int deaths = 0; QSignalSpy removed(song, &Song::controllerRemoved);
		connect(target.data(), &Controller::retiring, this, [&] { delete target.data(); }); connect(target.data(), &QObject::destroyed, this, [&] { ++deaths; });
		song->removeController(target.data()); QVERIFY(!target); QCOMPARE(deaths, 1); QCOMPARE(removed.count(), 0); QVERIFY(song->controllers().empty());
	}
	void throwingRemovalStillRetiresTheTargetAndReleasesAdmission_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void throwingRemovalStillRetiresTheTargetAndReleasesAdmission()
	{
		QFETCH(bool, standard); auto song = Engine::getSong(); const QPointer<Controller> target(rack()); bool invoked = false;
		const auto connection = connect(song, &Song::controllerRemoved, this, [&](Controller*) {
			if (invoked) { return; } invoked = true;
			try { if (standard) { throw std::runtime_error("fixture rack removal"); } throw 7; }
			catch (...) { QVERIFY(song->reportControllerNotificationFailure(std::current_exception())); }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("controller.remove", {{"controller", 0}});
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.contains("nativeWarnings")); QVERIFY(invoked); QVERIFY(!target); QVERIFY(song->controllers().empty());
		if (!standard) { QVERIFY(response.value("error").toString().contains("nonstandard")); } auto later = rack("Later"); QVERIFY(song->containsController(later));
	}
	void directDeletionPurgesTheRackAfterBindingRetirement_data()
	{ QTest::addColumn<bool>("blocked"); QTest::newRow("blocked") << true; QTest::newRow("unblocked") << false; }
	void directDeletionPurgesTheRackAfterBindingRetirement()
	{
		QFETCH(bool, blocked); auto song = Engine::getSong(); auto target = rack(); auto peer = rack("Peer");
		model()->setControllerConnection(new ControllerConnection(target)); instrument->panningModel()->setControllerConnection(new ControllerConnection(peer));
		const QPointer<ControllerConnection> binding(model()->controllerConnection()); target->blockSignals(blocked); binding->blockSignals(blocked); bool observed = false;
		const auto connection = connect(song, &Song::controllerRosterChanged, this, [&] {
			observed = true; QVERIFY(!binding); QVERIFY(!model()->controllerConnection()); QCOMPARE(model()->value(), 42.f); QCOMPARE(song->controllers().size(), std::size_t(1)); QCOMPARE(peer->connectionCount(), 1);
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); delete target; QVERIFY(observed); QVERIFY(!song->containsController(target)); QVERIFY(song->containsController(peer));
	}
	void aRunningControllerDestructorCanDetachItsRackEntry()
	{
		auto song = Engine::getSong(); const QPointer<Controller> target(rack()); int deaths = 0;
		connect(target.data(), &Controller::retiring, this, [&] { song->removeController(target.data()); });
		connect(target.data(), &QObject::destroyed, this, [&] { ++deaths; }); delete target.data(); QVERIFY(!target); QCOMPARE(deaths, 1); QVERIFY(song->controllers().empty());
	}
	void creationCallbacksCanRetireTheNewController_data()
	{ QTest::addColumn<bool>("nativeRemove"); QTest::newRow("direct-delete") << false; QTest::newRow("native-remove") << true; }
	void creationCallbacksCanRetireTheNewController()
	{
		QFETCH(bool, nativeRemove); auto song = Engine::getSong(); QPointer<Controller> created; bool invoked = false;
		const auto connection = connect(song, &Song::controllerAdded, this, [&](Controller* native) { if (!invoked) { invoked = true; created = native; if (nativeRemove) { song->removeController(native); } else { delete native; } } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("controller.add", create());
		QVERIFY(!response.value("ok").toBool()); QVERIFY(invoked); QVERIFY(!created); QVERIFY(song->controllers().empty());
	}
	void creationReceiptsUseCurrentIndicesAfterCallbackRemoval_data()
	{ QTest::addColumn<bool>("summary"); QTest::newRow("summary") << true; QTest::newRow("full") << false; }
	void creationReceiptsUseCurrentIndicesAfterCallbackRemoval()
	{
		QFETCH(bool, summary); auto song = Engine::getSong(); const QPointer<Controller> earlier(rack("Earlier")); bool invoked = false;
		const auto connection = connect(song, &Song::controllerAdded, this, [&](Controller*) { if (!invoked) { invoked = true; song->removeController(earlier.data()); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("controller.add", create(summary)); QVERIFY(response.value("ok").toBool());
		const auto result = response.value("result").toObject(); QCOMPARE(result.value("controller").toInt(), 0); QVERIFY(!earlier); QCOMPARE(song->controllers().front()->name(), QString("Created"));
		if (summary) { QCOMPARE(result.value("summary").toObject().value("index").toInt(), 0); } else { QCOMPARE(result.value("controllers").toArray().size(), 1); }
	}
	void nestedCreationDoesNotReplaceTheOuterReceiptIdentity()
	{
		auto song = Engine::getSong(); bool invoked = false; const auto connection = connect(song, &Song::controllerAdded, this, [&](Controller*) { if (!invoked) { invoked = true; rack("Nested"); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("controller.add", create(false)); QVERIFY(response.value("ok").toBool());
		QCOMPARE(response.value("result").toObject().value("controller").toInt(), 0); QCOMPARE(song->controllers().size(), std::size_t(2)); QCOMPARE(song->controllers()[0]->name(), QString("Created")); QCOMPARE(song->controllers()[1]->name(), QString("Nested"));
	}
	void receiptInspectionRefusesConcurrentRosterMutation()
	{
		auto song = Engine::getSong(); const QPointer<Controller> earlier(rack("Earlier")); QPointer<Controller> created; bool changed = false;
		const auto connection = connect(song, &Song::controllerAdded, this, [&](Controller* native) { created = native; }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		FileResourceIO::Scope work([&] { if (created && !changed) { changed = true; song->removeController(earlier.data()); } return false; }, false);
		const auto response = StudioCapabilities::execute("controller.add", create(false)); QVERIFY(!response.value("ok").toBool()); QVERIFY(changed); QVERIFY(created); QVERIFY(song->containsController(created.data())); QVERIFY(!earlier);
	}
	void fullRackInspectionStopsAfterAGetterRetiresAPeer()
	{
		auto song = Engine::getSong(); auto first = new LifecycleController(song); song->addController(first); const QPointer<Controller> peer(rack("Peer")); bool invoked = false;
		first->onRead = [&] { if (!invoked) { invoked = true; song->removeController(peer.data()); } };
		const auto response = StudioCapabilities::execute("controller.list", {}); first->onRead = {}; QVERIFY(!response.value("ok").toBool()); QVERIFY(invoked); QVERIFY(!peer); QVERIFY(song->containsController(first));
	}
	void checkedRenameHandlesRetirementFailureAndIndexChanges_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("retire") << 0; QTest::newRow("remove-earlier") << 1; QTest::newRow("standard-throw") << 2; QTest::newRow("nonstandard-throw") << 3; }
	void checkedRenameHandlesRetirementFailureAndIndexChanges()
	{
		QFETCH(int, action); auto song = Engine::getSong(); const QPointer<Controller> earlier(rack("Earlier")); auto target = new LifecycleController(song); song->addController(target); const QPointer<Controller> weak(target);
		target->onRename = [&] { if (action == 0) { delete target; } if (action == 1) { song->removeController(earlier.data()); } if (action == 2) { throw std::runtime_error("fixture rack rename"); } if (action == 3) { throw 7; } };
		const auto response = StudioCapabilities::execute("controller.edit", {{"controller", 1}, {"name", "Renamed"}});
		if (weak) { target->onRename = {}; }
		QCOMPARE(response.value("ok").toBool(), action == 1); if (action == 0) { QVERIFY(!weak); }
		if (action == 1) { QCOMPARE(response.value("result").toObject().value("controller").toInt(), 0); QCOMPARE(response.value("result").toObject().value("name").toString(), QString("Renamed")); }
	}
	void cancellationBeforeMutationsOrAfterPreparationLeavesNoNewController_data()
	{ QTest::addColumn<int>("operation"); QTest::newRow("add") << 0; QTest::newRow("edit") << 1; QTest::newRow("remove") << 2; QTest::newRow("prepared-add") << 3; }
	void cancellationBeforeMutationsOrAfterPreparationLeavesNoNewController()
	{
		QFETCH(int, operation); auto song = Engine::getSong(); const QPointer<Controller> original(rack()); bool timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		FileResourceIO::Scope work([&] { return operation != 3 || song->findChildren<LfoController*>().size() > 1; }, false);
		const auto op = operation == 1 ? "controller.edit" : operation == 2 ? "controller.remove" : "controller.add";
		const QJsonObject a = operation == 1 ? QJsonObject{{"controller", 0}, {"name", "Renamed"}} : operation == 2 ? QJsonObject{{"controller", 0}} : create();
		QVERIFY(!StudioCapabilities::execute(op, a).value("ok").toBool()); QVERIFY(!timer); QVERIFY(original); QCOMPARE(original->name(), QString("Original")); QCOMPARE(song->controllers().size(), std::size_t(1)); QCOMPARE(song->findChildren<LfoController*>().size(), 1);
	}
	void sharedDummyRemovalPreservesTheSingletonAndCustomDummyIsOwned_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("shared") << true; QTest::newRow("custom") << false; }
	void sharedDummyRemovalPreservesTheSingletonAndCustomDummyIsOwned()
	{
		QFETCH(bool, shared); auto song = Engine::getSong(); auto fallback = Controller::create(Controller::ControllerType::Dummy, nullptr);
		const QPointer<Controller> target(shared ? fallback : new Controller(Controller::ControllerType::Dummy, song, "Custom dummy")); song->addController(target.data());
		model()->setControllerConnection(new ControllerConnection(target.data())); const QPointer<ControllerConnection> binding(model()->controllerConnection()); song->removeController(target.data());
		QCOMPARE(bool(target), shared); QCOMPARE(bool(binding), shared); QCOMPARE(bool(model()->controllerConnection()), shared);
		if (!shared) { QCOMPARE(model()->value(), 42.f); }
		QVERIFY(song->controllers().empty()); QVERIFY(!fallback->isRetiring()); ControllerConnection next(nullptr); QCOMPARE(next.getController(), fallback);
	}
	void ownerRetirementDuringRemovalStopsTheOldContinuation()
	{
		auto owner = isolatedSong(); const QPointer<Song> weak(owner.get()); const QPointer<Controller> target(new LfoController(owner.get())); owner->addController(target.data());
		connect(owner.get(), &Song::controllerRemoved, this, [&](Controller*) { owner.reset(); }); weak->removeController(target.data()); QVERIFY(!weak); QVERIFY(!target);
	}
	void ownerDestructionDisconnectsRosterHooksBeforeMemberTeardown()
	{
		auto owner = isolatedSong(); const QPointer<Song> weak(owner.get()); const QPointer<Controller> target(new LfoController(owner.get())); owner->addController(target.data());
		model()->setControllerConnection(new ControllerConnection(target.data())); const QPointer<ControllerConnection> binding(model()->controllerConnection()); int notifications = 0;
		connect(owner.get(), &Song::controllerRosterChanged, this, [&] { ++notifications; }); owner.reset();
		QVERIFY(!weak); QVERIFY(!target); QVERIFY(!binding); QVERIFY(!model()->controllerConnection()); QCOMPARE(model()->value(), 42.f); QCOMPARE(notifications, 0);
	}
	void retiringControllersAreUnavailableToStructuredReads()
	{
		auto song = Engine::getSong(); auto target = rack(); bool observed = false; const QPointer<Controller> weak(target);
		connect(target, &Controller::retiring, this, [&] { observed = true; QVERIFY(!StudioCapabilities::execute("controller.list", {}).value("ok").toBool()); QVERIFY(!StudioCapabilities::execute("controller.edit", {{"controller", 0}, {"name", "Late"}}).value("ok").toBool()); });
		delete target; QVERIFY(observed); QVERIFY(!weak); QVERIFY(song->controllers().empty());
	}
	void peakTypeRemovalAdmissionRetainsTheEffectOwnedEntry()
	{
		const QPointer<Controller> target(new Controller(Controller::ControllerType::Peak, Engine::getSong(), "Effect-owned type tag")); Engine::getSong()->addController(target.data());
		QVERIFY(!StudioCapabilities::execute("controller.remove", {{"controller", 0}}).value("ok").toBool()); QVERIFY(target); QVERIFY(Engine::getSong()->containsController(target.data()));
	}
	void sessionUndoRedoReopeningPreservesRackAndBindings_data()
	{ QTest::addColumn<int>("operation"); QTest::newRow("add") << 0; QTest::newRow("edit") << 1; QTest::newRow("remove") << 2; }
	void sessionUndoRedoReopeningPreservesRackAndBindings()
	{
		QFETCH(int, operation); QTemporaryDir directory; QString error; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); auto native = rack(); model()->setControllerConnection(new ControllerConnection(native));
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			command = request("change", operation == 0 ? "controller.add" : operation == 1 ? "controller.edit" : "controller.remove", operation == 0 ? create() : operation == 1 ? QJsonObject{{"controller", 0}, {"name", "Renamed"}} : QJsonObject{{"controller", 0}});
			receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); replay(runtime, command, receipt);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); refresh(); QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(1)); QVERIFY(model()->controllerConnection()); QCOMPARE(Engine::getSong()->controllers()[0]->name(), QString("Original"));
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt); refresh();
		QCOMPARE(Engine::getSong()->controllers().size(), operation == 0 ? std::size_t(2) : operation == 1 ? std::size_t(1) : std::size_t(0)); QCOMPARE(bool(model()->controllerConnection()), operation != 2); QCOMPARE(model()->storedValue(), 42.f);
	}
	void sessionRecoversLifecycleNotificationFailuresAndCachesThem_data()
	{ QTest::addColumn<bool>("adding"); QTest::addColumn<bool>("standard"); QTest::newRow("add-standard") << true << true; QTest::newRow("add-nonstandard") << true << false; QTest::newRow("remove-standard") << false << true; QTest::newRow("remove-nonstandard") << false << false; }
	void sessionRecoversLifecycleNotificationFailuresAndCachesThem()
	{
		QFETCH(bool, adding); QFETCH(bool, standard); QTemporaryDir directory; QString error; QJsonObject receipt;
		const auto command = request("reject", adding ? "controller.add" : "controller.remove", adding ? create() : QJsonObject{{"controller", 0}});
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); auto native = rack(); model()->setControllerConnection(new ControllerConnection(native));
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); bool invoked = false;
			const auto callback = [&](Controller*) {
				if (invoked) { return; } invoked = true;
				try { if (standard) { throw std::runtime_error("fixture session controller lifecycle"); } throw 7; }
				catch (...) { QVERIFY(Engine::getSong()->reportControllerNotificationFailure(std::current_exception())); }
			};
			const auto connection = adding ? connect(Engine::getSong(), &Song::controllerAdded, this, callback) : connect(Engine::getSong(), &Song::controllerRemoved, this, callback);
			const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			receipt = runtime.process(command); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(invoked); replay(runtime, command, receipt); refresh();
			QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->controllers()[0]->name(), QString("Original")); QVERIFY(model()->controllerConnection()); QCOMPARE(model()->storedValue(), 42.f); QVERIFY(!runtime.stopping());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt); refresh();
		QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->controllers()[0]->name(), QString("Original"));
		QVERIFY(model()->controllerConnection()); QCOMPARE(model()->storedValue(), 42.f); QCOMPARE(Engine::getSong()->getTempo(), 123);
	}
};
QTEST_GUILESS_MAIN(StudioControllerLifecycleTest)
#include "StudioControllerLifecycleTest.moc"

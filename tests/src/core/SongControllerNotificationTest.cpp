/* Receiver-caught lifecycle failures leave Qt before native propagation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "LfoController.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"

using namespace lmms;
namespace
{
std::exception_ptr failure(int kind, int token = 263)
{
	try
	{
		if (kind == 1) { throw std::runtime_error("Native controller notification fixture"); }
		if (kind == 2) { throw token; }
	}
	catch (...) { return std::current_exception(); }
	return {};
}
}
class SongControllerNotificationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		qRegisterMetaType<Controller*>("lmms::Controller*"); QVERIFY(configuration.isValid());
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false);
		Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase()
	{
		Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free();
	}
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void firstFailureReturnsOnlyAfterLaterObservers_data()
	{
		QTest::addColumn<int>("notification"); QTest::addColumn<int>("kind");
		QTest::newRow("added-standard") << 0 << 1; QTest::newRow("added-nonstandard") << 0 << 2;
		QTest::newRow("removed-standard") << 1 << 1; QTest::newRow("removed-nonstandard") << 1 << 2;
		QTest::newRow("roster-standard") << 2 << 1; QTest::newRow("roster-nonstandard") << 2 << 2;
	}
	void firstFailureReturnsOnlyAfterLaterObservers()
	{
		QFETCH(int, notification); QFETCH(int, kind); auto song = Engine::getSong();
		const QPointer<Controller> target(new LfoController(song)); if (notification == 1) { song->addController(target.data()); }
		const auto first = failure(kind), later = failure(kind == 1 ? 2 : 1, 269); int reports = 0;
		const auto receiver = [&] {
			QVERIFY(song->reportControllerNotificationFailure(first)); ++reports;
			QVERIFY(song->reportControllerNotificationFailure(later)); ++reports;
		};
		const auto connection = notification == 0 ? connect(song, &Song::controllerAdded, this, receiver, Qt::DirectConnection)
			: notification == 1 ? connect(song, &Song::controllerRemoved, this, receiver, Qt::DirectConnection)
			: connect(song, &Song::controllerRosterChanged, this, receiver, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QSignalSpy added(song, &Song::controllerAdded), removed(song, &Song::controllerRemoved), roster(song, &Song::controllerRosterChanged);
		std::exception_ptr received;
		try { if (notification == 1) { song->removeController(target.data()); } else { song->addController(target.data()); } }
		catch (...) { received = std::current_exception(); }
		QVERIFY(received == first); QCOMPARE(reports, 2);
		QCOMPARE(notification == 0 ? added.size() : notification == 1 ? removed.size() : roster.size(), 1);
		QVERIFY(!song->reportControllerNotificationFailure(later));
		if (notification == 1) { QVERIFY(!target); QVERIFY(song->controllers().empty()); }
		else { QVERIFY(target); QVERIFY(song->containsController(target.data())); }
		QObject::disconnect(connection);
		if (target) { song->removeController(target.data()); }
		auto retry = new LfoController(song); song->addController(retry); QVERIFY(song->containsController(retry));
	}

	void nestedNotificationsRetainTheirOwnFailures_data()
	{
		QTest::addColumn<int>("outerKind"); QTest::addColumn<int>("innerKind");
		QTest::newRow("none-none") << 0 << 0; QTest::newRow("none-standard") << 0 << 1; QTest::newRow("none-nonstandard") << 0 << 2;
		QTest::newRow("standard-none") << 1 << 0; QTest::newRow("standard-standard") << 1 << 1; QTest::newRow("standard-nonstandard") << 1 << 2;
		QTest::newRow("nonstandard-none") << 2 << 0; QTest::newRow("nonstandard-standard") << 2 << 1; QTest::newRow("nonstandard-nonstandard") << 2 << 2;
	}
	void nestedNotificationsRetainTheirOwnFailures()
	{
		QFETCH(int, outerKind); QFETCH(int, innerKind); auto song = Engine::getSong();
		auto outer = new LfoController(song), inner = new LfoController(song);
		const auto outerFailure = failure(outerKind), innerFailure = failure(innerKind, 269);
		std::exception_ptr caughtInner, caughtOuter; int entered = 0;
		const auto connection = connect(song, &Song::controllerAdded, this, [&](Controller* controller) {
			++entered;
			if (controller == inner) { if (innerFailure) { QVERIFY(song->reportControllerNotificationFailure(innerFailure)); } return; }
			if (controller != outer) { return; }
			try { song->addController(inner); }
			catch (...) { caughtInner = std::current_exception(); }
			if (outerFailure) { QVERIFY(song->reportControllerNotificationFailure(outerFailure)); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QSignalSpy later(song, &Song::controllerAdded);
		try { song->addController(outer); }
		catch (...) { caughtOuter = std::current_exception(); }
		QVERIFY(caughtInner == innerFailure); QVERIFY(caughtOuter == outerFailure); QCOMPARE(entered, 2); QCOMPARE(later.size(), 2);
		QVERIFY(song->containsController(outer)); QVERIFY(song->containsController(inner));
		QVERIFY(!song->reportControllerNotificationFailure(failure(1)));
	}

	void emptyOutsideAndForeignThreadReportsAreRejected()
	{
		auto song = Engine::getSong(); const auto fault = failure(1); std::atomic_bool rejected{false};
		QVERIFY(!song->reportControllerNotificationFailure(fault)); QVERIFY(!song->reportControllerNotificationFailure({}));
		const auto connection = connect(song, &Song::controllerAdded, this, [&] {
			QVERIFY(!song->reportControllerNotificationFailure({}));
			auto worker = std::unique_ptr<QThread>(QThread::create([&] { rejected = !song->reportControllerNotificationFailure(fault); }));
			worker->start(); QVERIFY(worker->wait()); QVERIFY(rejected.load());
			QVERIFY(song->reportControllerNotificationFailure(fault));
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); std::exception_ptr received;
		try { song->addController(new LfoController(song)); }
		catch (...) { received = std::current_exception(); }
		QVERIFY(received == fault); QVERIFY(!song->reportControllerNotificationFailure(fault));
	}

	void destructionNotificationsStayInsideQt_data()
	{
		QTest::addColumn<bool>("explicitRemoval"); QTest::addColumn<int>("kind");
		QTest::newRow("remove-standard") << true << 1; QTest::newRow("remove-nonstandard") << true << 2;
		QTest::newRow("delete-standard") << false << 1; QTest::newRow("delete-nonstandard") << false << 2;
	}
	void destructionNotificationsStayInsideQt()
	{
		QFETCH(bool, explicitRemoval); QFETCH(int, kind); auto song = Engine::getSong();
		const QPointer<Controller> target(new LfoController(song)); song->addController(target.data());
		const auto fault = failure(kind); int reports = 0;
		const auto connection = connect(song, &Song::controllerRosterChanged, this, [&] {
			if (target) { return; } ++reports; QVERIFY(song->reportControllerNotificationFailure(fault));
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); std::exception_ptr received;
		QSignalSpy later(song, &Song::controllerRosterChanged);
		if (!explicitRemoval) { QTest::ignoreMessage(QtWarningMsg, "Native controller roster notification failed during destruction"); }
		try { if (explicitRemoval) { song->removeController(target.data()); } else { delete target.data(); } }
		catch (...) { received = std::current_exception(); }
		QVERIFY(!target); QCOMPARE(reports, 1); QVERIFY(song->controllers().empty());
		QCOMPARE(later.size(), explicitRemoval ? 2 : 1); QVERIFY(received == (explicitRemoval ? fault : std::exception_ptr{}));
		QVERIFY(!song->reportControllerNotificationFailure(fault));
	}

	void retiredPayloadDoesNotCancelTheAcceptedFailure_data()
	{ QTest::addColumn<bool>("adding"); QTest::newRow("added") << true; QTest::newRow("removed") << false; }
	void retiredPayloadDoesNotCancelTheAcceptedFailure()
	{
		QFETCH(bool, adding); auto song = Engine::getSong(); const QPointer<Controller> target(new LfoController(song));
		if (!adding) { song->addController(target.data()); }
		const auto fault = failure(1); int delivered = 0;
		const auto receiver = [&](Controller* controller) { delete controller; QVERIFY(song->reportControllerNotificationFailure(fault)); };
		const auto connection = adding ? connect(song, &Song::controllerAdded, this, receiver, Qt::DirectConnection)
			: connect(song, &Song::controllerRemoved, this, receiver, Qt::DirectConnection);
		const auto later = adding ? connect(song, &Song::controllerAdded, this, [&] { ++delivered; }, Qt::DirectConnection)
			: connect(song, &Song::controllerRemoved, this, [&] { ++delivered; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); QObject::disconnect(later); }); std::exception_ptr received;
		try { if (adding) { song->addController(target.data()); } else { song->removeController(target.data()); } }
		catch (...) { received = std::current_exception(); }
		QVERIFY(received == fault); QCOMPARE(delivered, 1); QVERIFY(!target); QVERIFY(song->controllers().empty());
		QVERIFY(!song->reportControllerNotificationFailure(fault));
	}

	void blockedSignalsDoNotLeaveAReportingFrame()
	{
		auto song = Engine::getSong(); int observers = 0;
		const auto connection = connect(song, &Song::controllerAdded, this, [&] { ++observers; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto restore = qScopeGuard([&] { song->blockSignals(false); });
		song->blockSignals(true); auto controller = new LfoController(song); song->addController(controller); song->removeController(controller);
		QCOMPARE(observers, 0); QVERIFY(song->signalsBlocked()); QVERIFY(song->controllers().empty());
		QVERIFY(!song->reportControllerNotificationFailure(failure(1)));
	}
};
QTEST_GUILESS_MAIN(SongControllerNotificationTest)
#include "SongControllerNotificationTest.moc"

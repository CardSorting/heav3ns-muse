/* Native model faults occur only after Qt notification delivery returns.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "ModelValueNotificationTestUtils.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"

using namespace lmms;
namespace
{
void fail(int kind)
{
	if (kind == 1) { throw std::runtime_error("native model notification fixture"); }
	if (kind == 2) { throw 251; }
}
}
class ModelValueNotificationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid());
		ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false);
		Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase()
	{
		Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free();
	}
	void init()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		Engine::projectJournal()->setJournalling(false);
	}

	void faultsFollowAllDirectObserversAndPermitRetry_data()
	{
		QTest::addColumn<bool>("automated"); QTest::addColumn<int>("failure");
		for (bool automated : {false, true}) { for (int failure : {1, 2})
		{
			const auto label = QByteArray::number(automated) + '-' + QByteArray::number(failure);
			QTest::newRow(label.constData()) << automated << failure;
		} }
	}
	void faultsFollowAllDirectObserversAndPermitRetry()
	{
		QFETCH(bool, automated); QFETCH(int, failure); FloatModel model(42, 0, 100, 0); int delivered = 0, caught = 0;
		QObject::connect(&model, &AutomatableModel::dataChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		ModelValueNotificationProbe fault(&model, [&] { QCOMPARE(delivered, 2); fail(failure); });
		QObject::connect(&model, &AutomatableModel::dataChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		try { model.setValue(70, automated); }
		catch (const std::runtime_error&) { caught = 1; }
		catch (int value) { QCOMPARE(value, 251); caught = 2; }
		QCOMPARE(caught, failure); QCOMPARE(delivered, 2); QCOMPARE(model.storedValue(), 70.f);
		fault.reset(); model.setValue(80, automated); QCOMPARE(delivered, 4); QCOMPARE(model.storedValue(), 80.f);
	}

	void unchangedNotificationsKeepNativeAutomationSemantics_data()
	{
		QTest::addColumn<bool>("automated"); QTest::addColumn<bool>("includeUnchanged");
		for (bool automated : {false, true}) { for (bool includeUnchanged : {false, true})
		{
			const auto label = QByteArray::number(automated) + '-' + QByteArray::number(includeUnchanged);
			QTest::newRow(label.constData()) << automated << includeUnchanged;
		} }
	}
	void unchangedNotificationsKeepNativeAutomationSemantics()
	{
		QFETCH(bool, automated); QFETCH(bool, includeUnchanged); FloatModel model(42, 0, 100, 0); int checkpoints = 0;
		QSignalSpy changed(&model, &AutomatableModel::dataChanged), unchanged(&model, &AutomatableModel::dataUnchanged);
		ModelValueNotificationProbe probe(&model, [&] { QCOMPARE(unchanged.size(), 1); ++checkpoints; }, includeUnchanged);
		model.setValue(42, automated); QCOMPARE(changed.size(), 0); QCOMPARE(unchanged.size(), automated ? 0 : 1);
		QCOMPARE(checkpoints, !automated && includeUnchanged ? 1 : 0);
	}

	void blockedNotificationsDoNotRunTheProbe_data()
	{ QTest::addColumn<bool>("changed"); QTest::newRow("changed") << true; QTest::newRow("unchanged") << false; }
	void blockedNotificationsDoNotRunTheProbe()
	{
		QFETCH(bool, changed); FloatModel model(42, 0, 100, 0); int checkpoints = 0;
		QSignalSpy delivered(&model, &AutomatableModel::dataChanged), unchanged(&model, &AutomatableModel::dataUnchanged);
		ModelValueNotificationProbe probe(&model, [&] { ++checkpoints; }, true);
		model.blockSignals(true); model.setValue(changed ? 70 : 42);
		QCOMPARE(checkpoints, 0); QCOMPARE(delivered.size(), 0); QCOMPARE(unchanged.size(), 0); QVERIFY(model.signalsBlocked());
		model.blockSignals(false); model.setValue(80); QCOMPARE(checkpoints, 1); QCOMPARE(delivered.size(), 1);
	}

	void pendingProbeOwnsItsCallableThroughRemoval_data()
	{
		QTest::addColumn<bool>("removeInObserver"); QTest::addColumn<int>("failure");
		for (bool removeInObserver : {false, true}) { for (int failure : {0, 1, 2})
		{
			const auto label = QByteArray::number(removeInObserver) + '-' + QByteArray::number(failure);
			QTest::newRow(label.constData()) << removeInObserver << failure;
		} }
	}
	void pendingProbeOwnsItsCallableThroughRemoval()
	{
		QFETCH(bool, removeInObserver); QFETCH(int, failure); FloatModel model(42, 0, 100, 0);
		auto witness = std::make_shared<int>(42); const std::weak_ptr<int> observed(witness);
		std::unique_ptr<ModelValueNotificationProbe> probe; int delivered = 0, checkpoints = 0, caught = 0; bool survived = false;
		QObject::connect(&model, &AutomatableModel::dataChanged, this, [&] {
			++delivered; if (removeInObserver) { probe.reset(); }
		}, Qt::DirectConnection);
		probe = std::make_unique<ModelValueNotificationProbe>(&model, [&, retained = witness] {
			++checkpoints; QCOMPARE(delivered, 2); probe.reset(); survived = retained && !observed.expired(); fail(failure);
		});
		witness.reset();
		QObject::connect(&model, &AutomatableModel::dataChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		try { model.setValue(70); }
		catch (const std::runtime_error&) { caught = 1; }
		catch (int value) { QCOMPARE(value, 251); caught = 2; }
		QCOMPARE(caught, failure); QCOMPARE(checkpoints, 1); QVERIFY(survived); QVERIFY(observed.expired()); QVERIFY(!probe);
		model.setValue(80); QCOMPARE(checkpoints, 1); QCOMPARE(delivered, 4);
	}

	void receiverRetirementSuppressesThePendingProbe()
	{
		auto model = std::make_unique<FloatModel>(42, 0, 100, 0); const QPointer<AutomatableModel> weak(model.get()); int checkpoints = 0;
		ModelValueNotificationProbe probe(model.get(), [&] { ++checkpoints; });
		QObject::connect(model.get(), &AutomatableModel::dataChanged, this, [&] { model.reset(); }, Qt::DirectConnection);
		model->setValue(70); QVERIFY(!weak); QCOMPARE(checkpoints, 0);
	}

	void lexicalScopesRestoreThePriorTarget()
	{
		FloatModel first(42, 0, 100, 0), second(42, 0, 100, 0); int outer = 0, inner = 0;
		ModelValueNotificationProbe previous(&first, [&] { ++outer; }); first.setValue(50);
		{
			ModelValueNotificationProbe nested(&second, [&] { ++inner; });
			first.setValue(60); second.setValue(60); QCOMPARE(outer, 1); QCOMPARE(inner, 1);
		}
		first.setValue(70); second.setValue(70); QCOMPARE(outer, 2); QCOMPARE(inner, 1);
		previous.reset(); first.setValue(80); QCOMPARE(outer, 2);
	}

	void anotherThreadHasAnIndependentProbeScope()
	{
		// Allocate/register and retire on the journal owner thread. Only the
		// detached model's automated value updates run on its temporary owner.
		FloatModel model(42, 0, 100, 0); const auto ownerThread = model.thread();
		std::atomic<int> outer{0}, inner{0}; std::exception_ptr failure;
		ModelValueNotificationProbe previous(&model, [&] { ++outer; });
		auto worker = std::unique_ptr<QThread>(QThread::create([&] {
			try
			{
				model.setValue(60, true);
				{ ModelValueNotificationProbe nested(&model, [&] { ++inner; }); model.setValue(70, true); }
				model.setValue(75, true);
			}
			catch (...) { failure = std::current_exception(); }
			model.moveToThread(ownerThread);
		}));
		model.moveToThread(worker.get()); worker->start(); QVERIFY(worker->wait());
		QVERIFY(!failure); QCOMPARE(model.thread(), ownerThread); QCOMPARE(outer.load(), 0); QCOMPARE(inner.load(), 1);
		model.setValue(80, true); QCOMPARE(outer.load(), 1); QCOMPARE(inner.load(), 1);
	}

	void aPeerFaultStopsPropagationAfterItsQtObservers_data()
	{
		QTest::addColumn<bool>("automated"); QTest::addColumn<int>("failure");
		for (bool automated : {false, true}) { for (int failure : {1, 2})
		{
			const auto label = QByteArray::number(automated) + '-' + QByteArray::number(failure);
			QTest::newRow(label.constData()) << automated << failure;
		} }
	}
	void aPeerFaultStopsPropagationAfterItsQtObservers()
	{
		QFETCH(bool, automated); QFETCH(int, failure); FloatModel first(42, 0, 100, 0), second(42, 0, 100, 0), last(42, 0, 100, 0);
		QVERIFY(first.linkToModel(&second)); QVERIFY(first.linkToModel(&last)); int delivered = 0, caught = 0;
		QObject::connect(&second, &AutomatableModel::dataChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		ModelValueNotificationProbe fault(&second, [&] { QCOMPARE(delivered, 2); fail(failure); });
		QObject::connect(&second, &AutomatableModel::dataChanged, this, [&] { ++delivered; }, Qt::DirectConnection);
		QSignalSpy untouched(&last, &AutomatableModel::dataChanged);
		try { first.setValue(70, automated); }
		catch (const std::runtime_error&) { caught = 1; }
		catch (int value) { QCOMPARE(value, 251); caught = 2; }
		QCOMPARE(caught, failure); QCOMPARE(delivered, 2); QCOMPARE(untouched.size(), 0);
		QCOMPARE(first.storedValue(), 70.f); QCOMPARE(second.storedValue(), 70.f); QCOMPARE(last.storedValue(), 42.f);
		fault.reset(); first.setValue(80, automated); QCOMPARE(last.storedValue(), 80.f); QCOMPARE(untouched.size(), 1);
	}
};
QTEST_GUILESS_MAIN(ModelValueNotificationTest)
#include "ModelValueNotificationTest.moc"

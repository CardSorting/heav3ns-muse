/* Native model setter callers under synchronous retirement and failure.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "MeterModel.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "TempoSyncKnobModel.h"

using namespace lmms;
namespace
{
std::unique_ptr<FloatModel> model() { return std::make_unique<FloatModel>(42, 0, 100, 0, nullptr); }
std::unique_ptr<TempoSyncKnobModel> tempo() { return std::make_unique<TempoSyncKnobModel>(42, 0, 1000, 0, 1000, nullptr); }
QDomElement settings(QDomDocument& document)
{
	auto root = document.createElement("controls"); document.appendChild(root); return root;
}
}
class ModelSetterOwnershipTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void initializationStopsAfterValueCallbackRetiresReceiver()
	{
		auto value = model(); const QPointer<AutomatableModel> weak(value.get()); int initialized = 0;
		QObject::connect(value.get(), &AutomatableModel::initValueChanged, this, [&] { ++initialized; }, Qt::DirectConnection);
		QObject::connect(value.get(), &AutomatableModel::dataChanged, this, [&] { value.reset(); }, Qt::DirectConnection);
		value->setInitValue(70); QVERIFY(!weak); QCOMPARE(initialized, 0);
	}

	void initializationRestoresJournallingAfterReturnedExceptions_data()
	{
		QTest::addColumn<bool>("standard"); QTest::addColumn<bool>("enabled");
		QTest::newRow("standard-enabled") << true << true; QTest::newRow("standard-disabled") << true << false;
		QTest::newRow("nonstandard-enabled") << false << true; QTest::newRow("nonstandard-disabled") << false << false;
	}
	void initializationRestoresJournallingAfterReturnedExceptions()
	{
		QFETCH(bool, standard); QFETCH(bool, enabled); auto value = model(); value->setJournalling(enabled);
		ModelValueNotificationProbe fault(value.get(), [&] { if (standard) { throw std::runtime_error("fixture init callback"); } throw 42; });
		try { value->setInitValue(70); QFAIL("Callback must fail"); } catch (const std::runtime_error&) { QVERIFY(standard); } catch (int) { QVERIFY(!standard); }
		fault.reset(); QCOMPARE(value->isJournalling(), enabled); value->setInitValue(80); QCOMPARE(value->storedValue(), 80.f); QCOMPARE(value->initValue(), 80.f);
	}

	void rangeStopsAfterPropertyOrValueRetirement_data()
	{ QTest::addColumn<bool>("property"); QTest::newRow("step-notification") << true; QTest::newRow("fitted-value-notification") << false; }
	void rangeStopsAfterPropertyOrValueRetirement()
	{
		QFETCH(bool, property); auto value = model(); const QPointer<AutomatableModel> weak(value.get()); int properties = 0;
		QObject::connect(value.get(), &AutomatableModel::propertiesChanged, this, [&] { ++properties; if (property) { value.reset(); } }, Qt::DirectConnection);
		if (!property) { QObject::connect(value.get(), &AutomatableModel::dataChanged, this, [&] { value.reset(); }, Qt::DirectConnection); }
		value->setRange(0, 20, 1); QVERIFY(!weak); QCOMPARE(properties, 1);
	}

	void nodeLoadingStopsBeforeScaleAfterValueRetirement()
	{
		QDomDocument document; auto root = settings(document); auto node = document.createElement("value"); node.setAttribute("value", 70); node.setAttribute("scale_type", "log"); root.appendChild(node);
		auto value = model(); const QPointer<AutomatableModel> weak(value.get()); QObject::connect(value.get(), &AutomatableModel::dataChanged, this, [&] { value.reset(); }, Qt::DirectConnection);
		value->loadSettings(root, "value"); QVERIFY(!weak);
	}

	void bindingIsPreparedBeforeAttachAndOldBindingCleanupIsGuarded_data()
	{ QTest::addColumn<bool>("cleanup"); QTest::newRow("attachment-retirement") << false; QTest::newRow("old-binding-destruction") << true; }
	void bindingIsPreparedBeforeAttachAndOldBindingCleanupIsGuarded()
	{
		QFETCH(bool, cleanup); auto song = Engine::getSong(); auto controller = new LfoController(song); song->addController(controller);
		auto value = model(); value->setControllerConnection(new ControllerConnection(controller)); const QPointer<ControllerConnection> old(value->controllerConnection()); const QPointer<AutomatableModel> weak(value.get());
		QDomDocument document; auto root = settings(document); root.setAttribute("value", 70); auto bindings = document.createElement("connection"); root.appendChild(bindings); auto binding = document.createElement("value"); binding.setAttribute("id", 0); bindings.appendChild(binding);
		bool prepared = false;
		QObject::connect(value.get(), &AutomatableModel::dataChanged, this, [&] { prepared = value->controllerConnection()->getController() == controller; if (!cleanup) { value.reset(); } }, Qt::DirectConnection);
		if (cleanup) { QObject::connect(old.data(), &QObject::destroyed, this, [&] { value.reset(); }, Qt::DirectConnection); }
		value->loadSettings(root, "value"); QVERIFY(prepared); QVERIFY(!weak); QVERIFY(!old);
	}

	void attachmentFailureReleasesPreviousBindingAndAllowsAnotherLoad()
	{
		auto value = model(); value->setControllerConnection(new ControllerConnection(nullptr)); const QPointer<ControllerConnection> old(value->controllerConnection());
		QDomDocument document; auto root = settings(document); root.setAttribute("value", 70); auto bindings = document.createElement("connection"); root.appendChild(bindings); auto binding = document.createElement("value"); binding.setAttribute("id", 0); bindings.appendChild(binding);
		ModelValueNotificationProbe fault(value.get(), [] { throw std::runtime_error("fixture binding attachment"); });
		try { value->loadSettings(root, "value"); QFAIL("Attachment must fail"); } catch (const std::runtime_error&) {}
		fault.reset(); QVERIFY(!old); QVERIFY(value->controllerConnection()); value->loadSettings(root, "value"); QCOMPARE(value->storedValue(), 70.f);
	}

	void meterStopsBeforeDenominatorAfterNumeratorRetirement_data()
	{ QTest::addColumn<bool>("load"); QTest::newRow("reset") << false; QTest::newRow("load") << true; }
	void meterStopsBeforeDenominatorAfterNumeratorRetirement()
	{
		QFETCH(bool, load); auto meter = std::make_unique<MeterModel>(nullptr); meter->setNumerator(3); meter->setDenominator(8); const QPointer<MeterModel> weak(meter.get()); int denominator = 0;
		QObject::connect(&meter->denominatorModel(), &AutomatableModel::dataChanged, this, [&] { ++denominator; }, Qt::DirectConnection);
		QObject::connect(&meter->numeratorModel(), &AutomatableModel::dataChanged, this, [&] { meter.reset(); }, Qt::DirectConnection);
		QDomDocument document; auto root = settings(document); root.setAttribute("meter_numerator", 7); root.setAttribute("meter_denominator", 16);
		if (load) { meter->loadSettings(root, "meter"); } else { meter->reset(); } QVERIFY(!weak); QCOMPARE(denominator, 0);
	}

	void tempoStopsAfterValueOrModeNotificationRetirement_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("value") << 0; QTest::newRow("mode") << 1; QTest::newRow("scale-recalculation") << 2; }
	void tempoStopsAfterValueOrModeNotificationRetirement()
	{
		QFETCH(int, phase); auto value = tempo(); if (phase == 2) { value->setSyncMode(TempoSyncKnobModel::SyncMode::QuarterNote); } const QPointer<AutomatableModel> weak(value.get()); int scales = 0;
		QObject::connect(value.get(), &TempoSyncKnobModel::scaleChanged, this, [&] { ++scales; }, Qt::DirectConnection);
		if (phase == 1) { QObject::connect(value.get(), &TempoSyncKnobModel::syncModeChanged, this, [&] { value.reset(); }, Qt::DirectConnection); }
		else { QObject::connect(value.get(), &AutomatableModel::dataChanged, this, [&] { value.reset(); }, Qt::DirectConnection); }
		if (phase == 2) { value->setScale(2000); } else { value->setSyncMode(TempoSyncKnobModel::SyncMode::QuarterNote); } QVERIFY(!weak); QCOMPARE(scales, 0);
	}

	void tempoRestoresJournallingAfterReturnedExceptions_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void tempoRestoresJournallingAfterReturnedExceptions()
	{
		QFETCH(bool, standard); auto value = tempo(); value->setJournalling(true);
		ModelValueNotificationProbe fault(value.get(), [&] { if (standard) { throw std::runtime_error("fixture tempo callback"); } throw 42; });
		try { value->setSyncMode(TempoSyncKnobModel::SyncMode::QuarterNote); QFAIL("Callback must fail"); } catch (const std::runtime_error&) { QVERIFY(standard); } catch (int) { QVERIFY(!standard); }
		fault.reset(); QVERIFY(value->isJournalling()); value->setScale(2000); QVERIFY(value->isJournalling()); QCOMPARE(value->syncMode(), TempoSyncKnobModel::SyncMode::QuarterNote);
	}

	void tempoLoadingStopsAfterBaseOrMeterRetirement_data()
	{ QTest::addColumn<bool>("meter"); QTest::newRow("base") << false; QTest::newRow("custom-meter") << true; }
	void tempoLoadingStopsAfterBaseOrMeterRetirement()
	{
		QFETCH(bool, meter); auto value = tempo(); const QPointer<AutomatableModel> weak(value.get()); QDomDocument document; auto root = settings(document);
		root.setAttribute("speed", meter ? 42 : 70); root.setAttribute("speed_numerator", 3); root.setAttribute("speed_denominator", 8); root.setAttribute("speed_syncmode", 4);
		auto target = meter ? static_cast<AutomatableModel*>(&value->getCustomMeterModel().numeratorModel()) : static_cast<AutomatableModel*>(value.get());
		QObject::connect(target, &AutomatableModel::dataChanged, this, [&] { value.reset(); }, Qt::DirectConnection); value->loadSettings(root, "speed"); QVERIFY(!weak);
	}

	void structuredTempoRetirementRefusesSuccessAndRuntimeRestores_data()
	{
		QTest::addColumn<int>("phase"); QTest::addColumn<bool>("runtime");
		QTest::newRow("native-numerator") << 0 << false; QTest::newRow("native-denominator") << 1 << false; QTest::newRow("native-sync") << 2 << false;
		QTest::newRow("runtime-numerator") << 0 << true; QTest::newRow("runtime-denominator") << 1 << true; QTest::newRow("runtime-sync") << 2 << true;
	}
	void structuredTempoRetirementRefusesSuccessAndRuntimeRestores()
	{
		QFETCH(int, phase); QFETCH(bool, runtime); QTemporaryDir directory; QString error; std::unique_ptr<StudioRuntime> studio;
		if (runtime) { studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY(studio->open(error)); }
		auto song = Engine::getSong(); auto controller = new LfoController(song); song->addController(controller); auto speed = controller->findChild<TempoSyncKnobModel*>(); QVERIFY(speed);
		if (runtime) { QVERIFY(studio->process({{"id", "baseline"}, {"op", "song.set"}, {"args", QJsonObject{{"tempo", song->getTempo()}}}}).value("ok").toBool()); }
		const QPointer<TempoSyncKnobModel> old(speed); const auto stored = speed->storedValue();
		auto target = phase == 0 ? static_cast<AutomatableModel*>(&speed->getCustomMeterModel().numeratorModel()) : phase == 1 ? static_cast<AutomatableModel*>(&speed->getCustomMeterModel().denominatorModel()) : static_cast<AutomatableModel*>(speed);
		const auto connection = QObject::connect(target, &AutomatableModel::dataChanged, this, [song, controller] { song->removeController(controller); }, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const QJsonObject args{{"parameter", static_cast<int>(speed->id())}, {"mode", 8}, {"customNumerator", 3}, {"customDenominator", 8}};
		const auto result = runtime ? studio->process({{"id", "tempo-retire"}, {"op", "parameter.sync"}, {"args", args}}) : StudioCapabilities::execute("parameter.sync", args);
		QVERIFY(!result.value("ok").toBool()); QVERIFY(!old);
		if (runtime) { QCOMPARE(song->controllers().size(), std::size_t{1}); auto restored = song->controllers().front()->findChild<TempoSyncKnobModel*>(); QVERIFY(restored); QCOMPARE(restored->storedValue(), stored); QCOMPARE(restored->syncMode(), TempoSyncKnobModel::SyncMode::None); QVERIFY(!studio->stopping()); }
		else { QVERIFY(song->controllers().empty()); }
	}

	void ordinaryTempoAndInitializationRetainNativeValuesAndSignals()
	{
		auto value = model(); QSignalSpy initialized(value.get(), &AutomatableModel::initValueChanged); value->setInitValue(170); QCOMPARE(value->storedValue(), 100.f); QCOMPARE(value->initValue(), 100.f); QCOMPARE(initialized.size(), 1); QCOMPARE(initialized.at(0).at(0).toFloat(), 170.f);
		auto synced = tempo(); int modes = 0; QObject::connect(synced.get(), &TempoSyncKnobModel::syncModeChanged, this, [&] { ++modes; QVERIFY(synced->isJournalling()); }); synced->setJournalling(true);
		synced->setSyncMode(TempoSyncKnobModel::SyncMode::QuarterNote); QVERIFY(qAbs(synced->storedValue() - 60000.f / Engine::getSong()->getTempo()) < 0.001f); QVERIFY(synced->isJournalling()); QCOMPARE(modes, 1);
		synced->setScale(2000); QVERIFY(qAbs(synced->storedValue() - 30000.f / Engine::getSong()->getTempo()) < 0.001f); QCOMPARE(modes, 1);
	}
};
QTEST_GUILESS_MAIN(ModelSetterOwnershipTest)
#include "ModelSetterOwnershipTest.moc"

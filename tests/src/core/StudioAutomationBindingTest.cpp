/* Checked native automation bindings and first-point publication.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QMetaMethod>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class BindingController final : public Controller
{
public:
	BindingController() : Controller(ControllerType::Dummy, nullptr, "fixture binding controller") { }
	std::function<void()> onRead;
	float reading = 0.25f;
	float currentValue(int) override { if (onRead) { onRead(); } return reading; }
};
class BindingModel final : public FloatModel
{
public:
	BindingModel() : FloatModel(42, 0, 100, 1, nullptr) { }
	bool supported = true;
	std::function<void()> onConnect;
	bool supportsAutomation() const override { return supported; }
protected:
	void connectNotify(const QMetaMethod& signal) override
	{
		QObject::connectNotify(signal);
		if (onConnect && signal.methodSignature() == "destroyed(lmms::jo_id_t)") { onConnect(); }
	}
};
}
class StudioAutomationBindingTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QPointer<AutomationClip> clip;
	InstrumentTrack* instrument = nullptr;
	bool journalling = false;
	void seed()
	{
		instrument = new InstrumentTrack(Engine::getSong());
		clip = static_cast<AutomationClip*>((new AutomationTrack(Engine::getSong()))->createClip(0));
		clip->addObject(instrument->volumeModel()); clip->putValues(0, 10, 20, false, true); clip->putValues(96, 80, 90, false, true);
		clip->setProgressionType(AutomationClip::ProgressionType::CubicHermite); clip->setTension(0.345678f);
		clip->setNodeTangents(96, 0.125f, -0.375f, true); clip->setAutoResize(false); clip->changeLength(192);
	}
	void empty() { clip->clearObjects(); clip->clear(); }
	AutomatableModel* target(bool connected) const { return connected ? instrument->panningModel() : instrument->volumeModel(); }
	void actions() { QTest::addColumn<bool>("connected"); QTest::newRow("connect") << true; QTest::newRow("disconnect") << false; }
	QJsonObject args(bool connected) const { return {{"track", 1}, {"clip", 0}, {"parameter", int(target(connected)->id())}, {"summary", true}}; }
	QString operation(bool connected) const { return connected ? "automation.connect" : "automation.disconnect"; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& a = {}) const { return {{"id", id}, {"op", op}, {"args", a}}; }
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
	void cleanup() { if (clip) { clip->setRecording(false); clip->discardRecordingTake(); } Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void firstConnectionPublishesBindingAndNativeSeedTogether_data()
	{ QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true; }
	void firstConnectionPublishesBindingAndNativeSeedTogether()
	{
		QFETCH(bool, logarithmic); empty(); FloatModel model(40, 1, 100, 0, nullptr); model.setScaleLogarithmic(logarithmic);
		const float expected = model.inverseScaledValue(model.value()); int notifications = 0;
		const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			++notifications; QCOMPARE(clip->objects().size(), std::size_t(1)); QCOMPARE(clip->firstObject(), static_cast<const AutomatableModel*>(&model));
			QCOMPARE(clip->getTimeMap().size(), 1); QCOMPARE(clip->valueAt(0), expected);
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY2(clip->editBinding(&model, true, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications, 1);
		clip->getTimeMap()[0].setInValue(45); QCOMPARE(clip->valueAt(0), 45.f);
	}
	void additionalConnectionAndDisconnectionPreserveCurve_data() { actions(); }
	void additionalConnectionAndDisconnectionPreserveCurve()
	{
		QFETCH(bool, connected); clip->setAutoResize(true); clip->changeLength(576); clip->setStartTimeOffset(-12); const auto before = clip->getTimeMap();
		QSignalSpy notifications(clip.data(), &Clip::dataChanged); bool changed = false; QString error;
		QVERIFY2(clip->editBinding(target(connected), connected, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications.count(), 1);
		QVERIFY(clip->getTimeMap() == before); QCOMPARE(clip->objects().size(), connected ? std::size_t(2) : std::size_t(0));
		QCOMPARE(clip->length().getTicks(), 576); QCOMPARE(clip->startTimeOffset().getTicks(), -12); QVERIFY(clip->getAutoResize());
		QCOMPARE(clip->getTension(), 0.345678f); QCOMPARE(clip->progressionType(), AutomationClip::ProgressionType::CubicHermite);
	}
	void duplicateConnectionAndAbsentDisconnectionDoNotNotify_data() { actions(); }
	void duplicateConnectionAndAbsentDisconnectionDoNotNotify()
	{
		QFETCH(bool, connected); const auto before = clip->getTimeMap(); QSignalSpy notifications(clip.data(), &Clip::dataChanged); bool changed = true; QString error;
		auto model = connected ? instrument->volumeModel() : instrument->panningModel();
		QVERIFY(clip->editBinding(model, connected, changed, error)); QVERIFY(!changed); QCOMPARE(notifications.count(), 0); QVERIFY(clip->getTimeMap() == before); QCOMPARE(clip->objects().size(), std::size_t(1));
	}
	void duplicateNativeBindingsAreAllRemoved()
	{
		QVERIFY(clip->addObject(instrument->volumeModel(), false)); const auto before = clip->getTimeMap(); bool changed = false; QString error;
		QVERIFY(clip->editBinding(instrument->volumeModel(), false, changed, error)); QVERIFY(changed); QVERIFY(clip->objects().empty()); QVERIFY(clip->getTimeMap() == before);
	}
	void pendingOnlyDisconnectionDoesNotReappearDuringResolution()
	{
		QDomDocument doc; auto root = doc.createElement("automationclip"); doc.appendChild(root); clip->saveSettings(doc, root); clip->loadSettings(root);
		QVERIFY(clip->objects().empty()); QSignalSpy notifications(clip.data(), &Clip::dataChanged); bool changed = true; QString error;
		QVERIFY(clip->editBinding(instrument->volumeModel(), false, changed, error)); QVERIFY(!changed); QCOMPARE(notifications.count(), 0);
		AutomationClip::resolveAllIDs(); QVERIFY(clip->objects().empty());
	}
	void busyCurvesRefuseBindingChanges_data()
	{
		QTest::addColumn<bool>("connected"); QTest::addColumn<int>("busy");
		QTest::newRow("connect-recording") << true << 0; QTest::newRow("disconnect-recording") << false << 0;
		QTest::newRow("connect-reserved") << true << 1; QTest::newRow("disconnect-reserved") << false << 1;
		QTest::newRow("connect-dragging") << true << 2; QTest::newRow("disconnect-dragging") << false << 2;
	}
	void busyCurvesRefuseBindingChanges()
	{
		QFETCH(bool, connected); QFETCH(int, busy); if (busy == 0) { clip->setRecording(true); } if (busy == 1) { clip->reserveRecordingTake(16, 0); }
		if (busy == 2) { clip->setDragValue(0, 10, false, false); }
		const auto finish = qScopeGuard([&] { clip->setRecording(false); clip->discardRecordingTake(); if (busy == 2) { clip->applyDragValue(); } });
		const auto before = clip->objects(); bool changed = true; QString error;
		QVERIFY(!clip->editBinding(target(connected), connected, changed, error)); QVERIFY(!changed); QVERIFY(clip->objects() == before);
	}
	void invalidAdmissionRefusesBeforePublication_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("null") << 0; QTest::newRow("unsupported") << 1; QTest::newRow("zero-meter") << 2; }
	void invalidAdmissionRefusesBeforePublication()
	{
		QFETCH(int, failure); const int meter = TimePos::ticksPerBar(); const auto restore = qScopeGuard([&] { TimePos::setTicksPerBar(meter); });
		BindingModel model; model.supported = failure != 1; if (failure == 2) { TimePos::setTicksPerBar(0); }
		const auto before = clip->objects(); bool changed = true; QString error;
		QVERIFY(!clip->editBinding(failure == 0 ? nullptr : &model, true, changed, error)); QVERIFY(!changed); QVERIFY(clip->objects() == before); QVERIFY(!error.isEmpty());
	}
	void unsupportedExistingTargetCanStillBeDisconnected()
	{
		BindingModel model; QVERIFY(clip->addObject(&model)); model.supported = false; bool changed = false; QString error;
		QVERIFY2(clip->editBinding(&model, false, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(clip->objects().size(), std::size_t(1));
	}
	void firstBindingUsesNativeSongAndPatternSizing_data()
	{
		QTest::addColumn<bool>("pattern"); QTest::addColumn<bool>("automatic"); QTest::newRow("song-auto") << false << true;
		QTest::newRow("song-manual") << false << false; QTest::newRow("pattern-auto") << true << true; QTest::newRow("pattern-manual") << true << false;
	}
	void firstBindingUsesNativeSongAndPatternSizing()
	{
		QFETCH(bool, pattern); QFETCH(bool, automatic); if (pattern)
		{
			new PatternTrack(Engine::getSong()); auto row = Track::create(Track::Type::Instrument, Engine::patternStore());
			row->getClips().at(0)->setAutoResize(false); row->getClips().at(0)->changeLength(384);
			clip = static_cast<AutomationClip*>(Track::create(Track::Type::Automation, Engine::patternStore())->getClips().at(0));
		}
		empty(); clip->setAutoResize(automatic); clip->changeLength(576); clip->setStartTimeOffset(-12); bool changed = false; QString error;
		QVERIFY2(clip->editBinding(instrument->volumeModel(), true, changed, error), qPrintable(error)); QVERIFY(changed);
		QCOMPARE(clip->length().getTicks(), automatic ? (pattern ? 384 : 192) : 576); QCOMPARE(clip->startTimeOffset().getTicks(), automatic && !pattern ? 0 : -12);
	}
	void nativeConstructorInterferenceRefusesFirstBinding_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("point") << 0; QTest::newRow("range") << 1; QTest::newRow("retire") << 2; QTest::newRow("throw") << 3; QTest::newRow("target-retire") << 4; }
	void nativeConstructorInterferenceRefusesFirstBinding()
	{
		QFETCH(int, failure); empty(); auto model = std::make_unique<BindingModel>(); bool invoked = false; const auto connection = connect(Engine::getSong(), &Song::lengthChanged, this, [&] {
			if (invoked) { return; } invoked = true;
			if (failure == 0) { clip->putValue(48, 55, false, true); } if (failure == 1) { model->setStep(0.5f); }
			if (failure == 2) { delete clip.data(); } if (failure == 3) { throw std::runtime_error("fixture binding construction"); }
			if (failure == 4) { model.reset(); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!clip->editBinding(model.get(), true, changed, error)); QVERIFY(invoked); QVERIFY(!changed); if (clip) { QVERIFY(clip->objects().empty()); QVERIFY(!clip->getTimeMap().contains(0)); }
	}
	void controllerReadIsGuardedBeforePublishingFirstBinding_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("point") << 0; QTest::newRow("receiver-retire") << 1; QTest::newRow("throw") << 2; }
	void controllerReadIsGuardedBeforePublishingFirstBinding()
	{
		QFETCH(int, failure); empty(); auto model = instrument->volumeModel(); BindingController controller;
		auto nativeConnection = std::make_unique<ControllerConnection>(&controller); model->setControllerConnection(nativeConnection.get());
		const auto finish = qScopeGuard([&] { model->setControllerConnection(nullptr); }); bool invoked = false;
		controller.onRead = [&] { invoked = true; if (failure == 0) { clip->putValue(48, 55, false, true); } if (failure == 1) { delete clip.data(); }
			if (failure == 2) { throw std::runtime_error("fixture binding controller read"); } };
		bool changed = true; QString error; QVERIFY(!clip->editBinding(model, true, changed, error)); QVERIFY(invoked); QVERIFY(!changed);
		if (clip) { QVERIFY(clip->objects().empty()); QVERIFY(!clip->getTimeMap().contains(0)); }
	}
	void firstBindingReadsTheNativeControllerValue()
	{
		empty(); BindingModel model; BindingController controller; auto nativeConnection = std::make_unique<ControllerConnection>(&controller);
		model.setControllerConnection(nativeConnection.get()); const auto finish = qScopeGuard([&] { model.setControllerConnection(nullptr); });
		int reads = 0; controller.onRead = [&] { ++reads; }; bool changed = false; QString error;
		QVERIFY2(clip->editBinding(&model, true, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(reads, 1);
		QCOMPARE(clip->valueAt(0), 25.f); QCOMPARE(model.storedValue(), 42.f);
	}
	void nonfiniteControllerSeedRefusesBeforePublication_data()
	{ QTest::addColumn<float>("value"); QTest::newRow("nan") << std::numeric_limits<float>::quiet_NaN(); QTest::newRow("infinity") << std::numeric_limits<float>::infinity(); }
	void nonfiniteControllerSeedRefusesBeforePublication()
	{
		QFETCH(float, value); empty(); BindingModel model; BindingController controller; controller.reading = value;
		auto nativeConnection = std::make_unique<ControllerConnection>(&controller); model.setControllerConnection(nativeConnection.get());
		const auto finish = qScopeGuard([&] { model.setControllerConnection(nullptr); }); bool changed = true; QString error;
		QVERIFY(!clip->editBinding(&model, true, changed, error)); QVERIFY(!changed); QVERIFY(clip->objects().empty()); QVERIFY(clip->getTimeMap().isEmpty());
	}
	void finalNotificationFailuresAreDetected_data()
	{
		QTest::addColumn<bool>("connected"); QTest::addColumn<int>("failure");
		QTest::newRow("connect-throw") << true << 0; QTest::newRow("disconnect-throw") << false << 0;
		QTest::newRow("connect-tamper") << true << 1; QTest::newRow("disconnect-tamper") << false << 1;
		QTest::newRow("connect-retire") << true << 2; QTest::newRow("disconnect-retire") << false << 2;
		QTest::newRow("connect-range") << true << 3; QTest::newRow("disconnect-range") << false << 3;
	}
	void finalNotificationFailuresAreDetected()
	{
		QFETCH(bool, connected); QFETCH(int, failure); auto model = target(connected); bool invoked = false;
		const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			if (invoked) { return; } invoked = true; if (failure == 0) { throw std::runtime_error("fixture binding notification"); }
			if (failure == 1) { if (connected) { clip->clearObjects(); } else { clip->addObject(model); } }
			if (failure == 2) { delete clip.data(); } if (failure == 3) { model->setStep(0.5f); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!clip->editBinding(model, connected, changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!error.isEmpty());
	}
	void signalWiringInterferenceIsCheckedBeforeAutomaticSizing()
	{
		empty(); BindingModel model; clip->setAutoResize(true); clip->changeLength(576); clip->setStartTimeOffset(-12); bool invoked = false;
		model.onConnect = [&] { invoked = true; clip->setRecording(true); };
		bool changed = true; QString error; QVERIFY(!clip->editBinding(&model, true, changed, error)); QVERIFY(invoked); QVERIFY(!changed);
		QCOMPARE(clip->length().getTicks(), 576); QCOMPARE(clip->startTimeOffset().getTicks(), -12);
		QCOMPARE(clip->objects().size(), std::size_t(1)); QVERIFY(clip->getTimeMap().contains(0)); clip->setRecording(false);
	}
	void finalNotificationRunsWithoutTheCurveMutex_data() { actions(); }
	void finalNotificationRunsWithoutTheCurveMutex()
	{
		QFETCH(bool, connected); float value = -1; const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			std::thread reader([&] { value = clip->valueAt(0); }); reader.join();
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY(clip->editBinding(target(connected), connected, changed, error)); QVERIFY(changed); QCOMPARE(value, 10.f);
	}
	void signalWiringFollowsNativeBindingLifetime_data() { actions(); }
	void signalWiringFollowsNativeBindingLifetime()
	{
		QFETCH(bool, connected); auto model = std::make_unique<BindingModel>(); if (!connected) { clip->addObject(model.get()); }
		bool changed = false; QString error; QVERIFY(clip->editBinding(model.get(), connected, changed, error)); QVERIFY(changed);
		QSignalSpy notifications(clip.data(), &Clip::dataChanged); model.reset(); QCOMPARE(notifications.count(), connected ? 1 : 0); QCOMPARE(clip->objects().size(), std::size_t(1));
	}
	void cancellationAndInterferenceCannotPublish_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("cancel") << 0; QTest::newRow("point") << 1; QTest::newRow("dimension") << 2; }
	void cancellationAndInterferenceCannotPublish()
	{
		QFETCH(int, failure); bool invoked = false, timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		FileResourceIO::Scope work([&] { if (!invoked) { invoked = true; if (failure == 1) { clip->putValue(48, 55, false, true); } if (failure == 2) { instrument->panningModel()->setStep(0.5f); } } return failure == 0; }, false);
		bool changed = true; QString error; QVERIFY(!clip->editBinding(instrument->panningModel(), true, changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!timer); QCOMPARE(clip->objects().size(), std::size_t(1));
	}
	void structuredSongAndPatternRoutesUseCheckedBindings_data()
	{
		QTest::addColumn<bool>("connected"); QTest::addColumn<bool>("pattern"); QTest::newRow("song-connect") << true << false;
		QTest::newRow("song-disconnect") << false << false; QTest::newRow("pattern-connect") << true << true; QTest::newRow("pattern-disconnect") << false << true;
	}
	void structuredSongAndPatternRoutesUseCheckedBindings()
	{
		QFETCH(bool, connected); QFETCH(bool, pattern); auto a = args(connected); if (pattern)
		{
			new PatternTrack(Engine::getSong()); clip = static_cast<AutomationClip*>(Track::create(Track::Type::Automation, Engine::patternStore())->getClips().at(0));
			clip->addObject(instrument->volumeModel()); a.insert("container", "pattern"); a.insert("track", 0);
		}
		const auto receipt = StudioCapabilities::execute(operation(connected), a); QVERIFY(receipt.value("ok").toBool());
		QVERIFY(receipt.value("result").toObject().value(connected ? "connected" : "disconnected").toBool());
		QCOMPARE(clip->objects().size(), connected ? std::size_t(2) : std::size_t(0));
	}
	void sessionUndoRedoReopenAndReplay_data() { actions(); }
	void sessionUndoRedoReopenAndReplay()
	{
		QFETCH(bool, connected); QTemporaryDir directory; QString error; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); command = request("binding", operation(connected), args(connected));
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); replay(runtime, command, receipt);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QCOMPARE(clip->objects().size(), std::size_t(1));
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt);
		clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QCOMPARE(clip->objects().size(), connected ? std::size_t(2) : std::size_t(0));
		QCOMPARE(clip->getTimeMap().value(96).getOutTangent(), -0.375f); QCOMPARE(clip->length().getTicks(), 192);
	}
	void sessionRecoversReturnedFailureAndReplaysIt_data() { actions(); }
	void sessionRecoversReturnedFailureAndReplaysIt()
	{
		QFETCH(bool, connected); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed();
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); const auto command = request("reject", operation(connected), args(connected));
		bool injected = false;
		const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] { if (!injected) { injected = true; throw std::runtime_error("fixture session binding"); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto receipt = runtime.process(command); QVERIFY(injected); QVERIFY(!receipt.value("ok").toBool()); replay(runtime, command, receipt);
		clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QCOMPARE(clip->objects().size(), std::size_t(1));
		QCOMPARE(clip->getTimeMap().value(96).getOutTangent(), -0.375f); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioAutomationBindingTest)
#include "StudioAutomationBindingTest.moc"

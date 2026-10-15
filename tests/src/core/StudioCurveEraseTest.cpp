/* Prepared native automation removal and clearing.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;

class StudioCurveEraseTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QPointer<AutomationClip> clip;
	InstrumentTrack* instrument = nullptr;
	bool journalling = false;
	AutomationClip::CurveEdit options(bool clear) const
	{ AutomationClip::CurveEdit edit; if (clear) { edit.clear = true; } else { edit.removePoint = 96; } return edit; }
	QJsonObject args(bool clear) const
	{ QJsonObject a{{"track", 1}, {"clip", 0}}; if (!clear) { a.insert("position", 96); } return a; }
	QString operation(bool clear) const { return clear ? "automation.clear" : "automation.removePoint"; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& a = {}) const
	{ return {{"id", id}, {"op", op}, {"args", a}}; }
	void seed()
	{
		instrument = new InstrumentTrack(Engine::getSong());
		clip = static_cast<AutomationClip*>((new AutomationTrack(Engine::getSong()))->createClip(0));
		clip->addObject(instrument->volumeModel()); clip->putValues(0, 10, 20, false, true); clip->putValues(96, 80, 90, false, true);
		clip->setProgressionType(AutomationClip::ProgressionType::CubicHermite); clip->setTension(0.345678f);
		clip->setNodeTangents(96, 0.125f, -0.375f, true); clip->setAutoResize(false); clip->changeLength(192);
	}
	void actions() { QTest::addColumn<bool>("clear"); QTest::newRow("remove") << false; QTest::newRow("clear") << true; }
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
	void removalRegeneratesNativeNeighboursAndRebindsSurvivors()
	{
		clip->putValues(48, 40, 50, false, true); auto edit = options(false); edit.removePoint = 48;
		QSignalSpy notifications(clip.data(), &Clip::dataChanged); bool changed = false; QString error;
		QVERIFY2(clip->editCurve(edit, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications.count(), 1);
		QVERIFY(!clip->getTimeMap().contains(48)); QCOMPARE(clip->getTimeMap().value(0).getOutTangent(), 0.625f);
		QCOMPARE(clip->getTimeMap().value(96).getInTangent(), 0.125f); QVERIFY(clip->getTimeMap().value(96).lockedTangents());
		clip->getTimeMap()[0].setInValue(15); QCOMPARE(clip->valueAt(0), 15.f);
	}
	void clearRetainsGeometrySettingsAndBindings()
	{
		clip->setAutoResize(true); clip->changeLength(576); clip->setStartTimeOffset(-24); const auto bindings = clip->objects();
		bool changed = false; QString error; QVERIFY(clip->editCurve(options(true), changed, error)); QVERIFY(changed);
		QVERIFY(clip->getTimeMap().isEmpty()); QCOMPARE(clip->length().getTicks(), 576); QCOMPARE(clip->startTimeOffset().getTicks(), -24);
		QVERIFY(clip->objects() == bindings); QVERIFY(clip->getAutoResize()); QCOMPARE(clip->getTension(), 0.345678f);
		QCOMPARE(clip->progressionType(), AutomationClip::ProgressionType::CubicHermite);
	}
	void absentRemovalAndEmptyClearRemainApplied_data() { actions(); }
	void absentRemovalAndEmptyClearRemainApplied()
	{
		QFETCH(bool, clear); auto edit = options(clear); if (!clear) { edit.removePoint = 48; }
		if (clear) { clip->clear(); } QSignalSpy notifications(clip.data(), &Clip::dataChanged); bool changed = false; QString error;
		QVERIFY(clip->editCurve(edit, changed, error)); QVERIFY(changed); QVERIFY(clip->editCurve(edit, changed, error)); QVERIFY(changed); QCOMPARE(notifications.count(), 2);
	}
	void conflictingActionsRefuseBeforePublication_data()
	{ QTest::addColumn<int>("pair"); QTest::newRow("point-remove") << 0; QTest::newRow("point-clear") << 1; QTest::newRow("remove-clear") << 2; }
	void conflictingActionsRefuseBeforePublication()
	{
		QFETCH(int, pair); auto edit = options(pair == 1); if (pair < 2) { edit.point = AutomationClip::PointEdit{}; } else { edit.clear = true; }
		const auto before = clip->getTimeMap(); bool changed = true; QString error; QVERIFY(!clip->editCurve(edit, changed, error)); QVERIFY(!changed); QVERIFY(clip->getTimeMap() == before);
	}
	void busyCurvesRefuseBothActions_data()
	{
		QTest::addColumn<bool>("clear"); QTest::addColumn<int>("busy");
		QTest::newRow("remove-recording") << false << 0; QTest::newRow("clear-recording") << true << 0;
		QTest::newRow("remove-reserved") << false << 1; QTest::newRow("clear-reserved") << true << 1;
		QTest::newRow("remove-dragging") << false << 2; QTest::newRow("clear-dragging") << true << 2;
	}
	void busyCurvesRefuseBothActions()
	{
		QFETCH(bool, clear); QFETCH(int, busy); if (busy == 0) { clip->setRecording(true); } if (busy == 1) { clip->reserveRecordingTake(16, 0); }
		if (busy == 2) { clip->setDragValue(0, 10, false, false); }
		const auto finish = qScopeGuard([&] { clip->setRecording(false); clip->discardRecordingTake(); if (busy == 2) { clip->applyDragValue(); } });
		const auto before = clip->getTimeMap(); bool changed = true; QString error; QVERIFY(!clip->editCurve(options(clear), changed, error)); QVERIFY(!changed); QVERIFY(clip->getTimeMap() == before);
	}
	void sizingUsesNativeRemovalAndClearRules_data()
	{
		QTest::addColumn<bool>("clear"); QTest::addColumn<bool>("pattern"); QTest::newRow("song-remove") << false << false;
		QTest::newRow("song-clear") << true << false; QTest::newRow("pattern-remove") << false << true; QTest::newRow("pattern-clear") << true << true;
	}
	void sizingUsesNativeRemovalAndClearRules()
	{
		QFETCH(bool, clear); QFETCH(bool, pattern); if (pattern)
		{
			new PatternTrack(Engine::getSong()); auto row = Track::create(Track::Type::Instrument, Engine::patternStore());
			row->getClips().at(0)->setAutoResize(false); row->getClips().at(0)->changeLength(384);
			clip = static_cast<AutomationClip*>(Track::create(Track::Type::Automation, Engine::patternStore())->getClips().at(0));
			clip->addObject(instrument->volumeModel()); clip->putValue(0, 10, false); clip->putValue(96, 80, false);
		}
		clip->setAutoResize(true); clip->changeLength(576); clip->setStartTimeOffset(-12); bool changed = false; QString error;
		QVERIFY2(clip->editCurve(options(clear), changed, error), qPrintable(error)); QVERIFY(changed);
		QCOMPARE(clip->length().getTicks(), clear ? 576 : pattern ? 384 : 192); QCOMPARE(clip->startTimeOffset().getTicks(), clear || pattern ? -12 : 0);
	}
	void selectedUnsupportedTicksCanBeRemoved_data()
	{ QTest::addColumn<int>("position"); QTest::newRow("negative") << -1; QTest::newRow("maximum-int") << std::numeric_limits<int>::max(); }
	void selectedUnsupportedTicksCanBeRemoved()
	{
		QFETCH(int, position); clip->getTimeMap().insert(position, AutomationNode(clip.data(), 42, position)); auto edit = options(false); edit.removePoint = position;
		bool changed = false; QString error; QVERIFY2(clip->editCurve(edit, changed, error), qPrintable(error)); QVERIFY(changed); QVERIFY(!clip->getTimeMap().contains(position));
	}
	void malformedRetainedTicksRefuseRemovalButCanBeCleared()
	{
		clip->getTimeMap().insert(std::numeric_limits<int>::min(), AutomationNode(clip.data(), 42, std::numeric_limits<int>::min()));
		const auto before = clip->getTimeMap(); bool changed = true; QString error; QVERIFY(!clip->editCurve(options(false), changed, error)); QVERIFY(!changed); QVERIFY(clip->getTimeMap() == before);
		QVERIFY(clip->editCurve(options(true), changed, error)); QVERIFY(changed); QVERIFY(clip->getTimeMap().isEmpty());
	}
	void nanSourceRefusesWithoutPublication_data() { actions(); }
	void nanSourceRefusesWithoutPublication()
	{
		QFETCH(bool, clear); clip->getTimeMap().insert(48, AutomationNode(clip.data(), std::numeric_limits<float>::quiet_NaN(), 48));
		bool changed = true; QString error; QVERIFY(!clip->editCurve(options(clear), changed, error)); QVERIFY(!changed);
		QVERIFY(clip->getTimeMap().contains(48)); QVERIFY(clip->getTimeMap().contains(96)); QVERIFY(!error.isEmpty());
	}
	void constructionInterferenceRefusesPublication_data() { actions(); }
	void constructionInterferenceRefusesPublication()
	{
		QFETCH(bool, clear); bool invoked = false; const auto connection = connect(Engine::getSong(), &Song::lengthChanged, this, [&] {
			if (!invoked) { invoked = true; clip->putValue(48, 55, false, true); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!clip->editCurve(options(clear), changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(clip->getTimeMap().contains(96)); QCOMPARE(clip->valueAt(48), 55.f);
	}
	void returnedNotificationFailuresAreDetected_data()
	{
		QTest::addColumn<bool>("clear"); QTest::addColumn<int>("failure");
		QTest::newRow("remove-throw") << false << 0; QTest::newRow("clear-throw") << true << 0;
		QTest::newRow("remove-tamper") << false << 1; QTest::newRow("clear-tamper") << true << 1;
		QTest::newRow("remove-retire") << false << 2; QTest::newRow("clear-retire") << true << 2;
	}
	void returnedNotificationFailuresAreDetected()
	{
		QFETCH(bool, clear); QFETCH(int, failure); bool invoked = false;
		const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			if (invoked) { return; } invoked = true; if (failure == 0) { throw std::runtime_error("fixture erase notification"); }
			if (failure == 1) { clip->putValue(48, 55, false, true); } if (failure == 2) { delete clip.data(); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!clip->editCurve(options(clear), changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!error.isEmpty());
	}
	void finalNotificationAllowsAnotherThreadToRead_data() { actions(); }
	void finalNotificationAllowsAnotherThreadToRead()
	{
		QFETCH(bool, clear); float value = -1; const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			std::thread reader([&] { value = clip->valueAt(0); }); reader.join();
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY(clip->editCurve(options(clear), changed, error)); QCOMPARE(value, clear ? 0.f : 10.f);
	}
	void missingGlobalCurveIsNotCreated_data() { actions(); }
	void missingGlobalCurveIsNotCreated()
	{
		QFETCH(bool, clear); const auto count = Engine::getSong()->globalAutomationTrack()->numOfClips();
		QJsonObject a{{"parameter", int(instrument->volumeModel()->id())}, {"summary", true}}; if (!clear) { a.insert("position", 96); }
		QVERIFY(StudioCapabilities::execute(clear ? "automation.global.clear" : "automation.global.removePoint", a).value("ok").toBool());
		QCOMPARE(Engine::getSong()->globalAutomationTrack()->numOfClips(), count);
	}
	void structuredRoutesRetainNativeContents_data()
	{
		QTest::addColumn<bool>("clear"); QTest::addColumn<int>("route");
		QTest::newRow("song-remove") << false << 0; QTest::newRow("song-clear") << true << 0;
		QTest::newRow("global-remove") << false << 1; QTest::newRow("global-clear") << true << 1;
		QTest::newRow("inline-remove") << false << 2; QTest::newRow("inline-clear") << true << 2;
	}
	void structuredRoutesRetainNativeContents()
	{
		QFETCH(bool, clear); QFETCH(int, route); auto a = args(clear); auto op = operation(clear); AutomationClip* native = clip.data(); std::shared_ptr<DetuningHelper> helper;
		if (route == 1)
		{
			native = AutomationClip::globalAutomationClip(instrument->volumeModel()); native->putValue(0, 10, false); native->putValue(96, 80, false);
			a = {{"parameter", int(instrument->volumeModel()->id())}, {"summary", true}}; op = clear ? "automation.global.clear" : "automation.global.removePoint";
		}
		if (route == 2)
		{
			auto midi = static_cast<MidiClip*>(instrument->createClip(0)); auto note = midi->addNote(Note(48, 0, 60), false); note->createDetuning(); helper = note->detuning();
			helper->setValue(3); native = helper->automationClip(); native->putValue(0, 0.5f, false); native->putValue(96, 2, false);
			a = {{"track", 0}, {"clip", 0}, {"note", 0}, {"summary", true}}; op = clear ? "note.detuning.clear" : "note.detuning.removePoint";
		}
		if (!clear) { a.insert("position", 96); } const auto bindings = native->objects();
		QVERIFY(StudioCapabilities::execute(op, a).value("ok").toBool()); QVERIFY(!native->getTimeMap().contains(96));
		QCOMPARE(native->getTimeMap().isEmpty(), clear); QVERIFY(native->objects() == bindings); if (helper) { QCOMPARE(helper->value(), clear ? 0.f : 3.f); }
	}
	void sessionRecoversDefaultResetInterference_data()
	{ QTest::addColumn<bool>("retire"); QTest::newRow("retire") << true; QTest::newRow("value-tamper") << false; }
	void sessionRecoversDefaultResetInterference()
	{
		QFETCH(bool, retire);
		QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); instrument = new InstrumentTrack(Engine::getSong());
		auto midi = static_cast<MidiClip*>(instrument->createClip(0)); auto note = midi->addNote(Note(48, 0, 60), false); note->createDetuning();
		const auto helper = note->detuning(); helper->setValue(3); helper->automationClip()->putValue(0, 0.5f, false);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); bool invoked = false;
		const auto connection = connect(helper.get(), &Model::dataChanged, this, [&] {
			if (!invoked && helper->value() == 0)
			{ invoked = true; if (retire) { midi->removeNote(note); } else { helper->setValue(2); } }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!runtime.process(request("retire", "note.detuning.clear", {{"track", 0}, {"clip", 0}, {"note", 0}, {"summary", true}})).value("ok").toBool()); QVERIFY(invoked);
		auto restored = static_cast<MidiClip*>(Engine::getSong()->tracks().at(0)->getClips().at(0)); QCOMPARE(restored->notes().size(), std::size_t(1));
		QCOMPARE(restored->notes().at(0)->detuning()->value(), 3.f); QCOMPARE(restored->notes().at(0)->detuning()->automationClip()->valueAt(0), 0.5f);
	}
	void sessionUndoRedoReopenAndReplay_data() { actions(); }
	void sessionUndoRedoReopenAndReplay()
	{
		QFETCH(bool, clear); QTemporaryDir directory; QString error; const auto command = request("erase", operation(clear), args(clear)); QJsonObject receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); replay(runtime, command, receipt);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QCOMPARE(clip->getTimeMap().value(96).getOutTangent(), -0.375f);
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt);
		clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QVERIFY(!clip->getTimeMap().contains(96)); QCOMPARE(clip->getTimeMap().isEmpty(), clear); QCOMPARE(clip->length().getTicks(), 192);
	}
	void sessionRecoversReturnedFailureAndCachesIt_data() { actions(); }
	void sessionRecoversReturnedFailureAndCachesIt()
	{
		QFETCH(bool, clear); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed();
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		const auto connection = connect(clip.data(), &Clip::dataChanged, this, [] { throw std::runtime_error("fixture session erase"); });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", operation(clear), args(clear));
		const auto receipt = runtime.process(command); QVERIFY(!receipt.value("ok").toBool()); replay(runtime, command, receipt);
		clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QCOMPARE(clip->getTimeMap().value(96).getOutTangent(), -0.375f); QCOMPARE(clip->valueAt(0), 10.f); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioCurveEraseTest)
#include "StudioCurveEraseTest.moc"

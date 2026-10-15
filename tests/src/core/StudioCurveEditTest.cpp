/* Prepared native automation point and settings edits.
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
#include "Engine.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "FileResourceIO.h"
#include <QTimer>
#include "StudioRuntime.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "DetuningHelper.h"
using namespace lmms;

class StudioCurveEditTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QPointer<AutomationClip> clip;
	InstrumentTrack* instrument = nullptr;
	bool journalling = false;
	AutomationClip::CurveEdit options() const
	{
		AutomationClip::CurveEdit edit; edit.progression = AutomationClip::ProgressionType::CubicHermite; edit.tension = 0.5f;
		AutomationClip::PointEdit point; point.position = 13; point.value = 60; point.outValue = 70;
		point.inTangent = 0.25f; point.outTangent = -0.5f; edit.point = point; return edit;
	}
	QJsonObject args() const { return {{"track", 1}, {"clip", 0}, {"position", 13}, {"value", 60}, {"outValue", 70}, {"progression", 2}, {"tension", 0.5}, {"inTangent", 0.25}, {"outTangent", -0.5}}; }
	QJsonObject request(const QString& id, const QString& op, const QJsonObject& a = {}) const { return {{"id", id}, {"op", op}, {"args", a}}; }
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
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		instrument = new InstrumentTrack(Engine::getSong());
		clip = static_cast<AutomationClip*>((new AutomationTrack(Engine::getSong()))->createClip(0));
		QVERIFY(clip->addObject(instrument->volumeModel())); clip->putValues(0, 10, 20, false, true); clip->putValues(96, 80, 90, false, true);
		clip->setProgressionType(AutomationClip::ProgressionType::CubicHermite); clip->setTension(0.345678f);
		QVERIFY(clip->setNodeTangents(96, 0.125f, -0.375f, true)); clip->setAutoResize(false); clip->changeLength(192);
	}
	void cleanup() { if (clip) { clip->setRecording(false); clip->discardRecordingTake(); } Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void completeNativePointIsVisibleInOneFinalNotification()
	{
		int notifications = 0; const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			++notifications; QCOMPARE(clip->valueAt(13), 60.f); QCOMPARE(clip->getTimeMap().value(13).getOutValue(), 70.f);
			QCOMPARE(clip->getTimeMap().value(13).getOutTangent(), -0.5f); QVERIFY(clip->getTimeMap().value(13).lockedTangents());
			QCOMPARE(clip->progressionType(), AutomationClip::ProgressionType::CubicHermite); QCOMPARE(clip->getTension(), 0.5f);
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY2(clip->editCurve(options(), changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications, 1);
		QCOMPARE(clip->getTimeMap().value(96).getInTangent(), 0.125f);
		clip->getTimeMap()[13].setInValue(65); QCOMPARE(clip->valueAt(13), 65.f);
	}
	void invalidFieldsRefuseBeforeAnyNativeWrite_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("negative-position") << 0; QTest::newRow("past-end") << 1;
		QTest::newRow("nonfinite-value") << 2; QTest::newRow("out-of-range") << 3; QTest::newRow("nonfinite-tangent") << 4;
		QTest::newRow("unlocked-manual-tangent") << 5; QTest::newRow("progression") << 6; QTest::newRow("tension") << 7;
	}
	void invalidFieldsRefuseBeforeAnyNativeWrite()
	{
		QFETCH(int, failure); auto edit = options(); if (failure == 0) { edit.point->position = -1; } if (failure == 1) { edit.point->position = MaxSongLength + 1; }
		if (failure == 2) { edit.point->value = std::numeric_limits<float>::quiet_NaN(); } if (failure == 3) { edit.point->outValue = 201; }
		if (failure == 4) { edit.point->inTangent = std::numeric_limits<float>::infinity(); } if (failure == 5) { edit.point->locked = false; }
		if (failure == 6) { edit.progression = static_cast<AutomationClip::ProgressionType>(99); } if (failure == 7) { edit.tension = -1; }
		const auto before = clip->getTimeMap(); bool changed = true; QString error; QVERIFY(!clip->editCurve(edit, changed, error));
		QVERIFY(!changed); QVERIFY(!error.isEmpty()); QVERIFY(clip->getTimeMap() == before); QCOMPARE(clip->getTension(), 0.345678f);
	}
	void settingsNoopAndEqualSettingsDoNotNotify_data()
	{ QTest::addColumn<bool>("equal"); QTest::newRow("empty") << false; QTest::newRow("equal") << true; }
	void settingsNoopAndEqualSettingsDoNotNotify()
	{
		QFETCH(bool, equal); AutomationClip::CurveEdit edit; if (equal) { edit.progression = clip->progressionType(); edit.tension = clip->getTension(); }
		QSignalSpy notifications(clip.data(), &Clip::dataChanged); bool changed = true; QString error;
		QVERIFY(clip->editCurve(edit, changed, error)); QVERIFY(!changed); QCOMPARE(notifications.count(), 0);
	}
	void recordingAndDraggingRefusePointEdits_data()
	{ QTest::addColumn<int>("busy"); QTest::newRow("recording") << 0; QTest::newRow("reserved") << 1; QTest::newRow("dragging") << 2; }
	void recordingAndDraggingRefusePointEdits()
	{
		QFETCH(int, busy); if (busy == 0) { clip->setRecording(true); } if (busy == 1) { clip->reserveRecordingTake(16, 0); }
		if (busy == 2) { clip->setDragValue(0, 10, false, false); }
		const auto finish = qScopeGuard([&] { clip->setRecording(false); clip->discardRecordingTake(); if (busy == 2) { clip->applyDragValue(); } });
		const auto before = clip->getTimeMap(); bool changed = true; QString error; QVERIFY(!clip->editCurve(options(), changed, error));
		QVERIFY(!changed); QVERIFY(clip->getTimeMap() == before);
	}
	void nativeConstructorCallbacksInvalidateAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("node-edit") << 1; QTest::newRow("retire") << 2; QTest::newRow("range") << 3; QTest::newRow("meter") << 4; }
	void nativeConstructorCallbacksInvalidateAdmission()
	{
		QFETCH(int, failure); const int meter = TimePos::ticksPerBar(); const auto restore = qScopeGuard([&] { TimePos::setTicksPerBar(meter); });
		bool invoked = false; const auto connection = connect(Engine::getSong(), &Song::lengthChanged, this, [&] {
			if (invoked) { return; } invoked = true;
			if (failure == 0) { throw std::runtime_error("fixture curve construction"); } if (failure == 1) { clip->putValue(48, 55, false, true); }
			if (failure == 2) { delete clip.data(); } if (failure == 3) { instrument->volumeModel()->setStep(0.5f); }
			if (failure == 4) { TimePos::setTicksPerBar(meter + 12); }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!clip->editCurve(options(), changed, error)); QVERIFY(invoked); QVERIFY(!changed);
		if (clip) { QVERIFY(!clip->getTimeMap().contains(13)); }
	}
	void returnedNotificationFailuresAreDetected_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("point-tamper") << 1; QTest::newRow("retire") << 2; QTest::newRow("range") << 3; QTest::newRow("geometry") << 4; }
	void returnedNotificationFailuresAreDetected()
	{
		QFETCH(int, failure); bool invoked = false; const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			if (invoked) { return; } invoked = true;
			if (failure == 0) { throw std::runtime_error("fixture curve notification"); } if (failure == 1) { clip->getTimeMap()[13].setOutValue(65); }
			if (failure == 2) { delete clip.data(); } if (failure == 3) { instrument->volumeModel()->setStep(0.5f); }
			if (failure == 4) { clip->changeLength(240); }
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error;
		QVERIFY(!clip->editCurve(options(), changed, error)); QVERIFY(invoked); QVERIFY(!changed);
		if (clip) { QVERIFY(clip->getTimeMap().contains(13)); }
	}
	void nativeSizingRetainsSongAndPatternOffsetRules_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void nativeSizingRetainsSongAndPatternOffsetRules()
	{
		QFETCH(bool, pattern); if (pattern)
		{
			new PatternTrack(Engine::getSong()); auto row = Track::create(Track::Type::Instrument, Engine::patternStore());
			row->getClips().at(0)->setAutoResize(false); row->getClips().at(0)->changeLength(384);
			clip = static_cast<AutomationClip*>(Track::create(Track::Type::Automation, Engine::patternStore())->getClips().at(0));
			QVERIFY(clip->addObject(instrument->volumeModel()));
		}
		clip->setAutoResize(true); clip->setStartTimeOffset(-12); auto edit = options(); if (!pattern) { edit.point->position = 384; }
		bool changed = false; QString error; QVERIFY2(clip->editCurve(edit, changed, error), qPrintable(error));
		QVERIFY(changed); QCOMPARE(clip->length().getTicks(), 384); QCOMPARE(clip->startTimeOffset().getTicks(), pattern ? -12 : 0);
	}
	void finalNativeNotificationDoesNotHoldTheClipMutex()
	{
		float value = -1; const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			std::thread reader([&] { value = clip->valueAt(13); }); reader.join();
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error;
		QVERIFY(clip->editCurve(options(), changed, error)); QCOMPARE(value, 60.f);
	}
	void nativeUnlockRegeneratesTangentsAndIgnoresGuiQuantization()
	{
		AutomationClip::setQuantization(24); const auto restore = qScopeGuard([] { AutomationClip::setQuantization(1); });
		bool changed = false; QString error; QVERIFY(clip->editCurve(options(), changed, error));
		auto edit = options(); edit.point->inTangent.reset(); edit.point->outTangent.reset(); edit.point->locked = false;
		QVERIFY(clip->editCurve(edit, changed, error)); QVERIFY(clip->getTimeMap().contains(13)); QVERIFY(!clip->getTimeMap().value(13).lockedTangents());
	}
	void structuredSongGlobalAndInlineRoutesUseNativePointEditing()
	{
		QVERIFY(StudioCapabilities::execute("automation.point", args()).value("ok").toBool()); QCOMPARE(clip->valueAt(13), 60.f);
		QVERIFY(StudioCapabilities::execute("automation.global.point", {{"parameter", int(instrument->volumeModel()->id())}, {"position", 13}, {"value", 60}}).value("ok").toBool());
		auto midi = static_cast<MidiClip*>(instrument->createClip(0)); auto note = midi->addNote(Note(48, 0, 60), false);
		QVERIFY(StudioCapabilities::execute("note.detuning.point", {{"track", 0}, {"clip", 0}, {"note", 0}, {"position", 13}, {"value", 2.5}}).value("ok").toBool());
		QCOMPARE(note->detuning()->automationClip()->valueAt(13), 2.5f);
	}
	void sessionRecoversReturnedCurveFailureAndReplaysItsReceipt()
	{
		QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error));
		instrument = new InstrumentTrack(Engine::getSong()); clip = static_cast<AutomationClip*>((new AutomationTrack(Engine::getSong()))->createClip(0));
		QVERIFY(clip->addObject(instrument->volumeModel())); clip->setAutoResize(false); clip->changeLength(192); clip->putValue(0, 10, false);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		const auto connection = connect(clip.data(), &Clip::dataChanged, this, [] { throw std::runtime_error("fixture runtime curve"); });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "automation.point", args());
		const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); auto replayed = runtime.process(command); QVERIFY(replayed.take("replayed").toBool()); QCOMPARE(replayed, response);
		clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0));
		QVERIFY(!clip->getTimeMap().contains(13)); QCOMPARE(clip->valueAt(0), 10.f); QVERIFY(!runtime.stopping());
	}
	void sessionUndoRedoAndReopeningRetainNativeCurveData()
	{
		QTemporaryDir directory; QString error; const auto command = request("point", "automation.point", args()); QJsonObject receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); instrument = new InstrumentTrack(Engine::getSong());
			clip = static_cast<AutomationClip*>((new AutomationTrack(Engine::getSong()))->createClip(0)); QVERIFY(clip->addObject(instrument->volumeModel()));
			clip->setAutoResize(false); clip->changeLength(192); clip->putValue(0, 10, false);
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); auto replayed = runtime.process(command); QVERIFY(replayed.take("replayed").toBool()); QCOMPARE(replayed, receipt);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QVERIFY(!clip->getTimeMap().contains(13));
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); auto replayed = reopened.process(command); QVERIFY(replayed.take("replayed").toBool()); QCOMPARE(replayed, receipt);
		clip = static_cast<AutomationClip*>(Engine::getSong()->tracks().at(1)->getClips().at(0)); QCOMPARE(clip->valueAt(13), 60.f);
		QCOMPARE(clip->getTimeMap().value(13).getOutTangent(), -0.5f);
	}
	void generatedNonfiniteTangentsRefuseBeforePublication()
	{
		const auto maximum = std::numeric_limits<float>::max(); instrument->volumeModel()->setRange(-maximum, maximum, 0);
		clip->putValues(0, -maximum, -maximum, false, true); auto edit = options();
		edit.point->position = 1; edit.point->value = maximum; edit.point->outValue = maximum;
		edit.point->inTangent.reset(); edit.point->outTangent.reset(); const auto before = clip->getTimeMap();
		bool changed = true; QString error; QVERIFY(!clip->editCurve(edit, changed, error)); QVERIFY(!changed); QVERIFY(clip->getTimeMap() == before);
	}
	void invalidMeterRefusesBeforeTemporaryNativeConstruction()
	{
		const int meter = TimePos::ticksPerBar(); const auto restore = qScopeGuard([&] { TimePos::setTicksPerBar(meter); });
		const auto before = clip->getTimeMap(); TimePos::setTicksPerBar(0); bool changed = true; QString error;
		QVERIFY(!clip->editCurve(options(), changed, error)); QVERIFY(!changed); QVERIFY(clip->getTimeMap() == before);
	}
	void unsupportedSourceTicksRefuseBeforeNativeTangentMath_data()
	{ QTest::addColumn<int>("position"); QTest::newRow("negative") << -1; QTest::newRow("past-limit") << MaxSongLength + 1; }
	void unsupportedSourceTicksRefuseBeforeNativeTangentMath()
	{
		QFETCH(int, position); clip->getTimeMap().insert(position, AutomationNode(clip.data(), 42, position)); const auto before = clip->getTimeMap();
		bool changed = true; QString error; QVERIFY(!clip->editCurve(options(), changed, error)); QVERIFY(!changed); QVERIFY(clip->getTimeMap() == before);
	}
	void cooperativeCancellationAndPointInterferenceCannotPublish_data()
	{ QTest::addColumn<bool>("cancel"); QTest::newRow("cancel") << true; QTest::newRow("point-interference") << false; }
	void cooperativeCancellationAndPointInterferenceCannotPublish()
	{
		QFETCH(bool, cancel); bool invoked = false, timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		FileResourceIO::Scope work([&] { if (!invoked) { invoked = true; if (!cancel) { clip->putValue(48, 55, false, true); } } return cancel; }, false);
		bool changed = true; QString error; QVERIFY(!clip->editCurve(options(), changed, error)); QVERIFY(invoked); QVERIFY(!changed); QVERIFY(!timer);
		QVERIFY(!clip->getTimeMap().contains(13)); if (!cancel) { QCOMPARE(clip->valueAt(48), 55.f); }
	}
	void sessionRecoversDetuningOwnerRetirement()
	{
		QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error));
		instrument = new InstrumentTrack(Engine::getSong()); auto midi = static_cast<MidiClip*>(instrument->createClip(0));
		auto note = midi->addNote(Note(48, 0, 60), false); note->createDetuning(); auto native = note->detuning()->automationClip(); native->putValue(0, 0.5f, false);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		bool invoked = false; const auto connection = connect(native, &Clip::dataChanged, this, [&] { if (!invoked) { invoked = true; midi->removeNote(note); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = runtime.process(request("retire", "note.detuning.point", {{"track", 0}, {"clip", 0}, {"note", 0}, {"position", 13}, {"value", 2.5}}));
		QVERIFY(invoked); QVERIFY(!response.value("ok").toBool());
		auto restored = static_cast<MidiClip*>(Engine::getSong()->tracks().at(0)->getClips().at(0)); QCOMPARE(restored->notes().size(), std::size_t(1));
		QCOMPARE(restored->notes().at(0)->detuning()->automationClip()->valueAt(0), 0.5f);
		QVERIFY(!restored->notes().at(0)->detuning()->automationClip()->getTimeMap().contains(13));
	}

};
QTEST_GUILESS_MAIN(StudioCurveEditTest)
#include "StudioCurveEditTest.moc"

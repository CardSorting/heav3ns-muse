/* Native note length constraints, reference geometry and agent recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <limits>
#include <thread>
#include "FileResourceIO.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "NativeReplayTestAccess.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}, {"mode", "maximum"}, {"length", 24}}; }
MidiClip* midi(int track = 0, int clip = 0) { return static_cast<MidiClip*>(Engine::getSong()->tracks().at(track)->getClip(clip)); }
void populate(MidiClip* clip)
{
	clip->clearNotes(); for (int index = 0; index < 2; ++index) { auto note = clip->addNote(Note(48, 24 + index * 48, 60 + index, 85 + index, -20 + index), false); note->createDetuning(); note->detuning()->automationClip()->putValue(24, 3.1415927f, false); }
	clip->notes()[0]->detuning()->linkToModel(clip->notes()[1]->detuning().get()); clip->notes()[0]->detuning()->automationClip()->mutedModel()->linkToModel(clip->notes()[1]->detuning()->automationClip()->soloModel());
}
void seed()
{
	auto first = new InstrumentTrack(Engine::getSong()), second = new InstrumentTrack(Engine::getSong()); populate(static_cast<MidiClip*>(first->createClip(0))); second->createClip(0);
}
bool linked(const Note* a, const Note* b)
{
	if (!a->detuning() || !b->detuning() || !a->detuning()->isLinkedToModel(b->detuning().get())) { return false; }
	const auto left = a->detuning()->automationClip(), right = b->detuning()->automationClip();
	return left->mutedModel()->isLinkedToModel(right->soloModel()) || right->mutedModel()->isLinkedToModel(left->soloModel());
}
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args, bool catalog = false)
{ if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; } auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output; for (const auto& value : calls) { const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact)); if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); } else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); } }
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; } return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-length-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionLengthTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing(); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->clearJournal(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::projectJournal()->setJournalling(false); Engine::projectJournal()->clearJournal(); Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void nativeConstraintsRetainNotesCurvesAndOutsideLinks()
	{
		seed(); auto clip = midi(); const auto notes = clip->notes(); const auto identity = notes[0]->selectionIdentity(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); QVERIFY(helper->linkToModel(midi(1)->mutedModel())); bool changed; QString error;
		QVERIFY2(clip->adjustNoteLengths(NoteVector{notes[1], notes[0]}, MidiClip::LengthMode::Maximum, 24, changed, error), qPrintable(error)); QVERIFY(changed); QVERIFY(clip->notes() == notes); QCOMPARE(notes[0]->length().getTicks(), 24); QCOMPARE(notes[1]->length().getTicks(), 24); QCOMPARE(notes[0]->pos().getTicks(), 24); QCOMPARE(notes[0]->key(), 60); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(curve->valueAt(24), 3.1415927f); QVERIFY(linked(notes[0], notes[1])); QVERIFY(helper->isLinkedToModel(midi(1)->mutedModel()));
		QVERIFY(clip->adjustNoteLengths(notes, MidiClip::LengthMode::Minimum, 72, changed, error)); QCOMPARE(notes[0]->length().getTicks(), 72); QCOMPARE(notes[1]->length().getTicks(), 72); QVERIFY(linked(notes[0], notes[1])); QCOMPARE(helper->automationClip(), curve);
	}
	void signedConstraintSemanticsRetainNativeTypes_data()
	{
		QTest::addColumn<int>("mode"); QTest::addColumn<int>("length"); QTest::addColumn<int>("bound"); QTest::addColumn<int>("expected");
		QTest::newRow("maximum-shortens") << 0 << 48 << 24 << 24; QTest::newRow("maximum-keeps-shorter") << 0 << 48 << 96 << 48;
		QTest::newRow("minimum-extends") << 1 << 48 << 96 << 96; QTest::newRow("minimum-keeps-longer") << 1 << 48 << 24 << 48;
		QTest::newRow("maximum-zero") << 0 << 48 << 0 << 0; QTest::newRow("minimum-step-to-positive") << 1 << -48 << 24 << 24;
		QTest::newRow("maximum-positive-to-negative") << 0 << 48 << -24 << -24; QTest::newRow("minimum-zero-noop") << 1 << 0 << 0 << 0;
	}
	void signedConstraintSemanticsRetainNativeTypes()
	{
		QFETCH(int, mode); QFETCH(int, length); QFETCH(int, bound); QFETCH(int, expected); seed(); auto note = midi()->notes()[0]; note->setLength(length); if (length < 0) { note->setType(Note::Type::Step); } const auto type = note->type(); bool changed; QString error; QVERIFY(midi()->adjustNoteLengths(NoteVector{note}, static_cast<MidiClip::LengthMode>(mode), bound, changed, error)); QCOMPARE(note->length().getTicks(), expected); QCOMPARE(note->pos().getTicks(), 24); QCOMPARE(note->type(), type); QCOMPARE(changed, length != expected);
	}
	void trimAndFillUseAllClipStartsWithNativeComparisonRules_data()
	{
		QTest::addColumn<int>("mode"); QTest::addColumn<int>("length"); QTest::addColumn<int>("referencePosition"); QTest::addColumn<int>("expected");
		QTest::newRow("trim-overlap") << 2 << 96 << 72 << 48; QTest::newRow("trim-does-not-extend") << 2 << 24 << 72 << 24;
		QTest::newRow("trim-skips-same-start") << 2 << 96 << 24 << 96; QTest::newRow("fill-gap") << 3 << 24 << 72 << 48;
		QTest::newRow("fill-skips-overlap-to-final-bar") << 3 << 96 << 72 << 168; QTest::newRow("fill-exact-end") << 3 << 48 << 72 << 48;
		QTest::newRow("fill-zero") << 3 << 0 << 72 << 0; QTest::newRow("fill-negative-to-own-start") << 3 << -48 << 72 << 0;
		QTest::newRow("fill-unselected-later-reference") << 3 << 96 << 240 << 216; QTest::newRow("trim-last-note-unchanged") << 2 << 96 << 0 << 96;
	}
	void trimAndFillUseAllClipStartsWithNativeComparisonRules()
	{
		QFETCH(int, mode); QFETCH(int, length); QFETCH(int, referencePosition); QFETCH(int, expected); seed(); const auto notes = midi()->notes(); notes[0]->setLength(length); if (length < 0) { notes[0]->setType(Note::Type::Step); } notes[1]->setPos(referencePosition); const auto type = notes[0]->type(); bool changed; QString error; QVERIFY2(midi()->adjustNoteLengths(NoteVector{notes[0]}, static_cast<MidiClip::LengthMode>(mode), 0, changed, error), qPrintable(error)); QCOMPARE(notes[0]->length().getTicks(), expected); QCOMPARE(notes[0]->pos().getTicks(), 24); QCOMPARE(notes[0]->type(), type); QCOMPARE(notes[1]->length().getTicks(), 48); QCOMPARE(notes[1]->pos().getTicks(), referencePosition); QVERIFY(linked(notes[0], notes[1]));
	}
	void fillUsesSelectedPhraseTailAndKeepsQueuedIdentities()
	{
		seed(); const auto notes = midi()->notes(); notes[0]->setLength(24); notes[1]->setLength(24); auto args = arguments(); args.insert("mode", "fill"); args.remove("length"); const auto before = StudioCapabilities::selection("note.selection.length", args); const auto result = StudioCapabilities::execute("note.selection.length", args); QVERIFY(result.value("ok").toBool()); QCOMPARE(notes[0]->length().getTicks(), 48); QCOMPARE(notes[1]->length().getTicks(), 120); QVERIFY(!before.changed(StudioCapabilities::selection("note.selection.length", args))); QVERIFY(linked(notes[0], notes[1]));
	}
	void nativeMalformedSelectionAndBoundsRefuseBeforeEdits_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("invalid-mode") << 0; QTest::newRow("upper-bound") << 1; QTest::newRow("lower-bound") << 2; QTest::newRow("trim-explicit-bound") << 3; QTest::newRow("fill-explicit-bound") << 4; QTest::newRow("null") << 5; QTest::newRow("duplicate") << 6; QTest::newRow("foreign") << 7; QTest::newRow("negative-source-position") << 8; QTest::newRow("source-length") << 9; }
	void nativeMalformedSelectionAndBoundsRefuseBeforeEdits()
	{
		QFETCH(int, failure); seed(); auto notes = midi()->notes(); auto selected = notes; auto mode = MidiClip::LengthMode::Maximum; int bound = 24; if (failure == 0) { mode = static_cast<MidiClip::LengthMode>(99); } if (failure == 1) { bound = MaxSongLength + 1; } if (failure == 2) { bound = -MaxSongLength - 1; } if (failure == 3) { mode = MidiClip::LengthMode::Trim; } if (failure == 4) { mode = MidiClip::LengthMode::Fill; } if (failure == 5) { selected.push_back(nullptr); } if (failure == 6) { selected.push_back(notes[0]); } if (failure == 7) { selected.push_back(midi(1)->addNote(Note(48, 0, 60), false)); } if (failure == 8) { notes[1]->setPos(-1); } if (failure == 9) { notes[1]->setLength(MaxSongLength + 1); } bool changed = true; QString error; QVERIFY(!midi()->adjustNoteLengths(selected, mode, bound, changed, error)); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QCOMPARE(notes[0]->length().getTicks(), 48); QVERIFY(linked(notes[0], notes[1]));
	}
	void endpointAndFinalBarOverflowRefuseCompleteEdit_data()
	{ QTest::addColumn<bool>("fill"); QTest::newRow("minimum-endpoint") << false; QTest::newRow("fill-final-bar") << true; }
	void endpointAndFinalBarOverflowRefuseCompleteEdit()
	{
		QFETCH(bool, fill); seed(); auto notes = midi()->notes(); notes[1]->setPos(MaxSongLength - 24); notes[1]->setLength(24); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); if (fill) { TimePos::setTicksPerBar(384); } bool changed = true; QString error; QVERIFY(!midi()->adjustNoteLengths(notes, fill ? MidiClip::LengthMode::Fill : MidiClip::LengthMode::Minimum, fill ? 0 : 96, changed, error)); QVERIFY(!changed); QCOMPARE(notes[0]->length().getTicks(), 48); QCOMPARE(notes[1]->length().getTicks(), 24);
	}
	void unchangedAndEmptySelectionsPublishNoNativeHistory()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); QSignalSpy notifications(midi(), &MidiClip::dataChanged); bool changed = true; QString error; QVERIFY(midi()->adjustNoteLengths(midi()->notes(), MidiClip::LengthMode::Maximum, 96, changed, error)); QVERIFY(!changed); QVERIFY(!journal->canUndo()); QVERIFY(midi()->adjustNoteLengths({}, MidiClip::LengthMode::Fill, 0, changed, error)); QVERIFY(!changed); QVERIFY(!journal->canUndo()); QCOMPARE(notifications.count(), 0);
	}
	void nativeHistoryPreservesGraphAndRefusesOutsideCoverage()
	{
		seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); bool changed; QString error; QVERIFY(midi()->adjustNoteLengths(midi()->notes(), MidiClip::LengthMode::Maximum, 24, changed, error)); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(midi()->notes()[0]->length().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->setJournalling(false); journal->clearJournal(); QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); journal->setJournalling(true); QVERIFY(!midi()->adjustNoteLengths(midi()->notes(), MidiClip::LengthMode::Minimum, 72, changed, error)); QVERIFY(!journal->canUndo()); QCOMPARE(midi()->notes()[0]->length().getTicks(), 24); journal->setJournalling(false); QVERIFY(midi()->adjustNoteLengths(midi()->notes(), MidiClip::LengthMode::Minimum, 72, changed, error)); QVERIFY(midi()->mutedModel()->isLinkedToModel(midi(1)->mutedModel()));
	}
	void ownershipRecordingAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("selected-note-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("unselected-reference-moved") << 5; QTest::newRow("fill-meter-changed") << 6; }
	void ownershipRecordingAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto notes = clip->notes(); auto outside = failure == 5 ? clip->addNote(Note(48, 36, 70), false) : nullptr; const QPointer<MidiClip> weak(clip); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); bool changed = true, success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		const auto edit = [&] { return clip->adjustNoteLengths(notes, failure == 6 ? MidiClip::LengthMode::Fill : MidiClip::LengthMode::Maximum, failure == 6 ? 0 : 24, changed, error); };
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = edit(); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = edit(); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(notes[0]); } if (failure == 4) { delete clip; } if (failure == 5) { outside->setPos(40); } if (failure == 6) { TimePos::setTicksPerBar(bar * 2); } } return failure == 2; }, false); success = edit(); }
		QVERIFY(!success); QVERIFY(!changed); QVERIFY(!timer); QVERIFY(!error.isEmpty()); if (weak && failure != 3) { QCOMPARE(weak->notes()[0]->length().getTicks(), 48); } if (failure == 5) { QCOMPARE(outside->pos().getTicks(), 40); }
	}
	void malformedStructuredRequestsRefuseMutation_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fractional-index") << 3; QTest::newRow("negative-index") << 4; QTest::newRow("string-index") << 5; QTest::newRow("missing-note") << 6; QTest::newRow("too-many") << 7; QTest::newRow("missing-mode") << 8; QTest::newRow("invalid-mode") << 9; QTest::newRow("missing-bound") << 10; QTest::newRow("fill-with-bound") << 11; QTest::newRow("bound-range") << 12; QTest::newRow("fractional-bound") << 13; QTest::newRow("invalid-container") << 14; QTest::newRow("wrong-type") << 15; QTest::newRow("unknown-grid") << 16; QTest::newRow("bool-index") << 17;
	}
	void malformedStructuredRequestsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{2}); } if (failure == 7) { QJsonArray notes; for (int i = 0; i < 4097; ++i) { notes.append(i); } args.insert("notes", notes); } if (failure == 8) { args.remove("mode"); } if (failure == 9) { args.insert("mode", "both"); } if (failure == 10) { args.remove("length"); } if (failure == 11) { args.insert("mode", "fill"); } if (failure == 12) { args.insert("length", -MaxSongLength - 1); } if (failure == 13) { args.insert("length", .5); } if (failure == 14) { args.insert("container", "other"); } if (failure == 15) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 16) { args.insert("grid", 48); } if (failure == 17) { args.insert("notes", QJsonArray{true}); }
		QVERIFY(!StudioCapabilities::execute("note.selection.length", args).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void receiptsFollowRequestOrderInSongAndPattern_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void receiptsFollowRequestOrderInSongAndPattern()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); const auto before = StudioCapabilities::selection("note.selection.length", args); const auto response = StudioCapabilities::execute("note.selection.length", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QVERIFY(result.value("changed").toBool()); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 1); QCOMPARE(receipt[0].toObject().value("note").toInt(), 1); QCOMPARE(receipt[0].toObject().value("length").toInt(), 24); QCOMPARE(receipt[1].toObject().value("note").toInt(), 0); QCOMPARE(receipt[1].toObject().value("position").toInt(), 24); QVERIFY(!before.changed(StudioCapabilities::selection("note.selection.length", args))); QVERIFY(linked(clip->notes()[0], clip->notes()[1]));
	}
	void runtimeUndoRedoReplayAndReopeningKeepLengthEdits()
	{
		QTemporaryDir directory; QString error; const auto command = request("length", "note.selection.length", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes()[0]->length().getTicks(), 24); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->length().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes()[0]->length().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRecoversAfterReturnedNotificationFailure_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("retire-note") << 1; QTest::newRow("retire-from-song-notification") << 2; }
	void runtimeRecoversAfterReturnedNotificationFailure()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto changed = [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[0]); } else { throw std::runtime_error("fixture length notification"); } }; if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); } const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(clip, &MidiClip::dataChanged, this, changed, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.selection.length", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsConstrainThenInspectWithoutChangingIdentity_data() { protocols(); }
	void scriptedAgentsConstrainThenInspectWithoutChangingIdentity()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Shorten the phrase"}, {"steps", QJsonArray{"Constrain note lengths", "Inspect detuning"}}, {"decisions", QJsonArray{}}}), call("length", "note.selection.length", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase shortened"}, {"evidence", QJsonArray{"length", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Shorten this phrase to eighth notes and inspect its detuning."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "length").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes()[0]->length().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void scriptedAgentsSkipRetiredSelectionsAndReplan_data() { protocols(); }
	void scriptedAgentsSkipRetiredSelectionsAndReplan()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("remove", "note.remove", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("length", "note.selection.length", arguments(), catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old selection"}, {"evidence", QJsonArray{"remove"}}})}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed remaining note"}, {"evidence", QJsonArray{"remove", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Constrain phrase lengths after the note selection changes."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "remove").value("ok").toBool()); QVERIFY(!record(directory, "length").value("ok").toBool()); QVERIFY(record(directory, "length").value("error").toString().contains("changed a queued native selection")); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48);
	}
	void largeSelectionRetainsGraphAndBoundedReceipts()
	{
		seed(); midi()->clearNotes(); QJsonArray selected; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(48, index * 48, 60), false); note->createDetuning(); if (index) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(note->detuning().get())); } selected.append(index); } const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); auto args = arguments(); args.insert("notes", selected); const auto response = StudioCapabilities::execute("note.selection.length", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(first->selectionIdentity(), identity); QCOMPARE(first->length().getTicks(), 24); QCOMPARE(midi()->notes()[1049]->length().getTicks(), 24); QCOMPARE(first->detuning()->countLinks(), std::size_t{1049});
	}
	void schemaAndBothToolRoutesRespectModeBoundsAndBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.length") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(properties.value("notes").toObject().value("maxItems").toInt(), 4096); QCOMPARE(properties.value("mode").toObject().value("enum").toArray().size(), 4); QCOMPARE(properties.value("length").toObject().value("minimum").toInt(), -MaxSongLength);
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_length"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.length"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.length")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionLengthTest)
#include "StudioNoteSelectionLengthTest.moc"

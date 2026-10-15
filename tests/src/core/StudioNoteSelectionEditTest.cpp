/* Native note property edits, ordering, full-field admission and agent recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <cmath>
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
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "NativeReplayTestAccess.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}}; }
MidiClip* midi(int track = 0, int clip = 0) { return static_cast<MidiClip*>(Engine::getSong()->tracks().at(track)->getClip(clip)); }
void populate(MidiClip* clip)
{
	clip->clearNotes(); for (int index = 0; index < 2; ++index) { auto note = clip->addNote(Note(48, 24 + index * 48, 60, 85 + index, -20 + index), false); note->createDetuning(); note->detuning()->automationClip()->putValue(24, 3.1415927f, false); }
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-edit-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionEditTest : public QObject
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

	void allFieldsPublishOnceAndRetainIdentityTypeSelectionAndControls()
	{
		seed(); const auto notes = midi()->notes(); const auto identity = notes[0]->selectionIdentity(); notes[0]->setSelected(true); const auto helper = notes[0]->detuning().get(); const auto curve = helper->automationClip(); MidiClip::NoteEdit edit; edit.position = 120; edit.length = 24; edit.key = 72; edit.volume = 110; edit.panning = -30; int notifications = 0; bool complete = true; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; for (auto note : midi()->notes()) { complete = complete && note->pos().getTicks() == 120 && note->length().getTicks() == 24 && note->key() == 72 && note->volume() == 110 && note->panning() == -30; } }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = false; QString error; QVERIFY2(midi()->editNotes(NoteVector{notes[1], notes[0]}, edit, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(notifications, 1); QVERIFY(complete); QCOMPARE(midi()->notes(), notes); QCOMPARE(notes[0]->selectionIdentity(), identity); QVERIFY(notes[0]->selected()); QVERIFY(!notes[1]->selected()); QCOMPARE(notes[0]->type(), Note::Type::Regular); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(curve->valueAt(24), 3.1415927f); QVERIFY(linked(notes[0], notes[1])); QCOMPARE(midi()->stepCount(), 16);
	}
	void omittedFieldsRetainSignedLengthsAndNativeTypes_data()
	{ QTest::addColumn<int>("length"); QTest::newRow("positive") << 48; QTest::newRow("negative-step") << -12; QTest::newRow("zero") << 0; }
	void omittedFieldsRetainSignedLengthsAndNativeTypes()
	{
		QFETCH(int, length); seed(); auto note = midi()->notes()[0]; note->setLength(length); if (length < 0) { note->setType(Note::Type::Step); } const auto type = note->type(); MidiClip::NoteEdit edit; edit.volume = 110; bool changed = false; QString error; QVERIFY(midi()->editNotes(NoteVector{note}, edit, changed, error)); QVERIFY(changed); QCOMPARE(note->length().getTicks(), length); QCOMPARE(note->pos().getTicks(), 24); QCOMPARE(note->key(), 60); QCOMPARE(note->panning(), panning_t{-20}); QCOMPARE(note->type(), type); QVERIFY(linked(note, midi()->notes()[1]));
	}
	void projectedKeyOrderingSupportsSubsequentNativeInsertion()
	{
		seed(); const auto low = midi()->notes()[0], high = midi()->notes()[1]; high->setPos(24); high->setKey(65); midi()->rearrangeAllNotes(); MidiClip::NoteEdit edit; edit.key = 72; bool changed = false; QString error; QVERIFY(midi()->editNotes(NoteVector{low}, edit, changed, error)); QCOMPARE(midi()->notes()[0], low); QCOMPARE(midi()->notes()[1], high); auto middle = midi()->addNote(Note(24, 24, 69), false); QCOMPARE(midi()->notes()[1], middle); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan)); QVERIFY(linked(low, high));
	}
	void nativeNoopsAvoidHistoryAndNotifications_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("empty-fields") << 0; QTest::newRow("same-fields") << 1; QTest::newRow("empty-selection") << 2; }
	void nativeNoopsAvoidHistoryAndNotifications()
	{
		QFETCH(int, mode); seed(); Engine::projectJournal()->setJournalling(true); MidiClip::NoteEdit edit; if (mode == 1) { edit.position = 24; edit.length = 48; edit.key = 60; edit.volume = 85; edit.panning = -20; } int notifications = 0; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); bool changed = true; QString error; QVERIFY(midi()->editNotes(mode == 2 ? NoteVector{} : NoteVector{midi()->notes()[0]}, edit, changed, error)); QVERIFY(!changed); QCOMPARE(notifications, 0); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void boundsAndCoverageRefuseBeforeAnyFieldChanges_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("volume-high") << 0; QTest::newRow("volume-low") << 1; QTest::newRow("panning-high") << 2; QTest::newRow("negative-position") << 3; QTest::newRow("length-overflow") << 4; QTest::newRow("key-high") << 5; QTest::newRow("endpoint") << 6; QTest::newRow("invalid-unselected-end") << 7; QTest::newRow("history-outside") << 8; QTest::newRow("zero-meter") << 9; QTest::newRow("later-note-endpoint") << 10; }
	void boundsAndCoverageRefuseBeforeAnyFieldChanges()
	{
		QFETCH(int, failure); seed(); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); auto first = midi()->notes()[0]; MidiClip::NoteEdit edit; edit.position = 96; edit.volume = 110; if (failure == 0) { edit.volume = 201; } if (failure == 1) { edit.volume = -1; } if (failure == 2) { edit.panning = 101; } if (failure == 3) { edit.position = -1; } if (failure == 4) { edit.length = std::numeric_limits<int>::max(); } if (failure == 5) { edit.key = 128; } if (failure == 6) { edit.position = MaxSongLength; } if (failure == 7) { midi()->notes()[1]->setLength(MaxSongLength); } if (failure == 8) { auto outside = midi(1)->addStepNote(0); outside->createDetuning(); QVERIFY(first->detuning()->linkToModel(outside->detuning().get())); Engine::projectJournal()->setJournalling(true); } if (failure == 9) { TimePos::setTicksPerBar(0); } if (failure == 10) { first->setLength(24); edit.position = MaxSongLength - 24; } bool changed = true; QString error; QVERIFY(!midi()->editNotes(failure == 10 ? midi()->notes() : NoteVector{first}, edit, changed, error)); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QCOMPARE(first->pos().getTicks(), 24); QCOMPARE(first->volume(), volume_t{85}); QCOMPARE(first->key(), 60); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void suppliedFieldsCanRepairMalformedSelectedGeometry()
	{
		seed(); auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); first->setPos(-10); first->setLength(std::numeric_limits<int>::max()); MidiClip::NoteEdit edit; edit.position = 0; edit.length = 24; bool changed = false; QString error; QVERIFY2(midi()->editNotes(NoteVector{first}, edit, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(first->pos().getTicks(), 0); QCOMPARE(first->length().getTicks(), 24); QCOMPARE(first->selectionIdentity(), identity); QVERIFY(linked(first, midi()->notes()[1]));
	}
	void ownerAndCooperativeScalarChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("selected-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("meter-changed") << 5; QTest::newRow("selected-volume-changed") << 6; QTest::newRow("count-changed") << 7; QTest::newRow("unselected-key-after-sort") << 8; }
	void ownerAndCooperativeScalarChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); auto first = clip->notes()[0], second = clip->notes()[1]; if (failure == 8) { second->setPos(24); second->setKey(65); clip->rearrangeAllNotes(); } const QPointer<MidiClip> weak(clip); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); MidiClip::NoteEdit edit; edit.key = 72; edit.volume = 110; bool changed = true, success = true, invoked = false, timer = false; int capture, checks = 0; QString error; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->editNotes(NoteVector{first}, edit, changed, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->editNotes(NoteVector{first}, edit, changed, error); }); worker.join(); } else { FileResourceIO::Scope scope([&] { ++checks; if (!invoked && (failure != 8 || checks == 7)) { invoked = true; if (failure == 3) { clip->removeNote(first); } if (failure == 4) { delete clip; } if (failure == 5) { TimePos::setTicksPerBar(bar + 12); } if (failure == 6) { first->setVolume(99); } if (failure == 7) { clip->addSteps(); } if (failure == 8) { second->setKey(80); } } return failure == 2; }, false); success = clip->editNotes(NoteVector{first}, edit, changed, error); } QVERIFY(!success); QVERIFY(!changed); QVERIFY(!timer); QVERIFY(!error.isEmpty()); if (failure == 8) { QVERIFY(invoked); QCOMPARE(first->key(), 60); QCOMPARE(first->volume(), volume_t{85}); QCOMPARE(second->key(), 80); } if (weak) { QCOMPARE(weak->notes().size(), failure == 3 ? std::size_t{1} : std::size_t{2}); }
	}
	void nativeUndoRedoKeepsCompleteGraphsAndOriginalUnequalValues_data()
	{ QTest::addColumn<bool>("volume"); QTest::newRow("volume") << true; QTest::newRow("panning") << false; }
	void nativeUndoRedoKeepsCompleteGraphsAndOriginalUnequalValues()
	{
		QFETCH(bool, volume); seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); MidiClip::NoteEdit edit; if (volume) { edit.volume = 110; } else { edit.panning = 30; } bool changed = false; QString error; QVERIFY(midi()->editNotes(midi()->notes(), edit, changed, error)); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(midi()->notes()[0]->volume(), volume_t{85}); QCOMPARE(midi()->notes()[1]->volume(), volume_t{86}); QCOMPARE(midi()->notes()[0]->panning(), panning_t{-20}); QCOMPARE(midi()->notes()[1]->panning(), panning_t{-19}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(!journal->canUndo()); journal->redo(); if (volume) { QCOMPARE(midi()->notes()[0]->volume(), volume_t{110}); QCOMPARE(midi()->notes()[1]->volume(), volume_t{110}); } else { QCOMPARE(midi()->notes()[0]->panning(), panning_t{30}); QCOMPARE(midi()->notes()[1]->panning(), panning_t{30}); } QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void malformedStructuredFieldsAndSelectionsDoNotPartiallyWrite_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("single-late-key") << 0; QTest::newRow("single-late-volume") << 1; QTest::newRow("single-late-panning") << 2; QTest::newRow("missing-track") << 3; QTest::newRow("missing-clip") << 4; QTest::newRow("empty") << 5; QTest::newRow("duplicate") << 6; QTest::newRow("fractional-index") << 7; QTest::newRow("unknown-field") << 8; QTest::newRow("sample-track") << 9; QTest::newRow("too-many") << 10; }
	void malformedStructuredFieldsAndSelectionsDoNotPartiallyWrite()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); args.insert("position", 96); if (failure < 3) { args.remove("notes"); args.insert("note", 0); args.insert(failure == 0 ? "key" : failure == 1 ? "volume" : "panning", failure == 0 ? 128 : failure == 1 ? 201 : 101); } if (failure == 3) { args.remove("track"); } if (failure == 4) { args.remove("clip"); } if (failure == 5) { args.insert("notes", QJsonArray{}); } if (failure == 6) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 7) { args.insert("notes", QJsonArray{.5}); } if (failure == 8) { args.insert("extra", true); } if (failure == 9) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 10) { QJsonArray indices; for (int index = 0; index < 4097; ++index) { indices.append(index); } args.insert("notes", indices); } const auto roster = midi()->notes(); QVERIFY(!StudioCapabilities::execute(failure < 3 ? "note.edit" : "note.selection.edit", args).value("ok").toBool()); QCOMPARE(midi()->notes(), roster); QCOMPARE(roster[0]->pos().getTicks(), 24); QCOMPARE(roster[1]->pos().getTicks(), 72); QCOMPARE(roster[0]->volume(), volume_t{85});
	}
	void requestOrderReceiptsAndFractionalNativeConversionWorkInBothContainers_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void requestOrderReceiptsAndFractionalNativeConversionWorkInBothContainers()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); args.insert("position", 0); args.insert("key", 72); args.insert("volume", 110.9); args.insert("panning", -20.9); const auto response = StudioCapabilities::execute("note.selection.edit", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QVERIFY(result.value("changed").toBool()); const auto receipts = result.value("notes").toArray(); QCOMPARE(receipts[0].toObject().value("sourceNote").toInt(), 1); QCOMPARE(receipts[0].toObject().value("note").toInt(), 1); QCOMPARE(receipts[1].toObject().value("sourceNote").toInt(), 0); QCOMPARE(receipts[1].toObject().value("note").toInt(), 0); QCOMPARE(receipts[0].toObject().value("volume").toInt(), 110); QCOMPARE(receipts[0].toObject().value("panning").toInt(), -20); QVERIFY(linked(clip->notes()[0], clip->notes()[1]));
	}
	void existingSingleEditReturnsCurrentIndexAndAdmitsAllFieldsFirst()
	{
		seed(); const auto first = midi()->notes()[0]; const auto response = StudioCapabilities::execute("note.edit", {{"track", 0}, {"clip", 0}, {"note", 0}, {"position", 96}, {"volume", 110.9}, {"panning", -20.9}}); QVERIFY(response.value("ok").toBool()); const auto result = response.value("result").toObject(); QCOMPARE(result.value("sourceNote").toInt(), 0); QCOMPARE(result.value("note").toInt(), 1); QCOMPARE(result.value("volume").toInt(), 110); QCOMPARE(midi()->notes()[1], first); QCOMPARE(first->panning(), panning_t{-20});
	}
	void runtimeReplayHistoryAndReopeningRetainEdits_data()
	{ QTest::addColumn<bool>("single"); QTest::newRow("single") << true; QTest::newRow("batch") << false; }
	void runtimeReplayHistoryAndReopeningRetainEdits()
	{
		QFETCH(bool, single); QTemporaryDir directory; QString error; auto args = arguments(); args.insert("volume", 110); if (single) { args.remove("notes"); args.insert("note", 0); } const auto command = request("edit", single ? "note.edit" : "note.selection.edit", args); QJsonObject receipt; { StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes()[0]->volume(), volume_t{110}); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->volume(), volume_t{85}); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->volume(), volume_t{110}); } StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes()[0]->volume(), volume_t{110}); QCOMPARE(midi()->notes()[1]->volume(), single ? volume_t{86} : volume_t{110}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRecoversReturnedNotificationAndSelectedOrUnselectedRetirement_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("selected-retired") << 1; QTest::newRow("unselected-retired") << 2; QTest::newRow("returned-scalar-change") << 3; }
	void runtimeRecoversReturnedNotificationAndSelectedOrUnselectedRetirement()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto connection = QObject::connect(clip, &MidiClip::dataChanged, this, [&] { if (done) { return; } done = true; if (failure == 0) { throw std::runtime_error("fixture property edit notification"); } if (failure == 3) { clip->notes()[0]->setVolume(111); } else { clip->removeNote(clip->notes()[failure == 1 ? 0 : 1]); } }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.edit", {{"track", 0}, {"clip", 0}, {"note", 0}, {"volume", 110}}); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->volume(), volume_t{85}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsEditFutureNotesAndInspectThem_data() { protocols(); }
	void scriptedAgentsEditFutureNotesAndInspectThem()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Change the phrase dynamics"}, {"steps", QJsonArray{"Add a note", "Change its velocity", "Inspect it"}}, {"decisions", QJsonArray{}}}), call("add", "note.add", {{"track", 0}, {"clip", 0}, {"position", 120}, {"length", 24}, {"key", 65}}, true), call("edit", "note.selection.edit", {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{2}}, {"volume", 110}}, catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 2}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native dynamics changed"}, {"evidence", QJsonArray{"add", "edit", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Add a note, set its velocity to 110 and inspect its detuning."}})).value("ok").toBool()); for (int index = 0; index < 8; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "edit").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes()[2]->volume(), volume_t{110});
	}
	void scriptedAgentsSkipReorderedNoteIndices_data() { protocols(); }
	void scriptedAgentsSkipReorderedNoteIndices()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("first", "note.edit", {{"track", 0}, {"clip", 0}, {"note", 0}, {"position", 120}}), call("stale", "note.selection.edit", {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{0}}, {"volume", 110}}, catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old selection"}, {"evidence", QJsonArray{"first"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed reordered indices"}, {"evidence", QJsonArray{"first"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Edit notes after reordering them."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "first").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QCOMPARE(midi()->notes()[0]->volume(), volume_t{86}); QCOMPARE(midi()->notes()[1]->pos().getTicks(), 120); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void scriptedAgentsSkipReplacedClipOwners_data() { protocols(); }
	void scriptedAgentsSkipReplacedClipOwners()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; auto args = arguments(); args.insert("volume", 110); return decision(protocol, {call("first", "clip.remove", {{"track", 0}, {"clip", 0}}, true), call("stale", "note.selection.edit", args, catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old clip"}, {"evidence", QJsonArray{"first"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed removed clip"}, {"evidence", QJsonArray{"first"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Edit notes after their clip changes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "first").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void largeSelectionRetainsControlGraphAndAllEditedIdentities()
	{
		seed(); midi()->clearNotes(); DetuningHelper* anchor = nullptr; QJsonArray indices; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(24, index * 24, 60), false); note->createDetuning(); if (anchor) { QVERIFY(anchor->linkToModel(note->detuning().get())); } else { anchor = note->detuning().get(); } indices.append(index); } const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); auto args = arguments(); args.insert("notes", indices); args.insert("volume", 110); args.insert("panning", -30); const auto response = StudioCapabilities::execute("note.selection.edit", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(first->selectionIdentity(), identity); QCOMPARE(anchor->countLinks(), std::size_t{1049}); QCOMPARE(midi()->notes()[1049]->volume(), volume_t{110}); QCOMPARE(midi()->notes()[1049]->panning(), panning_t{-30});
	}
	void schemaAndDirectCatalogRoutesStayWithinBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.edit") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto notes = schema.value("inputSchema").toObject().value("properties").toObject().value("notes").toObject(); QCOMPARE(notes.value("type").toString(), QString("array")); QCOMPARE(notes.value("minItems").toInt(), 1); QCOMPARE(notes.value("maxItems").toInt(), 4096); QVERIFY(notes.value("uniqueItems").toBool()); QCOMPARE(schema.value("inputSchema").toObject().value("properties").toObject().size(), 9); for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, single = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_edit"; single = single || tool.value("name") == "note_edit"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(single); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.edit"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.edit")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionEditTest)
#include "StudioNoteSelectionEditTest.moc"

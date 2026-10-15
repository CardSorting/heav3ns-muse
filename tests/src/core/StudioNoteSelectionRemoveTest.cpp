/* Native selected-note removal, survivor ownership, history and agent recovery.
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-remove-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionRemoveTest : public QObject
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

	void batchRemovalRetainsSurvivorIdentityAndDetachesOnlyRetiredControls()
	{
		seed(); const auto first = midi()->notes()[0], second = midi()->notes()[1]; const auto identity = first->selectionIdentity(); first->setSelected(true); auto third = midi()->addNote(Note(24, 120, 65), false); third->createDetuning(); QVERIFY(first->detuning()->linkToModel(third->detuning().get())); auto outside = midi(1)->addNote(Note(24, 0, 70), false); outside->createDetuning(); QVERIFY(first->detuning()->linkToModel(outside->detuning().get())); const auto helper = first->detuning().get(); const auto curve = helper->automationClip(); int notifications = 0; bool complete = true; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; complete = complete && midi()->notes().size() == 2 && midi()->notes()[0] == first && midi()->notes()[1] == third; }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); int removed = 0; QString error; QVERIFY2(midi()->removeNotes(NoteVector{second}, removed, error), qPrintable(error)); QCOMPARE(removed, 1); QCOMPARE(notifications, 1); QVERIFY(complete); QCOMPARE(first->selectionIdentity(), identity); QVERIFY(first->selected()); QCOMPARE(first->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(curve->valueAt(24), 3.1415927f); QCOMPARE(helper->countLinks(), std::size_t{2}); QVERIFY(helper->isLinkedToModel(third->detuning().get())); QVERIFY(helper->isLinkedToModel(outside->detuning().get())); QCOMPARE(curve->mutedModel()->countLinks(), std::size_t{0}); QCOMPARE(midi()->stepCount(), 16);
	}
	void malformedSelectedGeometryCanBeRemovedWithoutRetainingIt()
	{
		seed(); auto bad = midi()->notes()[0]; bad->setPos(-10); bad->setLength(std::numeric_limits<int>::max()); int removed = 0; QString error; QVERIFY2(midi()->removeNotes(NoteVector{bad}, removed, error), qPrintable(error)); QCOMPARE(removed, 1); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 72);
	}
	void completeClearAcceptsAliasedRosterAndPreservesClipMetadataAndCount()
	{
		seed(); midi()->addSteps(); midi()->setName("Keep this row"); midi()->addStepNote(20); int removed = 0; QString error; QVERIFY(midi()->removeNotes(midi()->notes(), removed, error)); QCOMPARE(removed, 3); QVERIFY(midi()->notes().empty()); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->name(), QString("Keep this row")); QCOMPARE(midi()->type(), MidiClip::Type::BeatClip); QVERIFY(midi()->clearNotes(removed, error)); QCOMPARE(removed, 0);
	}
	void emptyNativeRemovalPublishesNoHistoryOrNotifications()
	{
		seed(); Engine::projectJournal()->setJournalling(true); int notifications = 0; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); int removed = 99; QString error; QVERIFY(midi()->removeNotes({}, removed, error)); QCOMPARE(removed, 0); QCOMPARE(notifications, 0); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void malformedSelectionsAndHistoryCoverageRefuseBeforeCommit_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("duplicate") << 0; QTest::newRow("null") << 1; QTest::newRow("foreign") << 2; QTest::newRow("retained-invalid-end") << 3; QTest::newRow("outside-history-coverage") << 4; }
	void malformedSelectionsAndHistoryCoverageRefuseBeforeCommit()
	{
		QFETCH(int, failure); seed(); NoteVector selected{midi()->notes()[0]}; if (failure == 0) { selected.push_back(selected[0]); } if (failure == 1) { selected[0] = nullptr; } if (failure == 2) { selected[0] = midi(1)->addStepNote(0); } if (failure == 3) { midi()->notes()[1]->setLength(MaxSongLength); } if (failure == 4) { auto outside = midi(1)->addStepNote(0); outside->createDetuning(); QVERIFY(midi()->notes()[0]->detuning()->linkToModel(outside->detuning().get())); Engine::projectJournal()->setJournalling(true); } const auto roster = midi()->notes(); int removed = 88; QString error; QVERIFY(!midi()->removeNotes(selected, removed, error)); QCOMPARE(removed, 0); QVERIFY(!error.isEmpty()); QCOMPARE(midi()->notes(), roster); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void ownershipAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("selected-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("meter-changed") << 5; QTest::newRow("scalar-changed") << 6; QTest::newRow("count-changed") << 7; }
	void ownershipAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto first = clip->notes()[0]; const QPointer<MidiClip> weak(clip); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); bool success = true, invoked = false, timer = false; int removed = 0, capture; QString error; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->removeNotes(NoteVector{first}, removed, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->removeNotes(NoteVector{first}, removed, error); }); worker.join(); } else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(first); } if (failure == 4) { delete clip; } if (failure == 5) { TimePos::setTicksPerBar(bar + 12); } if (failure == 6) { first->setSelected(!first->selected()); } if (failure == 7) { clip->addSteps(); } } return failure == 2; }, false); success = clip->removeNotes(NoteVector{first}, removed, error); } QVERIFY(!success); QCOMPARE(removed, 0); QVERIFY(!timer); QVERIFY(!error.isEmpty()); if (weak) { QCOMPARE(weak->notes().size(), failure == 3 ? std::size_t{1} : std::size_t{2}); }
	}
	void nativeUndoRedoAndLegacyClearRestoreCompleteGraphs_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("selection") << 0; QTest::newRow("clear") << 1; QTest::newRow("legacy-clear") << 2; }
	void nativeUndoRedoAndLegacyClearRestoreCompleteGraphs()
	{
		QFETCH(int, mode); seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); int removed = 0; QString error; if (mode == 0) { QVERIFY(midi()->removeNotes(NoteVector{midi()->notes()[1]}, removed, error)); } else if (mode == 1) { QVERIFY(midi()->clearNotes(removed, error)); } else { midi()->clear(); } const auto size = mode == 0 ? std::size_t{1} : std::size_t{0}; QCOMPARE(midi()->notes().size(), size); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->stepCount(), 16); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(!journal->canUndo()); journal->redo(); QCOMPARE(midi()->notes().size(), size);
	}
	void malformedStructuredSelectionsRefuseMutation_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("missing-track") << 0; QTest::newRow("missing-clip") << 1; QTest::newRow("empty") << 2; QTest::newRow("duplicate") << 3; QTest::newRow("negative") << 4; QTest::newRow("past-end") << 5; QTest::newRow("fractional") << 6; QTest::newRow("unknown-field") << 7; QTest::newRow("sample-track") << 8; QTest::newRow("too-many") << 9; }
	void malformedStructuredSelectionsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.remove("track"); } if (failure == 1) { args.remove("clip"); } if (failure == 2) { args.insert("notes", QJsonArray{}); } if (failure == 3) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{2}); } if (failure == 6) { args.insert("notes", QJsonArray{.5}); } if (failure == 7) { args.insert("extra", true); } if (failure == 8) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 9) { QJsonArray indices; for (int index = 0; index < 4097; ++index) { indices.append(index); } args.insert("notes", indices); } const auto roster = midi()->notes(); QVERIFY(!StudioCapabilities::execute("note.selection.remove", args).value("ok").toBool()); QCOMPARE(midi()->notes(), roster);
	}
	void requestOrderRemovalReceiptsWorkInSongAndPattern_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void requestOrderRemovalReceiptsWorkInSongAndPattern()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); const auto response = StudioCapabilities::execute("note.selection.remove", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("removed").toInt(), 2); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 1); QCOMPARE(receipt[1].toObject().value("sourceNote").toInt(), 0); QVERIFY(clip->notes().empty()); QCOMPARE(clip->stepCount(), 16);
	}
	void existingSingleAndWholeClipOperationsUseCheckedRemoval_data()
	{ QTest::addColumn<bool>("clear"); QTest::newRow("single") << false; QTest::newRow("clear") << true; }
	void existingSingleAndWholeClipOperationsUseCheckedRemoval()
	{
		QFETCH(bool, clear); seed(); QJsonObject args{{"track", 0}, {"clip", 0}}; if (!clear) { args.insert("note", 1); } Engine::projectJournal()->setJournalling(true); const auto response = StudioCapabilities::execute(clear ? "note.clear" : "note.remove", args); QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("removed").toInt(), clear ? 2 : 1); QCOMPARE(midi()->notes().size(), clear ? std::size_t{0} : std::size_t{1}); Engine::projectJournal()->undo(); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeReplayHistoryAndReopeningPreserveRemoval_data()
	{ QTest::addColumn<bool>("clear"); QTest::newRow("selection") << false; QTest::newRow("clear") << true; }
	void runtimeReplayHistoryAndReopeningPreserveRemoval()
	{
		QFETCH(bool, clear); QTemporaryDir directory; QString error; auto args = arguments(); if (clear) { args.remove("notes"); } const auto command = request("remove", clear ? "note.clear" : "note.selection.remove", args); QJsonObject receipt; { StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QCOMPARE(runtime.process(command), receipt); QVERIFY(midi()->notes().empty()); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QVERIFY(midi()->notes().empty()); } StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(reopened.process(command), receipt); QVERIFY(midi()->notes().empty()); QCOMPARE(midi()->stepCount(), 16);
	}
	void runtimeRecoversReturnedNotificationSurvivorAndCountFailures_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("survivor-retired") << 1; QTest::newRow("count-changed") << 2; }
	void runtimeRecoversReturnedNotificationSurvivorAndCountFailures()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto connection = QObject::connect(clip, &MidiClip::dataChanged, this, [&] { if (done) { return; } done = true; if (failure == 0) { throw std::runtime_error("fixture removal notification"); } if (failure == 1) { clip->removeNote(clip->notes()[0]); } if (failure == 2) { clip->addSteps(); } }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); auto args = arguments(); args.insert("notes", QJsonArray{1}); const auto command = request("reject", "note.selection.remove", args); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->stepCount(), 16); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QCOMPARE(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsRemoveFutureNotesThenClearCurrentContents_data() { protocols(); }
	void scriptedAgentsRemoveFutureNotesThenClearCurrentContents()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Clear this phrase"}, {"steps", QJsonArray{"Create a note", "Remove it", "Clear the current phrase"}}}), call("add", "note.add", {{"track", 0}, {"clip", 0}, {"position", 120}, {"length", 24}, {"key", 65}}, true), call("remove", "note.selection.remove", {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{2}}}, catalog), call("clear", "note.clear", {{"track", 0}, {"clip", 0}}, true), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase cleared"}, {"evidence", QJsonArray{"add", "remove", "clear"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Add a note, remove it, then clear this phrase."}})).value("ok").toBool()); for (int index = 0; index < 8; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "remove").value("ok").toBool()); QCOMPARE(record(directory, "remove").value("result").toObject().value("removed").toInt(), 1); QVERIFY(record(directory, "clear").value("ok").toBool()); QCOMPARE(record(directory, "clear").value("result").toObject().value("removed").toInt(), 2); QVERIFY(midi()->notes().empty());
	}
	void scriptedAgentsSkipShiftedNoteIndices_data() { protocols(); }
	void scriptedAgentsSkipShiftedNoteIndices()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("first", "note.remove", {{"track", 0}, {"clip", 0}, {"note", 0}}, true), call("stale", "note.selection.remove", {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1}}}, catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old selection"}, {"evidence", QJsonArray{"first"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed changed indices"}, {"evidence", QJsonArray{"first"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Remove notes after their indices change."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "first").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 72); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void scriptedAgentsSkipReplacedClipOwners_data() { protocols(); }
	void scriptedAgentsSkipReplacedClipOwners()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("first", "clip.remove", {{"track", 0}, {"clip", 0}}, true), call("stale", "note.selection.remove", arguments(), catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old clip"}, {"evidence", QJsonArray{"first"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed removed clip"}, {"evidence", QJsonArray{"first"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Delete notes after their clip changes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "first").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void largeRemovalPreservesOneSurvivorAndDetachesNativeLinks()
	{
		seed(); midi()->clearNotes(); auto survivor = midi()->addNote(Note(24, 0, 60), false); survivor->createDetuning(); const auto anchor = survivor->detuning().get(); const auto identity = survivor->selectionIdentity(); QJsonArray indices; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(24, 24 + index * 24, 60), false); note->createDetuning(); QVERIFY(anchor->linkToModel(note->detuning().get())); indices.append(index + 1); } auto args = arguments(); args.insert("notes", indices); const auto response = StudioCapabilities::execute("note.selection.remove", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("removed").toInt(), 1050); QCOMPARE(response.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0], survivor); QCOMPARE(survivor->selectionIdentity(), identity); QCOMPARE(anchor->countLinks(), std::size_t{0});
	}
	void schemaAndDirectCatalogRoutesStayWithinBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.remove") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto notes = schema.value("inputSchema").toObject().value("properties").toObject().value("notes").toObject(); QCOMPARE(notes.value("type").toString(), QString("array")); QCOMPARE(notes.value("minItems").toInt(), 1); QCOMPARE(notes.value("maxItems").toInt(), 4096); QVERIFY(notes.value("uniqueItems").toBool()); QCOMPARE(schema.value("inputSchema").toObject().value("properties").toObject().size(), 4); for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_remove"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.remove"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.remove")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionRemoveTest)
#include "StudioNoteSelectionRemoveTest.moc"

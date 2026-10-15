/* Structured selected-note copying, queued identities and native recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
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
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}, {"targetTrack", 1}, {"targetClip", 0}, {"position", 192}}; }
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
bool linked(const Note* a, const Note* b) { return a->detuning() && b->detuning() && a->detuning()->isLinkedToModel(b->detuning().get()) && a->detuning()->automationClip()->mutedModel()->isLinkedToModel(b->detuning()->automationClip()->soloModel()); }
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args, bool catalog = false)
{ if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; } auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output; for (const auto& value : calls) { const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact)); if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); } else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); } }
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; } return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-selection-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionCopyTest : public QObject
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
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void receiptsFollowRequestOrderAfterDestinationSorting()
	{
		seed(); midi(1)->addNote(Note(48, 216, 70), false); const auto a = midi()->notes()[0], b = midi()->notes()[1]; auto result = StudioCapabilities::execute("note.selection.copy", arguments()); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); auto receipt = result.value("result").toObject(); const auto notes = receipt.value("notes").toArray(); QCOMPARE(receipt.value("copied").toInt(), 2); QCOMPARE(receipt.value("sourceOrigin").toInt(), 24); QCOMPARE(notes[0].toObject().value("sourceNote").toInt(), 1); QCOMPARE(notes[0].toObject().value("note").toInt(), 2); QCOMPARE(notes[0].toObject().value("position").toInt(), 240); QCOMPARE(notes[1].toObject().value("note").toInt(), 0); QCOMPARE(notes[1].toObject().value("position").toInt(), 192); QCOMPARE(midi()->notes()[0], a); QCOMPARE(midi()->notes()[1], b); QVERIFY(linked(midi(1)->notes()[0], midi(1)->notes()[2])); QVERIFY(!midi(1)->notes()[0]->detuning()->isLinkedToModel(a->detuning().get())); QCOMPARE(midi(1)->notes()[0]->detuning()->automationClip()->valueAt(24), a->detuning()->automationClip()->valueAt(24));
	}
	void copyingIntoSourceClipKeepsSourcesAndInvalidatesShiftedIndices()
	{
		seed(); auto args = arguments(); args.insert("targetTrack", 0); args.insert("position", 0); const auto before = StudioCapabilities::selection("note.selection.copy", args); const auto original = midi()->notes(); auto result = StudioCapabilities::execute("note.selection.copy", args); QVERIFY(result.value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{4}); QCOMPARE(midi()->notes()[1], original[0]); QCOMPARE(midi()->notes()[3], original[1]); QVERIFY(linked(midi()->notes()[0], midi()->notes()[2])); QVERIFY(before.changed(StudioCapabilities::selection("note.selection.copy", args)));
	}
	void songAndPatternDestinationsAppendToExistingCells_data()
	{ QTest::addColumn<int>("direction"); QTest::newRow("song-to-pattern") << 0; QTest::newRow("pattern-to-song") << 1; QTest::newRow("pattern-to-pattern") << 2; }
	void songAndPatternDestinationsAppendToExistingCells()
	{
		QFETCH(int, direction); new PatternTrack(Engine::getSong()); auto songTrack = new InstrumentTrack(Engine::getSong()); auto songClip = static_cast<MidiClip*>(songTrack->createClip(0)); auto first = new InstrumentTrack(Engine::patternStore()), second = new InstrumentTrack(Engine::patternStore()); auto patternA = static_cast<MidiClip*>(first->getClip(0)), patternB = static_cast<MidiClip*>(second->getClip(0)); auto from = direction == 0 ? songClip : patternA; auto to = direction == 1 ? songClip : direction == 0 ? patternA : patternB; populate(from); to->clearNotes(); const int columns = Engine::patternStore()->numOfPatterns(); const QPointer<MidiClip> cell(to);
		auto args = arguments(); args.insert("container", direction == 0 ? "song" : "pattern"); args.insert("track", direction == 0 ? 1 : 0); args.insert("targetContainer", direction == 1 ? "song" : "pattern"); args.insert("targetTrack", direction == 1 ? 1 : direction == 0 ? 0 : 1); auto result = StudioCapabilities::execute("note.selection.copy", args); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QCOMPARE(cell.data(), to); QCOMPARE(to->notes().size(), std::size_t{2}); QVERIFY(linked(to->notes()[0], to->notes()[1])); QCOMPARE(Engine::patternStore()->numOfPatterns(), columns);
	}
	void invalidArgumentsDoNotMutateEitherClip_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fraction") << 3; QTest::newRow("negative") << 4; QTest::newRow("string") << 5; QTest::newRow("bool") << 6; QTest::newRow("missing-note") << 7; QTest::newRow("too-many") << 8; QTest::newRow("missing-target-track") << 9; QTest::newRow("missing-target-clip") << 10; QTest::newRow("target-container") << 11; QTest::newRow("wrong-target-type") << 12; QTest::newRow("negative-position") << 13; QTest::newRow("endpoint-overflow") << 14; QTest::newRow("unknown-field") << 15;
	}
	void invalidArgumentsDoNotMutateEitherClip()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{true}); } if (failure == 7) { args.insert("notes", QJsonArray{2}); } if (failure == 8) { QJsonArray notes; for (int i = 0; i < 4097; ++i) { notes.append(0); } args.insert("notes", notes); } if (failure == 9) { args.remove("targetTrack"); } if (failure == 10) { args.remove("targetClip"); } if (failure == 11) { args.insert("targetContainer", "other"); } if (failure == 12) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("targetTrack", 2); } if (failure == 13) { args.insert("position", -1); } if (failure == 14) { args.insert("position", MaxSongLength); } if (failure == 15) { args.insert("replace", true); }
		QVERIFY(!StudioCapabilities::execute("note.selection.copy", args).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(midi(1)->notes().empty()); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void activeRecordingAndOpenGroupsRefuseBeforeInsertion_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("source-recording") << 0; QTest::newRow("target-recording") << 1; QTest::newRow("outside-linked-peer") << 2; }
	void activeRecordingAndOpenGroupsRefuseBeforeInsertion()
	{
		QFETCH(int, failure); seed(); int capture; Track* recording = failure == 0 ? midi()->getTrack() : midi(1)->getTrack(); if (failure < 2) { QVERIFY(recording->beginNoteRecording(&capture)); } const auto cleanup = qScopeGuard([&] { if (failure < 2) { recording->endNoteRecording(&capture); } }); auto args = arguments(); if (failure == 2) { args.insert("notes", QJsonArray{0}); } QVERIFY(!StudioCapabilities::execute("note.selection.copy", args).value("ok").toBool()); QVERIFY(midi(1)->notes().empty());
	}
	void nativeOwnedMidiBindingRemainsIndependentInTheCopy()
	{
		seed(); auto helper = midi()->notes()[0]->detuning().get(); helper->setValue(2); auto controller = new MidiController(nullptr); controller->midiPort()->setInputController(74); helper->setControllerConnection(new ControllerConnection(controller)); auto result = StudioCapabilities::execute("note.selection.copy", arguments()); QVERIFY(result.value("ok").toBool()); auto copied = midi(1)->notes()[0]->detuning().get(); QCOMPARE(copied->storedValue(), 2.f); QVERIFY(copied->controllerConnection()); QVERIFY(copied->controllerConnection()->getController() != controller); QVERIFY(linked(midi(1)->notes()[0], midi(1)->notes()[1]));
	}
	void runtimeUndoRedoReopeningAndReceiptsPreserveCopies()
	{
		QTemporaryDir directory; QString error; const auto command = request("copy", "note.selection.copy", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QCOMPARE(runtime.process(command), receipt); QCOMPARE(midi(1)->notes().size(), std::size_t{2}); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(midi(1)->notes().empty()); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QVERIFY(linked(midi(1)->notes()[0], midi(1)->notes()[1])); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(reopened.process(command), receipt); QCOMPARE(midi(1)->notes().size(), std::size_t{2}); QVERIFY(linked(midi(1)->notes()[0], midi(1)->notes()[1]));
	}
	void runtimeRestoresAfterInsertionFailureOrRetirement_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throwing-notification") << 0; QTest::newRow("retired-result-note") << 1; QTest::newRow("retired-from-song-notification") << 2; }
	void runtimeRestoresAfterInsertionFailureOrRetirement()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto target = midi(1); bool done = false;
		const auto changed = [&] { if (done || target->notes().empty()) { return; } done = true; if (failure) { target->removeNote(target->notes().front()); } else { throw std::runtime_error("fixture note selection insertion"); } };
		if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); }
		const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(target, &MidiClip::dataChanged, this, changed, Qt::DirectConnection);
		const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto result = runtime.process(request("reject", "note.selection.copy", arguments())); QVERIFY(!result.value("ok").toBool()); QVERIFY(done); QVERIFY(midi(1)->notes().empty()); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QCOMPARE(runtime.process(request("reject", "note.selection.copy", arguments())), result); QVERIFY(!runtime.stopping());
	}
	void queuedSelectionsTrackEachNoteAndBothClipOwners_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("remove-source-note") << 0; QTest::newRow("replace-source-note") << 1; QTest::newRow("replace-destination-clip") << 2; QTest::newRow("replace-source-clip") << 3; QTest::newRow("same-object-assignment") << 4; }
	void queuedSelectionsTrackEachNoteAndBothClipOwners()
	{
		QFETCH(int, change); seed(); auto args = arguments(); const auto before = StudioCapabilities::selection("note.selection.copy", args); if (change == 0 || change == 1) { midi()->removeNote(midi()->notes()[0]); if (change == 1) { midi()->addNote(Note(48, 24, 60), false); } } if (change == 2 || change == 3) { const int owner = change == 2 ? 1 : 0; auto track = midi(owner)->getTrack(); delete midi(owner); track->createClip(0); } if (change == 4) { *midi()->notes()[0] = Note(48, 24, 64); } QCOMPARE(before.changed(StudioCapabilities::selection("note.selection.copy", args)), change != 4);
	}
	void unresolvedFutureNoteIndicesCanBeCreatedByEarlierCalls()
	{
		seed(); auto args = arguments(); args.insert("notes", QJsonArray{2}); const auto before = StudioCapabilities::selection("note.selection.copy", args); midi()->addNote(Note(48, 96, 64), false); QVERIFY(!before.changed(StudioCapabilities::selection("note.selection.copy", args))); QVERIFY(StudioCapabilities::execute("note.selection.copy", args).value("ok").toBool()); QCOMPARE(midi(1)->notes().size(), std::size_t{1});
	}
	void scriptedNativeAgentPlansCopiesAndInspectsNotes_data() { protocols(); }
	void scriptedNativeAgentPlansCopiesAndInspectsNotes()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Repeat the linked phrase"}, {"steps", QJsonArray{"Copy selected notes", "Inspect detuning"}}}), call("copy", "note.selection.copy", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 1}, {"clip", 0}, {"note", 0}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase copied"}, {"evidence", QJsonArray{"copy", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Repeat the selected phrase and retain its detuning relationships."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "copy").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QVERIFY(linked(midi(1)->notes()[0], midi(1)->notes()[1]));
	}
	void staleQueuedCopiesAreSkippedAndReplanned_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::addColumn<bool>("source"); for (const auto protocol : {QString("chat"), QString("responses")}) { for (const auto catalog : {false, true}) { for (const auto source : {false, true}) { QTest::newRow(qPrintable(protocol + QString("-%1-%2").arg(catalog).arg(source))) << protocol << catalog << source; } } }
	}
	void staleQueuedCopiesAreSkippedAndReplanned()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QFETCH(bool, source); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; const auto change = source ? call("change", "note.remove", {{"track", 0}, {"clip", 0}, {"note", 0}}) : call("change", "clip.remove", {{"track", 1}, {"clip", 0}}); return decision(protocol, {change, call("copy", "note.selection.copy", arguments(), catalog), call("premature", "agent.finish", {{"status", "complete"}, {"summary", "Old plan"}, {"evidence", QJsonArray{"change"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed changed note selection"}, {"evidence", QJsonArray{"change"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Copy the phrase after the selection changes."}})).value("ok").toBool()); for (int index = 0; index < 5; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "change").value("ok").toBool()); const auto skipped = record(directory, "copy"); QVERIFY(!skipped.value("ok").toBool()); QVERIFY(skipped.value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(job(directory).value("operations").toInt(), 2);
	}
	void cancellationAndLargeSelectionsRetainNativeAdmission()
	{
		seed(); bool timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); QVERIFY(!StudioCapabilities::execute("note.selection.copy", arguments(), [] { return true; }).value("ok").toBool()); QVERIFY(!timer); QVERIFY(midi(1)->notes().empty());
		midi()->clearNotes(); QJsonArray selected; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(48, index * 48, 60), false); note->createDetuning(); if (index) { QVERIFY(midi()->notes().front()->detuning()->linkToModel(note->detuning().get())); } selected.append(index); } auto args = arguments(); args.insert("notes", selected); const auto result = StudioCapabilities::execute("note.selection.copy", args); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QCOMPARE(result.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(midi(1)->notes().front()->detuning()->countLinks(), std::size_t{1049});
	}
	void schemaAndBothToolRoutesRetainTheirDeclarationBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.copy") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto notes = schema.value("inputSchema").toObject().value("properties").toObject().value("notes").toObject(); QCOMPARE(notes.value("minItems").toInt(), 1); QCOMPARE(notes.value("maxItems").toInt(), 4096); QVERIFY(notes.value("uniqueItems").toBool()); QCOMPARE(notes.value("items").toObject().value("type").toString(), QString("integer"));
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_copy"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.copy"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.copy")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionCopyTest)
#include "StudioNoteSelectionCopyTest.moc"

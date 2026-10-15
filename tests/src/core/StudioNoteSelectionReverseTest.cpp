/* Native note reversal, geometry, graph continuity and agent replanning.
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
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}}; }
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-reversal-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionReverseTest : public QObject
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

	void nativeReversePreservesGraphsAndDoubleReverseRestoresPositions()
	{
		seed(); auto clip = midi(); const auto notes = clip->notes(); const auto identity = notes[0]->selectionIdentity(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); QVERIFY(helper->linkToModel(midi(1)->mutedModel())); bool changed; QString error;
		QVERIFY2(clip->reverseNotes(NoteVector{notes[1], notes[0]}, changed, error), qPrintable(error)); QVERIFY(changed); QCOMPARE(clip->notes()[0], notes[1]); QCOMPARE(clip->notes()[1], notes[0]); QCOMPARE(notes[0]->pos().getTicks(), 72); QCOMPARE(notes[1]->pos().getTicks(), 24); QCOMPARE(notes[0]->key(), 60); QCOMPARE(notes[0]->length().getTicks(), 48); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(curve->valueAt(24), 3.1415927f); QVERIFY(linked(notes[0], notes[1])); QVERIFY(helper->isLinkedToModel(midi(1)->mutedModel()));
		clip->reverseNotes(clip->notes()); QVERIFY(clip->notes() == notes); QCOMPARE(notes[0]->pos().getTicks(), 24); QCOMPARE(notes[1]->pos().getTicks(), 72); QVERIFY(linked(notes[0], notes[1]));
	}
	void nativeGeometryRetainsLengthsKeysAndTypes_data()
	{
		QTest::addColumn<int>("positionA"); QTest::addColumn<int>("lengthA"); QTest::addColumn<int>("positionB"); QTest::addColumn<int>("lengthB"); QTest::addColumn<int>("expectedA"); QTest::addColumn<int>("expectedB");
		QTest::newRow("unequal-lengths") << 24 << 24 << 96 << 48 << 120 << 24;
		QTest::newRow("overlapping") << 24 << 96 << 48 << 24 << 24 << 72;
		QTest::newRow("same-start") << 24 << 24 << 24 << 48 << 48 << 24;
		QTest::newRow("zero-length") << 24 << 0 << 72 << 48 << 120 << 24;
		QTest::newRow("mixed-step-length") << 24 << -48 << 72 << 48 << 168 << 24;
		QTest::newRow("all-step-lengths") << 24 << -48 << 72 << -48 << 72 << 24;
	}
	void nativeGeometryRetainsLengthsKeysAndTypes()
	{
		QFETCH(int, positionA); QFETCH(int, lengthA); QFETCH(int, positionB); QFETCH(int, lengthB); QFETCH(int, expectedA); QFETCH(int, expectedB); seed(); const auto notes = midi()->notes(); notes[0]->setPos(positionA); notes[0]->setLength(lengthA); notes[1]->setPos(positionB); notes[1]->setLength(lengthB); if (lengthA < 0) { notes[0]->setType(Note::Type::Step); } if (lengthB < 0) { notes[1]->setType(Note::Type::Step); } const auto typeA = notes[0]->type(), typeB = notes[1]->type(); bool changed; QString error;
		QVERIFY2(midi()->reverseNotes(NoteVector{notes[1], notes[0]}, changed, error), qPrintable(error)); QCOMPARE(notes[0]->pos().getTicks(), expectedA); QCOMPARE(notes[1]->pos().getTicks(), expectedB); QCOMPARE(notes[0]->length().getTicks(), lengthA); QCOMPARE(notes[1]->length().getTicks(), lengthB); QCOMPARE(notes[0]->type(), typeA); QCOMPARE(notes[1]->type(), typeB); QCOMPARE(notes[0]->key(), 60); QCOMPARE(notes[1]->key(), 61); QVERIFY(linked(notes[0], notes[1]));
	}
	void nativeMalformedSelectionAndOverflowRefuseBeforeEditing_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("null") << 0; QTest::newRow("duplicate") << 1; QTest::newRow("foreign") << 2; QTest::newRow("negative-source-position") << 3; QTest::newRow("source-length") << 4; QTest::newRow("source-endpoint") << 5; QTest::newRow("reversed-position-overflow") << 6; }
	void nativeMalformedSelectionAndOverflowRefuseBeforeEditing()
	{
		QFETCH(int, failure); seed(); auto notes = midi()->notes(); auto selected = notes; if (failure == 0) { selected.push_back(nullptr); } if (failure == 1) { selected.push_back(notes[0]); } if (failure == 2) { selected.push_back(midi(1)->addNote(Note(48, 0, 60), false)); } if (failure == 3) { notes[1]->setPos(-1); } if (failure == 4) { notes[1]->setLength(MaxSongLength + 1); } if (failure == 5) { notes[1]->setPos(MaxSongLength); } if (failure == 6) { notes[0]->setPos(0); notes[0]->setLength(-48); notes[1]->setPos(MaxSongLength); notes[1]->setLength(0); } const auto before = notes[0]->pos(); bool changed = true; QString error;
		QVERIFY(!midi()->reverseNotes(selected, changed, error)); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QCOMPARE(notes[0]->pos(), before); QVERIFY(linked(notes[0], notes[1]));
	}
	void singleEmptyAndSymmetricSelectionsPublishNoNativeHistory()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); QSignalSpy notifications(midi(), &MidiClip::dataChanged); bool changed = true; QString error; QVERIFY(midi()->reverseNotes(NoteVector{midi()->notes()[0]}, changed, error)); QVERIFY(!changed); QVERIFY(!journal->canUndo()); QVERIFY(midi()->reverseNotes({}, changed, error)); QVERIFY(!changed); auto notes = midi()->notes(); notes[1]->setPos(24); QVERIFY(midi()->reverseNotes(notes, changed, error)); QVERIFY(!changed); QVERIFY(!journal->canUndo()); QCOMPARE(notifications.count(), 0);
	}
	void nativeHistoryPreservesGraphAndRefusesOpenClipCoverage()
	{
		seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); bool changed; QString error; QVERIFY(midi()->reverseNotes(midi()->notes(), changed, error)); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes()[0]->key(), 60); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes()[0]->key(), 61); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->setJournalling(false); journal->clearJournal(); QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); journal->setJournalling(true); QVERIFY(!midi()->reverseNotes(midi()->notes(), changed, error)); QVERIFY(!changed); QVERIFY(!journal->canUndo()); QCOMPARE(midi()->notes()[0]->key(), 61); journal->setJournalling(false); QVERIFY(midi()->reverseNotes(midi()->notes(), changed, error)); QCOMPARE(midi()->notes()[0]->key(), 60); QVERIFY(midi()->mutedModel()->isLinkedToModel(midi(1)->mutedModel()));
	}
	void recordingOwnershipAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("selected-note-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("unselected-position-changed") << 5; }
	void recordingOwnershipAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto notes = clip->notes(); auto outside = failure == 5 ? clip->addNote(Note(48, 36, 70), false) : nullptr; const QPointer<MidiClip> weak(clip); bool changed = true, success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->reverseNotes(notes, changed, error); clip->getTrack()->endNoteRecording(&capture); }
		else if (failure == 1) { std::thread worker([&] { success = clip->reverseNotes(notes, changed, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(notes[0]); } if (failure == 4) { delete clip; } if (failure == 5) { outside->setPos(40); } } return failure == 2; }, false); success = clip->reverseNotes(notes, changed, error); }
		QVERIFY(!success); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QVERIFY(!timer); if (weak && failure != 3) { QCOMPARE(weak->notes()[0]->key(), 60); } if (failure == 5) { QCOMPARE(outside->pos().getTicks(), 40); }
	}
	void invalidStructuredArgumentsRefuseMutation_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fractional-index") << 3; QTest::newRow("negative-index") << 4; QTest::newRow("string-index") << 5; QTest::newRow("missing-note") << 6; QTest::newRow("too-many") << 7; QTest::newRow("missing-notes") << 8; QTest::newRow("unknown-grid") << 9; QTest::newRow("invalid-container") << 10; QTest::newRow("wrong-type") << 11; QTest::newRow("bool-index") << 12;
	}
	void invalidStructuredArgumentsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{2}); } if (failure == 7) { QJsonArray notes; for (int i = 0; i < 4097; ++i) { notes.append(i); } args.insert("notes", notes); } if (failure == 8) { args.remove("notes"); } if (failure == 9) { args.insert("grid", 48); } if (failure == 10) { args.insert("container", "other"); } if (failure == 11) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 12) { args.insert("notes", QJsonArray{true}); }
		QVERIFY(!StudioCapabilities::execute("note.selection.reverse", args).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->key(), 60); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void structuredReceiptsFollowRequestOrderInSongAndPattern_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void structuredReceiptsFollowRequestOrderInSongAndPattern()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); const auto notes = clip->notes(); const auto before = StudioCapabilities::selection("note.selection.reverse", args); const auto response = StudioCapabilities::execute("note.selection.reverse", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QVERIFY(result.value("changed").toBool()); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 1); QCOMPARE(receipt[0].toObject().value("note").toInt(), 0); QCOMPARE(receipt[0].toObject().value("position").toInt(), 24); QCOMPARE(receipt[1].toObject().value("note").toInt(), 1); QCOMPARE(receipt[1].toObject().value("position").toInt(), 72); QCOMPARE(receipt[1].toObject().value("length").toInt(), 48); QCOMPARE(clip->notes()[0], notes[1]); QCOMPARE(clip->notes()[1], notes[0]); QVERIFY(linked(notes[0], notes[1])); QVERIFY(before.changed(StudioCapabilities::selection("note.selection.reverse", args)));
	}
	void runtimeUndoRedoReplayAndReopeningPreserveReversedGraph()
	{
		QTemporaryDir directory; QString error; const auto command = request("reverse", "note.selection.reverse", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes()[0]->key(), 61); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->key(), 60); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->key(), 61); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes()[0]->key(), 61); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRecoversAfterReturnedNativeNotifications_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("retire-note") << 1; QTest::newRow("retire-from-song-notification") << 2; }
	void runtimeRecoversAfterReturnedNativeNotifications()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto changed = [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[0]); } else { throw std::runtime_error("fixture reversal notification"); } }; if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); } const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(clip, &MidiClip::dataChanged, this, changed, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.selection.reverse", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->key(), 60); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsReverseThenDiscoverNewIndices_data() { protocols(); }
	void scriptedAgentsReverseThenDiscoverNewIndices()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Reverse the phrase"}, {"steps", QJsonArray{"Reverse notes", "Inspect the result"}}, {"decisions", QJsonArray{}}}), call("reverse", "note.selection.reverse", arguments(), catalog)}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 1}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase reversed"}, {"evidence", QJsonArray{"reverse", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Reverse the phrase and inspect its original first note."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "reverse").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes()[1]->key(), 60); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void scriptedAgentsSkipStaleIndicesAndReplan_data() { protocols(); }
	void scriptedAgentsSkipStaleIndicesAndReplan()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("reverse", "note.selection.reverse", arguments(), catalog), call("stale", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old ordering"}, {"evidence", QJsonArray{"reverse"}}})}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 1}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed reversed note order"}, {"evidence", QJsonArray{"reverse", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Reverse the phrase, then inspect the moved note after rediscovery."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "reverse").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes()[1]->key(), 60);
	}
	void largeLinkedSelectionKeepsIdentityAndRequestOrderReceipts()
	{
		seed(); midi()->clearNotes(); QJsonArray selected; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(25, index * 48, 60 + index % 12), false); note->createDetuning(); if (index) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(note->detuning().get())); } selected.append(index); } const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); auto args = arguments(); args.insert("notes", selected); const auto response = StudioCapabilities::execute("note.selection.reverse", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto receipt = response.value("result").toObject().value("notes").toArray(); QCOMPARE(receipt.size(), 1050); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 0); QCOMPARE(receipt[0].toObject().value("note").toInt(), 1049); QCOMPARE(receipt[1049].toObject().value("note").toInt(), 0); QCOMPARE(midi()->notes()[1049], first); QCOMPARE(first->selectionIdentity(), identity); QCOMPARE(first->detuning()->countLinks(), std::size_t{1049}); QCOMPARE(midi()->notes()[0]->key(), 65);
	}
	void schemaAndToolRoutesKeepBoundsAndDeclarationBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.reverse") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(properties.size(), 4); QCOMPARE(properties.value("notes").toObject().value("maxItems").toInt(), 4096); QVERIFY(properties.value("notes").toObject().value("uniqueItems").toBool()); QVERIFY(!properties.contains("grid"));
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_reverse"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.reverse"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.reverse")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionReverseTest)
#include "StudioNoteSelectionReverseTest.moc"

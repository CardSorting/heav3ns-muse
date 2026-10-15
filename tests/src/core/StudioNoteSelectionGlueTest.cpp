/* Native note gluing, survivor ownership, history and queued selection recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-glue-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionGlueTest : public QObject
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

	void nativeGlueKeepsSurvivorSettingsAndRetiresConsumedControls()
	{
		seed(); auto clip = midi(); const auto notes = clip->notes(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); const auto identity = notes[0]->selectionIdentity(); const QPointer<DetuningHelper> consumed(notes[1]->detuning().get()); const QPointer<AutomationClip> consumedCurve(notes[1]->detuning()->automationClip()); QVERIFY(helper->linkToModel(midi(1)->mutedModel())); MidiClip::GlueResult result; QString error;
		QVERIFY2(clip->glueNotes(NoteVector{notes[1], notes[0]}, result, error), qPrintable(error)); QCOMPARE(result.removed, 1); QCOMPARE(result.survivors.size(), std::size_t{2}); QCOMPARE(result.survivors[0], notes[0]); QCOMPARE(result.survivors[1], notes[0]); QCOMPARE(clip->notes().size(), std::size_t{1}); QCOMPARE(clip->notes()[0], notes[0]); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->length().getTicks(), 96); QCOMPARE(notes[0]->pos().getTicks(), 24); QCOMPARE(notes[0]->volume(), volume_t{85}); QCOMPARE(notes[0]->panning(), panning_t{-20}); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(curve->valueAt(24), 3.1415927f); QVERIFY(consumed.isNull()); QVERIFY(consumedCurve.isNull()); QVERIFY(helper->isLinkedToModel(midi(1)->mutedModel())); QCOMPARE(curve->mutedModel()->countLinks(), std::size_t{0});
	}
	void nativeGeometryAndCanonicalTiesMatchGlueRules_data()
	{
		QTest::addColumn<int>("lengthA"); QTest::addColumn<int>("positionB"); QTest::addColumn<int>("lengthB"); QTest::addColumn<int>("keyB"); QTest::addColumn<int>("removed"); QTest::addColumn<int>("expectedLength");
		QTest::newRow("touching") << 48 << 72 << 48 << 60 << 1 << 96; QTest::newRow("overlap") << 72 << 72 << 48 << 60 << 1 << 96;
		QTest::newRow("contained") << 120 << 72 << 24 << 60 << 1 << 120; QTest::newRow("gap") << 24 << 72 << 48 << 60 << 0 << 24;
		QTest::newRow("different-key") << 48 << 72 << 48 << 61 << 0 << 48; QTest::newRow("same-start-native-order") << 24 << 24 << 48 << 60 << 1 << 48;
		QTest::newRow("negative-and-zero") << -48 << 24 << 0 << 60 << 1 << 0; QTest::newRow("negative-gap") << -48 << 72 << 48 << 60 << 0 << -48;
		QTest::newRow("zero-and-positive") << 0 << 24 << 48 << 60 << 1 << 48;
	}
	void nativeGeometryAndCanonicalTiesMatchGlueRules()
	{
		QFETCH(int, lengthA); QFETCH(int, positionB); QFETCH(int, lengthB); QFETCH(int, keyB); QFETCH(int, removed); QFETCH(int, expectedLength); seed(); const auto notes = midi()->notes(); notes[0]->setLength(lengthA); if (lengthA < 0) { notes[0]->setType(Note::Type::Step); } const auto type = notes[0]->type(); notes[1]->setPos(positionB); notes[1]->setLength(lengthB); notes[1]->setKey(keyB); MidiClip::GlueResult result; QString error;
		QVERIFY2(midi()->glueNotes(NoteVector{notes[1], notes[0]}, result, error), qPrintable(error)); QCOMPARE(result.removed, removed); QCOMPARE(notes[0]->length().getTicks(), expectedLength); QCOMPARE(notes[0]->type(), type); QCOMPARE(notes[0]->pos().getTicks(), 24); QCOMPARE(result.survivors[1], notes[0]); QCOMPARE(result.survivors[0], removed ? notes[0] : notes[1]); QCOMPARE(midi()->notes().size(), static_cast<std::size_t>(2 - removed));
	}
	void chainsRespectSelectionAndKeepDifferentKeysSeparate()
	{
		seed(); auto a = midi()->notes()[0], b = midi()->notes()[1]; auto c = midi()->addNote(Note(24, 120, 60), false); auto other = midi()->addNote(Note(48, 24, 61), false); MidiClip::GlueResult result; QString error; QVERIFY(midi()->glueNotes(NoteVector{c, a}, result, error)); QCOMPARE(result.removed, 0); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(midi()->glueNotes(NoteVector{c, other, a, b}, result, error)); QCOMPARE(result.removed, 2); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(result.survivors[0], a); QCOMPARE(result.survivors[1], other); QCOMPARE(result.survivors[2], a); QCOMPARE(result.survivors[3], a); QCOMPARE(a->length().getTicks(), 120); QCOMPARE(other->length().getTicks(), 48);
	}
	void emptySingleAndSeparatedSelectionsPublishNoHistory()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); QSignalSpy notifications(midi(), &MidiClip::dataChanged); MidiClip::GlueResult result; QString error; QVERIFY(midi()->glueNotes({}, result, error)); QVERIFY(result.survivors.empty()); QCOMPARE(result.removed, 0); QVERIFY(midi()->glueNotes(NoteVector{midi()->notes()[0]}, result, error)); QCOMPARE(result.survivors[0], midi()->notes()[0]); midi()->notes()[1]->setPos(144); QVERIFY(midi()->glueNotes(midi()->notes(), result, error)); QCOMPARE(result.removed, 0); QVERIFY(!journal->canUndo()); QCOMPARE(notifications.count(), 0);
	}
	void nativeMalformedSelectionsRefuseBeforeMutation_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("null") << 0; QTest::newRow("duplicate") << 1; QTest::newRow("foreign") << 2; QTest::newRow("negative-position") << 3; QTest::newRow("source-length") << 4; QTest::newRow("source-endpoint") << 5; QTest::newRow("source-key") << 6; }
	void nativeMalformedSelectionsRefuseBeforeMutation()
	{
		QFETCH(int, failure); seed(); const auto notes = midi()->notes(); auto selected = notes; if (failure == 0) { selected.push_back(nullptr); } if (failure == 1) { selected.push_back(notes[0]); } if (failure == 2) { selected.push_back(midi(1)->addNote(Note(48, 0, 60), false)); } if (failure == 3) { notes[1]->setPos(-1); } if (failure == 4) { notes[1]->setLength(MaxSongLength + 1); } if (failure == 5) { notes[1]->setPos(MaxSongLength); } if (failure == 6) { *notes[1] = Note(48, 72, NumKeys); } MidiClip::GlueResult result; QString error; QVERIFY(!midi()->glueNotes(selected, result, error)); QVERIFY(!error.isEmpty()); QCOMPARE(result.removed, 0); QVERIFY(result.survivors.empty()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(notes[0]->length().getTicks(), 48);
	}
	void nativeClipUndoRedoRestoresConsumedControlsAndGraph()
	{
		seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); MidiClip::GlueResult result; QString error; QVERIFY(midi()->glueNotes(midi()->notes(), result, error)); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 96); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 96); QVERIFY(std::abs(midi()->notes()[0]->detuning()->automationClip()->valueAt(24) - 3.1415927f) < 1e-5f); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void enabledHistoryRefusesOpenGraphBeforeRetirement()
	{
		seed(); QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); auto journal = Engine::projectJournal(); journal->setJournalling(true); MidiClip::GlueResult result; QString error; QVERIFY(!midi()->glueNotes(midi()->notes(), result, error)); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(!journal->canUndo()); journal->setJournalling(false); QVERIFY(midi()->glueNotes(midi()->notes(), result, error)); QCOMPARE(midi()->notes().size(), std::size_t{1}); QVERIFY(midi()->mutedModel()->isLinkedToModel(midi(1)->mutedModel()));
	}
	void ownershipRecordingAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("selected-note-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("unselected-position-changed") << 5; }
	void ownershipRecordingAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto notes = clip->notes(); auto outside = failure == 5 ? clip->addNote(Note(48, 36, 70), false) : nullptr; const QPointer<MidiClip> weak(clip); MidiClip::GlueResult result; bool success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->glueNotes(notes, result, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->glueNotes(notes, result, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(notes[0]); } if (failure == 4) { delete clip; } if (failure == 5) { outside->setPos(40); } } return failure == 2; }, false); success = clip->glueNotes(notes, result, error); }
		QVERIFY(!success); QVERIFY(!error.isEmpty()); QVERIFY(!timer); QVERIFY(result.survivors.empty()); QCOMPARE(result.removed, 0); if (weak && failure != 3) { QCOMPARE(weak->notes()[0]->length().getTicks(), 48); } if (failure == 5) { QCOMPARE(outside->pos().getTicks(), 40); }
	}
	void invalidStructuredArgumentsRefuseMutation_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fractional-index") << 3; QTest::newRow("negative-index") << 4; QTest::newRow("string-index") << 5; QTest::newRow("missing-note") << 6; QTest::newRow("too-many") << 7; QTest::newRow("missing-notes") << 8; QTest::newRow("unknown-position") << 9; QTest::newRow("invalid-container") << 10; QTest::newRow("wrong-type") << 11; QTest::newRow("bool-index") << 12;
	}
	void invalidStructuredArgumentsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{2}); } if (failure == 7) { QJsonArray notes; for (int i = 0; i < 4097; ++i) { notes.append(i); } args.insert("notes", notes); } if (failure == 8) { args.remove("notes"); } if (failure == 9) { args.insert("position", 48); } if (failure == 10) { args.insert("container", "other"); } if (failure == 11) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 12) { args.insert("notes", QJsonArray{true}); }
		QVERIFY(!StudioCapabilities::execute("note.selection.glue", args).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void receiptsMapConsumedSourcesToSurvivorsInBothContainers_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void receiptsMapConsumedSourcesToSurvivorsInBothContainers()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); const auto first = clip->notes()[0]; const auto before = StudioCapabilities::selection("note.selection.glue", args); const auto response = StudioCapabilities::execute("note.selection.glue", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("removed").toInt(), 1); QCOMPARE(result.value("survivors").toInt(), 1); QVERIFY(result.value("changed").toBool()); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 1); QCOMPARE(receipt[0].toObject().value("survivingSourceNote").toInt(), 0); QCOMPARE(receipt[0].toObject().value("note").toInt(), 0); QVERIFY(receipt[0].toObject().value("merged").toBool()); QCOMPARE(receipt[1].toObject().value("sourceNote").toInt(), 0); QVERIFY(!receipt[1].toObject().value("merged").toBool()); QCOMPARE(receipt[1].toObject().value("length").toInt(), 96); QCOMPARE(clip->notes()[0], first); QVERIFY(before.changed(StudioCapabilities::selection("note.selection.glue", args)));
	}
	void runtimeHistoryReplayAndReopeningPreserveGlue()
	{
		QTemporaryDir directory; QString error; const auto command = request("glue", "note.selection.glue", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes().size(), std::size_t{1}); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 96); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 96);
	}
	void runtimeRecoversAfterReturnedNativeNotificationFailure_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("retire-survivor") << 1; QTest::newRow("retire-from-song-notification") << 2; }
	void runtimeRecoversAfterReturnedNativeNotificationFailure()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto changed = [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[0]); } else { throw std::runtime_error("fixture glue notification"); } }; if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); } const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(clip, &MidiClip::dataChanged, this, changed, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.selection.glue", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsGlueThenInspectRetainedNote_data() { protocols(); }
	void scriptedAgentsGlueThenInspectRetainedNote()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Join the phrase"}, {"steps", QJsonArray{"Glue selected notes", "Inspect survivor"}}, {"decisions", QJsonArray{}}}), call("glue", "note.selection.glue", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase joined"}, {"evidence", QJsonArray{"glue", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Join these touching notes and inspect the retained detuning."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "glue").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 96);
	}
	void scriptedAgentsSkipConsumedSelectionsAndReplan_data() { protocols(); }
	void scriptedAgentsSkipConsumedSelectionsAndReplan()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("glue", "note.selection.glue", arguments(), catalog), call("stale", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 1}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old notes"}, {"evidence", QJsonArray{"glue"}}})}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed survivor"}, {"evidence", QJsonArray{"glue", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Join the phrase, then inspect the surviving note after rediscovery."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "glue").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes().size(), std::size_t{1});
	}
	void largeLinkedSelectionMapsAllSourcesToOneSurvivor()
	{
		seed(); midi()->clearNotes(); QJsonArray selected; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(48, index * 48, 60), false); note->createDetuning(); if (index) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(note->detuning().get())); } selected.append(index); } const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); auto args = arguments(); args.insert("notes", selected); const auto response = StudioCapabilities::execute("note.selection.glue", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("removed").toInt(), 1049); QCOMPARE(result.value("notes").toArray().size(), 1050); QCOMPARE(result.value("notes").toArray()[1049].toObject().value("survivingSourceNote").toInt(), 0); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0], first); QCOMPARE(first->selectionIdentity(), identity); QCOMPARE(first->length().getTicks(), 1050 * 48); QCOMPARE(first->detuning()->countLinks(), std::size_t{0});
	}
	void schemaAndBothToolRoutesKeepSelectionBoundsAndBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.glue") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(properties.size(), 4); QCOMPARE(properties.value("notes").toObject().value("maxItems").toInt(), 4096); QVERIFY(properties.value("notes").toObject().value("uniqueItems").toBool());
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_glue"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.glue"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.glue")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionGlueTest)
#include "StudioNoteSelectionGlueTest.moc"

/* Native and structured note shifting, ownership, history and queued identities.
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
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}, {"semitones", 12}, {"ticks", 96}}; }
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
class StudioNoteSelectionShiftTest : public QObject
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

	void nativeShiftRetainsNotesCurvesAndOutsideLinks()
	{
		seed(); auto clip = midi(); const auto notes = clip->notes(); auto helper = notes[0]->detuning().get(); const auto curve = helper->automationClip(); const auto identity = notes[0]->selectionIdentity(); QVERIFY(helper->linkToModel(midi(1)->mutedModel()));
		MidiClip::ShiftResult result; QString error; QVERIFY2(clip->shiftNotes(NoteVector{notes[1], notes[0]}, 12, 96, result, error), qPrintable(error)); QVERIFY(result.changed); QCOMPARE(result.appliedTicks, 96); QCOMPARE(clip->notes(), notes); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(curve->valueAt(24), 3.1415927f); QCOMPARE(notes[0]->pos().getTicks(), 120); QCOMPARE(notes[1]->pos().getTicks(), 168); QCOMPARE(notes[0]->key(), 72); QCOMPARE(notes[1]->key(), 73); QVERIFY(linked(notes[0], notes[1])); QVERIFY(helper->isLinkedToModel(midi(1)->mutedModel()));
		notes[1]->setType(Note::Type::Step); notes[1]->setLength(-48); QVERIFY(clip->shiftNotes(NoteVector{notes[1]}, -12, -48, result, error)); QCOMPARE(notes[1]->type(), Note::Type::Step); QCOMPARE(notes[1]->length().getTicks(), -48); QCOMPARE(notes[1]->key(), 61); notes[1]->setLength(0); QVERIFY(clip->shiftNotes(NoteVector{notes[1]}, 12, 48, result, error)); QCOMPARE(notes[1]->length().getTicks(), 0); QVERIFY(linked(notes[0], notes[1])); QCOMPARE(helper->automationClip(), curve);
	}
	void nativeClampingUsesWideArithmeticAndEarliestSelectedNote_data()
	{
		QTest::addColumn<int>("semitones"); QTest::addColumn<int>("ticks"); QTest::addColumn<int>("applied"); QTest::addColumn<int>("firstKey"); QTest::addColumn<int>("secondKey");
		QTest::newRow("unsorted-left-limit") << 0 << -100 << -24 << 60 << 61;
		QTest::newRow("minimum-integer-ticks") << 0 << std::numeric_limits<int>::min() << -24 << 60 << 61;
		QTest::newRow("maximum-integer-transpose") << std::numeric_limits<int>::max() << 0 << 0 << NumKeys - 1 << NumKeys - 1;
		QTest::newRow("minimum-integer-transpose") << std::numeric_limits<int>::min() << 0 << 0 << 0 << 0;
	}
	void nativeClampingUsesWideArithmeticAndEarliestSelectedNote()
	{
		QFETCH(int, semitones); QFETCH(int, ticks); QFETCH(int, applied); QFETCH(int, firstKey); QFETCH(int, secondKey); seed(); auto notes = midi()->notes(); MidiClip::ShiftResult result; QString error;
		QVERIFY(midi()->shiftNotes(NoteVector{notes[1], notes[0]}, semitones, ticks, result, error)); QCOMPARE(result.appliedTicks, applied); QCOMPARE(notes[0]->pos().getTicks(), 24 + applied); QCOMPARE(notes[1]->pos().getTicks(), 72 + applied); QCOMPARE(notes[0]->key(), firstKey); QCOMPARE(notes[1]->key(), secondKey); QVERIFY(linked(notes[0], notes[1]));
	}
	void stableSortingPreservesUnselectedNotesAndTieOrder()
	{
		seed(); auto first = midi()->notes()[0], last = midi()->notes()[1]; auto outside = midi()->addNote(Note(48, 48, 61), false); MidiClip::ShiftResult result; QString error; QVERIFY(midi()->shiftNotes(NoteVector{first}, 1, 24, result, error)); QCOMPARE(midi()->notes()[0], first); QCOMPARE(midi()->notes()[1], outside); QCOMPARE(midi()->notes()[2], last); QCOMPARE(outside->pos().getTicks(), 48); QCOMPARE(last->pos().getTicks(), 72); QVERIFY(linked(first, last));
	}
	void projectedKeyOrderingSupportsLaterNativeInsertion()
	{
		seed(); const auto notes = midi()->notes(); notes[1]->setPos(24); midi()->rearrangeAllNotes(); MidiClip::ShiftResult result; QString error; QVERIFY(midi()->shiftNotes(NoteVector{notes[0]}, 12, 0, result, error)); QCOMPARE(midi()->notes()[0], notes[0]); QCOMPARE(midi()->notes()[1], notes[1]); auto inserted = midi()->addNote(Note(48, 24, 65), false); QCOMPARE(midi()->notes()[1], inserted); QCOMPARE(midi()->notes()[2], notes[1]); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan)); QVERIFY(linked(notes[0], notes[1]));
	}
	void nativeAdmissionRefusesBeforeChangingAnySelectedNote_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("null") << 0; QTest::newRow("duplicate") << 1; QTest::newRow("foreign") << 2; QTest::newRow("negative-source-position") << 3; QTest::newRow("source-length") << 4; QTest::newRow("source-endpoint") << 5; QTest::newRow("destination-endpoint") << 6; QTest::newRow("maximum-integer-ticks") << 7;
	}
	void nativeAdmissionRefusesBeforeChangingAnySelectedNote()
	{
		QFETCH(int, failure); seed(); auto notes = midi()->notes(); NoteVector selection = notes; int ticks = 96;
		if (failure == 0) { selection.push_back(nullptr); } if (failure == 1) { selection.push_back(notes[0]); } if (failure == 2) { selection.push_back(midi(1)->addNote(Note(48, 0, 60), false)); } if (failure == 3) { notes[1]->setPos(-1); } if (failure == 4) { notes[1]->setLength(MaxSongLength + 1); } if (failure == 5) { notes[1]->setPos(MaxSongLength); } if (failure == 6) { ticks = MaxSongLength; } if (failure == 7) { ticks = std::numeric_limits<int>::max(); }
		const auto position = notes[0]->pos(); const int key = notes[0]->key(); MidiClip::ShiftResult result; QString error; QVERIFY(!midi()->shiftNotes(selection, 12, ticks, result, error)); QVERIFY(!error.isEmpty()); QVERIFY(!result.changed); QCOMPARE(result.appliedTicks, 0); QCOMPARE(notes[0]->pos(), position); QCOMPARE(notes[0]->key(), key); QVERIFY(linked(notes[0], notes[1]));
	}
	void noOpAndEmptySelectionsDoNotPublishHistoryOrNotifications()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); QSignalSpy notifications(midi(), &MidiClip::dataChanged); MidiClip::ShiftResult result; QString error;
		QVERIFY(midi()->shiftNotes(midi()->notes(), 0, 0, result, error)); QVERIFY(!result.changed); QVERIFY(!journal->canUndo()); QCOMPARE(notifications.count(), 0); QVERIFY(midi()->shiftNotes({}, 12, 96, result, error)); QVERIFY(!result.changed); QVERIFY(!journal->canUndo());
	}
	void nativeClipHistoryRestoresClosedGraphAndSameClip()
	{
		seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); MidiClip::ShiftResult result; QString error; QVERIFY(midi()->shiftNotes(midi()->notes(), 12, 96, result, error)); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QCOMPARE(midi()->notes()[0]->key(), 60); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 120); QCOMPARE(midi()->notes()[0]->key(), 72); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void enabledHistoryRefusesOpenClipGraphBeforeMutation()
	{
		seed(); QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); auto journal = Engine::projectJournal(); journal->setJournalling(true); MidiClip::ShiftResult result; QString error; QVERIFY(!midi()->shiftNotes(midi()->notes(), 12, 96, result, error)); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QCOMPARE(midi()->notes()[0]->key(), 60); QVERIFY(!journal->canUndo()); journal->setJournalling(false); QVERIFY(midi()->shiftNotes(midi()->notes(), 12, 96, result, error)); QVERIFY(midi()->mutedModel()->isLinkedToModel(midi(1)->mutedModel()));
	}
	void nativeNotificationFailureRetainsUsableCheckpoint()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [] { throw std::runtime_error("fixture direct shift notification"); }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); MidiClip::ShiftResult result; QString error;
		QVERIFY(!midi()->shiftNotes(midi()->notes(), 12, 96, result, error)); QVERIFY(!result.changed); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 120); QVERIFY(journal->canUndo()); QObject::disconnect(connection); journal->undo(); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void ownershipRecordingAndCancellationRemainNative_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("selection-retired-in-check") << 3; QTest::newRow("clip-retired-in-check") << 4; }
	void ownershipRecordingAndCancellationRemainNative()
	{
		QFETCH(int, failure); seed(); const auto clip = midi(); const auto selection = clip->notes(); const QPointer<MidiClip> weak(clip); MidiClip::ShiftResult result; QString error; int capture; bool timer = false, invoked = false, success = true; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->shiftNotes(selection, 12, 96, result, error); clip->getTrack()->endNoteRecording(&capture); }
		else if (failure == 1) { std::thread worker([&] { success = clip->shiftNotes(selection, 12, 96, result, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(selection[0]); } if (failure == 4) { delete clip; } } return failure == 2; }, false); success = clip->shiftNotes(selection, 12, 96, result, error); }
		QVERIFY(!success); QVERIFY(!result.changed); QVERIFY(!error.isEmpty()); QVERIFY(!timer); if (weak && failure != 3) { QCOMPARE(weak->notes()[0]->pos().getTicks(), 24); QCOMPARE(weak->notes()[0]->key(), 60); }
	}
	void invalidStructuredRequestsAreRejected_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fraction") << 3; QTest::newRow("negative") << 4; QTest::newRow("string") << 5; QTest::newRow("missing-note") << 6; QTest::newRow("too-many") << 7; QTest::newRow("no-shift-fields") << 8; QTest::newRow("semitone-range") << 9; QTest::newRow("tick-range") << 10; QTest::newRow("fractional-shift") << 11; QTest::newRow("wrong-container") << 12; QTest::newRow("wrong-clip-type") << 13; QTest::newRow("unknown-field") << 14; QTest::newRow("endpoint-overflow") << 15;
	}
	void invalidStructuredRequestsAreRejected()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{2}); } if (failure == 7) { QJsonArray notes; for (int index = 0; index < 4097; ++index) { notes.append(index); } args.insert("notes", notes); } if (failure == 8) { args.remove("semitones"); args.remove("ticks"); } if (failure == 9) { args.insert("semitones", NumKeys + 1); } if (failure == 10) { args.insert("ticks", -MaxSongLength - 1); } if (failure == 11) { args.insert("ticks", .5); } if (failure == 12) { args.insert("container", "other"); } if (failure == 13) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 14) { args.insert("position", 0); } if (failure == 15) { args.insert("ticks", MaxSongLength); }
		QVERIFY(!StudioCapabilities::execute("note.selection.shift", args).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QCOMPARE(midi()->notes()[0]->key(), 60); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void structuredReceiptsFollowRequestOrderInSongAndPattern_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void structuredReceiptsFollowRequestOrderInSongAndPattern()
	{
		QFETCH(bool, pattern); MidiClip* clip;
		if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); }
		auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); args.insert("notes", QJsonArray{0}); args.insert("ticks", 96); const auto notes = clip->notes(); const auto before = StudioCapabilities::selection("note.selection.shift", args); const auto result = StudioCapabilities::execute("note.selection.shift", args); QVERIFY(result.value("ok").toBool()); const auto payload = result.value("result").toObject(); const auto receipt = payload.value("notes").toArray().first().toObject(); QVERIFY(payload.value("changed").toBool()); QCOMPARE(payload.value("appliedTicks").toInt(), 96); QCOMPARE(receipt.value("sourceNote").toInt(), 0); QCOMPARE(receipt.value("note").toInt(), 1); QCOMPARE(receipt.value("position").toInt(), 120); QCOMPARE(receipt.value("key").toInt(), 72); QCOMPARE(clip->notes()[0], notes[1]); QCOMPARE(clip->notes()[1], notes[0]); QVERIFY(linked(notes[0], notes[1])); QVERIFY(before.changed(StudioCapabilities::selection("note.selection.shift", args)));
	}
	void runtimeHistoryReplayAndReopeningPreserveShiftedGraph()
	{
		QTemporaryDir directory; QString error; const auto command = request("shift", "note.selection.shift", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 120); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->key(), 72); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRecoversFromReturnedNativeNotificationFailures_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("retire-note") << 1; QTest::newRow("retire-from-song-notification") << 2; }
	void runtimeRecoversFromReturnedNativeNotificationFailures()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false;
		const auto changed = [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[0]); } else { throw std::runtime_error("fixture note shift notification"); } };
		if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); } const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(clip, &MidiClip::dataChanged, this, changed, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto command = request("reject", "note.selection.shift", arguments()); const auto result = runtime.process(command); QVERIFY(!result.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QCOMPARE(midi()->notes()[0]->key(), 60); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), result); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsShiftThenInspectWithoutLosingIdentity_data() { protocols(); }
	void scriptedAgentsShiftThenInspectWithoutLosingIdentity()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Transpose the phrase"}, {"steps", QJsonArray{"Shift notes", "Inspect detuning"}}, {"decisions", QJsonArray{}}}), call("shift", "note.selection.shift", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Phrase shifted"}, {"evidence", QJsonArray{"shift", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Transpose the phrase up an octave and shift it half a bar."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "shift").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes()[0]->key(), 72); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void scriptedAgentsReplanWhenSortingChangesQueuedIndices_data() { protocols(); }
	void scriptedAgentsReplanWhenSortingChangesQueuedIndices()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; auto args = arguments(); args.insert("notes", QJsonArray{0}); StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("shift", "note.selection.shift", args, catalog), call("stale", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old ordering"}, {"evidence", QJsonArray{"shift"}}})}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 1}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed sorted notes"}, {"evidence", QJsonArray{"shift", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Move the first note past the second, then inspect the moved note."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "shift").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes()[1]->key(), 72);
	}
	void largeSelectionRetainsInlineGraphAndReportsBoundedReceipts()
	{
		seed(); midi()->clearNotes(); QJsonArray selected; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(24, index * 48, 60), false); note->createDetuning(); if (index) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(note->detuning().get())); } selected.append(index); } auto args = arguments(); args.insert("notes", selected); const auto result = StudioCapabilities::execute("note.selection.shift", args); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QCOMPARE(result.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(midi()->notes()[0]->detuning()->countLinks(), std::size_t{1049}); QCOMPARE(midi()->notes()[1049]->key(), 72);
	}
	void schemaAndToolRoutesKeepBoundsAndDeclarationBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.shift") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); const auto notes = properties.value("notes").toObject(); QCOMPARE(notes.value("minItems").toInt(), 1); QCOMPARE(notes.value("maxItems").toInt(), 4096); QVERIFY(notes.value("uniqueItems").toBool()); QVERIFY(!properties.contains("targetTrack")); QCOMPARE(properties.value("ticks").toObject().value("minimum").toInt(), -MaxSongLength);
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_shift"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.shift"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.shift")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionShiftTest)
#include "StudioNoteSelectionShiftTest.moc"

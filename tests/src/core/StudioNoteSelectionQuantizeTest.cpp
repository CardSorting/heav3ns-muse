/* Native in-place quantization, control graph continuity and agent recovery.
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
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}, {"grid", 48}}; }
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-quantization-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionQuantizeTest : public QObject
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

	void nativeQuantizationRetainsNotesHelpersCurvesAndOutsideLinks()
	{
		seed(); auto clip = midi(); auto notes = clip->notes(); notes[0]->setLength(25); notes[1]->setLength(11); const auto identity = notes[0]->selectionIdentity(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); QVERIFY(helper->linkToModel(midi(1)->mutedModel()));
		bool changed = false; QString error; QVERIFY2(clip->quantizeNotes(NoteVector{notes[1], notes[0]}, 48, MidiClip::QuantizeMode::Both, changed, error), qPrintable(error)); QVERIFY(changed); QVERIFY(clip->notes() == notes); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(curve->valueAt(24), 3.1415927f); QCOMPARE(notes[0]->pos().getTicks(), 48); QCOMPARE(notes[1]->pos().getTicks(), 96); QCOMPARE(notes[0]->length().getTicks(), 48); QCOMPARE(notes[1]->length().getTicks(), 48); QCOMPARE(notes[0]->key(), 60); QVERIFY(linked(notes[0], notes[1])); QVERIFY(helper->isLinkedToModel(midi(1)->mutedModel()));
	}
	void nativeRoundingAndModesMatchPianoRollConventions_data()
	{
		QTest::addColumn<int>("position"); QTest::addColumn<int>("length"); QTest::addColumn<int>("grid"); QTest::addColumn<int>("mode"); QTest::addColumn<int>("expectedPosition"); QTest::addColumn<int>("expectedLength");
		QTest::newRow("both") << 24 << 25 << 48 << 0 << 48 << 48;
		QTest::newRow("positions") << 24 << 25 << 48 << 1 << 48 << 25;
		QTest::newRow("lengths") << 24 << 25 << 48 << 2 << 24 << 48;
		QTest::newRow("below-half") << 23 << 23 << 48 << 0 << 0 << 48;
		QTest::newRow("at-half") << 24 << 24 << 48 << 0 << 48 << 48;
		QTest::newRow("above-half") << 25 << 25 << 48 << 0 << 48 << 48;
		QTest::newRow("unit-grid") << 25 << 25 << 1 << 0 << 25 << 25;
		QTest::newRow("negative-step-length") << 24 << -48 << 48 << 0 << 48 << -48;
		QTest::newRow("negative-rounds-to-zero") << 24 << -24 << 48 << 0 << 48 << 48;
		QTest::newRow("negative-native-truncation") << 24 << -84 << 48 << 0 << 48 << -48;
		QTest::newRow("zero-length-skips-position") << 24 << 0 << 48 << 0 << 24 << 0;
		QTest::newRow("maximum-grid") << 24 << 25 << MaxSongLength << 0 << 0 << MaxSongLength;
	}
	void nativeRoundingAndModesMatchPianoRollConventions()
	{
		QFETCH(int, position); QFETCH(int, length); QFETCH(int, grid); QFETCH(int, mode); QFETCH(int, expectedPosition); QFETCH(int, expectedLength); seed(); auto note = midi()->notes()[0]; note->setPos(position); note->setLength(length); if (length < 0) { note->setType(Note::Type::Step); } const auto type = note->type(); const auto identity = note->selectionIdentity(); bool changed; QString error;
		QVERIFY2(midi()->quantizeNotes(NoteVector{note}, grid, static_cast<MidiClip::QuantizeMode>(mode), changed, error), qPrintable(error)); QCOMPARE(note->pos().getTicks(), expectedPosition); QCOMPARE(note->length().getTicks(), expectedLength); QCOMPARE(note->type(), type); QCOMPARE(note->selectionIdentity(), identity); QCOMPARE(changed, position != expectedPosition || length != expectedLength);
	}
	void nativeInvalidAdmissionDoesNotEditOtherNotes_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("zero-grid") << 0; QTest::newRow("negative-grid") << 1; QTest::newRow("oversize-grid") << 2; QTest::newRow("invalid-mode") << 3; QTest::newRow("null-note") << 4; QTest::newRow("duplicate-note") << 5; QTest::newRow("foreign-note") << 6; QTest::newRow("negative-source-position") << 7; QTest::newRow("source-length-out-of-range") << 8; }
	void nativeInvalidAdmissionDoesNotEditOtherNotes()
	{
		QFETCH(int, failure); seed(); auto selection = midi()->notes(); int grid = 48; auto mode = MidiClip::QuantizeMode::Both;
		if (failure == 0) { grid = 0; } if (failure == 1) { grid = -1; } if (failure == 2) { grid = MaxSongLength + 1; } if (failure == 3) { mode = static_cast<MidiClip::QuantizeMode>(99); } if (failure == 4) { selection.push_back(nullptr); } if (failure == 5) { selection.push_back(selection[0]); } if (failure == 6) { selection.push_back(midi(1)->addNote(Note(48, 0, 60), false)); } if (failure == 7) { selection[1]->setPos(-1); } if (failure == 8) { selection[1]->setLength(MaxSongLength + 1); }
		bool changed = true; QString error; QVERIFY(!midi()->quantizeNotes(selection, grid, mode, changed, error)); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QCOMPARE(selection[0]->pos().getTicks(), 24); QCOMPARE(selection[0]->length().getTicks(), 48); QVERIFY(linked(selection[0], selection[1]));
	}
	void endpointOverflowRefusesCompleteEdit_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("both") << 0; QTest::newRow("positions") << 1; QTest::newRow("lengths") << 2; }
	void endpointOverflowRefusesCompleteEdit()
	{
		QFETCH(int, mode); seed(); auto notes = midi()->notes(); notes[1]->setPos(MaxSongLength - 24); notes[1]->setLength(24); bool changed = true; QString error; QVERIFY(!midi()->quantizeNotes(notes, 48, static_cast<MidiClip::QuantizeMode>(mode), changed, error)); QVERIFY(!changed); QCOMPARE(notes[0]->pos().getTicks(), 24); QCOMPARE(notes[1]->pos().getTicks(), MaxSongLength - 24); QCOMPARE(notes[1]->length().getTicks(), 24); QVERIFY(linked(notes[0], notes[1]));
	}
	void noOpEmptyAndZeroLengthSelectionsPublishNoNativeHistory()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); QSignalSpy notifications(midi(), &MidiClip::dataChanged); bool changed = true; QString error;
		QVERIFY(midi()->quantizeNotes(midi()->notes(), 24, MidiClip::QuantizeMode::Both, changed, error)); QVERIFY(!changed); QVERIFY(!journal->canUndo()); QCOMPARE(notifications.count(), 0); QVERIFY(midi()->quantizeNotes({}, 48, MidiClip::QuantizeMode::Both, changed, error)); QVERIFY(!changed); QVERIFY(!journal->canUndo()); midi()->notes()[0]->setLength(0); QVERIFY(midi()->quantizeNotes(NoteVector{midi()->notes()[0]}, 48, MidiClip::QuantizeMode::Both, changed, error)); QVERIFY(!changed); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(!journal->canUndo());
	}
	void nativeUndoRedoRetainsClipAndClosedDetuningGraph()
	{
		seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); bool changed; QString error; QVERIFY(midi()->quantizeNotes(midi()->notes(), 48, MidiClip::QuantizeMode::Both, changed, error)); QVERIFY(changed); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void enabledHistoryRefusesOutsideGraphWhileDisabledHistoryKeepsIt()
	{
		seed(); QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); auto journal = Engine::projectJournal(); journal->setJournalling(true); bool changed; QString error; QVERIFY(!midi()->quantizeNotes(midi()->notes(), 48, MidiClip::QuantizeMode::Both, changed, error)); QVERIFY(!changed); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(!journal->canUndo()); journal->setJournalling(false); QVERIFY(midi()->quantizeNotes(midi()->notes(), 48, MidiClip::QuantizeMode::Both, changed, error)); QVERIFY(midi()->mutedModel()->isLinkedToModel(midi(1)->mutedModel()));
	}
	void ownershipRecordingAndCancellationRetainAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("selected-note-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("unselected-note-moved") << 5; }
	void ownershipRecordingAndCancellationRetainAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto notes = clip->notes(); auto outside = failure == 5 ? clip->addNote(Note(48, 36, 70), false) : nullptr; const QPointer<MidiClip> weak(clip); bool changed = true, success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->quantizeNotes(notes, 48, MidiClip::QuantizeMode::Both, changed, error); clip->getTrack()->endNoteRecording(&capture); }
		else if (failure == 1) { std::thread worker([&] { success = clip->quantizeNotes(notes, 48, MidiClip::QuantizeMode::Both, changed, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(notes[0]); } if (failure == 4) { delete clip; } if (failure == 5) { outside->setPos(40); } } return failure == 2; }, false); success = clip->quantizeNotes(notes, 48, MidiClip::QuantizeMode::Both, changed, error); }
		QVERIFY(!success); QVERIFY(!changed); QVERIFY(!error.isEmpty()); QVERIFY(!timer); if (weak && failure != 3) { QCOMPARE(weak->notes()[0]->pos().getTicks(), 24); } if (failure == 5) { QCOMPARE(outside->pos().getTicks(), 40); }
	}
	void malformedStructuredRequestsRefuseMutation_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fractional-index") << 3; QTest::newRow("negative-index") << 4; QTest::newRow("string-index") << 5; QTest::newRow("missing-note") << 6; QTest::newRow("too-many") << 7; QTest::newRow("missing-grid") << 8; QTest::newRow("zero-grid") << 9; QTest::newRow("oversize-grid") << 10; QTest::newRow("fractional-grid") << 11; QTest::newRow("invalid-mode") << 12; QTest::newRow("invalid-container") << 13; QTest::newRow("wrong-clip-type") << 14; QTest::newRow("unknown-field") << 15; QTest::newRow("negative-grid") << 16;
	}
	void malformedStructuredRequestsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{2}); } if (failure == 7) { QJsonArray notes; for (int i = 0; i < 4097; ++i) { notes.append(i); } args.insert("notes", notes); } if (failure == 8) { args.remove("grid"); } if (failure == 9) { args.insert("grid", 0); } if (failure == 10) { args.insert("grid", MaxSongLength + 1); } if (failure == 11) { args.insert("grid", .5); } if (failure == 12) { args.insert("mode", "nearest"); } if (failure == 13) { args.insert("container", "other"); } if (failure == 14) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 15) { args.insert("ticks", 0); } if (failure == 16) { args.insert("grid", -1); }
		QVERIFY(!StudioCapabilities::execute("note.selection.quantize", args).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QCOMPARE(midi()->notes()[0]->length().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void sortedReceiptsAndDefaultModeWorkInBothContainers_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void sortedReceiptsAndDefaultModeWorkInBothContainers()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } const auto notes = clip->notes(); clip->addNote(Note(48, 36, 70), false); auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); args.insert("notes", QJsonArray{2, 0}); const auto before = StudioCapabilities::selection("note.selection.quantize", args); const auto response = StudioCapabilities::execute("note.selection.quantize", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("mode").toString(), QString("both")); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 2); QCOMPARE(receipt[0].toObject().value("note").toInt(), 2); QCOMPARE(receipt[0].toObject().value("position").toInt(), 96); QCOMPARE(receipt[1].toObject().value("note").toInt(), 1); QCOMPARE(receipt[1].toObject().value("length").toInt(), 48); QCOMPARE(clip->notes()[1], notes[0]); QCOMPARE(clip->notes()[2], notes[1]); QVERIFY(linked(notes[0], notes[1])); QVERIFY(before.changed(StudioCapabilities::selection("note.selection.quantize", args)));
	}
	void runtimeUndoRedoReplayAndReopeningKeepQuantizedGraph()
	{
		QTemporaryDir directory; QString error; const auto command = request("quantize", "note.selection.quantize", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 48); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRecoversAfterReturnedNotificationFailure_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("retire-note") << 1; QTest::newRow("retire-from-song-notification") << 2; }
	void runtimeRecoversAfterReturnedNotificationFailure()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto changed = [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[0]); } else { throw std::runtime_error("fixture quantization notification"); } }; if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); } const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(clip, &MidiClip::dataChanged, this, changed, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.selection.quantize", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 24); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsQuantizeAndInspectNativeNotes_data() { protocols(); }
	void scriptedAgentsQuantizeAndInspectNativeNotes()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Tighten the phrase"}, {"steps", QJsonArray{"Quantize notes", "Inspect detuning"}}, {"decisions", QJsonArray{}}}), call("quantize", "note.selection.quantize", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase quantized"}, {"evidence", QJsonArray{"quantize", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Quantize this phrase to quarter-note positions and lengths."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "quantize").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes()[0]->pos().getTicks(), 48); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void scriptedAgentsReplanAfterQuantizationReordersIndices_data() { protocols(); }
	void scriptedAgentsReplanAfterQuantizationReordersIndices()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; auto args = arguments(); args.insert("notes", QJsonArray{0}); StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("quantize", "note.selection.quantize", args, catalog), call("stale", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old indices"}, {"evidence", QJsonArray{"quantize"}}})}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 1}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed quantized phrase"}, {"evidence", QJsonArray{"quantize", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); midi()->addNote(Note(48, 36, 70), false); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Quantize the first note and inspect it after sorting."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "quantize").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes()[1]->pos().getTicks(), 48);
	}
	void largeSelectionKeepsLinkedGraphAndBoundedReceipts()
	{
		seed(); midi()->clearNotes(); QJsonArray selection; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(25, index * 48 + 24, 60), false); note->createDetuning(); if (index) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(note->detuning().get())); } selection.append(index); } auto args = arguments(); args.insert("notes", selection); const auto result = StudioCapabilities::execute("note.selection.quantize", args); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QCOMPARE(result.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(midi()->notes()[0]->detuning()->countLinks(), std::size_t{1049}); QCOMPARE(midi()->notes()[1049]->pos().getTicks(), 1050 * 48); QCOMPARE(midi()->notes()[1049]->length().getTicks(), 48);
	}
	void schemaAndBothToolRoutesRespectBoundsAndDeclarationBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.quantize") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(properties.value("grid").toObject().value("minimum").toInt(), 1); QCOMPARE(properties.value("grid").toObject().value("maximum").toInt(), MaxSongLength); QCOMPARE(properties.value("mode").toObject().value("default").toString(), QString("both")); QVERIFY(!properties.contains("semitones")); QCOMPARE(properties.value("notes").toObject().value("maxItems").toInt(), 4096);
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_quantize"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.quantize"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.quantize")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionQuantizeTest)
#include "StudioNoteSelectionQuantizeTest.moc"
